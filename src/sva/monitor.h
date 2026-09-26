// Generates a synthesizable Verilog monitor for a lowered assertion.
//
// The output replaces the original directive in the source file. It declares
// the monitor state and ends in a *trivial* concurrent directive over a single
// fail/match wire, which Yosys+slang and EBMC both accept:
//
//     <label>: assert property (@(posedge clk) disable iff (D) !qfv_<label>_fail);
#pragma once

#include <string>

#include "sva/seq_ir.h"

namespace qfv::sva {

struct MonitorOptions {
    /// Also emit a reachability goal for the antecedent match (vacuity check, T1).
    bool vacuityCover = false;
    /// Emit covers and vacuity goals as `assert property (!goal)` labelled
    /// `qfv_cover__<label>` / `qfv_trigger__<label>`, so that they become BTOR2
    /// `bad` properties the engine treats as reachability goals.
    bool reachAsBad = false;
};

std::string emitMonitor(const AssertionIR& ir, const MonitorOptions& opts = {});

} // namespace qfv::sva
