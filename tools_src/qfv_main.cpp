// qfv: QuickFV command-line entry point.
//
//   qfv lint        [opts] files...   T0 diagnostics + assertion inventory (JSON)
//   qfv compile-sva [opts] -o DIR files...
//                                     rewrite sources with SVA lowered to monitors
//   qfv check-sva   [opts] --module M --sva TEXT files...
//                                     T0 for one new assertion inserted into module M
//   qfv bmc         [opts] --top T [--reset-expr E] [--budget S] files...
//   qfv bmc         --btor FILE [--budget S]
//                                     vacuity + bug hunt (simulation, incremental BMC,
//                                     k-induction, rIC3); NDJSON events, certified traces
//   qfv sim         [opts] --top T files...   random simulation / witness replay
//   qfv serve       [--work DIR]    session over stdio (JSON lines), used by the Tcl package
//   qfv mcp         [--work DIR]    MCP server over stdio, for agents
//
// opts: --top T   -D NAME[=VAL]   -I DIR   --drop-unsupported   --vacuity-covers
//
// Output is one JSON object on stdout. Exit code: 0 ok, 1 errors in the
// design or assertion, 2 usage or tool error.
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <stdlib.h>
#include <unistd.h>

#include "json.h"
#include "server.h"
#include "session/reset_env.h"
#include "lint.h"
#include "session.h"
#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/text/SourceManager.h"
#include "engine/external.h"
#include "engine/hunt.h"
#include "engine/portfolio.h"
#include "engine/sim.h"
#include "engine/trace.h"
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
    bool resetFree = false; // leave reset unconstrained after the reset cycles
    int resetCycles = 1;
    double budget = 600;
    int maxDepth = 1 << 30;
    double simSeconds = 0.25;
    bool replay = false;    // re-run every trace on the RTL in Verilator
    bool noRic3 = false;    // do not use rIC3 to prove vacuity
    bool certify = false;   // LIDRUP certificates for every UNSAT answer
};

[[noreturn]] void usage(const std::string& msg) {
    std::cerr << "qfv: " << msg << "\n"
              << "usage: qfv lint|compile-sva|check-sva [--top T] [-D N[=V]] [-I DIR] "
                 "[-o DIR] [--module M --sva TEXT] [--drop-unsupported] [--vacuity-covers] "
                 "files...\n"
                 "       qfv bmc|sim --top T --clock C [--reset-expr E] [--reset-cycles N] [--reset-free] "
                 "[--budget S] [--max-depth K] [--replay] [--certify] [--witness FILE] [--sim S] [--work DIR] "
                 "[--btor FILE] files...\n"
                 "       qfv serve|mcp [--work DIR]\n";
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
        else if (s == "--reset-cycles") {
            auto n = next();
            if (n.empty() || n.size() > 3 || n.find_first_not_of("0123456789") != std::string::npos ||
                std::stoi(n) < 1 || std::stoi(n) > 255)
                usage("--reset-cycles must be an integer from 1 to 255");
            a.resetCycles = std::stoi(n);
        }
        else if (s == "--budget")
            a.budget = std::stod(next());
        else if (s == "--max-depth")
            a.maxDepth = std::stoi(next());
        else if (s == "--sim")
            a.simSeconds = std::stod(next());
        else if (s == "--replay")
            a.replay = true;
        else if (s == "--no-ric3")
            a.noRic3 = true;
        else if (s == "--certify")
            a.certify = true;
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
/// With `lower` false, supported assertions keep their original SVA text and
/// only unsupported ones are dropped (used for independent RTL replay).
CompileResult compileSva(Session& session, const std::string& outDir, bool dropUnsupported,
                         bool vacuityCovers, bool reachAsBad = false, bool lower = true) {
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
    mo.reachAsBad = reachAsBad;
    for (auto& s : res.sites) {
        auto r = s.replaceRange;
        if (s.supported) {
            if (lower)
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

/// slang arguments (defines, include dirs, sources) for a command file.
std::vector<std::string> slangArgs(const Args& a, const std::vector<std::string>& files, const std::string& cwd) {
    std::vector<std::string> v;
    for (auto& d : a.session.defines)
        v.insert(v.end(), {"-D", d});
    for (auto& d : a.session.includeDirs)
        v.insert(v.end(), {"-I", dollarFreePath(fs::absolute(d).string(), cwd, true)});
    for (auto& f : files)
        v.push_back(dollarFreePath(fs::absolute(f).string(), cwd));
    return v;
}

/// Yosys recipe: elaborate with slang, map memories to registers (the bit-level
/// engine has no arrays yet), then SymbiYosys's formal prep, then BTOR2. Runs
/// in the work directory; `argsFile` and `btorPath` are relative names there.
std::string yosysScript(const Args& a, const std::string& argsFile, const std::string& btorPath) {
    return "read_slang -F " + argsFile + " --top " + a.session.top + "\n" +
           "hierarchy -top " + a.session.top + "\nproc\n"
           "setundef -anyseq\n" // X is a free value (formal semantics), not an opt choice
           "prep -top " + a.session.top + "\n"
           "memory -nomap\nmemory_map\nopt_clean\n"
           "async2sync\nchformal -assume -early\n"
           "formalff -setundef -clk2ff -ff2anyinit -hierarchy\n"
           "chformal -live -fair -cover -remove\nopt_clean\n"
           "flatten\nsetundef -undriven -anyseq\nopt -fast\n"
           "delete -output\ndffunmap\n"
           "write_btor " + btorPath + "\n";
}

std::string toolPath(const char* env, const char* fallback) {
    const char* v = std::getenv(env);
    return v ? v : fallback;
}

/// Verilator replays the trace on SV sources and must report `expectFail`
/// (an assertion label) failing.
std::string certifyReplay(const Args& args, const std::string& tb, const std::vector<std::string>& files,
                          const std::string& expectFail, const std::string& dir) {
    fs::create_directories(dir);
    std::error_code ec;
    // Verilator's generated makefiles cannot build in a directory with spaces
    // (GNU make): then the build goes to a private temporary directory.
    std::string obj = dir + "/obj";
    std::unique_ptr<void, std::function<void(void*)>> tempObj;
    if (dir.find_first_of(" \t$") != std::string::npos) {
        std::string tmpl = (fs::temp_directory_path() / "qfv_replay_XXXXXX").string();
        if (!mkdtemp(tmpl.data()))
            return "replay-build-error (cannot create a temporary build directory)";
        obj = tmpl;
        tempObj = {obj.data(), [obj](void*) { std::error_code e; fs::remove_all(obj, e); }};
    }
    else {
        fs::remove_all(obj, ec);
    }
    // Verilator runs in `dir`: with dollarFreePath, no file argument it would
    // expand contains '$'.
    std::vector<std::string> cmd = {toolPath("QFV_VERILATOR", "verilator"), "--binary", "--timing", "--assert",
                                    "-Wno-fatal", "-Wno-lint", "-Wno-style", "--top-module", "qfv_replay",
                                    "-Mdir", obj, "-o", "sim"};
    for (auto& d : args.session.defines)
        cmd.push_back("-D" + d);
    try {
        cmd.push_back(dollarFreePath(tb, dir));
        for (auto& f : files)
            cmd.push_back(dollarFreePath(f, dir));
    }
    catch (const std::exception& e) {
        return std::string("replay-build-error (") + e.what() + ")";
    }
    std::atomic<bool> never{false};
    auto now = [] { return std::chrono::steady_clock::now(); };
    auto build = runProcess(cmd, now() + std::chrono::minutes(30), never, dir);
    if (!build.finished || build.exitCode != 0 || !fs::exists(obj + "/sim")) {
        std::ofstream(dir + "/build.log") << build.output;
        return "replay-build-error (see " + dir + "/build.log)";
    }
    // Keep going after the first failure: several assertions may fail together.
    auto run = runProcess({obj + "/sim", "+verilator+error+limit+100000"}, now() + std::chrono::minutes(10), never);
    if (!run.finished)
        return "replay-timeout";
    auto& out = run.output;
    // Verilator prints "Assertion failed in <hier.path.label>" per failure.
    bool other = false;
    for (size_t at = 0; (at = out.find("Assertion failed in ", at)) != std::string::npos;) {
        at += 20;
        auto name = out.substr(at, out.find_first_of(":' \n", at) - at);
        if (name.size() >= expectFail.size() + 1 &&
            name.compare(name.size() - expectFail.size() - 1, std::string::npos, "." + expectFail) == 0)
            return "confirmed";
        other = true;
    }
    return other ? "other-assertion-failed" : "not-reproduced";
}

int cmdBmc(Session& session, const Args& args) {
    auto t0 = std::chrono::steady_clock::now();
    std::mutex outMutex;
    auto emit = [&](JsonWriter& w) {
        std::lock_guard lock(outMutex);
        std::cout << w.str() << "\n" << std::flush;
    };
    // RTL replays take seconds (a Verilator build each), so they run in the
    // background while the search continues, and report as separate events.
    std::vector<std::future<void>> replays;
    std::string work = fs::absolute(args.workDir.empty() ? "qfv_work" : args.workDir).string();
    fs::create_directories(work);
    std::string btor = args.btor;
    double frontMs = 0;
    std::vector<std::string> compiledFiles;
    std::vector<std::string> replayFiles; // original SVA; unsupported assertions dropped
    std::vector<sva::AssertionSite> sites;

    if (btor.empty()) {
        if (args.session.top.empty())
            usage("bmc needs --top");
        static const std::regex ident(R"([A-Za-z_][A-Za-z0-9_$]*)");
        if (!std::regex_match(args.session.top, ident) || !std::regex_match(args.clock, ident))
            usage("--top and --clock must be plain SystemVerilog identifiers");
        auto res = compileSva(session, work + "/src", /*dropUnsupported=*/true,
                              /*vacuityCovers=*/true, /*reachAsBad=*/true);
        if (hasErrors(res.diags)) {
            JsonWriter w;
            w.beginObject().field("event", "error").field("stage", "elaborate");
            w.key("diagnostics");
            writeDiagnostics(w, res.diags);
            w.endObject();
            emit(w);
            return 2;
        }
        // Unsupported assertions and covers are dropped (and reported), but an
        // assumption never is: without it every verdict is about a wider environment.
        std::vector<const sva::AssertionSite*> badAssumes;
        for (auto& s : res.sites)
            if (s.directive == "assume" && !s.supported)
                badAssumes.push_back(&s);
        if (!badAssumes.empty()) {
            JsonWriter w;
            w.beginObject().field("event", "error").field("stage", "environment")
                .field("error", "unsupported or erroneous assumptions are never dropped; rewrite them in the "
                                "supported subset or check this design in JasperGold");
            w.key("assumptions").beginArray();
            for (auto* s : badAssumes)
                w.beginObject().field("label", s->label).field("file", s->loc.file)
                    .field("line", uint64_t(s->loc.line)).field("reason", s->reason).endObject();
            w.endArray().endObject();
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
        compiledFiles = res.files;
        sites = res.sites;
        if (args.replay)
            replayFiles = compileSva(session, work + "/replay_src", true, false, false,
                                     /*lower=*/false).files;
        auto files = res.files;
        if (!args.resetExpr.empty()) {
            auto envPath = work + "/qfv_env.sv";
            std::ofstream(envPath) << resetEnvironment(args.session.top, args.clock, args.resetExpr,
                                                       args.resetCycles, args.resetFree);
            files.push_back(envPath);
            compiledFiles.push_back(envPath);
        }
        btor = work + "/model.btor";
        auto ys = work + "/model.ys";
        std::string argsError;
        try {
            argsError = writeSlangArgsFile(work + "/model.f", slangArgs(args, files, work));
        }
        catch (const std::exception& e) {
            argsError = e.what();
        }
        std::ofstream(ys) << yosysScript(args, "model.f", "model.btor");
        std::atomic<bool> never{false};
        auto y = argsError.empty()
                     ? runProcess({toolPath("QFV_YOSYS", "yosys"), "-q", "-m", "slang", "-l", work + "/yosys.log",
                                   "-s", ys},
                                  std::chrono::steady_clock::now() + std::chrono::hours(1), never, work)
                     : ProcessResult{true, 1, argsError};
        if (!y.finished || y.exitCode != 0) {
            std::ifstream logf(work + "/yosys.log");
            std::stringstream log;
            log << logf.rdbuf();
            JsonWriter w;
            w.beginObject().field("event", "error").field("stage", "yosys").field("log", work + "/yosys.log");
            if (!argsError.empty())
                w.field("error", argsError);
            else if (!y.finished)
                w.field("error", "yosys did not finish");
            if (log.str().find("Found topological loop") != std::string::npos)
                w.field("error", "combinational loop in the design (usually a latch inferred in always_comb: a "
                                 "branch that assigns nothing). QuickFV v1 does not model latches.");
            w.endObject();
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

    HuntOptions ho;
    ho.budgetSeconds = args.budget;
    ho.maxDepth = args.maxDepth;
    ho.simSeconds = args.simSeconds;
    if (!args.noRic3) {
        // Portfolio: rIC3 on every property (AIGER dump of the model). Its CEXs
        // become our traces; its proofs count only if Certifaiger verifies them.
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(int64_t(args.budget * 1000));
        ho.externalEngineName = "rIC3";
        ho.externalEngine = [&, deadline](size_t p, const std::atomic<bool>& cancel) {
            return runRic3(ts, p, {}, work + "/portfolio", deadline, cancel);
        };
    }
    if (args.certify) {
        ho.certifyDir = work + "/certificate";
        fs::create_directories(ho.certifyDir);
    }
    Hunt hunt(ts, ho);
    ReplayOptions ro;
    ro.top = args.session.top;
    ro.clock = args.clock;

    // Reported status per property: a trace btorsim did not confirm is only a
    // candidate (*_UNCONFIRMED), never a CEX / REACHABLE verdict.
    std::map<size_t, std::string> reported;
    auto results = hunt.run([&](size_t p, const Verdict& r) {
        auto& prop = ts.props[p];
        std::string base, replay;
        if (r.cex) {
            // One file set per trace: a shortened trace must not overwrite the
            // files a background replay of the longer one is still using.
            base = work + "/trace_" + prop.label + (prop.isTrigger ? "_trigger" : "") + "_len" +
                   std::to_string(r.cex->depth + 1);
            std::ofstream(base + ".wit") << btor2Witness(ts, p, *r.cex);
            std::string log;
            replay = replayBtorsim(btor, base + ".wit", p, r.cex->depth, std::chrono::seconds(30), &log);
            std::ofstream(base + ".btorsim.log") << log;
        }
        auto status = r.cex && replay != "confirmed" ? r.status + "_UNCONFIRMED" : r.status;
        reported[p] = status;
        JsonWriter w;
        w.beginObject()
            .field("event", "result")
            .field("assertion", prop.label)
            .field("property", prop.name)
            .field("goal", prop.kind == TransitionSystem::Property::Kind::Assert ? "no-failure"
                           : prop.isTrigger                                     ? "trigger-reachable"
                                                                                : "cover")
            .field("status", status)
            .field("engine", r.engine)
            .field("ms", r.ms);
        if (r.cex) {
            std::ofstream(base + ".json") << jsonTrace(ts, p, *r.cex, args.clock);
            std::ofstream(base + ".vcd") << vcdTrace(ts, *r.cex, args.clock);
            std::ofstream(base + "_tb.sv") << replayTestbench(ts, *r.cex, ro);
            w.field("length", r.cex->depth + 1).field("minimized", r.minimized);
            if (xDependent(ts, p, *r.cex))
                w.field("x_dependent", "fails only for some values of X; not reproducible in 2-state simulation");
            w.key("trace").beginObject()
                .field("json", base + ".json")
                .field("vcd", base + ".vcd")
                .field("witness", base + ".wit")
                .field("testbench", base + "_tb.sv")
                .endObject();
            w.key("certified").beginObject();
            w.field("btorsim", replay);
            if (args.replay && args.btor.empty()) {
                bool isAssert = prop.kind == TransitionSystem::Property::Kind::Assert;
                // Asserts replay on the ORIGINAL SVA (independent of our compiler);
                // reachability goals only exist in the compiled monitors.
                auto files = isAssert ? replayFiles : compiledFiles;
                auto expect = isAssert ? prop.label : prop.name.substr(prop.name.rfind('.') + 1);
                auto dir = base + "_replay";
                std::string what = isAssert ? "rtl_replay" : "monitor_replay";
                std::string label = prop.label;
                int length = r.cex->depth + 1;
                w.field(what, "pending");
                replays.push_back(std::async(std::launch::async, [=, &args, &emit] {
                    auto verdict = certifyReplay(args, base + "_tb.sv", files, expect, dir);
                    JsonWriter c;
                    c.beginObject()
                        .field("event", "certified")
                        .field("assertion", label)
                        .field("length", length)
                        .field(what, verdict)
                        .endObject();
                    emit(c);
                }));
            }
            w.endObject();
        }
        else if (r.status == "UNREACHABLE" || r.status == "PROVEN") {
            if (r.engine == "induction")
                w.field("induction_k", r.inductionK);
            else
                w.field("proof_certified", r.proofCertified).field("detail", r.detail);
        }
        else {
            w.field("depth_reached", r.depthChecked).field("note", "bounded: not a proof");
        }
        w.endObject();
        emit(w);
    });

    for (auto& f : replays)
        f.wait();
    if (args.certify) {
        // Every UNSAT answer the verdicts rely on ("no CEX at depth k", induction
        // steps) must be verified by lidrup-check against our own query log.
        JsonWriter c;
        c.beginObject().field("event", "certificate").field("dir", ho.certifyDir);
        c.key("solvers").beginArray();
        for (auto& r : certifyBounded(ho.certifyDir, hunt.bmcClaims, hunt.stepClaims))
            c.beginObject().field("solver", r.solver).field("unsat_claims", uint64_t(r.unsatClaims))
                .field("lidrup_check", r.result).endObject();
        c.endArray()
            .field("trusted_base", "Yosys+slang elaboration and the BTOR2-to-CNF encoding (see SPEC section 8)")
            .endObject();
        emit(c);
    }

    // One verdict per assertion (SPEC section 4.2). A module's assertion is one
    // property per instance ("u1.P", "u2.P"): with several instances, each gets
    // its own verdict under its hierarchical name.
    auto instanceOf = [&](size_t p) {
        auto& n = ts.props[p].name;
        auto dot = n.rfind('.');
        return dot == std::string::npos ? std::string() : n.substr(0, dot);
    };
    std::map<std::string, std::set<std::string>> instancesOf;
    for (size_t p = 0; p < ts.props.size(); p++)
        instancesOf[ts.props[p].label].insert(instanceOf(p));
    std::map<std::string, std::pair<const Verdict*, const Verdict*>> byLabel; // key -> (assert, trigger)
    std::map<std::string, std::vector<std::string>> keysOf;                    // label -> keys
    for (size_t p = 0; p < ts.props.size(); p++) {
        if (reported.count(p))
            results[p].status = reported[p];
        auto& label = ts.props[p].label;
        auto key = instancesOf[label].size() > 1 ? instanceOf(p) + "." + label : label;
        if (!byLabel.count(key))
            keysOf[label].push_back(key);
        auto& e = byLabel[key];
        if (ts.props[p].kind == TransitionSystem::Property::Kind::Assert)
            e.first = &results[p];
        else if (ts.props[p].isTrigger)
            e.second = &results[p];
    }
    // Assertions (or triggers) that synthesis folded to a constant are no longer
    // in the model: a missing trigger is constant false (vacuous by construction);
    // a missing assertion with no trigger can never fail.
    std::map<std::string, std::string> folded;
    auto foldedEvent = [&](const std::string& key, const char* goal, const char* status, const char* note) {
        JsonWriter w;
        w.beginObject().field("event", "result").field("assertion", key).field("goal", goal)
            .field("status", status).field("engine", "synthesis").field("note", note).endObject();
        emit(w);
    };
    for (auto& s : sites) {
        if (!s.supported || s.ir.kind != sva::DirectiveKind::Assert)
            continue;
        std::string label = s.label.empty() ? s.ir.label : s.label;
        bool hasTrigger = !s.ir.ante.elems.empty();
        auto keys = keysOf[label];
        if (keys.empty()) { // nothing of it left in the model
            keys.push_back(label);
            byLabel[label];
        }
        for (auto& key : keys) {
            auto& e = byLabel[key];
            if (hasTrigger && !e.second) {
                folded[key] = "VACUOUS";
                foldedEvent(key, "trigger-reachable", "UNREACHABLE", "the trigger is constant false after synthesis");
            }
            else if (!hasTrigger && !e.first) {
                folded[key] = "TRIVIALLY_TRUE";
                foldedEvent(key, "no-failure", "TRIVIALLY_TRUE", "the assertion cannot fail: constant true after synthesis");
            }
        }
    }

    int cex = 0, unconfirmed = 0;
    JsonWriter w;
    w.beginObject().field("event", "summary").field("ms", msSince(t0)).field("sim_cycles", hunt.simCycles);
    w.key("verdicts").beginObject();
    for (auto& [label, pr] : byLabel) {
        auto [as, tr] = pr;
        std::string verdict;
        if (folded.count(label))
            verdict = folded[label];
        else if (as && (as->status == "CEX" || as->status == "CEX_UNCONFIRMED"))
            verdict = as->status;
        // Vacuity outranks a proof: an assertion that holds only because its
        // trigger never fires tells nothing about the design.
        else if ((as && as->status == "VACUOUS") || (tr && tr->status == "UNREACHABLE"))
            verdict = "VACUOUS";
        else if (as && as->status == "PROVEN")
            verdict = "PROVEN";
        else if (tr && (tr->status == "NOT_REACHED" || tr->status == "REACHABLE_UNCONFIRMED"))
            verdict = "POSSIBLY_VACUOUS";
        else if (as)
            verdict = "PASS_BOUNDED";
        else
            verdict = tr ? tr->status : "?";
        cex += verdict == "CEX";
        unconfirmed += verdict == "CEX_UNCONFIRMED";
        w.field(label, verdict);
    }
    w.endObject().endObject();
    emit(w);
    // An unconfirmed CEX is inconclusive: a tool error, not a pass.
    return cex ? 1 : unconfirmed ? 2 : 0;
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
    // Session servers take no source files: the design arrives with `load`.
    if (argc >= 2 && (std::string(argv[1]) == "serve" || std::string(argv[1]) == "mcp")) {
        std::string work = "qfv_session";
        for (int i = 2; i + 1 < argc; i++)
            if (std::string(argv[i]) == "--work")
                work = argv[i + 1];
        return std::string(argv[1]) == "serve" ? runServe(work) : runMcp(work);
    }
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
