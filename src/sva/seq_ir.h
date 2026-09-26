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

namespace qfv::sva {

/// A boolean leaf, as SV expression text. Sampled-value calls inside it have
/// already been replaced by helper signal names (see SampledHelper).
struct Leaf {
    std::string text;
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
};

enum class DirectiveKind { Assert, Assume, Cover };

struct AssertionIR {
    std::string label;       // assertion label (or generated), reused for the output
    DirectiveKind kind = DirectiveKind::Assert;
    std::string clockEvent;  // e.g. "posedge clk"
    std::string disableText; // empty when there is no disable iff
    Chain ante;              // empty for a plain property (always triggered)
    Chain cons;              // unused for covers
    std::vector<SampledHelper> helpers;

    std::string toString() const;
};

} // namespace qfv::sva
