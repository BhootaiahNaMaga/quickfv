#include "lint.h"

#include "slang/diagnostics/DiagnosticEngine.h"

namespace qfv {

using namespace slang;

std::vector<DiagInfo> collectDiagnostics(ast::Compilation& comp, SourceManager& sm) {
    DiagnosticEngine engine(sm);
    std::vector<DiagInfo> out;
    for (auto& diag : comp.getAllDiagnostics()) {
        DiagInfo d;
        switch (engine.getSeverity(diag.code, diag.location)) {
            case DiagnosticSeverity::Ignored: continue;
            case DiagnosticSeverity::Note: d.severity = "note"; break;
            case DiagnosticSeverity::Warning: d.severity = "warning"; break;
            default: d.severity = "error"; break;
        }
        d.code = std::string(toString(diag.code));
        d.message = engine.formatMessage(diag);
        d.loc = sva::toLocation(sm, diag.location);
        out.push_back(std::move(d));
    }
    return out;
}

static void writeLoc(JsonWriter& w, const sva::Location& l) {
    w.field("file", l.file).field("line", uint64_t(l.line)).field("column", uint64_t(l.column));
}

void writeDiagnostics(JsonWriter& w, const std::vector<DiagInfo>& diags) {
    w.beginArray();
    for (auto& d : diags) {
        w.beginObject().field("severity", d.severity).field("code", d.code).field("message",
                                                                                   d.message);
        writeLoc(w, d.loc);
        w.endObject();
    }
    w.endArray();
}

void writeAssertions(JsonWriter& w, const std::vector<sva::AssertionSite>& sites) {
    w.beginArray();
    for (auto& s : sites) {
        w.beginObject()
            .field("label", s.label)
            .field("directive", s.directive)
            .field("module", s.module);
        writeLoc(w, s.loc);
        w.field("status", s.supported ? "supported" : s.error ? "error" : "unsupported");
        if (s.supported) {
            w.field("normalized", s.ir.toString());
            if (s.ir.kind != sva::DirectiveKind::Cover)
                w.field("max_consequent_cycles", uint64_t(s.ir.cons.maxLength()));
        }
        else {
            w.field("reason", s.reason);
            w.key("reason_at").beginObject();
            writeLoc(w, s.reasonLoc);
            w.endObject();
        }
        w.endObject();
    }
    w.endArray();
}

} // namespace qfv
