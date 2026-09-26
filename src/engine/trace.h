// Counterexample / witness traces and their output formats.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "model/ts.h"

namespace qfv {

struct Cex {
    int depth = 0; // frame in which the property's bad signal is true (0-based)
    std::vector<std::vector<int8_t>> initLatches;         // [latch][bit]: 0/1, -1 = don't care
    std::vector<std::vector<std::vector<int8_t>>> inputs; // [frame][input][bit]
};

/// BTOR2 witness (the format `btorsim -c` checks).
std::string btor2Witness(const TransitionSystem& ts, size_t prop, const Cex& cex);

/// Compact JSON trace for agents: per frame, the inputs and the named RTL registers.
std::string jsonTrace(const TransitionSystem& ts, size_t prop, const Cex& cex,
                      const std::string& clock);

/// VCD waveform of inputs and named RTL registers.
std::string vcdTrace(const TransitionSystem& ts, const Cex& cex, const std::string& clock);

struct ReplayOptions {
    std::string top;
    std::string clock;
    /// Extra lines placed inside the testbench after the DUT instance (checks).
    std::string extraChecks;
};

/// Standalone SV testbench that sets the CEX's initial RTL state, drives its
/// inputs cycle by cycle, and lets the simulator's own SVA checking decide.
std::string replayTestbench(const TransitionSystem& ts, const Cex& cex, const ReplayOptions& opts);

/// True if the trace needs particular values of X (synthetic free inputs): the
/// property is no longer violated at the trace's end when every X is 0. Such a
/// CEX is real under formal X semantics but not in a 2-state simulation.
bool xDependent(const TransitionSystem& ts, size_t prop, const Cex& cex);

/// True if a latch name refers to real RTL state that exists in the original
/// sources (not Yosys-internal, a qfv monitor, or the qfv reset environment).
bool isRtlLatch(const TransitionSystem::Latch& l);

} // namespace qfv
