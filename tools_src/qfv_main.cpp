// qfv: QuickFV command-line entry point (M1: lint, compile-sva, check-sva).
//
//   qfv lint        [opts] files...   T0 diagnostics + assertion inventory (JSON)
//   qfv compile-sva [opts] -o DIR files...
//                                     rewrite sources with SVA lowered to monitors
//   qfv check-sva   [opts] --module M --sva TEXT files...
//                                     T0 for one new assertion inserted into module M
//   qfv bmc         [opts] --top T [--reset-expr E] [--budget S] files...
//   qfv bmc         --btor FILE [--budget S]
//                                     bug hunt with the built-in incremental BMC;
//                                     NDJSON events, BTOR2 witnesses for CEXs
//
// opts: --top T   -D NAME[=VAL]   -I DIR   --drop-unsupported   --vacuity-covers
//
// Output is one JSON object on stdout. Exit code: 0 ok, 1 errors in the
// design or assertion, 2 usage or tool error.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>

#include "json.h"
#include "lint.h"
#include "session.h"
#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/text/SourceManager.h"
#include "engine/bmc.h"
#include "engine/sim.h"
#include "model/ts.h"
#include "sva/lower.h"
#include "sva/monitor.h"

using namespace qfv;
namespace fs = std::filesystem;

namespace {

struct Args {
    std::string command;
    SessionOptions session;
    std::string outDir;
    std::string module;
    std::string sva;
    bool dropUnsupported = false;
    bool vacuityCovers = false;
    // bmc
    std::string btor;       // skip the SV flow and check this BTOR2 file
    std::string witness;    // sim: BTOR2 witness to replay
    std::string workDir;
    std::string clock = "clk";
    std::string resetExpr;  // JasperGold-style `reset -expression`
    bool resetFree = false; // leave reset unconstrained after the first cycle
    double budget = 600;
    int maxDepth = 1 << 30;
};

[[noreturn]] void usage(const std::string& msg) {
    std::cerr << "qfv: " << msg << "\n"
              << "usage: qfv lint|compile-sva|check-sva [--top T] [-D N[=V]] [-I DIR] "
                 "[-o DIR] [--module M --sva TEXT] [--drop-unsupported] [--vacuity-covers] "
                 "files...\n";
    std::exit(2);
}

Args parseArgs(int argc, char** argv) {
    if (argc < 2)
        usage("missing command");
    Args a;
    a.command = argv[1];
    for (int i = 2; i < argc; i++) {
        std::string s = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc)
                usage("missing value for " + s);
            return argv[++i];
        };
        if (s == "--top")
            a.session.top = next();
        else if (s == "-D")
            a.session.defines.push_back(next());
        else if (s.rfind("-D", 0) == 0)
            a.session.defines.push_back(s.substr(2));
        else if (s == "-I")
            a.session.includeDirs.push_back(next());
        else if (s == "-o")
            a.outDir = next();
        else if (s == "--module")
            a.module = next();
        else if (s == "--sva")
            a.sva = next();
        else if (s == "--drop-unsupported")
            a.dropUnsupported = true;
        else if (s == "--vacuity-covers")
            a.vacuityCovers = true;
        else if (s == "--btor")
            a.btor = next();
        else if (s == "--witness")
            a.witness = next();
        else if (s == "--work")
            a.workDir = next();
        else if (s == "--clock")
            a.clock = next();
        else if (s == "--reset-expr")
            a.resetExpr = next();
        else if (s == "--reset-free")
            a.resetFree = true;
        else if (s == "--budget")
            a.budget = std::stod(next());
        else if (s == "--max-depth")
            a.maxDepth = std::stoi(next());
        else if (s.rfind("-", 0) == 0)
            usage("unknown option " + s);
        else
            a.session.files.push_back(s);
    }
    if (a.session.files.empty() && a.btor.empty())
        usage("no source files");
    return a;
}

bool hasErrors(const std::vector<DiagInfo>& diags) {
    for (auto& d : diags)
        if (d.severity == "error")
            return true;
    return false;
}

int cmdLint(Session& session, const Args&) {
    auto t0 = std::chrono::steady_clock::now();
    auto comp = session.compile();
    double elabMs = msSince(t0);
    auto diags = collectDiagnostics(*comp, session.sourceManager());
    auto sites = sva::collectAssertions(*comp);

    JsonWriter w;
    w.beginObject()
        .field("command", "lint")
        .field("parse_ms", session.parseMs())
        .field("elab_ms", elabMs)
        .field("ok", !hasErrors(diags));
    w.key("diagnostics");
    writeDiagnostics(w, diags);
    w.key("assertions");
    writeAssertions(w, sites);
    w.endObject();
    std::cout << w.str() << "\n";
    return hasErrors(diags) ? 1 : 0;
}

/// Replaces `// ` in front of each line, so arbitrary text becomes a comment.
std::string commentOut(std::string_view text, const std::string& reason) {
    std::string r = "// qfv: UNSUPPORTED (" + reason + "); dropped:\n// ";
    for (char c : text) {
        r += c;
        if (c == '\n')
            r += "// ";
    }
    return r + "\n";
}

struct CompileResult {
    std::vector<std::string> files;
    std::vector<DiagInfo> diags;
    std::vector<sva::AssertionSite> sites;
    int unsupported = 0;
    double ms = 0;
};

/// Lowers every supported assertion to a monitor and writes rewritten sources
/// to `outDir`. Unchanged files are referenced in place.
CompileResult compileSva(Session& session, const std::string& outDir, bool dropUnsupported,
                         bool vacuityCovers) {
    CompileResult res;
    auto t0 = std::chrono::steady_clock::now();
    auto comp = session.compile();
    res.diags = collectDiagnostics(*comp, session.sourceManager());
    res.sites = sva::collectAssertions(*comp);
    auto& sm = session.sourceManager();

    // Group text replacements by source buffer.
    struct Repl {
        size_t begin, end;
        std::string text;
    };
    std::map<uint32_t, std::vector<Repl>> repls;
    sva::MonitorOptions mo;
    mo.vacuityCover = vacuityCovers;
    for (auto& s : res.sites) {
        auto r = s.replaceRange;
        if (s.supported) {
            repls[r.start().buffer().getId()].push_back(
                {r.start().offset(), r.end().offset(), sva::emitMonitor(s.ir, mo)});
        }
        else {
            res.unsupported++;
            if (dropUnsupported) {
                auto src = sm.getSourceText(r.start().buffer());
                auto orig = src.substr(r.start().offset(), r.end().offset() - r.start().offset());
                repls[r.start().buffer().getId()].push_back(
                    {r.start().offset(), r.end().offset(), commentOut(orig, s.reason)});
            }
        }
    }

    fs::create_directories(outDir);
    std::set<std::string> used;
    for (size_t i = 0; i < session.numTrees(); i++) {
        std::string path = session.options().files[i];
        uint32_t id = session.buffer(i).getId();
        if (!repls.count(id)) {
            res.files.push_back(fs::absolute(path).string());
            continue;
        }
        auto text = std::string(session.sourceText(i));
        auto& rs = repls[id];
        std::sort(rs.begin(), rs.end(), [](auto& a, auto& b) { return a.begin > b.begin; });
        for (auto& r : rs)
            text.replace(r.begin, r.end - r.begin, r.text);
        std::string name = fs::path(path).filename().string();
        while (used.count(name))
            name = "_" + name;
        used.insert(name);
        auto outPath = fs::absolute(fs::path(outDir) / name).string();
        std::ofstream(outPath) << text;
        res.files.push_back(outPath);
    }
    res.ms = msSince(t0);
    return res;
}

int cmdCompile(Session& session, const Args& args) {
    if (args.outDir.empty())
        usage("compile-sva needs -o DIR");
    auto res = compileSva(session, args.outDir, args.dropUnsupported, args.vacuityCovers);
    bool ok = !hasErrors(res.diags) && (res.unsupported == 0 || args.dropUnsupported);
    JsonWriter w;
    w.beginObject()
        .field("command", "compile-sva")
        .field("ms", res.ms)
        .field("ok", ok)
        .field("unsupported", res.unsupported);
    w.key("files").beginArray();
    for (auto& f : res.files)
        w.value(f);
    w.endArray();
    w.key("diagnostics");
    writeDiagnostics(w, res.diags);
    w.key("assertions");
    writeAssertions(w, res.sites);
    w.endObject();
    std::cout << w.str() << "\n";
    return ok ? 0 : 1;
}

int cmdCheck(Session& session, const Args& args) {
    if (args.module.empty() || args.sva.empty())
        usage("check-sva needs --module M --sva TEXT");
    auto where = session.findModuleEnd(args.module);
    if (!where)
        usage("module '" + args.module + "' not found in the sources");
    auto [index, offset] = *where;

    auto t0 = std::chrono::steady_clock::now();
    std::string text(session.sourceText(index));
    std::string insert = "\n" + args.sva + "\n";
    text.insert(offset, insert);
    auto tree = session.reparse(index, text);
    auto comp = session.compile(index, tree);
    double ms = msSince(t0);

    auto buffer = tree->root().sourceRange().start().buffer();
    size_t lo = offset, hi = offset + insert.size();
    auto inProbe = [&](const sva::Location& l, slang::SourceLocation loc) {
        (void)l;
        return loc.buffer() == buffer && loc.offset() >= lo && loc.offset() < hi;
    };

    auto& sm = session.sourceManager();
    std::vector<DiagInfo> diags;
    {
        slang::DiagnosticEngine engine(sm);
        for (auto& d : comp->getAllDiagnostics()) {
            auto loc = sm.getFullyOriginalLoc(d.location);
            if (!inProbe({}, loc))
                continue;
            DiagInfo di;
            auto sev = engine.getSeverity(d.code, d.location);
            if (sev == slang::DiagnosticSeverity::Ignored)
                continue;
            di.severity = sev == slang::DiagnosticSeverity::Warning ? "warning"
                          : sev == slang::DiagnosticSeverity::Note  ? "note"
                                                                    : "error";
            di.code = std::string(toString(d.code));
            di.message = engine.formatMessage(d);
            di.loc = sva::toLocation(sm, loc);
            di.loc.line = di.loc.line - sm.getLineNumber(slang::SourceLocation(buffer, lo)) + 0;
            diags.push_back(std::move(di));
        }
    }
    std::vector<sva::AssertionSite> sites;
    for (auto& s : sva::collectAssertions(*comp))
        if (inProbe(s.loc, s.replaceRange.start()))
            sites.push_back(std::move(s));

    if (hasErrors(diags)) {
        // Parser error recovery can still produce a lowerable assertion; it is not.
        for (auto& s : sites) {
            s.supported = false;
            s.error = true;
            s.reason = "assertion has errors; see diagnostics";
            s.reasonLoc = s.loc;
        }
    }
    bool ok = !hasErrors(diags) && !sites.empty();
    for (auto& s : sites)
        ok = ok && s.supported;

    JsonWriter w;
    w.beginObject()
        .field("command", "check-sva")
        .field("module", args.module)
        .field("ms", ms)
        .field("ok", ok)
        .field("note", "line numbers in diagnostics are relative to the --sva text");
    w.key("diagnostics");
    writeDiagnostics(w, diags);
    w.key("assertions");
    writeAssertions(w, sites);
    w.endObject();
    std::cout << w.str() << "\n";
    return ok ? 0 : 1;
}

/// JasperGold-style reset: the reset expression holds in the first cycle, so the
/// search starts from the reset state, and (unless --reset-free) stays inactive
/// afterwards.
std::string resetEnv(const Args& a) {
    std::string s = "// Generated by qfv: reset -expression (" + a.resetExpr + ")\n"
                    "module qfv_env (input clk, input rst_active);\n"
                    "    reg first_cycle = 1'b1;\n"
                    "    always @(posedge clk) first_cycle <= 1'b0;\n"
                    "    always @(*) begin\n"
                    "        if (first_cycle) assume (rst_active);\n";
    if (!a.resetFree)
        s += "        else assume (!rst_active);\n";
    s += "    end\nendmodule\n\nbind " + a.session.top + " qfv_env qfv_env_inst (.clk(" +
         a.clock + "), .rst_active(" + a.resetExpr + "));\n";
    return s;
}

/// Yosys recipe: elaborate with slang, map memories to registers (the bit-level
/// engine has no arrays yet), then SymbiYosys's formal prep, then BTOR2.
std::string yosysScript(const Args& a, const std::vector<std::string>& files,
                        const std::string& btorPath) {
    std::string defs;
    for (auto& d : a.session.defines)
        defs += " -D " + d;
    std::string incs;
    for (auto& d : a.session.includeDirs)
        incs += " -I " + d;
    std::string srcs;
    for (auto& f : files)
        srcs += " " + f;
    return "read_slang" + defs + incs + " --top " + a.session.top + srcs + "\n" +
           "prep -top " + a.session.top + "\n"
           "memory -nomap\nmemory_map\nopt_clean\n"
           "async2sync\nchformal -assume -early\n"
           "formalff -setundef -clk2ff -ff2anyinit -hierarchy\n"
           "chformal -live -fair -cover -remove\nopt_clean\n"
           "flatten\nsetundef -undriven -anyseq\nopt -fast\n"
           "delete -output\ndffunmap\n"
           "write_btor " + btorPath + "\n";
}

int cmdBmc(Session& session, const Args& args) {
    auto t0 = std::chrono::steady_clock::now();
    auto emit = [](JsonWriter& w) { std::cout << w.str() << "\n" << std::flush; };
    std::string work = args.workDir.empty() ? "qfv_work" : args.workDir;
    fs::create_directories(work);
    std::string btor = args.btor;
    double frontMs = 0;

    if (btor.empty()) {
        if (args.session.top.empty())
            usage("bmc needs --top");
        auto res = compileSva(session, work + "/src", /*dropUnsupported=*/true, false);
        if (hasErrors(res.diags)) {
            JsonWriter w;
            w.beginObject().field("event", "error").field("stage", "elaborate");
            w.key("diagnostics");
            writeDiagnostics(w, res.diags);
            w.endObject();
            emit(w);
            return 2;
        }
        {
            JsonWriter w;
            w.beginObject().field("event", "assertions").field("unsupported", res.unsupported);
            w.key("assertions");
            writeAssertions(w, res.sites);
            w.endObject();
            emit(w);
        }
        auto files = res.files;
        if (!args.resetExpr.empty()) {
            auto envPath = fs::absolute(fs::path(work) / "qfv_env.sv").string();
            std::ofstream(envPath) << resetEnv(args);
            files.push_back(envPath);
        }
        btor = fs::absolute(fs::path(work) / "model.btor").string();
        auto ys = fs::path(work) / "model.ys";
        std::ofstream(ys) << yosysScript(args, files, btor);
        const char* yosysEnv = std::getenv("QFV_YOSYS");
        std::string yosys = yosysEnv ? yosysEnv : "yosys";
        std::string cmd = yosys + " -q -m slang -l " + (fs::path(work) / "yosys.log").string() +
                          " -s " + ys.string() + " > /dev/null 2>&1";
        if (std::system(cmd.c_str()) != 0) {
            JsonWriter w;
            w.beginObject()
                .field("event", "error")
                .field("stage", "yosys")
                .field("log", (fs::path(work) / "yosys.log").string())
                .endObject();
            emit(w);
            return 2;
        }
        frontMs = msSince(t0);
    }

    auto t1 = std::chrono::steady_clock::now();
    TransitionSystem ts;
    std::string err;
    if (!loadBtor2(btor, ts, err)) {
        JsonWriter w;
        w.beginObject().field("event", "error").field("stage", "load").field("message", err).endObject();
        emit(w);
        return 2;
    }
    {
        JsonWriter w;
        w.beginObject()
            .field("event", "model")
            .field("btor", btor)
            .field("frontend_ms", frontMs)
            .field("load_ms", msSince(t1))
            .field("inputs", uint64_t(ts.inputs.size()))
            .field("latches", uint64_t(ts.latches.size()))
            .field("ands", uint64_t(ts.aig.numAnds()))
            .field("constraints", uint64_t(ts.constraints.size()));
        w.key("properties").beginArray();
        for (auto& p : ts.props)
            w.value(p.name);
        w.endArray().endObject();
        emit(w);
    }

    BmcOptions bo;
    bo.budgetSeconds = args.budget;
    bo.maxDepth = args.maxDepth;
    Bmc bmc(ts, bo);
    int cexCount = 0;
    auto results = bmc.run([&](size_t p, const PropResult& r) {
        JsonWriter w;
        w.beginObject()
            .field("event", "result")
            .field("property", ts.props[p].name)
            .field("status", r.status)
            .field("ms", r.ms);
        if (r.cex) {
            cexCount++;
            auto path = (fs::path(work) / ("cex_" + std::to_string(p) + ".wit")).string();
            std::ofstream(path) << btor2Witness(ts, p, *r.cex);
            w.field("depth", r.cex->depth)
                .field("length", r.cex->depth + 1)
                .field("witness", fs::absolute(path).string());
        }
        else {
            w.field("depth_reached", r.depthChecked).field("note", "bounded: not a proof");
        }
        w.endObject();
        emit(w);
    });
    JsonWriter w;
    w.beginObject()
        .field("event", "summary")
        .field("ms", msSince(t0))
        .field("cex", cexCount)
        .field("pass_bounded", int(results.size()) - cexCount)
        .field("sat_vars", uint64_t(bmc.numVars()))
        .field("sat_clauses", uint64_t(bmc.numClauses()))
        .endObject();
    emit(w);
    return cexCount ? 1 : 0;
}

/// Replays a BTOR2 witness on our bit-blasted model; prints the reached bad
/// properties in btorsim's format so the two can be compared directly.
int cmdSim(const Args& args) {
    TransitionSystem ts;
    SimTrace trace;
    std::string err;
    if (!loadBtor2(args.btor, ts, err) || !parseWitness(args.witness, ts, trace, err)) {
        std::cerr << "qfv: " << err << "\n";
        return 2;
    }
    bool held = true;
    auto bads = simulate(ts, trace, held);
    std::cout << "reached bad state properties {";
    for (size_t f = 0; f < bads.size(); f++)
        for (size_t p : bads[f])
            std::cout << " b" << p << "@" << f;
    std::cout << " }\n" << (held ? "constraints always satisfied" : "constraint violated") << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    auto args = parseArgs(argc, argv);
    Session session(args.session);
    if (args.command == "bmc" && !args.btor.empty())
        return cmdBmc(session, args);
    if (args.command == "sim")
        return cmdSim(args);
    std::string err;
    if (!session.load(err)) {
        std::cerr << "qfv: " << err << "\n";
        return 2;
    }
    if (args.command == "lint")
        return cmdLint(session, args);
    if (args.command == "compile-sva")
        return cmdCompile(session, args);
    if (args.command == "check-sva")
        return cmdCheck(session, args);
    if (args.command == "bmc")
        return cmdBmc(session, args);
    usage("unknown command " + args.command);
}
