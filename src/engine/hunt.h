// The T1/T2 search: one time budget, three engines, streamed verdicts.
//
//   1. Random simulation (short slice): cheap hits on deep-but-likely events.
//   2. Incremental BMC, depth by depth: the shortest CEX/witness for each
//      property. A trace found by simulation is kept, and BMC keeps looking
//      *below* its length to replace it with a shorter one.
//   3. k-induction step (arbitrary start state), for reachability goals only:
//      proves a vacuity trigger or cover can never fire (UNREACHABLE).
//
// Assertions never get PROVEN here: an uncertified proof is not reported
// (SPEC section 4.2); induction is only used to prove vacuity.
#pragma once

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "engine/trace.h"
#include "model/ts.h"

namespace qfv {

struct HuntOptions {
    double budgetSeconds = 600;
    int maxDepth = 1 << 30;
    double simSeconds = 0.25;   // random-simulation slice at the start
    int maxInductionDepth = 20; // k-induction attempts for reachability goals
    uint64_t seed = 1;
    /// Optional unbounded prover for reachability goals, run concurrently
    /// (e.g. rIC3's IC3 as a subprocess). Returns "UNREACHABLE", "REACHABLE" or
    /// "" (unknown); must stop promptly when `cancel` becomes true.
    std::function<std::string(size_t prop, const std::atomic<bool>& cancel)> externalProver;
    std::string externalProverName = "external";
};

struct Verdict {
    // Assert: CEX | PASS_BOUNDED.   Reach: REACHABLE | UNREACHABLE | NOT_REACHED.
    std::string status = "RUNNING";
    std::string engine;      // sim | bmc | induction
    int depthChecked = -1;   // BMC: no hit in frames 0..depthChecked
    int inductionK = -1;     // UNREACHABLE: proved k-inductive
    double ms = 0;
    bool minimized = false;  // a shorter trace replaced an earlier one
    std::optional<Cex> cex;
};

class Hunt {
public:
    using Event = std::function<void(size_t prop, const Verdict&)>;

    Hunt(const TransitionSystem& ts, HuntOptions opts) : ts(ts), opts(opts) {}

    /// `onEvent` fires when a property gets a trace or a proof, when a trace is
    /// shortened, and once at the end for properties still open.
    std::vector<Verdict> run(const Event& onEvent);

    uint64_t simCycles = 0;

private:
    const TransitionSystem& ts;
    HuntOptions opts;
};

} // namespace qfv
