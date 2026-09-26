// T0: syntax/type diagnostics plus the assertion inventory (supported or not,
// with reasons), rendered as JSON.
#pragma once

#include <string>
#include <vector>

#include "json.h"
#include "session.h"
#include "sva/lower.h"

namespace qfv {

struct DiagInfo {
    std::string severity; // error / warning / note
    std::string code;
    std::string message;
    sva::Location loc;
};

std::vector<DiagInfo> collectDiagnostics(slang::ast::Compilation& comp,
                                         slang::SourceManager& sm);

void writeDiagnostics(JsonWriter& w, const std::vector<DiagInfo>& diags);
void writeAssertions(JsonWriter& w, const std::vector<sva::AssertionSite>& sites);

} // namespace qfv
