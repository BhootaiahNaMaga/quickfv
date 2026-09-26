// External engines in the portfolio (SPEC section 6.1), run as subprocesses on
// an AIGER dump of the current model, one property at a time:
//
//   rIC3 (IC3/BMC/k-induction portfolio, HWMCC 2024-25 winner)
//     - SAT:   its AIGER witness is parsed into our own trace, which then goes
//              through the usual BMC shortening and replay certification;
//     - UNSAT: it writes a witness circuit, and the proof counts only if
//              Certifaiger (+ aigsplit, aigtocnf, kissat) verifies it.
#pragma once

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "engine/trace.h"
#include "model/ts.h"

namespace qfv {

/// AIGER (ASCII) of the model: inputs, latches (with reset values), ANDs, the
/// given properties as bad states, constraints (plus `extraConstraints`).
struct AigerLayout {
    std::vector<std::pair<size_t, size_t>> inputBits; // AIGER input -> (ts input, bit)
    std::vector<std::pair<size_t, size_t>> latchBits; // AIGER latch -> (ts latch, bit)
};
bool writeAiger(const TransitionSystem& ts, const std::string& path, const std::vector<size_t>& props,
                const std::vector<Lit>& extraConstraints, AigerLayout* layout);

struct ExternalVerdict {
    enum Kind { Unknown, Holds, Fails } kind = Unknown;
    std::optional<Cex> cex;     // Fails: the trace, in our terms
    bool certified = false;     // Holds: Certifaiger verified the witness circuit
    std::string detail;         // engine output summary / certificate check result
};

/// Runs rIC3 on property `prop` until `deadline` or `cancel`.
ExternalVerdict runRic3(const TransitionSystem& ts, size_t prop, const std::vector<Lit>& extraConstraints,
                        const std::string& workDir, std::chrono::steady_clock::time_point deadline,
                        const std::atomic<bool>& cancel);

/// Verifies an AIGER witness circuit for `model` with Certifaiger.
bool checkWitnessCircuit(const std::string& model, const std::string& witness, std::string& log,
                         std::chrono::steady_clock::time_point deadline, const std::atomic<bool>& cancel);

} // namespace qfv
