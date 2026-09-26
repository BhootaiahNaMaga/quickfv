// JasperGold-style setup files (SPEC section 7.2): a Tcl *subset*.
//
// Supported: analyze, elaborate, clock, reset, assume, assert, cover, prove,
// set_prove_time_limit, clear, set / $var. Tcl syntax: words, {braces}
// (literal, nested), "quotes" (with $var), # comments, ; and newlines,
// backslash-newline. Anything else is an error with a line number: a setup
// file that silently means something different from JasperGold is worse than
// one that fails.
#pragma once

#include <string>
#include <vector>

namespace qfv {

struct SetupProperty {
    std::string kind; // assert | assume | cover
    std::string name;
    std::string text; // the property expression, as written
    int line = 0;
};

struct SetupConfig {
    std::vector<std::string> files; // absolute paths, in analyze order
    std::vector<std::string> defines;
    std::vector<std::string> includeDirs;
    std::string top;
    std::vector<std::string> paramOverrides; // NAME=VALUE
    std::string clock;
    std::string resetExpr;
    int resetCycles = 1; // `reset -cycles N` (QuickFV extension)
    std::vector<SetupProperty> properties;
    double timeLimitSeconds = 600;
    bool timeLimitSet = false; // set_prove_time_limit / prove -time_limit given
    bool prove = false;
};

/// Parses `path`. Returns false with `error` ("file:line: message").
bool parseSetupFile(const std::string& path, SetupConfig& cfg, std::string& error);

/// Same, from text (`baseDir` resolves relative file names).
bool parseSetupText(const std::string& text, const std::string& baseDir, const std::string& name,
                    SetupConfig& cfg, std::string& error);

} // namespace qfv
