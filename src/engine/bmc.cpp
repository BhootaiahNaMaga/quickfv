#include "engine/bmc.h"

#include "cadical.hpp"

namespace qfv {

namespace {

/// Stops CaDiCaL when the wall-clock budget is exhausted.
struct Deadline : public CaDiCaL::Terminator {
    std::chrono::steady_clock::time_point at;
    bool terminate() override { return std::chrono::steady_clock::now() >= at; }
};

} // namespace

struct Bmc::Impl {
    CaDiCaL::Solver solver;
    Deadline deadline;
};

Bmc::Bmc(const TransitionSystem& ts, BmcOptions opts) : ts(ts), opts(opts), impl(new Impl) {
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
}

Bmc::~Bmc() { delete impl; }

void Bmc::addClause(std::initializer_list<int> c) {
    for (int l : c)
        impl->solver.add(l);
    impl->solver.add(0);
    clauses++;
}

int Bmc::lit(int frame, Lit l) {
    int v = encode(frame, varOf(l));
    return isNeg(l) ? -v : v;
}

/// Returns the SAT literal of AIG variable `var` in `frame`, encoding its cone
/// iteratively (a latch in frame k refers to its next-state logic in frame k-1).
int Bmc::encode(int frame, uint32_t var) {
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
                    int8_t init = latch.init[bitOfVar[x]];
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

void Bmc::ensureFrame(int frame) {
    while (int(map.size()) <= frame) {
        map.emplace_back(ts.aig.numVars(), 0);
        int f = int(map.size()) - 1;
        // Constraints (assumptions of the design) hold in every frame.
        for (Lit c : ts.constraints) {
            int l = lit(f, c);
            addClause({l});
        }
    }
}

Cex Bmc::extract(int depth) {
    auto value = [&](int f, Lit l) -> int8_t {
        int m = map[f][varOf(l)];
        if (!m)
            return -1; // never encoded: irrelevant to the violation
        int v = impl->solver.val(m) > 0 ? 1 : 0;
        return int8_t(isNeg(l) ? !v : v);
    };
    Cex cex;
    cex.depth = depth;
    for (auto& latch : ts.latches) {
        std::vector<int8_t> bits;
        for (size_t b = 0; b < latch.cur.size(); b++)
            bits.push_back(latch.init[b] >= 0 ? latch.init[b] : value(0, latch.cur[b]));
        cex.initLatches.push_back(bits);
    }
    for (int f = 0; f <= depth; f++) {
        std::vector<std::vector<int8_t>> frame;
        for (auto& in : ts.inputs) {
            std::vector<int8_t> bits;
            for (Lit l : in.bits)
                bits.push_back(value(f, l));
            frame.push_back(bits);
        }
        cex.inputs.push_back(frame);
    }
    return cex;
}

std::vector<PropResult> Bmc::run(const Event& onEvent) {
    start = std::chrono::steady_clock::now();
    impl->deadline.at = start + std::chrono::milliseconds(int64_t(opts.budgetSeconds * 1000));
    impl->solver.connect_terminator(&impl->deadline);
    auto elapsedMs = [&] {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count();
    };

    std::vector<PropResult> results(ts.props.size());
    size_t open = results.size();
    bool outOfTime = false;
    for (int k = 0; k <= opts.maxDepth && open && !outOfTime; k++) {
        for (size_t p = 0; p < ts.props.size() && !outOfTime; p++) {
            auto& r = results[p];
            if (r.status == "CEX")
                continue;
            int bad = lit(k, ts.props[p].bad);
            impl->solver.assume(bad);
            int res = impl->solver.solve();
            if (res == 10) {
                r.status = "CEX";
                r.cex = extract(k);
                r.ms = elapsedMs();
                open--;
                onEvent(p, r);
            }
            else if (res == 20) {
                r.depthChecked = k;
            }
            else {
                outOfTime = true; // interrupted by the deadline
            }
        }
    }
    for (size_t p = 0; p < results.size(); p++) {
        if (results[p].status != "CEX") {
            results[p].status = "PASS_BOUNDED";
            results[p].ms = elapsedMs();
            onEvent(p, results[p]);
        }
    }
    return results;
}

std::string btor2Witness(const TransitionSystem& ts, size_t prop, const Cex& cex) {
    auto bitsText = [](const std::vector<int8_t>& bits) {
        std::string s;
        for (size_t i = bits.size(); i-- > 0;)
            s += bits[i] > 0 ? '1' : '0'; // don't-care bits are set to 0
        return s;
    };
    std::string w = "sat\nb" + std::to_string(prop) + "\n#0\n";
    for (size_t i = 0; i < ts.latches.size(); i++) {
        bool hasInit = true;
        for (auto b : ts.latches[i].init)
            hasInit = hasInit && b >= 0;
        if (!hasInit)
            w += std::to_string(i) + " " + bitsText(cex.initLatches[i]) + "\n";
    }
    for (size_t f = 0; f < cex.inputs.size(); f++) {
        w += "@" + std::to_string(f) + "\n";
        for (size_t i = 0; i < ts.inputs.size(); i++)
            w += std::to_string(i) + " " + bitsText(cex.inputs[f][i]) + "\n";
    }
    return w + ".\n";
}

} // namespace qfv
