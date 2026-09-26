// Time-frame expansion of a transition system into one incremental CaDiCaL
// instance. Frame k's logic is Tseitin-encoded lazily (only the cone of what
// is queried) and exactly once.
//
// Two uses:
//   - BMC: frame 0 starts in the initial states.
//   - k-induction step: frame 0 is an *arbitrary* state (freeInit), so an
//     UNSAT query holds for every reachable state, not just those near reset.
#pragma once

#include <chrono>
#include <vector>

#include "model/ts.h"

namespace CaDiCaL {
class Solver;
}

namespace qfv {

class Unroller {
public:
    Unroller(const TransitionSystem& ts, bool freeInit);
    ~Unroller();
    Unroller(const Unroller&) = delete;
    Unroller& operator=(const Unroller&) = delete;

    /// SAT literal of AIG literal `l` in frame `frame` (encodes it if needed).
    int lit(int frame, Lit l);

    /// Solves under the given assumption literals. Returns 10 (SAT), 20
    /// (UNSAT) or 0 (stopped by the deadline).
    int solve(const std::vector<int>& assumptions);

    /// Value of `l` in `frame` in the last SAT model: 0/1, or -1 if the literal
    /// was never encoded (so it cannot matter to the result).
    int8_t value(int frame, Lit l) const;

    void setDeadline(std::chrono::steady_clock::time_point t);

    /// Call after latches were added to the transition system (the AIG itself
    /// may grow at any time).
    void syncLatches();

    /// Adds a switchable assumption: `c` must hold in every frame while the
    /// assumption is active. Returns its id. Implemented with an activation
    /// literal, so it can be turned off without rebuilding the solver.
    size_t addAssumption(Lit c);
    void setAssumptionActive(size_t id, bool active);
    size_t numVars() const { return size_t(nextVar - 1); }
    size_t numClauses() const { return clauses; }

private:
    int encode(int frame, uint32_t var);
    void ensureFrame(int frame);
    void addClause(std::initializer_list<int> c);

    const TransitionSystem& ts;
    bool freeInit;
    struct Impl;
    Impl* impl;
    std::vector<std::vector<int>> map; // [frame][aig var] -> SAT literal (0 = not encoded)
    std::vector<int32_t> latchOfVar, bitOfVar;
    struct Assumption {
        Lit lit;
        int act;
        bool active;
    };
    std::vector<Assumption> assumptions;
    uint32_t knownVars = 0;
    int nextVar = 1;
    int trueVar = 0;
    size_t clauses = 0;
};

} // namespace qfv
