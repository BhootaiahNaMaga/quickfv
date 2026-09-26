#include "engine/unroll.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>

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

Unroller::Unroller(const TransitionSystem& ts, bool freeInit, const std::string& certBase) :
    ts(ts), freeInit(freeInit), impl(new Impl), certBase(certBase) {
    syncLatches();
    if (!certBase.empty()) {
        // Tracing must start before the first clause. Quiet: CaDiCaL would print
        // "c opening file" on stdout, which is our NDJSON event stream.
        impl->solver.set("quiet", 1);
        impl->solver.set("lidrup", 1);
        impl->solver.set("binary", 0);
        if (!impl->solver.trace_proof((certBase + ".lidrup").c_str()) ||
            !(icnf = std::fopen((certBase + ".icnf").c_str(), "w")))
            throw std::runtime_error("cannot write certificate files " + certBase + ".*");
        std::fprintf(icnf, "p icnf\n");
    }
    // "Lucky phases" preprocessing runs at the start of every solve() and does
    // not poll the terminator; on a large incremental unrolling it went
    // quadratic and blew through the time budget (M4). Useless for BMC anyway.
    impl->solver.set("lucky", 0);
    trueVar = nextVar++;
    addClause({trueVar});
    impl->solver.connect_terminator(&impl->deadline);
}

Unroller::~Unroller() {
    finishCertificate();
    delete impl;
}

void Unroller::finishCertificate() {
    if (!icnf)
        return;
    std::fclose(icnf);
    icnf = nullptr;
    impl->solver.close_proof_trace();
    if (!lastQueryUnknown || lastQueryOffset < 0)
        return;
    // The last query was cut off by the time budget. It proves nothing, and the
    // checker cannot pair its UNKNOWN answer, so both files are cut back to just
    // before it: what remains covers every answer the verdicts rely on.
    std::filesystem::resize_file(certBase + ".icnf", std::uintmax_t(lastQueryOffset));
    auto proof = certBase + ".lidrup";
    std::ifstream in(proof, std::ios::binary | std::ios::ate);
    std::streamoff size = in.tellg(), cut = -1;
    const std::streamoff chunk = 1 << 20;
    std::string buf;
    for (std::streamoff end = size; end > 0 && cut < 0;) {
        std::streamoff start = std::max<std::streamoff>(0, end - chunk);
        buf.resize(size_t(end - start + 2));
        in.seekg(start);
        in.read(buf.data(), end - start);
        buf.resize(size_t(in.gcount()));
        for (size_t i = buf.size(); i-- > 0;)
            if (buf[i] == 'q' && i + 1 < buf.size() && buf[i + 1] == ' ' && (start + std::streamoff(i) == 0 || (i > 0 && buf[i - 1] == '\n'))) {
                cut = start + std::streamoff(i);
                break;
            }
        end = start + 1; // overlap by one byte so a line start at a chunk edge is seen
        if (start == 0)
            break;
    }
    in.close();
    if (cut >= 0)
        std::filesystem::resize_file(proof, std::uintmax_t(cut));
}

void Unroller::setDeadline(std::chrono::steady_clock::time_point t) { impl->deadline.at = t; }

void Unroller::syncLatches() {
    uint32_t n = ts.aig.numVars();
    latchOfVar.assign(n, -1);
    bitOfVar.assign(n, -1);
    for (size_t i = 0; i < ts.latches.size(); i++)
        for (size_t b = 0; b < ts.latches[i].cur.size(); b++) {
            latchOfVar[varOf(ts.latches[i].cur[b])] = int32_t(i);
            bitOfVar[varOf(ts.latches[i].cur[b])] = int32_t(b);
        }
}

size_t Unroller::addAssumption(Lit c) {
    int act = nextVar++;
    for (int f = 0; f < int(map.size()); f++)
        addClause({-act, lit(f, c)});
    assumptions.push_back({c, act, true});
    return assumptions.size() - 1;
}

void Unroller::setAssumptionActive(size_t id, bool active) { assumptions.at(id).active = active; }

void Unroller::addClause(std::initializer_list<int> c) {
    for (int l : c)
        impl->solver.add(l);
    impl->solver.add(0);
    clauses++;
    if (icnf) {
        std::fputc('i', icnf);
        for (int l : c)
            std::fprintf(icnf, " %d", l);
        std::fprintf(icnf, " 0\n");
    }
}

int Unroller::lit(int frame, Lit l) {
    int v = encode(frame, varOf(l));
    return isNeg(l) ? -v : v;
}

/// SAT literal of AIG variable `var` in `frame`. Iterative: a latch in frame k
/// refers to its next-state logic in frame k-1, so recursion could be deep.
int Unroller::encode(int frame, uint32_t var) {
    ensureFrame(frame);
    // The AIG grows as a session adds monitors; frames grow with it (checked
    // once per growth, not per call: this is the hottest path).
    if (knownVars != ts.aig.numVars()) {
        knownVars = ts.aig.numVars();
        latchOfVar.resize(knownVars, -1);
        bitOfVar.resize(knownVars, -1);
        for (auto& m : map)
            m.resize(knownVars, 0);
    }
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
        for (auto& a : assumptions)
            addClause({-a.act, lit(f, a.lit)});
    }
}

int Unroller::solve(const std::vector<int>& extra) {
    std::vector<int> all;
    for (auto& a : assumptions)
        if (a.active)
            all.push_back(a.act);
    all.insert(all.end(), extra.begin(), extra.end());
    for (int a : all)
        impl->solver.assume(a);
    int res = impl->solver.solve();
    if (icnf) {
        // Our own record of the query and the answer we are relying on.
        lastQueryOffset = std::ftell(icnf);
        lastQueryUnknown = res == 0;
        std::fputc('q', icnf);
        for (int a : all)
            std::fprintf(icnf, " %d", a);
        std::fprintf(icnf, " 0\n");
        // lidrup-check requires a conclusion after each answer: for UNSAT the
        // failed assumptions ('f', here all of them), for SAT values that the
        // proof's model must agree with ('v', here the query literals, which
        // hold in any model of the query).
        auto conclude = [&](char type) {
            std::fputc(type, icnf);
            for (int a : all)
                std::fprintf(icnf, " %d", a);
            std::fprintf(icnf, " 0\n");
        };
        if (res == 20) {
            std::fprintf(icnf, "s UNSATISFIABLE\n");
            conclude('f');
            unsatClaims++;
        }
        else if (res == 10) {
            std::fprintf(icnf, "s SATISFIABLE\n");
            conclude('v');
        }
        else
            std::fprintf(icnf, "s UNKNOWN\n");
    }
    return res;
}

int8_t Unroller::value(int frame, Lit l) const {
    if (frame >= int(map.size()) || varOf(l) >= map[frame].size())
        return -1;
    int m = map[frame][varOf(l)];
    if (!m)
        return -1;
    int v = impl->solver.val(m) > 0 ? 1 : 0;
    return int8_t(isNeg(l) ? !v : v);
}

} // namespace qfv
