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
    /// Also emit `cover property` on the antecedent match (vacuity check, T1).
    bool vacuityCover = false;
};

std::string emitMonitor(const AssertionIR& ir, const MonitorOptions& opts = {});

} // namespace qfv::sva
