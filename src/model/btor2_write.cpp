// Bit-level transition system → BTOR2, so that external tools (rIC3, btorsim)
// can check the in-memory model, including monitors emitted in a session.
//
// Every AIG variable becomes a 1-bit node; negation uses BTOR2's negative
// argument ids. Latches and inputs keep their order, so witness indices line
// up with ts.latches / ts.inputs (one state per latch *bit*, see below).
#include "model/btor2_write.h"

#include <fstream>
#include <map>

namespace qfv {

bool writeBtor2(const TransitionSystem& ts, const std::string& path, std::optional<size_t> onlyProp,
                Btor2Layout* layout) {
    std::ofstream out(path);
    if (!out)
        return false;
    const auto& aig = ts.aig;
    int64_t next = 1;
    const int64_t sort = next++;
    out << sort << " sort bitvec 1\n";
    const int64_t zero = next++;
    out << zero << " zero " << sort << "\n";
    // Constants before states: BTOR2 requires an init value's id to be smaller
    // than its state's id.
    const int64_t one = next++;
    out << one << " one " << sort << "\n";
    std::vector<int64_t> id(aig.numVars(), 0);
    id[0] = zero;
    auto ref = [&](Lit l) { return isNeg(l) ? -id[varOf(l)] : id[varOf(l)]; };

    Btor2Layout lay;
    // Inputs, one BTOR2 input per bit (named "<name>[<bit>]" for multi-bit).
    for (auto& in : ts.inputs)
        for (size_t b = 0; b < in.bits.size(); b++) {
            id[varOf(in.bits[b])] = next;
            out << next++ << " input " << sort << " "
                << (in.bits.size() == 1 ? in.name : in.name + "[" + std::to_string(b) + "]") << "\n";
            lay.inputBits.push_back({&in - ts.inputs.data(), b});
        }
    for (auto& l : ts.latches)
        for (size_t b = 0; b < l.cur.size(); b++) {
            id[varOf(l.cur[b])] = next;
            std::string name = l.name.empty() ? "" : " " + (l.cur.size() == 1 ? l.name : l.name + "[" + std::to_string(b) + "]");
            out << next++ << " state " << sort << name << "\n";
            lay.stateBits.push_back({&l - ts.latches.data(), b});
        }
    // AND gates in index order (topological).
    for (uint32_t v = 1; v < aig.numVars(); v++) {
        if (aig.kindOf(v) != Aig::Kind::And)
            continue;
        id[v] = next;
        out << next++ << " and " << sort << " " << ref(aig.fanin0(v)) << " " << ref(aig.fanin1(v)) << "\n";
    }
    for (auto& l : ts.latches)
        for (size_t b = 0; b < l.cur.size(); b++) {
            int64_t s = id[varOf(l.cur[b])];
            if (l.init[b] >= 0)
                out << next++ << " init " << sort << " " << s << " " << (l.init[b] ? one : zero) << "\n";
            out << next++ << " next " << sort << " " << s << " " << ref(l.next[b]) << "\n";
        }
    // btorsim crashes on a negated argument of `constraint` (valid BTOR2, but
    // not handled there), so negations get an explicit `not` node here.
    auto positive = [&](Lit l) -> int64_t {
        int64_t r = ref(l);
        if (r >= 0)
            return r;
        out << next << " not " << sort << " " << -r << "\n";
        return next++;
    };
    for (Lit c : ts.constraints) {
        int64_t a = positive(c);
        out << next++ << " constraint " << a << "\n";
    }
    for (size_t p = 0; p < ts.props.size(); p++) {
        if (onlyProp && *onlyProp != p)
            continue;
        lay.badOfProp[p] = lay.numBads++;
        int64_t a = positive(ts.props[p].bad);
        out << next++ << " bad " << a << " " << ts.props[p].name << "\n";
    }
    if (layout)
        *layout = lay;
    return bool(out);
}

bool writeBtor2WithConstraints(const TransitionSystem& ts, const std::string& path,
                               const std::vector<Lit>& extraConstraints, std::optional<size_t> onlyProp,
                               Btor2Layout* layout) {
    TransitionSystem copy = ts;
    for (Lit c : extraConstraints)
        copy.constraints.push_back(c);
    return writeBtor2(copy, path, onlyProp, layout);
}

std::string btor2WitnessForLayout(const TransitionSystem& ts, const Btor2Layout& lay, size_t prop,
                                  const Cex& cex) {
    std::string w = "sat\nb" + std::to_string(lay.badOfProp.at(prop)) + "\n#0\n";
    for (size_t i = 0; i < lay.stateBits.size(); i++) {
        auto [l, b] = lay.stateBits[i];
        if (ts.latches[l].init[b] < 0)
            w += std::to_string(i) + " " + (cex.initLatches[l][b] > 0 ? "1" : "0") + "\n";
    }
    for (size_t f = 0; f < cex.inputs.size(); f++) {
        w += "@" + std::to_string(f) + "\n";
        for (size_t i = 0; i < lay.inputBits.size(); i++) {
            auto [in, b] = lay.inputBits[i];
            w += std::to_string(i) + " " + (cex.inputs[f][in][b] > 0 ? "1" : "0") + "\n";
        }
    }
    return w + ".\n";
}

} // namespace qfv
