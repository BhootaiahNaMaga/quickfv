#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "engine/trace.h"
#include "model/ts.h"

namespace qfv {

/// Where each BTOR2 node of a written model came from (for witnesses).
struct Btor2Layout {
    std::vector<std::pair<size_t, size_t>> inputBits; // BTOR2 input index -> (input, bit)
    std::vector<std::pair<size_t, size_t>> stateBits; // BTOR2 state index -> (latch, bit)
    std::map<size_t, size_t> badOfProp;               // ts prop -> BTOR2 bad index
    size_t numBads = 0;
};

/// Writes the bit-level model as BTOR2 (all properties, or only `onlyProp`).
bool writeBtor2(const TransitionSystem& ts, const std::string& path,
                std::optional<size_t> onlyProp = std::nullopt, Btor2Layout* layout = nullptr);

/// Same, with extra constraints (e.g. a session's active assumptions).
bool writeBtor2WithConstraints(const TransitionSystem& ts, const std::string& path,
                               const std::vector<Lit>& extraConstraints,
                               std::optional<size_t> onlyProp = std::nullopt,
                               Btor2Layout* layout = nullptr);

/// BTOR2 witness for a model written with writeBtor2 (bit-level indices).
std::string btor2WitnessForLayout(const TransitionSystem& ts, const Btor2Layout& layout, size_t prop,
                                  const Cex& cex);

} // namespace qfv
