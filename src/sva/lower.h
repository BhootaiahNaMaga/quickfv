// Lowers slang's elaborated concurrent assertions to the v1 IR (seq_ir.h).
// Anything outside the v1 subset is reported as unsupported with a reason and a
// source location. Nothing is ever approximated.
#pragma once

#include <string>
#include <vector>

#include "slang/ast/Compilation.h"
#include "slang/text/SourceLocation.h"
#include "sva/seq_ir.h"

namespace qfv::sva {

struct Location {
    std::string file;
    size_t line = 0;
    size_t column = 0;
};

/// One concurrent assertion directive found in the design (deduplicated across
/// instances of the same module).
struct AssertionSite {
    std::string label;      // label as written, or "" if unlabeled
    std::string directive;  // assert / assume / cover / restrict / expect
    std::string module;     // definition that contains it
    Location loc;
    slang::SourceRange replaceRange; // text to replace with the monitor (whole directive)
    bool supported = false;
    bool error = false;     // has syntax/type errors (vs. valid SVA outside the subset)
    std::string reason;     // why it is unsupported or erroneous
    Location reasonLoc;     // location of the offending construct
    AssertionIR ir;         // valid when supported
};

/// Collects and lowers every concurrent assertion in an elaborated compilation.
std::vector<AssertionSite> collectAssertions(slang::ast::Compilation& compilation);

Location toLocation(const slang::SourceManager& sm, slang::SourceLocation loc);

} // namespace qfv::sva
