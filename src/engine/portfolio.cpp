#include "engine/portfolio.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include <condition_variable>
#include <mutex>
#include <thread>

#include "engine/external.h"

namespace qfv {

namespace fs = std::filesystem;

namespace {

std::string tool(const char* env, const char* fallback) {
    const char* v = std::getenv(env);
    return v ? v : fallback;
}

} // namespace

bool writeAiger(const TransitionSystem& ts, const std::string& path, const std::vector<size_t>& props,
                const std::vector<Lit>& extraConstraints, AigerLayout* layout) {
    const auto& aig = ts.aig;
    // AIGER variable numbering: inputs, then latches, then AND gates.
    std::vector<uint32_t> var(aig.numVars(), 0);
    uint32_t next = 1;
    AigerLayout lay;
    for (size_t i = 0; i < ts.inputs.size(); i++)
        for (size_t b = 0; b < ts.inputs[i].bits.size(); b++) {
            var[varOf(ts.inputs[i].bits[b])] = next++;
            lay.inputBits.push_back({i, b});
        }
    for (size_t l = 0; l < ts.latches.size(); l++)
        for (size_t b = 0; b < ts.latches[l].cur.size(); b++) {
            var[varOf(ts.latches[l].cur[b])] = next++;
            lay.latchBits.push_back({l, b});
        }
    std::vector<uint32_t> ands;
    for (uint32_t v = 1; v < aig.numVars(); v++)
        if (aig.kindOf(v) == Aig::Kind::And) {
            var[v] = next++;
            ands.push_back(v);
        }
    auto lit = [&](Lit l) { return 2 * var[varOf(l)] + (isNeg(l) ? 1 : 0); };
    std::vector<Lit> constraints = ts.constraints;
    constraints.insert(constraints.end(), extraConstraints.begin(), extraConstraints.end());

    std::ofstream out(path);
    if (!out)
        return false;
    out << "aag " << next - 1 << " " << lay.inputBits.size() << " " << lay.latchBits.size() << " 0 "
        << ands.size() << " " << props.size() << " " << constraints.size() << "\n";
    for (size_t k = 0; k < lay.inputBits.size(); k++)
        out << 2 * (k + 1) << "\n";
    for (auto [l, b] : lay.latchBits) {
        auto& latch = ts.latches[l];
        uint32_t cur = 2 * var[varOf(latch.cur[b])];
        int8_t init = latch.init[b];
        // Reset value: 0, 1, or the latch itself (uninitialized).
        out << cur << " " << lit(latch.next[b]) << " " << (init < 0 ? cur : uint32_t(init)) << "\n";
    }
    for (size_t p : props)
        out << lit(ts.props[p].bad) << "\n";
    for (Lit c : constraints)
        out << lit(c) << "\n";
    for (uint32_t v : ands)
        out << 2 * var[v] << " " << lit(aig.fanin0(v)) << " " << lit(aig.fanin1(v)) << "\n";
    if (layout)
        *layout = lay;
    return bool(out);
}

namespace {

/// Parses an AIGER witness ("1 / b0 / <latch init bits> / <input bits>* / .").
std::optional<Cex> parseAigerWitness(const std::string& text, const TransitionSystem& ts,
                                     const AigerLayout& lay) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line) && line != "1") {
    }
    if (line != "1")
        return std::nullopt;
    std::getline(in, line); // b0
    std::string init;
    std::getline(in, init);
    Cex cex;
    for (auto& l : ts.latches)
        cex.initLatches.emplace_back(l.cur.size(), -1);
    for (size_t k = 0; k < lay.latchBits.size() && k < init.size(); k++) {
        auto [l, b] = lay.latchBits[k];
        cex.initLatches[l][b] = init[k] == '1' ? 1 : init[k] == '0' ? 0 : -1;
    }
    while (std::getline(in, line) && line != ".") {
        std::vector<std::vector<int8_t>> frame;
        for (auto& i : ts.inputs)
            frame.emplace_back(i.bits.size(), 0);
        for (size_t k = 0; k < lay.inputBits.size() && k < line.size(); k++) {
            auto [i, b] = lay.inputBits[k];
            frame[i][b] = line[k] == '1' ? 1 : 0;
        }
        cex.inputs.push_back(frame);
    }
    if (cex.inputs.empty())
        return std::nullopt;
    cex.depth = int(cex.inputs.size()) - 1;
    return cex;
}

} // namespace

bool checkWitnessCircuit(const std::string& model, const std::string& witness, std::string& log,
                         std::chrono::steady_clock::time_point deadline, const std::atomic<bool>& cancel) {
    // Certifaiger builds one combinational check circuit whose outputs are the
    // proof obligations (reset, transition, safety, base, step); each must be
    // unsatisfiable. Mirrors Certifaiger's check_unsat without its runlim.
    auto dir = fs::absolute(witness + ".check").string();
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto r = runProcess({tool("QFV_CERTIFAIGER", "certifaiger"), model, witness, dir + "/check.aig"}, deadline, cancel);
    log = r.output;
    if (!r.finished || r.exitCode != 0)
        return false;
    // aigsplit writes one file per obligation into the current directory.
    auto split = runProcess({tool("QFV_AIGSPLIT", "aigsplit"), "-n", "check.aig", "split_"}, deadline, cancel, dir);
    if (!split.finished || split.exitCode != 0) {
        log += split.output;
        return false;
    }
    int checked = 0;
    for (auto& e : fs::directory_iterator(dir)) {
        auto name = e.path().filename().string();
        if (name == "check.aig" || e.path().extension() != ".aig")
            continue;
        auto cnf = e.path().string() + ".cnf";
        runProcess({tool("QFV_AIGTOCNF", "aigtocnf"), e.path().string(), cnf}, deadline, cancel);
        auto sat = runProcess({tool("QFV_KISSAT", "kissat"), "-q", cnf}, deadline, cancel);
        log += name + ": exit " + std::to_string(sat.exitCode) + "\n";
        if (!sat.finished || sat.exitCode != 20) // 20 = UNSAT: obligation holds
            return false;
        checked++;
    }
    log += std::to_string(checked) + " obligations unsatisfiable\n";
    return checked > 0;
}

namespace {

/// At most one external process per core (minus two for our own engines).
class Slots {
public:
    bool acquire(std::chrono::steady_clock::time_point deadline, const std::atomic<bool>& cancel) {
        std::unique_lock lock(m);
        while (used >= limit()) {
            if (cancel || std::chrono::steady_clock::now() >= deadline)
                return false;
            cv.wait_for(lock, std::chrono::milliseconds(20));
        }
        used++;
        return true;
    }
    void release() {
        std::lock_guard lock(m);
        used--;
        cv.notify_one();
    }

private:
    static unsigned limit() {
        unsigned n = std::thread::hardware_concurrency();
        return n > 3 ? n - 2 : 1;
    }
    std::mutex m;
    std::condition_variable cv;
    unsigned used = 0;
};
Slots slots;

} // namespace

ExternalVerdict runRic3(const TransitionSystem& ts, size_t prop, const std::vector<Lit>& extraConstraints,
                        const std::string& workDir, std::chrono::steady_clock::time_point deadline,
                        const std::atomic<bool>& cancel) {
    ExternalVerdict v;
    if (!slots.acquire(deadline, cancel))
        return v;
    struct Release {
        ~Release() { slots.release(); }
    } release;
    fs::create_directories(workDir);
    auto base = workDir + "/ric3_p" + std::to_string(prop);
    AigerLayout lay;
    if (!writeAiger(ts, base + ".aag", {prop}, extraConstraints, &lay))
        return v;
    auto r = runProcess({tool("QFV_RIC3", "rIC3"), base + ".aag", base + "_witness.aig", "--witness"},
                        deadline, cancel);
    std::ofstream(base + ".log") << r.output;
    if (!r.finished)
        return v;
    std::istringstream lines(r.output);
    for (std::string l; std::getline(lines, l);) {
        if (l == "SAT") {
            v.kind = ExternalVerdict::Fails;
            v.cex = parseAigerWitness(r.output, ts, lay);
            v.detail = "rIC3 found a counterexample";
            if (!v.cex)
                v.kind = ExternalVerdict::Unknown;
            return v;
        }
        if (l == "UNSAT") {
            v.kind = ExternalVerdict::Holds;
            std::string log;
            v.certified = fs::exists(base + "_witness.aig") &&
                          checkWitnessCircuit(base + ".aag", base + "_witness.aig", log, deadline, cancel);
            std::ofstream(base + "_certificate.log") << log;
            v.detail = v.certified ? "rIC3 proof, witness circuit verified by Certifaiger"
                                   : "rIC3 proof NOT certified (see " + base + "_certificate.log)";
            return v;
        }
    }
    return v;
}

} // namespace qfv
