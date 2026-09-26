// Intermediate representation of the v1 SVA subset (SPEC.md section 5).
//
// Every supported property normalizes to
//
//     disable iff (D)  ANTE |-> CONS        (|=> is |-> with CONS delayed by 1)
//
// where ANTE and CONS are *delay chains*: b0 ##[lo1:hi1] b1 ... ##[loN:hiN] bN,
// each bi a boolean over design signals, with an optional leading delay
// ##[lo0:hi0] before b0 (measured from the attempt's start cycle). A plain
// boolean or sequence property P is `1 |-> P`; a cover is the ANTE chain alone.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace slang::ast {
class Expression;
class CallExpression;
class Symbol;
} // namespace slang::ast

namespace qfv::sva {

/// A boolean leaf: SV expression text (sampled-value calls replaced by helper
/// signal names, see SampledHelper) for text emission, and the slang AST node
/// for direct AIG emission (valid while its Compilation is alive).
struct Leaf {
    std::string text;
    const slang::ast::Expression* expr = nullptr;
};

/// One element of a delay chain: wait [lo, hi] cycles after the previous
/// element (or after the attempt start, for the first element), then `leaf`
/// must hold. lo == hi == 0 means "same cycle" (fusion, ##0).
struct ChainElem {
    uint32_t lo = 0;
    uint32_t hi = 0;
    Leaf leaf;
};

struct Chain {
    std::vector<ChainElem> elems;

    /// Longest possible match length in cycles, counted from the start cycle.
    uint32_t maxLength() const;
    std::string toString() const;
};

/// A register chain for $past/$rose/$fell/$stable/$changed on one expression.
/// Signal `<name>_0` is the current value; `<name>_k` is the value k clocks ago.
struct SampledHelper {
    std::string name;
    std::string exprText;
    uint32_t width = 1;
    uint32_t depth = 1; // number of registered stages
    std::string function;                              // $past, $rose, ...
    const slang::ast::CallExpression* call = nullptr;  // the call it replaces
    const slang::ast::Expression* arg = nullptr;       // the sampled expression
};

enum class DirectiveKind { Assert, Assume, Cover };

struct AssertionIR {
    std::string label;       // assertion label (or generated), reused for the output
    DirectiveKind kind = DirectiveKind::Assert;
    std::string clockEvent;  // e.g. "posedge clk"
    std::string disableText; // empty when there is no disable iff
    const slang::ast::Expression* disableExpr = nullptr;
    const slang::ast::Expression* clockExpr = nullptr;
    bool clockPosedge = true;
    const slang::ast::Symbol* scope = nullptr; // for constant evaluation
    Chain ante;              // empty for a plain property (always triggered)
    Chain cons;              // unused for covers
    std::vector<SampledHelper> helpers;

    std::string toString() const;
};

} // namespace qfv::sva
