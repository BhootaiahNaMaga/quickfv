// Incremental bounded model checking on one CaDiCaL instance.
//
// Frame k's logic is Tseitin-encoded on demand (only the cone of influence of
// what is queried), and each frame is encoded once. A property is checked at
// frame k by *assuming* its bad literal, so every property shares the unrolling
// and the solver's learned clauses. Depth grows until the time budget runs out
// (SPEC section 4.3: the budget is time, not depth).
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "model/ts.h"

namespace qfv {

struct Cex {
    int depth = 0; // frame in which the property is violated (0-based)
    std::vector<std::vector<int8_t>> initLatches;              // [latch][bit]: 0/1, -1 = don't care
    std::vector<std::vector<std::vector<int8_t>>> inputs;      // [frame][input][bit]
};

struct PropResult {
    std::string status = "RUNNING"; // CEX | PASS_BOUNDED
    int depthChecked = -1;          // last frame proven clean
    double ms = 0;                  // time at which the result was found
    std::optional<Cex> cex;
};

struct BmcOptions {
    double budgetSeconds = 600;
    int maxDepth = 1 << 30;
};

class Bmc {
public:
    using Event = std::function<void(size_t prop, const PropResult&)>;

    Bmc(const TransitionSystem& ts, BmcOptions opts);
    ~Bmc();

    /// Runs until every property has a CEX, the depth limit, or the budget.
    /// `onEvent` fires once per property when it gets a CEX, and at the end.
    std::vector<PropResult> run(const Event& onEvent);

    size_t numClauses() const { return clauses; }
    size_t numVars() const { return size_t(nextVar - 1); }

private:
    int lit(int frame, Lit l);
    int encode(int frame, uint32_t var);
    void addClause(std::initializer_list<int> c);
    void ensureFrame(int frame);
    Cex extract(int depth);

    const TransitionSystem& ts;
    BmcOptions opts;
    struct Impl;
    Impl* impl;
    std::vector<std::vector<int>> map; // [frame][aig var] -> SAT literal (0 = not yet encoded)
    std::vector<int32_t> latchOfVar;   // aig var -> latch index (-1 if not a latch bit)
    std::vector<int32_t> bitOfVar;
    int nextVar = 1;
    int trueVar = 0;
    size_t clauses = 0;
    std::chrono::steady_clock::time_point start;
};

/// BTOR2 witness text (the format btorsim -c checks) for a CEX of property `prop`.
std::string btor2Witness(const TransitionSystem& ts, size_t prop, const Cex& cex);

} // namespace qfv
