#include "session/design_session.h"
#include "session/reset_env.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

#include "engine/external.h"
#include "engine/hunt.h"
#include "engine/portfolio.h"
#include "engine/unroll.h"
#include "lint.h"
#include "model/btor2_write.h"
#include "slang/ast/ASTVisitor.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/statements/MiscStatements.h"
#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/text/SourceManager.h"
#include "sva/aig_emit.h"
#include "sva/lower.h"

namespace qfv {

namespace fs = std::filesystem;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

std::string tool(const char* env, const char* fallback) {
    const char* v = std::getenv(env);
    return v ? v : fallback;
}

double msSinceT(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

json diagJson(const std::vector<DiagInfo>& diags) {
    json a = json::array();
    for (auto& d : diags)
        a.push_back({{"severity", d.severity}, {"code", d.code}, {"message", d.message},
                     {"file", d.loc.file}, {"line", d.loc.line}, {"column", d.loc.column}});
    return a;
}

bool hasError(const std::vector<DiagInfo>& diags) {
    for (auto& d : diags)
        if (d.severity == "error")
            return true;
    return false;
}

/// slang arguments of a session model (sources, defines, include dirs,
/// parameter overrides), for a command file: see writeSlangArgsFile.
std::vector<std::string> slangArgs(const SetupConfig& c, const std::vector<std::string>& files,
                                   const std::string& cwd) {
    std::vector<std::string> a;
    for (auto& d : c.defines)
        a.insert(a.end(), {"-D", d});
    for (auto& i : c.includeDirs)
        a.insert(a.end(), {"-I", dollarFreePath(fs::absolute(i).string(), cwd, true)});
    for (auto& p : c.paramOverrides)
        a.insert(a.end(), {"-G", p});
    for (auto& f : files)
        a.push_back(dollarFreePath(fs::absolute(f).string(), cwd));
    return a;
}

/// Yosys recipe for a session model: every named wire is kept and exposed as
/// a BTOR2 output, so assertions added later can refer to it by name. Runs in
/// the work directory: `args` (the slang command file) and `btor` are relative
/// names there, so no user path is ever tokenized by Yosys.
std::string sessionYosysScript(const SetupConfig& c, const std::string& args, const std::string& btor) {
    return "read_slang -F " + args + " --top " + c.top + "\n"
                 // Order matters: keep every named wire before any optimization, and map
                 // memories to per-element registers BEFORE flattening (flattening
                 // first merged a register array into one misnamed vector).
                 "hierarchy -top " + c.top + "\nproc\n"
                 // X (e.g. an out-of-range read) is an unconstrained value in formal
                 // semantics: make it an explicit free input before any optimization,
                 // instead of letting opt pick a convenient constant.
                 "setundef -anyseq\n"
                 "setattr -set keep 1 w:*\n"
                 "memory -nomap\nmemory_map\nopt_clean\nflatten\n"
                 "async2sync\nchformal -assume -early\n"
                 "formalff -setundef -clk2ff -ff2anyinit -hierarchy\n"
                 "chformal -live -fair -cover -remove\nopt_clean\n"
                 "expose w:*\n"
                 "setundef -undriven -anyseq\nopt -fast\ndffunmap\n"
                 "write_btor " + btor + "\n";
}

/// Rewrites a property's text so every design name is a hierarchical path
/// from the top (JasperGold's `assert -name N {...}` is evaluated in the top scope).
struct NameCollector : public slang::ast::ASTVisitor<NameCollector, slang::ast::VisitFlags::Expressions> {
    std::vector<const slang::ast::ValueExpressionBase*> names;
    void handle(const slang::ast::NamedValueExpression& e) { names.push_back(&e); }
};

std::string topScopeText(const slang::ast::ConcurrentAssertionStatement& stmt,
                         const slang::SourceManager& sm, const std::string& instancePath) {
    auto* syn = stmt.propertySpec.syntax;
    if (!syn)
        return "";
    auto range = syn->sourceRange();
    if (sm.isMacroLoc(range.start()))
        return "";
    auto text = std::string(sm.getSourceText(range.start().buffer())
                                .substr(range.start().offset(), range.end().offset() - range.start().offset()));
    if (instancePath.empty())
        return text;
    NameCollector nc;
    stmt.propertySpec.visit(nc);
    std::vector<std::pair<size_t, size_t>> spots;
    for (auto n : nc.names) {
        auto r = n->sourceRange;
        if (r.start().buffer() != range.start().buffer() || r.start().offset() < range.start().offset() ||
            r.end().offset() > range.end().offset())
            continue;
        spots.push_back({r.start().offset() - range.start().offset(), r.end().offset() - range.start().offset()});
    }
    std::sort(spots.begin(), spots.end(), [](auto& a, auto& b) { return a.first > b.first; });
    spots.erase(std::unique(spots.begin(), spots.end()), spots.end());
    for (auto [b, e] : spots)
        text.insert(b, instancePath + ".");
    return text;
}


/// A Tcl word for `s`: as is when it has no special characters, else braced
/// (or backslash-escaped when braces would not quote it).
std::string tclWord(const std::string& s) {
    if (!s.empty() && s.find_first_of(" \t\n;$[]{}\"\\#") == std::string::npos)
        return s;
    int depth = 0;
    bool braceable = s.empty() || s.back() != '\\';
    for (char c : s) {
        depth += c == '{' ? 1 : c == '}' ? -1 : 0;
        braceable = braceable && depth >= 0 && c != '\\';
    }
    if (braceable && depth == 0)
        return "{" + s + "}";
    std::string out;
    for (char c : s) {
        if (std::string(" \t;$[]{}\"\\#").find(c) != std::string::npos)
            out += '\\';
        out += c == '\n' ? std::string("\\n") : std::string(1, c);
    }
    return out;
}

/// Yosys's "topological loop" means a combinational cycle, which in practice is
/// a latch inferred in always_comb (a branch that assigns nothing). Say so.
static std::string explainYosysError(const std::string& output) {
    if (output.find("Found topological loop") == std::string::npos)
        return "";
    return "combinational loop in the design (usually a latch inferred in always_comb: a case/if branch "
           "that assigns nothing, e.g. a 'case' without 'default'). QuickFV v1 does not model latches: "
           "assign in every branch (or add a default), or hand the design to JasperGold.";
}

} // namespace

DesignSession::DesignSession(std::string workDir) : work(fs::absolute(workDir).string()) {
    fs::create_directories(work);
}

DesignSession::~DesignSession() = default;

std::string DesignSession::uniqueId(const std::string& label) const {
    std::set<std::string> used;
    for (auto& e : entries)
        used.insert(e.id);
    if (!used.count(label))
        return label;
    for (int n = 2;; n++)
        if (!used.count(label + "#" + std::to_string(n)))
            return label + "#" + std::to_string(n);
}

std::vector<Lit> DesignSession::activeAssumptions() const {
    std::vector<Lit> v;
    for (auto& e : entries)
        if (e.kind == "assume" && e.status == "ready" && e.active)
            v.push_back(e.assumeLit);
    return v;
}

void DesignSession::markStale() {
    for (auto& e : entries)
        if (!e.last.is_null())
            e.last["stale"] = true;
}

json DesignSession::load(const SetupConfig& c) {
    // Transactional: the new design replaces the session only if every stage
    // succeeds; on any failure the previous design (if any) stays loaded as it was.
    struct Saved {
        SetupConfig cfg;
        std::unique_ptr<Session> src;
        std::unique_ptr<TransitionSystem> ts;
        std::unique_ptr<Unroller> bmc, step;
        std::vector<Entry> entries;
        std::vector<std::string> removedFileAssumptions;
        json loadInfo;
        size_t rebuilds;
    };
    Saved old{std::move(cfg), std::move(src), std::move(ts), std::move(bmc), std::move(step),
              std::move(entries), std::move(removedFileAssumptions), std::move(loadInfo), rebuilds};
    entries.clear();
    removedFileAssumptions.clear();
    rebuilds = 0;
    bool previous = old.ts != nullptr;
    auto result = loadNew(c);
    if (result.value("ok", false))
        return result;
    cfg = std::move(old.cfg);
    src = std::move(old.src);
    ts = std::move(old.ts);
    bmc = std::move(old.bmc);
    step = std::move(old.step);
    entries = std::move(old.entries);
    removedFileAssumptions = std::move(old.removedFileAssumptions);
    loadInfo = std::move(old.loadInfo);
    rebuilds = old.rebuilds;
    result["previous_design"] = previous ? "still loaded (unchanged)" : "none";
    return result;
}

json DesignSession::loadNew(const SetupConfig& c) {
    auto t0 = Clock::now();
    cfg = c;
    // Names that end up in generated Yosys, SV and Tcl text must be identifiers.
    static const std::regex ident(R"([A-Za-z_][A-Za-z0-9_$]*)");
    for (auto* name : {&cfg.top, &cfg.clock})
        if (!std::regex_match(*name, ident))
            return {{"ok", false}, {"stage", "setup"}, {"error", "'" + *name + "' is not a plain SystemVerilog identifier"}};
    SessionOptions so{cfg.files, cfg.defines, cfg.includeDirs, cfg.top, cfg.paramOverrides};
    src = std::make_unique<Session>(so);
    std::string err;
    if (!src->load(err))
        return {{"ok", false}, {"stage", "parse"}, {"error", err}};
    auto comp = src->compile();
    auto diags = collectDiagnostics(*comp, src->sourceManager());
    if (hasError(diags))
        return {{"ok", false}, {"stage", "elaborate"}, {"diagnostics", diagJson(diags)}};
    auto sites = sva::collectAssertions(*comp);
    double parseMs = msSinceT(t0);

    // Design-only sources: every assertion is taken out (they come back through
    // the session, as monitors in the AIG).
    std::map<uint32_t, std::vector<std::pair<size_t, size_t>>> cuts;
    for (auto& s : sites)
        cuts[s.replaceRange.start().buffer().getId()].push_back(
            {s.replaceRange.start().offset(), s.replaceRange.end().offset()});
    fs::create_directories(work + "/design");
    std::vector<std::string> files;
    for (size_t i = 0; i < src->numTrees(); i++) {
        auto id = src->buffer(i).getId();
        if (!cuts.count(id)) {
            files.push_back(cfg.files[i]);
            continue;
        }
        std::string text(src->sourceText(i));
        auto& cs = cuts[id];
        std::sort(cs.begin(), cs.end(), [](auto& a, auto& b) { return a.first > b.first; });
        for (auto [b, e] : cs)
            text.replace(b, e - b, "/* qfv: assertion moved into the session */");
        auto out = work + "/design/" + std::to_string(i) + "_" + fs::path(cfg.files[i]).filename().string();
        std::ofstream(out) << text;
        files.push_back(out);
    }
    if (!cfg.resetExpr.empty()) {
        std::ofstream(work + "/design/qfv_env.sv")
            << resetEnvironment(cfg.top, cfg.clock, cfg.resetExpr, cfg.resetCycles, false);
        files.push_back(work + "/design/qfv_env.sv");
    }
    auto btor = work + "/design.btor";
    std::string argsError;
    try {
        argsError = writeSlangArgsFile(work + "/design.f", slangArgs(cfg, files, work));
    }
    catch (const std::exception& e) {
        argsError = e.what();
    }
    if (!argsError.empty())
        return {{"ok", false}, {"stage", "yosys"}, {"error", argsError}};
    std::ofstream(work + "/design.ys") << sessionYosysScript(cfg, "design.f", "design.btor");
    auto t1 = Clock::now();
    std::atomic<bool> never{false};
    auto y = runProcess({tool("QFV_YOSYS", "yosys"), "-q", "-m", "slang", "-l", work + "/yosys.log", "-s",
                         work + "/design.ys"},
                        Clock::now() + std::chrono::hours(1), never, work);
    double yosysMs = msSinceT(t1);
    if (!y.finished || y.exitCode != 0) {
        json err = {{"ok", false}, {"stage", "yosys"}, {"log", work + "/yosys.log"}, {"output", y.output.substr(0, 2000)}};
        if (auto why = explainYosysError(y.output); !why.empty())
            err["error"] = why;
        return err;
    }
    ts = std::make_unique<TransitionSystem>();
    if (!loadBtor2(btor, *ts, err)) {
        ts.reset();
        return {{"ok", false}, {"stage", "load"}, {"error", err}};
    }
    // Several names can alias one register (a DUT register and the checker
    // port bound to it). Traces and replay testbenches need the name of the
    // actual variable, so ask slang which alias is one.
    for (auto& latch : ts->latches) {
        if (latch.aliases.size() < 2)
            continue;
        for (auto& alias : latch.aliases) {
            auto sym = comp->getRoot().lookupName(cfg.top + "." + alias);
            if (sym && sym->kind == slang::ast::SymbolKind::Variable) {
                latch.name = alias;
                break;
            }
        }
    }
    if (!ts->signals.count(cfg.clock)) {
        ts.reset();
        return {{"ok", false}, {"stage", "load"}, {"error", "clock '" + cfg.clock + "' is not a design input"}};
    }
    bmc = std::make_unique<Unroller>(*ts, false);
    step = std::make_unique<Unroller>(*ts, true);

    // Assertions from the source files, then from the setup file.
    auto fileResult = addFromCompilation(*comp, SIZE_MAX, 0, 0, "", "file");
    json setupResults = json::array();
    for (auto& p : cfg.properties) {
        auto text = p.name + ": " + p.kind + " property (@(posedge " + cfg.clock + ") " + p.text + ");";
        setupResults.push_back(add(cfg.top, text, "setup"));
    }
    // The environment must be exactly the one written: an assumption that cannot
    // be modeled (or a setup property with errors) would silently widen it, and
    // every CEX / PASS_BOUNDED after that would be about a different design.
    json rejected = json::array();
    auto collect = [&](const json& r, bool fromSetup) {
        for (auto& a : r.value("added", json::array()))
            if (a["status"] != "ready" && (a["kind"] == "assume" || (fromSetup && a["status"] == "error")))
                rejected.push_back(a);
        if (fromSetup && r.value("added", json::array()).empty() && !r.value("ok", false))
            rejected.push_back(r);
    };
    collect(fileResult, false);
    for (auto& r : setupResults)
        collect(r, true);
    if (!rejected.empty()) {
        std::string names;
        for (auto& a : rejected)
            names += (names.empty() ? "" : ", ") + a.value("id", std::string("?"));
        return {{"ok", false},
                {"stage", "environment"},
                {"error", "the environment cannot be modeled exactly (" + names +
                              "): unsupported or erroneous assumptions / setup properties are never dropped"},
                {"rejected", rejected},
                {"setup_results", setupResults}};
    }
    loadInfo = {{"ok", true},
                {"top", cfg.top},
                {"parse_ms", parseMs},
                {"yosys_ms", yosysMs},
                {"total_ms", msSinceT(t0)},
                {"inputs", ts->inputs.size()},
                {"registers", ts->latches.size()},
                {"and_gates", ts->aig.numAnds()},
                {"named_signals", ts->signals.size()},
                {"time_limit_s", cfg.timeLimitSeconds}};
    json out = loadInfo;
    out["assertions"] = list()["assertions"];
    return out;
}

json DesignSession::add(const std::string& module0, const std::string& text, const std::string& source) {
    if (!ts)
        return {{"ok", false}, {"error", "no design loaded"}};
    std::string module = module0.empty() ? cfg.top : module0;
    auto where = src->findModuleEnd(module);
    if (!where)
        return {{"ok", false}, {"error", "module '" + module + "' not found in the sources"}};
    auto t0 = Clock::now();
    auto [index, offset] = *where;
    std::string newText(src->sourceText(index));
    std::string insert = "\n" + text + "\n";
    newText.insert(offset, insert);
    auto tree = src->reparse(index, newText);
    auto comp = src->compile(index, tree);
    auto buffer = tree->root().sourceRange().start().buffer();
    auto result = addFromCompilation(*comp, buffer.getId(), offset, offset + insert.size(), module, source);
    result["ms"] = msSinceT(t0);
    return result;
}

json DesignSession::addFromCompilation(slang::ast::Compilation& comp, size_t probeBuffer, size_t lo, size_t hi,
                                       const std::string& module, const std::string& source) {
    auto& sm = src->sourceManager();
    auto inProbe = [&](slang::SourceLocation loc) {
        if (probeBuffer == SIZE_MAX)
            return true;
        auto l = sm.getFullyOriginalLoc(loc);
        return l.buffer().getId() == probeBuffer && l.offset() >= lo && l.offset() < hi;
    };
    std::vector<DiagInfo> diags;
    if (probeBuffer != SIZE_MAX) {
        slang::DiagnosticEngine engine(sm);
        for (auto& d : comp.getAllDiagnostics()) {
            if (!inProbe(d.location))
                continue;
            auto sev = engine.getSeverity(d.code, d.location);
            if (sev == slang::DiagnosticSeverity::Ignored)
                continue;
            DiagInfo di;
            di.severity = sev == slang::DiagnosticSeverity::Warning ? "warning"
                          : sev == slang::DiagnosticSeverity::Note  ? "note"
                                                                    : "error";
            di.code = std::string(toString(d.code));
            di.message = engine.formatMessage(d);
            di.loc = sva::toLocation(sm, d.location);
            diags.push_back(di);
        }
    }
    bool errors = hasError(diags);
    json added = json::array();
    size_t latchesBefore = ts->latches.size();
    // One monitor per instance: a module's assertion must hold in every instance.
    for (auto& site : sva::collectAssertions(comp, /*perInstance=*/true)) {
        if (!inProbe(site.replaceRange.start()))
            continue;
        Entry e;
        e.label = site.label.empty() ? (site.supported ? site.ir.label : "assertion_L" + std::to_string(site.loc.line))
                                     : site.label;
        // Several instances of one directive: ids are hierarchical ("u1.P").
        e.instancePath = site.numInstances > 1 ? site.instancePath : "";
        e.id = uniqueId(e.instancePath.empty() ? e.label : e.instancePath + "." + e.label);
        e.kind = site.directive;
        e.module = site.module.empty() ? module : site.module;
        e.source = source;
        auto r = site.replaceRange;
        e.text = std::string(sm.getSourceText(r.start().buffer()).substr(r.start().offset(),
                                                                         r.end().offset() - r.start().offset()));
        if (errors || site.error) {
            e.status = "error";
            e.reason = "assertion has errors; see diagnostics";
        }
        else if (!site.supported) {
            e.status = "unsupported";
            e.reason = site.reason;
        }
        else {
            try {
                auto res = sva::emitIntoAig(site.ir, *ts, cfg.top, cfg.clock);
                e.assertProp = res.assertProp;
                e.triggerProp = res.triggerProp;
                if (site.ir.kind == sva::DirectiveKind::Assume)
                    e.assumeLit = res.assumeOk;
                if (site.stmt)
                    e.topScopeText = topScopeText(*site.stmt, sm, site.instancePath);
            }
            catch (const sva::EmitError& ex) {
                e.status = "unsupported";
                e.reason = ex.what();
            }
        }
        added.push_back({{"id", e.id}, {"label", e.label}, {"kind", e.kind}, {"module", e.module},
                         {"status", e.status}, {"reason", e.reason}});
        // An assumption that is not modeled is not part of the environment: it is
        // reported and rejected, never kept as a silently inactive entry.
        if (e.kind == "assume" && e.status != "ready")
            continue;
        entries.push_back(std::move(e));
    }
    if (ts->latches.size() != latchesBefore) {
        bmc->syncLatches();
        step->syncLatches();
    }
    bool assumptionsChanged = false;
    for (auto& e : entries)
        if (e.kind == "assume" && e.status == "ready" && e.assumeBmc == SIZE_MAX) {
            e.assumeBmc = bmc->addAssumption(e.assumeLit);
            e.assumeStep = step->addAssumption(e.assumeLit);
            assumptionsChanged = true;
        }
    if (assumptionsChanged)
        markStale();
    bool ok = !errors && !added.empty();
    for (auto& a : added)
        ok = ok && a["status"] == "ready";
    return {{"ok", ok}, {"added", added}, {"diagnostics", diagJson(diags)},
            {"note", added.empty() && !errors ? "no concurrent assertion found in the text" : ""}};
}

void DesignSession::rebuildUnrollers() {
    bmc = std::make_unique<Unroller>(*ts, false);
    step = std::make_unique<Unroller>(*ts, true);
    for (auto& e : entries)
        if (e.kind == "assume" && e.status == "ready") {
            e.assumeBmc = bmc->addAssumption(e.assumeLit);
            e.assumeStep = step->addAssumption(e.assumeLit);
            bmc->setAssumptionActive(e.assumeBmc, e.active);
            step->setAssumptionActive(e.assumeStep, e.active);
        }
    rebuilds++;
}

json DesignSession::remove(const std::string& id) {
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (it->id != id)
            continue;
        if (it->kind == "assume" && it->assumeBmc != SIZE_MAX) {
            bmc->setAssumptionActive(it->assumeBmc, false);
            step->setAssumptionActive(it->assumeStep, false);
            markStale();
        }
        // The sources still contain a file assumption: JasperGold will apply it.
        if (it->kind == "assume" && it->source == "file")
            removedFileAssumptions.push_back(it->id);
        // The monitor's gates stay in the AIG but are never queried again, so
        // they cost nothing (encoding is lazy).
        entries.erase(it);
        return {{"ok", true}, {"removed", id}};
    }
    return {{"ok", false}, {"error", "no assertion '" + id + "'"}};
}

json DesignSession::list() const {
    json a = json::array();
    for (auto& e : entries) {
        json j = {{"id", e.id}, {"label", e.label}, {"kind", e.kind}, {"module", e.module},
                  {"source", e.source}, {"status", e.status}};
        if (!e.reason.empty())
            j["reason"] = e.reason;
        if (!e.last.is_null())
            j["last"] = e.last;
        a.push_back(j);
    }
    return {{"assertions", a}};
}

json DesignSession::check(const std::vector<std::string>& ids, double budget, double sim, const Emit& emit,
                          bool certify) {
    if (!ts)
        return {{"ok", false}, {"error", "no design loaded"}};
    auto t0 = Clock::now();
    std::map<size_t, Entry*> byProp;
    std::vector<size_t> props;
    std::vector<std::string> missing;
    for (auto& e : entries) {
        bool wanted = ids.empty() ? (e.kind != "assume") : std::count(ids.begin(), ids.end(), e.id) > 0;
        if (!wanted)
            continue;
        if (e.status != "ready") {
            emit({{"event", "result"}, {"id", e.id}, {"status", e.status == "error" ? "ERROR" : "UNSUPPORTED"},
                  {"reason", e.reason}});
            continue;
        }
        for (size_t p : {e.assertProp, e.triggerProp})
            if (p != SIZE_MAX) {
                byProp[p] = &e;
                props.push_back(p);
            }
    }
    for (auto& id : ids)
        if (std::none_of(entries.begin(), entries.end(), [&](auto& e) { return e.id == id; }))
            missing.push_back(id);
    if (props.empty())
        return {{"ok", missing.empty()}, {"missing", missing}, {"verdicts", json::object()}};

    auto assumptions = activeAssumptions();
    // One BTOR2 of the current model (with active assumptions) for btorsim.
    auto dir = work + "/check_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    fs::create_directories(dir);
    Btor2Layout layout;
    writeBtor2WithConstraints(*ts, dir + "/model.btor", assumptions, std::nullopt, &layout);

    // Persistent unrollers speed up re-checks, but an unbounded clause database
    // slows every later solve (M4: 20k frames left behind by one deep check cut
    // another check to depth 3). Past a size limit, rebuild: the model stays
    // loaded, only the SAT encoding is dropped (and re-created lazily).
    if (bmc->numClauses() + step->numClauses() > kMaxSessionClauses)
        rebuildUnrollers();
    HuntOptions ho;
    ho.budgetSeconds = budget;
    ho.simSeconds = sim;
    ho.onlyProps = props;
    for (auto& e : entries)
        if (e.assertProp != SIZE_MAX && e.triggerProp != SIZE_MAX && e.kind == "assert")
            ho.assertOfTrigger[e.triggerProp] = e.assertProp;
    ho.simConstraints = assumptions;
    if (!std::getenv("QFV_NO_RIC3")) {
        auto deadline = Clock::now() + std::chrono::milliseconds(int64_t(budget * 1000));
        ho.externalEngineName = "rIC3";
        ho.externalEngine = [this, dir, assumptions, deadline](size_t p, const std::atomic<bool>& cancel) {
            return runRic3(*ts, p, assumptions, dir + "/portfolio", deadline, cancel);
        };
    }
    if (certify) {
        ho.certifyDir = dir + "/certificate";
        fs::create_directories(ho.certifyDir);
    }
    Hunt hunt(*ts, ho);
    hunt.useUnrollers(bmc.get(), step.get());
    ReplayOptions ro{cfg.top, cfg.clock, ""};

    std::map<size_t, json> events;
    auto verdicts = hunt.run([&](size_t p, const Verdict& r) {
        auto& e = *byProp[p];
        bool isTrigger = p == e.triggerProp && e.kind != "cover";
        json ev = {{"event", "result"}, {"id", e.id}, {"goal", isTrigger ? "trigger-reachable" : e.kind == "cover" ? "cover" : "no-failure"},
                   {"status", r.status}, {"engine", r.engine}, {"ms", r.ms}};
        if (r.cex) {
            auto base = dir + "/" + e.id + (isTrigger ? "_trigger" : "") + "_len" + std::to_string(r.cex->depth + 1);
            std::ofstream(base + ".json") << jsonTrace(*ts, p, *r.cex, cfg.clock);
            std::ofstream(base + ".vcd") << vcdTrace(*ts, *r.cex, cfg.clock);
            std::ofstream(base + "_tb.sv") << replayTestbench(*ts, *r.cex, ro);
            std::ofstream(base + ".wit") << btor2WitnessForLayout(*ts, layout, p, *r.cex);
            // btorsim replays the witness on the written model: exact property and frame.
            std::string log;
            auto replay = replayBtorsim(dir + "/model.btor", base + ".wit", layout.badOfProp.at(p), r.cex->depth,
                                        std::chrono::seconds(30), &log);
            std::ofstream(base + ".btorsim.log") << log;
            ev["length"] = r.cex->depth + 1;
            ev["minimized"] = r.minimized;
            if (xDependent(*ts, p, *r.cex))
                ev["x_dependent"] = "fails only for some values of X (e.g. an out-of-range read); "
                                    "real in formal semantics, not reproducible in 2-state simulation";
            ev["trace"] = {{"json", base + ".json"}, {"vcd", base + ".vcd"}, {"testbench", base + "_tb.sv"},
                           {"witness", base + ".wit"}, {"model", dir + "/model.btor"}};
            ev["certified"] = {{"btorsim", replay}};
            // Only a replayed trace is a result; anything else is a candidate.
            if (replay != "confirmed") {
                ev["status"] = r.status + "_UNCONFIRMED";
                ev["note"] = "the trace was not confirmed by btorsim (" + replay + "); see " + base + ".btorsim.log";
            }
        }
        else if (r.status == "UNREACHABLE" || r.status == "PROVEN") {
            ev["proof"] = r.engine == "induction" ? "k-induction, k=" + std::to_string(r.inductionK) : r.engine;
            if (r.engine != "induction") {
                ev["proof_certified"] = r.proofCertified;
                ev["detail"] = r.detail;
            }
        }
        else {
            ev["depth_reached"] = r.depthChecked;
            ev["note"] = "bounded: not a proof";
        }
        events[p] = ev;
        emit(ev);
    });

    json summary = json::object();
    std::set<Entry*> seen;
    for (auto [p, e] : byProp) {
        if (!seen.insert(e).second)
            continue;
        // Statuses as reported (a trace btorsim did not confirm is *_UNCONFIRMED).
        auto status = [&](size_t q) -> std::string {
            if (q == SIZE_MAX)
                return "";
            return events.count(q) ? events[q]["status"].get<std::string>() : verdicts[q].status;
        };
        bool as = e->assertProp != SIZE_MAX, tr = e->triggerProp != SIZE_MAX;
        std::string sa = status(e->assertProp), st = status(e->triggerProp);
        std::string v;
        if (e->kind == "cover")
            v = st == "REACHABLE"               ? "COVERED"
                : st == "REACHABLE_UNCONFIRMED" ? "COVERED_UNCONFIRMED"
                : st == "UNREACHABLE"           ? "UNREACHABLE"
                                                : "NOT_COVERED";
        else if (sa == "CEX" || sa == "CEX_UNCONFIRMED")
            v = sa;
        // Vacuity outranks a proof (a vacuous proof says nothing about the design).
        else if (sa == "VACUOUS" || st == "UNREACHABLE")
            v = "VACUOUS";
        else if (sa == "PROVEN")
            v = "PROVEN";
        // An unconfirmed trigger witness does not show the trigger can fire.
        else if (st == "NOT_REACHED" || st == "REACHABLE_UNCONFIRMED")
            v = "POSSIBLY_VACUOUS";
        else
            v = "PASS_BOUNDED";
        json last = {{"verdict", v}, {"budget_s", budget}};
        if (as && e->assertProp != SIZE_MAX && events.count(e->assertProp))
            last["result"] = events[e->assertProp];
        if (tr && events.count(e->triggerProp))
            last["trigger"] = events[e->triggerProp];
        e->last = last;
        summary[e->id] = v;
    }
    json certificate = nullptr;
    if (certify) {
        certificate = json::array();
        for (auto& r : certifyBounded(ho.certifyDir, hunt.bmcClaims, hunt.stepClaims))
            certificate.push_back({{"solver", r.solver}, {"unsat_claims", r.unsatClaims}, {"lidrup_check", r.result}});
    }
    return {{"ok", true}, {"ms", msSinceT(t0)}, {"sim_cycles", hunt.simCycles}, {"verdicts", summary},
            {"certificate", certificate},
            {"missing", missing}, {"assumptions_active", assumptions.size()},
            {"solver_rebuilds", rebuilds}};
}

json DesignSession::trace(const std::string& id, const std::vector<std::string>& signals, int from, int to) const {
    for (auto& e : entries) {
        if (e.id != id)
            continue;
        if (e.last.is_null() || !e.last.contains("result") || !e.last["result"].contains("trace"))
            return {{"ok", false}, {"error", "no counterexample recorded for '" + id + "'; run check first"}};
        std::ifstream in(e.last["result"]["trace"]["json"].get<std::string>());
        json t = json::parse(in);
        json frames = json::array();
        for (auto& f : t["frames"]) {
            int c = f["cycle"];
            if (c < from || (to >= 0 && c > to))
                continue;
            if (!signals.empty()) {
                for (auto part : {"inputs", "registers"}) {
                    json keep = json::object();
                    for (auto& [k, v] : f[part].items())
                        for (auto& s : signals)
                            if (k.find(s) != std::string::npos)
                                keep[k] = v;
                    f[part] = keep;
                }
            }
            frames.push_back(f);
        }
        t["frames"] = frames;
        t["ok"] = true;
        return t;
    }
    return {{"ok", false}, {"error", "no assertion '" + id + "'"}};
}

std::string DesignSession::exportJasperGold() const {
    // Names of session properties in JasperGold: unique (ids are), and plain.
    auto jgName = [](const Entry& e) {
        std::string n = e.id;
        for (auto& c : n)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
                c = '_';
        return n;
    };
    std::ostringstream o;
    o << "# Generated by QuickFV (qfv): JasperGold handoff for top '" << cfg.top << "'.\n"
      << "# File assertions are already in the analyzed sources; setup and session\n"
      << "# assertions are added below, in top scope. QuickFV results are listed as\n"
      << "# comments so the JasperGold run can focus on what is still open.\n";
    if (!removedFileAssumptions.empty()) {
        o << "#\n# WARNING: the environments differ. These assumptions were removed in the\n"
          << "# QuickFV session but are still in the sources, so JasperGold applies them;\n"
          << "# QuickFV's results below were obtained WITHOUT them:\n";
        for (auto& id : removedFileAssumptions)
            o << "#   " << id << "\n";
    }
    o << "\nclear -all\n";
    std::string defs;
    for (auto& d : cfg.defines)
        defs += " " + tclWord("+define+" + d);
    for (auto& i : cfg.includeDirs)
        defs += " " + tclWord("+incdir+" + i);
    for (auto& f : cfg.files)
        o << "analyze -sv12" << defs << " " << tclWord(f) << "\n";
    o << "elaborate -top " << cfg.top;
    for (auto& p : cfg.paramOverrides) {
        auto eq = p.find('=');
        o << " -parameter " << tclWord(p.substr(0, eq)) << " " << tclWord(p.substr(eq + 1));
    }
    o << "\nclock " << cfg.clock << "\n";
    if (!cfg.resetExpr.empty()) {
        o << "reset -expression " << tclWord(cfg.resetExpr) << "\n";
        if (cfg.resetCycles > 1)
            o << "# QuickFV held reset for " << cfg.resetCycles
              << " cycles (reset -cycles, a QuickFV extension); JasperGold's reset analysis decides its own length\n";
    }
    o << "\n";
    for (auto& e : entries) {
        if (e.source == "file" || e.status == "error")
            continue;
        if (e.topScopeText.empty()) {
            o << "# " << e.id << ": could not be rewritten for top scope; add it to the sources\n";
            continue;
        }
        std::string cmd = e.kind == "assume" ? "assume" : e.kind == "cover" ? "cover" : "assert";
        o << cmd << " -name " << jgName(e) << " " << tclWord(e.topScopeText) << "\n";
    }
    o << "\n# QuickFV results:\n";
    std::vector<std::string> open;
    for (auto& e : entries) {
        if (e.kind == "assume")
            continue;
        std::string v = e.status != "ready" ? e.status : e.last.is_null() ? "not checked" : e.last["verdict"].get<std::string>();
        std::string extra;
        if (v == "PASS_BOUNDED" && e.last.contains("result") && e.last["result"].contains("depth_reached"))
            extra = " (no CEX to depth " + std::to_string(e.last["result"]["depth_reached"].get<int>()) + ", " +
                    std::to_string(int(e.last["budget_s"].get<double>())) + " s)";
        bool stale = !e.last.is_null() && e.last.value("stale", false);
        if (stale)
            extra += " [stale: assumptions changed since]";
        // File assertions keep JasperGold's own names (<top>.<instance path>.<label>).
        std::string ref = e.source == "file" ? e.id : jgName(e);
        o << "#   " << ref << ": " << v << extra << "\n";
        // Only a current, settled result takes a property off JasperGold's list.
        bool settled = v == "CEX" || v == "VACUOUS" || v == "PROVEN" || v == "COVERED" || v == "UNREACHABLE";
        if (!settled || stale)
            open.push_back(ref);
    }
    o << "\n# Still open after QuickFV (proof needed):\n";
    if (open.empty())
        o << "#   (none)\nprove -all\n";
    else {
        o << "prove -property {";
        for (size_t i = 0; i < open.size(); i++)
            o << (i ? " " : "") << "*" << open[i];
        o << "}\n";
    }
    return o.str();
}

} // namespace qfv
