// qfv: QuickFV command-line entry point (M1: lint, compile-sva, check-sva).
//
//   qfv lint        [opts] files...   T0 diagnostics + assertion inventory (JSON)
//   qfv compile-sva [opts] -o DIR files...
//                                     rewrite sources with SVA lowered to monitors
//   qfv check-sva   [opts] --module M --sva TEXT files...
//                                     T0 for one new assertion inserted into module M
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
        else if (s.rfind("-", 0) == 0)
            usage("unknown option " + s);
        else
            a.session.files.push_back(s);
    }
    if (a.session.files.empty())
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

int cmdCompile(Session& session, const Args& args) {
    if (args.outDir.empty())
        usage("compile-sva needs -o DIR");
    auto t0 = std::chrono::steady_clock::now();
    auto comp = session.compile();
    auto diags = collectDiagnostics(*comp, session.sourceManager());
    auto sites = sva::collectAssertions(*comp);
    auto& sm = session.sourceManager();

    // Group text replacements by source buffer.
    struct Repl {
        size_t begin, end;
        std::string text;
    };
    std::map<uint32_t, std::vector<Repl>> repls;
    sva::MonitorOptions mo;
    mo.vacuityCover = args.vacuityCovers;
    int unsupported = 0;
    for (auto& s : sites) {
        auto r = s.replaceRange;
        if (s.supported) {
            repls[r.start().buffer().getId()].push_back(
                {r.start().offset(), r.end().offset(), sva::emitMonitor(s.ir, mo)});
        }
        else {
            unsupported++;
            if (args.dropUnsupported) {
                auto src = sm.getSourceText(r.start().buffer());
                auto orig = src.substr(r.start().offset(), r.end().offset() - r.start().offset());
                repls[r.start().buffer().getId()].push_back(
                    {r.start().offset(), r.end().offset(), commentOut(orig, s.reason)});
            }
        }
    }

    fs::create_directories(args.outDir);
    std::vector<std::string> outFiles;
    std::set<std::string> used;
    for (size_t i = 0; i < session.numTrees(); i++) {
        auto text = std::string(session.sourceText(i));
        std::string path = session.options().files[i];
        uint32_t id = session.buffer(i).getId();
        if (!repls.count(id)) {
            outFiles.push_back(path);
            continue;
        }
        auto& rs = repls[id];
        std::sort(rs.begin(), rs.end(), [](auto& a, auto& b) { return a.begin > b.begin; });
        for (auto& r : rs)
            text.replace(r.begin, r.end - r.begin, r.text);
        std::string name = fs::path(path).filename().string();
        while (used.count(name))
            name = "_" + name;
        used.insert(name);
        auto outPath = (fs::path(args.outDir) / name).string();
        std::ofstream(outPath) << text;
        outFiles.push_back(outPath);
    }

    JsonWriter w;
    w.beginObject()
        .field("command", "compile-sva")
        .field("ms", msSince(t0))
        .field("ok", !hasErrors(diags) && (unsupported == 0 || args.dropUnsupported))
        .field("unsupported", unsupported);
    w.key("files").beginArray();
    for (auto& f : outFiles)
        w.value(f);
    w.endArray();
    w.key("diagnostics");
    writeDiagnostics(w, diags);
    w.key("assertions");
    writeAssertions(w, sites);
    w.endObject();
    std::cout << w.str() << "\n";
    return hasErrors(diags) || (unsupported && !args.dropUnsupported) ? 1 : 0;
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

} // namespace

int main(int argc, char** argv) {
    auto args = parseArgs(argc, argv);
    Session session(args.session);
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
    usage("unknown command " + args.command);
}
