// Bit-level transition system built from a BTOR2 model.
#pragma once

#include <cstdint>
#include <map>
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
        /// Assert: `bad` true = violation. Reach: `bad` true = the goal was
        /// reached (a vacuity trigger or a cover), which is the good outcome.
        enum class Kind { Assert, Reach };
        std::string name;   // BTOR2 symbol, e.g. "fifo_tb_inst.qfv_trigger__fifo_0"
        std::string label;  // user-facing: assertion label (instance path stripped)
        Kind kind = Kind::Assert;
        bool isTrigger = false; // Reach goal = antecedent of assertion `label` fires
        int64_t btorId = 0;
        Lit bad = kFalse;
    };

    Aig aig;
    std::vector<Input> inputs;
    std::vector<Latch> latches;
    std::vector<Property> props;
    std::vector<Lit> constraints; // must hold in every frame
    /// Named design signals (inputs, registers, and wires exposed as BTOR2
    /// outputs), by hierarchical name relative to the top: "fifo_tb_inst.wr_push".
    std::map<std::string, std::vector<Lit>> signals;
};

/// Loads a BTOR2 file and bit-blasts it. Arrays, liveness (justice/fairness)
/// and non-constant init are rejected with an error message.
bool loadBtor2(const std::string& path, TransitionSystem& ts, std::string& error);

} // namespace qfv
