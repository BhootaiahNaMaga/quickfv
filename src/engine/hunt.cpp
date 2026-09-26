#include "engine/hunt.h"

#include <chrono>
#include <cstdlib>
#include <future>
#include <memory>
#include <sys/wait.h>

#include "engine/randsim.h"
#include "engine/unroll.h"

namespace qfv {

using Clock = std::chrono::steady_clock;
using Kind = TransitionSystem::Property::Kind;

namespace {

Cex extractCex(const TransitionSystem& ts, const Unroller& u, int depth) {
    Cex cex;
    cex.depth = depth;
    for (auto& latch : ts.latches) {
        std::vector<int8_t> bits;
        for (size_t b = 0; b < latch.cur.size(); b++)
            bits.push_back(latch.init[b] >= 0 ? latch.init[b] : u.value(0, latch.cur[b]));
        cex.initLatches.push_back(bits);
    }
    for (int f = 0; f <= depth; f++) {
        std::vector<std::vector<int8_t>> frame;
        for (auto& in : ts.inputs) {
            std::vector<int8_t> bits;
            for (Lit l : in.bits)
                bits.push_back(u.value(f, l));
            frame.push_back(bits);
        }
        cex.inputs.push_back(frame);
    }
    return cex;
}

std::string hitStatus(Kind k) { return k == Kind::Assert ? "CEX" : "REACHABLE"; }

} // namespace

std::vector<Verdict> Hunt::run(const Event& onEvent) {
    auto start = Clock::now();
    auto deadline = start + std::chrono::milliseconds(int64_t(opts.budgetSeconds * 1000));
    auto ms = [&] { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); };

    size_t n = ts.props.size();
    std::vector<Verdict> v(n);
    std::vector<bool> done(n, false);
    if (!opts.onlyProps.empty()) { // session mode: everything else counts as done
        done.assign(n, true);
        for (size_t p : opts.onlyProps)
            done[p] = false;
    }
    std::vector<bool> skipped = done;
    // A trace of length d exists: BMC keeps looking in frames < d for a shorter one.
    std::vector<int> shortenBelow(n, -1);
    size_t open = 0;
    for (bool d : done)
        open += !d;

    std::unique_ptr<Unroller> ownBmc, ownStep;
    bool useOwn = !extBmc || !opts.certifyDir.empty(); // certificates need a fresh solver
    if (useOwn) {
        std::string cb = opts.certifyDir.empty() ? "" : opts.certifyDir + "/bmc";
        std::string cs = opts.certifyDir.empty() ? "" : opts.certifyDir + "/step";
        ownBmc = std::make_unique<Unroller>(ts, /*freeInit=*/false, cb);
        ownStep = std::make_unique<Unroller>(ts, /*freeInit=*/true, cs);
        if (extBmc) { // session mode: carry its switchable assumptions over
            for (Lit c : opts.simConstraints) {
                ownBmc->addAssumption(c);
                ownStep->addAssumption(c);
            }
        }
    }
    Unroller& bmc = useOwn ? *ownBmc : *extBmc;
    Unroller& step = useOwn ? *ownStep : *extStep;
    bmc.setDeadline(deadline);
    step.setDeadline(deadline);

    /// A trigger proven unreachable settles its assertion: it can never fail.
    auto settleVacuous = [&](size_t trigger, const std::string& engine) {
        auto it = opts.assertOfTrigger.find(trigger);
        if (it == opts.assertOfTrigger.end() || done[it->second])
            return;
        auto& a = v[it->second];
        a.status = "VACUOUS";
        a.engine = engine;
        a.ms = ms();
        done[it->second] = true;
        open--;
        onEvent(it->second, a);
    };

    /// Induction step: from ANY state, k frames without the goal cannot be
    /// followed by the goal. With the base case (BMC: no goal in frames 0..k),
    /// the goal is unreachable. Returns the solver result.
    auto tryInduction = [&](size_t p, int k) {
        std::vector<int> assumptions;
        for (int i = 0; i < k; i++)
            assumptions.push_back(-step.lit(i, ts.props[p].bad));
        assumptions.push_back(step.lit(k, ts.props[p].bad));
        int sres = step.solve(assumptions);
        if (sres == 20) {
            auto& r = v[p];
            r.status = "UNREACHABLE";
            r.engine = "induction";
            r.inductionK = k;
            r.ms = ms();
            done[p] = true;
            open--;
            onEvent(p, r);
            settleVacuous(p, "induction");
        }
        return sres;
    };

    // 0. Cheap vacuity pre-pass (k <= 1): constant or near-constant triggers
    //    are reported in milliseconds, before anything else runs.
    for (int k = 0; k <= 1; k++)
        for (size_t p = 0; p < n; p++) {
            if (done[p] || ts.props[p].kind != Kind::Reach)
                continue;
            int res = bmc.solve({bmc.lit(k, ts.props[p].bad)});
            if (res != 20)
                continue; // reachable (sim/BMC will report the witness) or out of time
            v[p].depthChecked = k;
            tryInduction(p, k);
        }

    // 1. Random simulation.
    {
        RandSimOptions ro;
        ro.seed = opts.seed;
        RandSim sim(ts, ro);
        auto simEnd = std::min(deadline, start + std::chrono::milliseconds(int64_t(opts.simSeconds * 1000)));
        std::vector<bool> watch(n);
        for (size_t p = 0; p < n; p++)
            watch[p] = !done[p];
        sim.run(simEnd, watch, [&](size_t p, const Cex& cex) {
            auto& r = v[p];
            r.status = hitStatus(ts.props[p].kind);
            r.engine = "sim";
            r.cex = cex;
            r.ms = ms();
            done[p] = true;
            shortenBelow[p] = cex.depth;
            open--;
            onEvent(p, r);
        }, opts.simConstraints);
        simCycles = sim.cyclesSimulated();
    }

    // Portfolio: an external engine on every open property, concurrently.
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    std::vector<std::future<ExternalVerdict>> external(n);
    if (opts.externalEngine)
        for (size_t p = 0; p < n; p++)
            if (!done[p] || shortenBelow[p] >= 0)
                external[p] = std::async(std::launch::async, [this, p, cancel] {
                    return opts.externalEngine(p, *cancel);
                });
    auto pollExternal = [&] {
        for (size_t p = 0; p < n; p++) {
            if (!external[p].valid() ||
                external[p].wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                continue;
            auto ev = external[p].get();
            auto& r = v[p];
            bool isAssert = ts.props[p].kind == Kind::Assert;
            if (ev.kind == ExternalVerdict::Fails && ev.cex) {
                if (done[p] && (!r.cex || r.cex->depth <= ev.cex->depth))
                    continue; // we already have a trace at least as short
                bool had = done[p];
                r.status = hitStatus(ts.props[p].kind);
                r.engine = opts.externalEngineName;
                r.cex = ev.cex;
                r.ms = ms();
                r.minimized = had;
                r.detail = ev.detail;
                if (!had)
                    open--;
                done[p] = true;
                shortenBelow[p] = ev.cex->depth; // BMC keeps looking for a shorter one
                onEvent(p, r);
            }
            else if (ev.kind == ExternalVerdict::Holds && !done[p]) {
                if (isAssert && !ev.certified) {
                    r.detail = ev.detail; // an uncertified proof is never reported as PROVEN
                    continue;
                }
                r.status = isAssert ? "PROVEN" : "UNREACHABLE";
                r.engine = opts.externalEngineName;
                r.proofCertified = ev.certified;
                r.detail = ev.detail;
                r.ms = ms();
                done[p] = true;
                open--;
                onEvent(p, r);
                if (!isAssert)
                    settleVacuous(p, opts.externalEngineName);
            }
        }
    };

    // 2 + 3. BMC with interleaved induction for reachability goals.
    bool outOfTime = Clock::now() >= deadline;
    auto wanted = [&](size_t p, int k) { return !done[p] || k < shortenBelow[p]; };

    int bmcDone = -1; // every wanted property has been checked in frames 0..bmcDone
    for (int k = 0; k <= opts.maxDepth && !outOfTime; k++) {
        bool any = false;
        for (size_t p = 0; p < n; p++)
            any = any || wanted(p, k);
        if (!any)
            break;
        for (size_t p = 0; p < n && !outOfTime; p++) {
            pollExternal();
            if (!wanted(p, k))
                continue;
            auto& r = v[p];
            int res = bmc.solve({bmc.lit(k, ts.props[p].bad)});
            if (res == 0) {
                outOfTime = true;
                break;
            }
            if (res == 10) {
                bool shorter = done[p];
                r.status = hitStatus(ts.props[p].kind);
                r.engine = "bmc";
                r.cex = extractCex(ts, bmc, k);
                r.ms = ms();
                r.minimized = shorter;
                if (!shorter)
                    open--;
                done[p] = true;
                shortenBelow[p] = -1; // BMC's trace is the shortest possible
                onEvent(p, r);
                continue;
            }
            if (!done[p])
                r.depthChecked = k;
            if (done[p] || ts.props[p].kind != Kind::Reach || k > opts.maxInductionDepth)
                continue;
            if (tryInduction(p, k) == 0) {
                outOfTime = true;
                break;
            }
        }
        if (!outOfTime)
            bmcDone = k;
    }

    // BMC is out of depth or time: give still-running engines the rest of the budget.
    // (A trace is already minimal once BMC has cleared every shorter depth.)
    for (size_t p = 0; p < n; p++)
        if ((!done[p] || shortenBelow[p] - 1 > bmcDone) && external[p].valid())
            external[p].wait_until(deadline);
    pollExternal();
    cancel->store(true);
    for (auto& f : external)
        if (f.valid())
            f.wait();

    for (size_t p = 0; p < n; p++) {
        if (done[p] || skipped[p])
            continue;
        auto& r = v[p];
        r.status = ts.props[p].kind == Kind::Assert ? "PASS_BOUNDED" : "NOT_REACHED";
        r.engine = "bmc";
        r.ms = ms();
        onEvent(p, r);
    }
    if (useOwn && !opts.certifyDir.empty()) {
        bmcClaims = bmc.numUnsatClaims();
        stepClaims = step.numUnsatClaims();
        bmc.finishCertificate();
        step.finishCertificate();
    }
    return v;
}

std::vector<CertReport> certifyBounded(const std::string& dir, size_t bmcClaims, size_t stepClaims) {
    std::vector<CertReport> out;
    const char* env = std::getenv("QFV_LIDRUP_CHECK");
    std::string tool = env ? env : "lidrup-check";
    for (auto [name, claims] : {std::pair{"bmc", bmcClaims}, std::pair{"step", stepClaims}}) {
        CertReport r;
        r.solver = name;
        r.unsatClaims = claims;
        std::string cmd = tool + " -q " + dir + "/" + name + ".icnf " + dir + "/" + name + ".lidrup 2>&1";
        std::string output;
        if (FILE* p = popen(cmd.c_str(), "r")) {
            char buf[512];
            while (fgets(buf, sizeof buf, p))
                output += buf;
            int st = pclose(p);
            int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
            if (code == 0)
                r.result = "verified";
            else if (code == 127)
                r.result = "unavailable (lidrup-check not found)";
            else
                r.result = "FAILED: " + output.substr(0, 300);
        }
        out.push_back(r);
    }
    return out;
}

} // namespace qfv
