#!/usr/bin/env python3
"""FVEval Design2SVA sample: real RTL, assertions written by the "agent".

Design2SVA gives an LLM the RTL and asks for assertions. Here the agent's
assertions are derived from the RTL the way an LLM would read it:
  FSMs:      one assertion per transition branch of the case statement
             (state == Sx && condition |=> state == Sy), plus a legal-state
             invariant when the encoding has unused codes;
  pipelines: valid latency (in_vld |-> ##DEPTH out_vld, and the negation).
Each design is checked clean, then against automatic operator mutants of its
RTL (mutation score = how much of the behaviour the assertions pin down).

Usage: casestudy/m6_report/design2sva.py [--fsm N] [--pipeline N] [--auto N]
"""
import argparse, csv, json, os, re, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tests"))
sys.path.insert(0, HERE)
from serve_client import Serve  # noqa: E402
from campaign import auto_mutants  # noqa: E402

BIN = os.path.join(ROOT, "tools", "oss-cad-suite", "bin")
os.environ["PATH"] = ":".join([BIN, os.path.join(ROOT, "tools", "lidrup-check"),
                               os.path.join(ROOT, "tools", "certifaiger", "build"), os.environ["PATH"]])
csv.field_size_limit(10 ** 9)


def fsm_assertions(rtl):
    states = dict(re.findall(r"parameter\s+(S\d+)\s*=\s*([^;]+);", rtl))
    width = int(re.search(r"parameter\s+FSM_WIDTH\s*=\s*(\d+)", rtl).group(1))
    body = rtl[rtl.index("case(state)"):rtl.index("endcase")]
    out = []
    for m in re.finditer(r"(S\d+):\s*begin(.*?)\n\s{12}end\n", body, re.S):
        src, block = m.group(1), m.group(2)
        branches = re.findall(r"(if|else if|else)\s*(?:\((.*?)\))?\s*begin\s*next_state\s*=\s*(S\d+);", block, re.S)
        if not branches:  # unconditional transition
            m2 = re.search(r"next_state\s*=\s*(S\d+);", block)
            if m2:
                branches = [("else", "", m2.group(1))]
        prior = []
        for kind, cond, dst in branches:
            guard = [f"state == {src}"] + [f"!({c})" for c in prior]
            if kind != "else":
                guard.append(f"({cond})")
                prior.append(cond)
            out.append((f"t_{src}_{len(out)}", " && ".join(guard) + f" |=> state == {dst}"))
    if len(states) < (1 << width):
        out.append(("legal_state", "(" + " || ".join(f"state == {s}" for s in states) + ")"))
    return out


def pipeline_assertions(rtl):
    depth = int(re.search(r"`define\s+DEPTH\s+(\d+)", rtl).group(1))
    return [("lat_vld", f"in_vld |-> ##{depth} out_vld"), ("lat_idle", f"!in_vld |-> ##{depth} !out_vld")]


def all_or_none(items):
    items = list(items)
    return all(items) if items else None


def check(rtl, tb, top, props, budget, tmp):
    src = os.path.join(tmp, "design.sv")
    open(src, "w").write(rtl + "\n" + tb)
    setup = os.path.join(tmp, "setup.tcl")
    open(setup, "w").write(f"analyze -sv12 {src}\nelaborate -top {top}\nclock clk\nreset -expression {{!reset_}}\n")
    s = Serve(os.path.join(tmp, "s"))
    r, _, load_ms = s.call("load", setup_file=setup)
    if not r.get("ok"):
        s.close()
        return {"error": json.dumps(r)[:400]}
    add_ms, add_errors = [], []
    for name, sva in props:
        rr, _, ms = s.call("add", module=top, name=name, sva=sva, disable_iff="!reset_")
        add_ms.append(ms)
        if not rr.get("ok"):
            add_errors.append(name)
    res, ev, ms = s.call("check", budget_s=budget)
    s.close()
    v = res.get("verdicts", {})
    return {"load_ms": round(load_ms), "add_ms_max": round(max(add_ms or [0]), 1), "check_ms": round(ms),
            "verdicts": v, "add_errors": add_errors,
            # None: nothing to certify (not "all certified").
            "cex_certified": all_or_none(e["certified"]["btorsim"] == "confirmed" for e in ev if e.get("status") == "CEX"),
            "proven_certified": all_or_none(e.get("proof_certified") for e in ev if e.get("status") == "PROVEN"),
            "first_cex_ms": min((e["ms"] for e in ev if e.get("status") == "CEX" and e.get("goal") == "no-failure"),
                                default=None)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fsm", type=int, default=6)
    ap.add_argument("--pipeline", type=int, default=4)
    ap.add_argument("--auto", type=int, default=6)
    ap.add_argument("--budget", type=float, default=10)
    a = ap.parse_args()
    results = []
    for kind, n, gen, top in [("fsm", a.fsm, fsm_assertions, "fsm"), ("pipeline", a.pipeline, pipeline_assertions, "pipeline")]:
        rows = list(csv.DictReader(open(os.path.join(ROOT, "tools", "FVEval", "data_design2sva", "data",
                                                     f"design2sva_{kind}.csv"))))
        if kind == "fsm":
            # 51/96 FSMs infer a latch in always_comb (a state branch assigns
            # nothing); QuickFV v1 rejects those (combinational loop), so the
            # sample is drawn from the latch-free ones. Counted in the report.
            rows = [r for r in rows if not re.search(r"S\d+:\s*begin\s*end", r["prompt"])]
        if n == 0:
            continue
        step = max(1, len(rows) // n)
        for r in rows[::step][:n]:
            rtl, tb = r["prompt"], r["testbench"]
            props = gen(rtl)
            with tempfile.TemporaryDirectory() as tmp:
                clean = check(rtl, tb, top, props, a.budget, tmp)
            entry = {"kind": kind, "task": r["task_id"], "rtl_lines": len(rtl.splitlines()),
                     "assertions": len(props), "clean": clean, "mutants": []}
            for mname, mtext in auto_mutants(rtl, a.auto):
                with tempfile.TemporaryDirectory() as tmp:
                    m = check(mtext, tb, top, props, a.budget, tmp)
                if "error" in m:
                    continue  # the mutant does not elaborate
                killed = sorted(k for k, v in m["verdicts"].items() if v == "CEX")
                entry["mutants"].append({"mutant": mname, "killed_by": killed, "certified": m["cex_certified"],
                                         "first_cex_ms": m["first_cex_ms"]})
            results.append(entry)
            cv = clean.get("verdicts", {})
            kills = sum(bool(x["killed_by"]) for x in entry["mutants"])
            print(f"{kind:8s} {r['task_id']:34s} lines={entry['rtl_lines']:4d} asserts={len(props):2d} "
                  f"clean={dict((k, cv.count(k) if False else list(cv.values()).count(k)) for k in set(cv.values()))} "
                  f"check={clean.get('check_ms')}ms mutants killed {kills}/{len(entry['mutants'])} {clean.get('error', '')}",
                  flush=True)
    json.dump(results, open(os.path.join(HERE, "design2sva_results.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
