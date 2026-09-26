// Bit-parallel random simulation: 64 random traces advance together, one per
// bit of a machine word. Design constraints (e.g. the reset environment) are
// honoured by re-drawing the inputs of only the lanes that violate them; a
// lane that still violates after several tries is dropped.
//
// Cheap and unbiased by depth, so it finds deep-but-likely events (a FIFO
// filling up) that BMC reaches only after many frames.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

#include "engine/trace.h"
#include "model/ts.h"

namespace qfv {

struct RandSimOptions {
    uint64_t seed = 1;
    int cyclesPerRun = 256;        // first run length; doubles after each run...
    int maxCyclesPerRun = 16384;   // ...up to this. A random walk gets ~sqrt(n)
                                   // deep in n cycles, so deep states need long runs.
    int retries = 16;       // input re-draws per lane when a constraint fails
};

class RandSim {
public:
    RandSim(const TransitionSystem& ts, RandSimOptions opts);

    /// Simulates until `deadline` or until every watched property was hit.
    /// `watch[p]` selects properties; `onHit(p, trace)` fires once per property.
    void run(std::chrono::steady_clock::time_point deadline, std::vector<bool> watch,
             const std::function<void(size_t, const Cex&)>& onHit);

    uint64_t cyclesSimulated() const { return cycles; }

private:
    uint64_t rand64();
    void evaluate(std::vector<uint64_t>& val) const;

    const TransitionSystem& ts;
    RandSimOptions opts;
    uint64_t state;
    uint64_t cycles = 0;
};

} // namespace qfv
