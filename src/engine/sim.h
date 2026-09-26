// Cycle simulation of a bit-level transition system (one trace at a time).
// Used to cross-check the bit-blaster against btorsim, and as the base for
// random simulation (M3).
#pragma once

#include <string>
#include <vector>

#include "model/ts.h"

namespace qfv {

struct SimTrace {
    std::vector<std::vector<int8_t>> initLatches;         // [latch][bit], -1 = 0
    std::vector<std::vector<std::vector<int8_t>>> inputs; // [frame][input][bit]
};

/// Simulates the trace; returns, per frame, the indices of properties whose
/// bad signal is true, and whether every constraint held in every frame.
std::vector<std::vector<size_t>> simulate(const TransitionSystem& ts, const SimTrace& trace,
                                          bool& constraintsHeld);

/// Parses a BTOR2 witness (the btorsim format) into a trace.
bool parseWitness(const std::string& path, const TransitionSystem& ts, SimTrace& trace,
                  std::string& error);

} // namespace qfv
