#!/usr/bin/env python3
"""M6 mutation campaign on the FVEval NL2SVA designs (with our DUTs).

For each design: the clean DUT, its hand-written bugs, and automatic operator
mutants of the DUT source. Each variant is loaded into a qfv session with the
environment the agent settled on (assumptions, reset length) and all
assertions (FVEval references plus the agent's refined ones), then checked.

Recorded per variant: verdicts, time to the first CEX, certification of
every CEX (btorsim) and PROVEN (Certifaiger), and an independent RTL replay of
the first CEX in Verilator on the ORIGINAL SVA (mutated DUT + FVEval checker).

Usage: casestudy/m6_report/campaign.py [--budget S] [--auto N] [--designs ...]
"""
import argparse, json, os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tests"))
from serve_client import Serve  # noqa: E402

BIN = os.path.join(ROOT, "tools", "oss-cad-suite", "bin")
os.environ["PATH"] = ":".join([BIN, os.path.join(ROOT, "tools", "lidrup-check"),
                               os.path.join(ROOT, "tools", "certifaiger", "build"), os.environ["PATH"]])
D = os.path.join(HERE, "designs")
M0 = os.path.join(ROOT, "casestudy", "m0_fifo")

DESIGNS = {
    "fifo_1r1w": dict(
        dut=os.path.join(M0, "rtl", "fifo.sv"), top="fifo", props=[os.path.join(M0, "sva", "fifo_1r1w_props.sv")],
        defines=["NO_LIVENESS"], reset_cycles=1, bugs=["OVERFLOW", "UNDERFLOW", "DATA", "DEEP"],
        assumptions=[], extra=[]),
    "arbiter_rr": dict(
        dut=os.path.join(D, "arbiter_rr.sv"), top="arbiter_rr", props=[os.path.join(D, "arbiter_rr_props.sv")],
        defines=[], reset_cycles=1, bugs=["BUSY", "RR", "ID", "LAST"],
        assumptions=[("arbiter_rr_tb", "env_hold_busy_exclusive", "!(hold && busy)"),
                     ("arbiter_rr_tb", "env_hold_keeps_request", "hold |-> |(last_gnt & req)")],
        extra=[]),
    "counter": dict(
        dut=os.path.join(D, "counter.sv"), top="counter", props=[os.path.join(D, "counter_props.sv")],
        defines=[], reset_cycles=2, bugs=["WRAP", "FLOOR", "IDLE"],
        assumptions=[("counter_tb", "env_jump_in_range", "jump_vld |-> (jump_value >= min && jump_value <= max)")],
        extra=[("counter_tb", "counter_0_refined",
                "(count_d1 == max) && (net_incr_d1 > 0) && !jump_vld_d1 && !tb_reset_1_cycle_pulse_shadow |-> count == max"),
               ("counter_tb", "counter_1_refined",
                "(count_d1 == min) && (net_decr_d1 > 0) && !jump_vld_d1 && !tb_reset_1_cycle_pulse_shadow |-> count == min")]),
}
# Reference assertions the agent found too strong (kept for the record, not used to judge mutants).
TOO_STRONG = {"counter_0", "counter_1"}

# (`<=` is left alone: in these RTL files it is mostly the non-blocking assignment.)
MUTATIONS = [("==", "!="), ("!=", "=="), ("&&", "||"), ("||", "&&"), (">=", ">"), (" > ", " >= "),
             (" < ", " <= "), ("+ 1'b1", "+ 1'b0"), ("- pop", "+ pop"), ("~", ""), ("!", "")]


def auto_mutants(src, limit):
    """Single-site operator mutations inside module bodies (never in comments/ifdefs)."""
    lines = src.split("\n")
    out = []
    inactive = []  # stack: inside an `ifdef BUG_* branch (not compiled in the clean DUT)
    for i, line in enumerate(lines):
        t = line.strip()
        if t.startswith("`ifdef") or t.startswith("`ifndef"):
            inactive.append(t.startswith("`ifdef BUG_"))
            continue
        if t.startswith("`else") and inactive:
            inactive[-1] = not inactive[-1]
            continue
        if t.startswith("`endif") and inactive:
            inactive.pop()
            continue
        if any(inactive):
            continue
        code = line.split("//")[0]
        if not code.strip() or code.strip().startswith("`") or "parameter" in code or "input" in code \
                or "output" in code or "module" in code:
            continue
        for a, b in MUTATIONS:
            pos = code.find(a)
            if pos < 0 or (a == "!" and code[pos:pos + 2] == "!="):
                continue
            if a == "==" and "===" in code:
                continue
            mutated = list(lines)
            mutated[i] = line[:pos] + b + line[pos + len(a):]
            out.append((f"L{i + 1}:{a}->{b or 'drop'}", "\n".join(mutated)))
            break
    step = max(1, len(out) // limit) if limit else 1
    return out[::step][:limit]


def replay(design, cfg, dut_file, tb, label, defines, tmp):
    """Verilator on the mutated DUT + the ORIGINAL FVEval checker, plus the
    agent's extra assertions appended to the checker module (as written)."""
    props = []
    for p in cfg["props"]:
        text = open(p).read()
        if cfg["extra"]:
            mod = cfg["extra"][0][0]
            extra = "".join(f"{n}: assert property (@(posedge clk) disable iff (tb_reset) {sva});\n"
                            for _, n, sva in cfg["extra"])
            m = re.search(r"module\s+" + mod + r"\b.*?endmodule", text, re.S)
            if m:
                text = text[:m.end() - len("endmodule")] + extra + text[m.end() - len("endmodule"):]
        out = os.path.join(tmp, "replay_" + os.path.basename(p))
        open(out, "w").write(text)
        props.append(out)
    obj = os.path.join(tmp, "replay_obj")
    subprocess.run(["rm", "-rf", obj])
    defs = [f"-D{d}" for d in defines]
    b = subprocess.run([os.path.join(BIN, "verilator"), "--binary", "--timing", "--assert", "-Wno-fatal", "-Wno-lint",
                        "-Wno-style", "--top-module", "qfv_replay", "-Mdir", obj, "-o", "sim", *defs, tb, dut_file,
                        *props], capture_output=True, text=True)
    open(os.path.join(tmp, "replay_build.log"), "w").write(b.stderr)
    if b.returncode:
        return "build-error"
    r = subprocess.run([os.path.join(obj, "sim"), "+verilator+error+limit+1000"], capture_output=True, text=True)
    return "confirmed" if re.search(r"Assertion failed in \S*\." + re.escape(label) + r"\b", r.stdout + r.stderr) \
        else "NOT-REPRODUCED"


def run_variant(design, cfg, name, dut_text, defines, budget, tmp):
    dut_file = os.path.join(tmp, f"{design}_{re.sub(r'[^A-Za-z0-9]', '_', name)}.sv")
    open(dut_file, "w").write(dut_text)
    setup = os.path.join(tmp, "setup.tcl")
    defs = "".join(f" +define+{d}" for d in defines)
    lines = [f"analyze -sv12{defs} {dut_file}"] + [f"analyze -sv12{defs} {p}" for p in cfg["props"]]
    lines += [f"elaborate -top {cfg['top']}", "clock clk", f"reset -expression {{!reset_}} -cycles {cfg['reset_cycles']}"]
    open(setup, "w").write("\n".join(lines) + "\n")
    s = Serve(os.path.join(tmp, "session"))
    r, _, load_ms = s.call("load", setup_file=setup)
    if not r.get("ok"):
        s.close()
        return {"variant": name, "error": json.dumps(r)[:300]}
    for mod, n, sva in cfg["assumptions"]:
        ra, _, _ = s.call("add_assumption", module=mod, name=n, sva=sva, disable_iff="tb_reset")
        if not ra.get("ok"):  # a missing assumption changes every verdict
            s.close()
            return {"variant": name, "error": f"assumption {n} rejected: " + json.dumps(ra)[:300]}
    add_ms, add_errors = [], []
    for mod, n, sva in cfg["extra"]:
        rr, _, ms = s.call("add", module=mod, name=n, sva=sva, disable_iff="tb_reset")
        add_ms.append(ms)
        if not rr.get("ok"):
            add_errors.append(n)
    res, ev, check_ms = s.call("check", budget_s=budget)
    s.close()
    verdicts = res.get("verdicts", {})
    cex = [e for e in ev if e.get("status") == "CEX" and e.get("goal") == "no-failure"]
    proven = [e for e in ev if e.get("status") == "PROVEN"]
    judged = {k: v for k, v in verdicts.items() if k not in TOO_STRONG}
    killers = sorted(k for k, v in judged.items() if v == "CEX")
    first = min(cex, key=lambda e: e["ms"]) if cex else None
    rep = "-"
    if killers:
        f2 = min((e for e in cex if e["id"] in killers), key=lambda e: e["ms"])
        rep = replay(design, cfg, dut_file, f2["trace"]["testbench"], f2["id"], defines, tmp)
        if f2.get("x_dependent"):
            rep += " (x-dependent)"
    return {
        "variant": name, "load_ms": round(load_ms), "check_ms": round(check_ms), "verdicts": verdicts,
        "killed_by": killers,
        "first_cex_ms": round(min((e["ms"] for e in cex if e["id"] not in TOO_STRONG), default=-1), 1),
        # None: nothing to certify (not "all certified").
        "cex_certified": all(e["certified"]["btorsim"] == "confirmed" for e in cex) if cex else None,
        "proven_certified": all(e.get("proof_certified") for e in proven) if proven else None,
        "add_errors": add_errors,
        "n_cex": len(cex), "n_proven": len(proven), "rtl_replay": rep,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--budget", type=float, default=10)
    ap.add_argument("--auto", type=int, default=8, help="automatic mutants per design")
    ap.add_argument("--designs", nargs="*", default=list(DESIGNS))
    a = ap.parse_args()
    results = {}
    import tempfile
    for design in a.designs:
        cfg = DESIGNS[design]
        src = open(cfg["dut"]).read()
        variants = [("clean", src, cfg["defines"])]
        variants += [(f"bug:{b}", src, cfg["defines"] + [f"BUG_{b}"]) for b in cfg["bugs"]]
        variants += [(f"auto:{n}", t, cfg["defines"]) for n, t in auto_mutants(src, a.auto)]
        results[design] = []
        for name, text, defs in variants:
            with tempfile.TemporaryDirectory() as tmp:
                t0 = time.time()
                r = run_variant(design, cfg, name, text, defs, a.budget, tmp)
            r["wall_s"] = round(time.time() - t0, 1)
            results[design].append(r)
            print(f"{design:11s} {name:24s} killed_by={r.get('killed_by')} first_cex={r.get('first_cex_ms')}ms "
                  f"proven={r.get('n_proven')} certified=({r.get('cex_certified')},{r.get('proven_certified')}) "
                  f"replay={r.get('rtl_replay')} {r.get('error', '')}", flush=True)
    json.dump(results, open(os.path.join(HERE, "campaign_results.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
