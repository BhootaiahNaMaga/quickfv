// Direct emission of lowered assertions into a loaded design's AIG.
//
// This is what lets a session add or edit an assertion without re-running
// Yosys: the SVA's boolean expressions are compiled from slang's typed AST
// straight to AIG gates over the design's named signals, and the monitor's
// registers become new latches of the transition system.
#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "model/ts.h"
#include "sva/seq_ir.h"

namespace qfv::sva {

/// Raised for expressions outside what the AIG emitter supports; `where` is a
/// short description of the offending construct.
struct EmitError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct EmitResult {
    size_t assertProp = SIZE_MAX;   // index into ts.props (asserts)
    size_t triggerProp = SIZE_MAX;  // trigger goal (asserts with an antecedent), or the cover goal
    Lit assumeOk = kTrue;           // assumes: true in every frame the assumption holds
};

/// Emits `ir` into `ts`. `topPath` is the top instance name slang uses as the
/// first element of hierarchical paths (e.g. "fifo"). `clockName` is the
/// design's clock input; the assertion must be clocked on its posedge.
EmitResult emitIntoAig(const AssertionIR& ir, TransitionSystem& ts, const std::string& topPath,
                       const std::string& clockName);

} // namespace qfv::sva
