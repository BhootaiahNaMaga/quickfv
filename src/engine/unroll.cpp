#include "engine/unroll.h"

#include "cadical.hpp"

namespace qfv {

namespace {

/// Stops CaDiCaL when the wall-clock deadline passes.
struct Deadline : public CaDiCaL::Terminator {
    std::chrono::steady_clock::time_point at = std::chrono::steady_clock::time_point::max();
    bool terminate() override { return std::chrono::steady_clock::now() >= at; }
};

} // namespace

struct Unroller::Impl {
    CaDiCaL::Solver solver;
    Deadline deadline;
};

Unroller::Unroller(const TransitionSystem& ts, bool freeInit) :
    ts(ts), freeInit(freeInit), impl(new Impl) {
    uint32_t n = ts.aig.numVars();
    latchOfVar.assign(n, -1);
    bitOfVar.assign(n, -1);
    for (size_t i = 0; i < ts.latches.size(); i++)
        for (size_t b = 0; b < ts.latches[i].cur.size(); b++) {
            latchOfVar[varOf(ts.latches[i].cur[b])] = int32_t(i);
            bitOfVar[varOf(ts.latches[i].cur[b])] = int32_t(b);
        }
    trueVar = nextVar++;
    addClause({trueVar});
    impl->solver.connect_terminator(&impl->deadline);
}

Unroller::~Unroller() { delete impl; }

void Unroller::setDeadline(std::chrono::steady_clock::time_point t) { impl->deadline.at = t; }

void Unroller::addClause(std::initializer_list<int> c) {
    for (int l : c)
        impl->solver.add(l);
    impl->solver.add(0);
    clauses++;
}

int Unroller::lit(int frame, Lit l) {
    int v = encode(frame, varOf(l));
    return isNeg(l) ? -v : v;
}

/// SAT literal of AIG variable `var` in `frame`. Iterative: a latch in frame k
/// refers to its next-state logic in frame k-1, so recursion could be deep.
int Unroller::encode(int frame, uint32_t var) {
    ensureFrame(frame);
    if (int m = map[frame][var])
        return m;
    std::vector<std::pair<int, uint32_t>> stack{{frame, var}};
    auto mapped = [&](int f, Lit l) -> int {
        int m = map[f][varOf(l)];
        return m == 0 ? 0 : (isNeg(l) ? -m : m);
    };
    while (!stack.empty()) {
        auto [f, x] = stack.back();
        if (map[f][x]) {
            stack.pop_back();
            continue;
        }
        switch (ts.aig.kindOf(x)) {
            case Aig::Kind::Const:
                map[f][x] = -trueVar; // variable 0 is constant false
                stack.pop_back();
                break;
            case Aig::Kind::Input: {
                int32_t li = latchOfVar[x];
                if (li < 0) { // primary input: fresh variable per frame
                    map[f][x] = nextVar++;
                    stack.pop_back();
                    break;
                }
                auto& latch = ts.latches[li];
                if (f == 0) {
                    int8_t init = freeInit ? -1 : latch.init[bitOfVar[x]];
                    map[f][x] = init < 0 ? nextVar++ : (init ? trueVar : -trueVar);
                    stack.pop_back();
                    break;
                }
                Lit nx = latch.next[bitOfVar[x]];
                if (int m = mapped(f - 1, nx)) {
                    map[f][x] = m;
                    stack.pop_back();
                }
                else {
                    stack.push_back({f - 1, varOf(nx)});
                }
                break;
            }
            case Aig::Kind::And: {
                Lit a = ts.aig.fanin0(x), b = ts.aig.fanin1(x);
                int la = mapped(f, a), lb = mapped(f, b);
                if (!la)
                    stack.push_back({f, varOf(a)});
                if (!lb)
                    stack.push_back({f, varOf(b)});
                if (la && lb) {
                    int v = nextVar++;
                    addClause({-v, la});
                    addClause({-v, lb});
                    addClause({v, -la, -lb});
                    map[f][x] = v;
                    stack.pop_back();
                }
                break;
            }
        }
    }
    return map[frame][var];
}

void Unroller::ensureFrame(int frame) {
    while (int(map.size()) <= frame) {
        map.emplace_back(ts.aig.numVars(), 0);
        int f = int(map.size()) - 1;
        // Constraints (design assumptions) hold in every frame.
        for (Lit c : ts.constraints) {
            int l = lit(f, c);
            addClause({l});
        }
    }
}

int Unroller::solve(const std::vector<int>& assumptions) {
    for (int a : assumptions)
        impl->solver.assume(a);
    return impl->solver.solve();
}

int8_t Unroller::value(int frame, Lit l) const {
    if (frame >= int(map.size()))
        return -1;
    int m = map[frame][varOf(l)];
    if (!m)
        return -1;
    int v = impl->solver.val(m) > 0 ? 1 : 0;
    return int8_t(isNeg(l) ? !v : v);
}

} // namespace qfv
