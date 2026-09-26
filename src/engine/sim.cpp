#include "engine/sim.h"

#include <fstream>
#include <sstream>

namespace qfv {

std::vector<std::vector<size_t>> simulate(
    const TransitionSystem& ts, const SimTrace& trace, bool& constraintsHeld,
    std::vector<std::vector<std::vector<int8_t>>>* latchValues) {
    const auto& aig = ts.aig;
    std::vector<uint8_t> val(aig.numVars(), 0);
    auto get = [&](Lit l) -> uint8_t { return val[varOf(l)] ^ uint8_t(isNeg(l)); };
    auto bit = [](int8_t b) -> uint8_t { return b > 0 ? 1 : 0; };

    for (size_t i = 0; i < ts.latches.size(); i++) {
        auto& latch = ts.latches[i];
        for (size_t b = 0; b < latch.cur.size(); b++) {
            int8_t init = latch.init[b];
            val[varOf(latch.cur[b])] =
                init >= 0 ? uint8_t(init)
                          : (i < trace.initLatches.size() ? bit(trace.initLatches[i][b]) : 0);
        }
    }
    constraintsHeld = true;
    std::vector<std::vector<size_t>> badsPerFrame;
    for (size_t f = 0; f < trace.inputs.size(); f++) {
        if (latchValues) {
            std::vector<std::vector<int8_t>> frame;
            for (auto& latch : ts.latches) {
                std::vector<int8_t> bits;
                for (Lit l : latch.cur)
                    bits.push_back(int8_t(get(l)));
                frame.push_back(bits);
            }
            latchValues->push_back(frame);
        }
        for (size_t i = 0; i < ts.inputs.size(); i++)
            for (size_t b = 0; b < ts.inputs[i].bits.size(); b++)
                val[varOf(ts.inputs[i].bits[b])] = bit(trace.inputs[f][i][b]);
        // AND gates are created after their fanins, so index order is topological.
        for (uint32_t v = 1; v < aig.numVars(); v++)
            if (aig.kindOf(v) == Aig::Kind::And)
                val[v] = get(aig.fanin0(v)) & get(aig.fanin1(v));
        for (Lit c : ts.constraints)
            constraintsHeld = constraintsHeld && get(c);
        std::vector<size_t> bads;
        for (size_t p = 0; p < ts.props.size(); p++)
            if (get(ts.props[p].bad))
                bads.push_back(p);
        badsPerFrame.push_back(bads);
        // Clock edge: all latches take their next value simultaneously.
        std::vector<uint8_t> next;
        for (auto& latch : ts.latches)
            for (Lit l : latch.next)
                next.push_back(get(l));
        size_t k = 0;
        for (auto& latch : ts.latches)
            for (Lit l : latch.cur)
                val[varOf(l)] = next[k++];
    }
    return badsPerFrame;
}

bool parseWitness(const std::string& path, const TransitionSystem& ts, SimTrace& trace,
                  std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    auto toBits = [](const std::string& s, size_t width) {
        std::vector<int8_t> bits(width, 0);
        for (size_t i = 0; i < width && i < s.size(); i++)
            bits[i] = s[s.size() - 1 - i] == '1' ? 1 : 0;
        return bits;
    };
    trace.initLatches.assign(ts.latches.size(), {});
    for (size_t i = 0; i < ts.latches.size(); i++)
        trace.initLatches[i].assign(ts.latches[i].cur.size(), 0);
    std::string line;
    bool inStates = false;
    while (std::getline(in, line)) {
        if (line.empty() || line == "sat" || line[0] == 'b' || line[0] == 'j')
            continue;
        if (line == ".")
            break;
        if (line[0] == '#') {
            inStates = true;
            continue;
        }
        if (line[0] == '@') {
            inStates = false;
            trace.inputs.emplace_back();
            for (auto& inp : ts.inputs)
                trace.inputs.back().emplace_back(inp.bits.size(), 0);
            continue;
        }
        std::istringstream ls(line);
        size_t idx;
        std::string value;
        ls >> idx >> value;
        if (inStates) {
            if (idx < ts.latches.size())
                trace.initLatches[idx] = toBits(value, ts.latches[idx].cur.size());
        }
        else if (!trace.inputs.empty() && idx < ts.inputs.size()) {
            trace.inputs.back()[idx] = toBits(value, ts.inputs[idx].bits.size());
        }
    }
    return true;
}

} // namespace qfv
