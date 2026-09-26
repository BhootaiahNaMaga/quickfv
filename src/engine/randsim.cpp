#include "engine/randsim.h"

namespace qfv {

RandSim::RandSim(const TransitionSystem& ts, RandSimOptions opts) :
    ts(ts), opts(opts), state(opts.seed * 0x9E3779B97F4A7C15ull + 1) {}

uint64_t RandSim::rand64() {
    // xorshift64*
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 0x2545F4914F6CDD1Dull;
}

void RandSim::evaluate(std::vector<uint64_t>& val) const {
    const auto& aig = ts.aig;
    for (uint32_t v = 1; v < aig.numVars(); v++) {
        if (aig.kindOf(v) != Aig::Kind::And)
            continue;
        Lit a = aig.fanin0(v), b = aig.fanin1(v);
        uint64_t va = val[varOf(a)] ^ (isNeg(a) ? ~0ull : 0);
        uint64_t vb = val[varOf(b)] ^ (isNeg(b) ? ~0ull : 0);
        val[v] = va & vb;
    }
}

void RandSim::run(std::chrono::steady_clock::time_point deadline, std::vector<bool> watch,
                  const std::function<void(size_t, const Cex&)>& onHit,
                  const std::vector<Lit>& extraConstraints) {
    const auto& aig = ts.aig;
    auto word = [&](const std::vector<uint64_t>& val, Lit l) {
        return val[varOf(l)] ^ (isNeg(l) ? ~0ull : 0);
    };
    size_t open = 0;
    for (bool w : watch)
        open += w;

    std::vector<uint64_t> val(aig.numVars(), 0);
    int runLength = opts.cyclesPerRun;
    while (open && std::chrono::steady_clock::now() < deadline) {
        // New run: initial states (free bits random), history recorded for traces.
        std::vector<std::vector<uint64_t>> initWords(ts.latches.size());
        for (size_t l = 0; l < ts.latches.size(); l++) {
            auto& latch = ts.latches[l];
            for (size_t b = 0; b < latch.cur.size(); b++) {
                int8_t init = latch.init[b];
                uint64_t w = init < 0 ? rand64() : (init ? ~0ull : 0);
                val[varOf(latch.cur[b])] = w;
                initWords[l].push_back(w);
            }
        }
        std::vector<std::vector<std::vector<uint64_t>>> inputWords; // [frame][input][bit]
        uint64_t alive = ~0ull;

        for (int f = 0; f < runLength && alive && open; f++) {
            std::vector<std::vector<uint64_t>> frame(ts.inputs.size());
            for (size_t i = 0; i < ts.inputs.size(); i++)
                for (Lit l : ts.inputs[i].bits) {
                    uint64_t w = rand64();
                    val[varOf(l)] = w;
                    frame[i].push_back(w);
                }
            evaluate(val);
            // Re-draw inputs for lanes that violate a constraint.
            for (int t = 0; t <= opts.retries; t++) {
                uint64_t ok = ~0ull;
                for (Lit c : ts.constraints)
                    ok &= word(val, c);
                for (Lit c : extraConstraints)
                    ok &= word(val, c);
                uint64_t bad = ~ok & alive;
                if (!bad || t == opts.retries) {
                    alive &= ok;
                    break;
                }
                for (size_t i = 0; i < ts.inputs.size(); i++)
                    for (size_t b = 0; b < ts.inputs[i].bits.size(); b++) {
                        uint64_t w = (frame[i][b] & ~bad) | (rand64() & bad);
                        frame[i][b] = w;
                        val[varOf(ts.inputs[i].bits[b])] = w;
                    }
                evaluate(val);
            }
            inputWords.push_back(frame);
            cycles += 64;

            for (size_t p = 0; p < ts.props.size(); p++) {
                if (!watch[p])
                    continue;
                uint64_t hit = word(val, ts.props[p].bad) & alive;
                if (!hit)
                    continue;
                int lane = __builtin_ctzll(hit);
                Cex cex;
                cex.depth = f;
                for (size_t l = 0; l < ts.latches.size(); l++) {
                    std::vector<int8_t> bits;
                    for (uint64_t w : initWords[l])
                        bits.push_back(int8_t((w >> lane) & 1));
                    cex.initLatches.push_back(bits);
                }
                for (auto& fr : inputWords) {
                    std::vector<std::vector<int8_t>> ins;
                    for (auto& in : fr) {
                        std::vector<int8_t> bits;
                        for (uint64_t w : in)
                            bits.push_back(int8_t((w >> lane) & 1));
                        ins.push_back(bits);
                    }
                    cex.inputs.push_back(ins);
                }
                watch[p] = false;
                open--;
                onHit(p, cex);
            }
            // Clock edge.
            std::vector<uint64_t> next;
            for (auto& latch : ts.latches)
                for (Lit l : latch.next)
                    next.push_back(word(val, l));
            size_t k = 0;
            for (auto& latch : ts.latches)
                for (Lit l : latch.cur)
                    val[varOf(l)] = next[k++];
            if ((f & 15) == 15 && std::chrono::steady_clock::now() >= deadline)
                break;
        }
        runLength = std::min(runLength * 2, opts.maxCyclesPerRun);
    }
}

} // namespace qfv
