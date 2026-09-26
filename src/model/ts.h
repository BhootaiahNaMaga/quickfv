// Bit-level transition system built from a BTOR2 model.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "model/aig.h"

namespace qfv {

struct TransitionSystem {
    struct Input {
        std::string name;
        int64_t btorId = 0;
        std::vector<Lit> bits; // LSB first; AIG inputs
    };
    struct Latch {
        std::string name;
        int64_t btorId = 0;
        std::vector<Lit> cur;      // LSB first; AIG inputs standing for the current value
        std::vector<Lit> next;     // next-state function over inputs and `cur`
        std::vector<int8_t> init;  // per bit: 0, 1, or -1 (unconstrained)
        bool synthetic = false;    // introduced by Yosys or a qfv monitor, not RTL state
    };
    struct Property {
        std::string name;
        int64_t btorId = 0;
        Lit bad = kFalse; // true in a frame = property violated in that frame
    };

    Aig aig;
    std::vector<Input> inputs;
    std::vector<Latch> latches;
    std::vector<Property> props;
    std::vector<Lit> constraints; // must hold in every frame
};

/// Loads a BTOR2 file and bit-blasts it. Arrays, liveness (justice/fairness)
/// and non-constant init are rejected with an error message.
bool loadBtor2(const std::string& path, TransitionSystem& ts, std::string& error);

} // namespace qfv
