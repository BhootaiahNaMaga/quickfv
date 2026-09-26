#!/usr/bin/env python3
"""Direct AIG emission (session mode) vs the text-monitor + Yosys flow.

For every property in tests/sva_equiv/props.txt, with and without
`disable iff (rst)`, on a design whose signals are free inputs:

  session: `qfv serve` loads the design once; each property is added and
           checked with no Yosys run (SVA -> AIG directly).
  flow:    `qfv bmc` compiles the property to a text monitor and runs Yosys
           (the path validated in M1 against EBMC, Verilator and golden tests).

They must agree on the verdict and on the shortest CEX length: the session
reports one frame less, because Yosys registers a clocked assertion's
condition (lesson 3 in casestudy/m2_engine/README.md).

Every session CEX is also replayed in Verilator on the text-monitor sources,
where the assertion must fail.

Usage: tests/session_equiv/run_session_equiv.py [--filter SUBSTR]
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tests"))
from serve_client import Serve  # noqa: E402

QFV = os.path.join(ROOT, "build", "qfv")
BIN = os.path.join(ROOT, "tools", "oss-cad-suite", "bin")
DESIGN = ("module t(input clk, input rst, input a, input b, input c, input d, input [2:0] x);\n"
          "endmodule\n")


def props():
    with open(os.path.join(ROOT, "tests", "sva_equiv", "props.txt")) as f:
        return [l.strip() for l in f if l.strip() and not l.startswith("#")]


def flow(prop, disable, tmp):
    """Yosys flow: verdict and shortest CEX length."""
    dis = "disable iff (rst) " if disable else ""
    src = os.path.join(tmp, "flow.sv")
    open(src, "w").write(DESIGN.replace("endmodule", f"P: assert property (@(posedge clk) {dis}{prop});\nendmodule"))
    env = {**os.environ, "PATH": BIN + ":" + os.environ["PATH"]}
    r = subprocess.run([QFV, "bmc", "--top", "t", "--clock", "clk", "--budget", "3", "--max-depth", "12",
                        "--no-ric3", "--work", os.path.join(tmp, "flow"), src],
                       capture_output=True, text=True, env=env)
    ev = [json.loads(l) for l in r.stdout.splitlines() if l.startswith("{")]
    summary = [e for e in ev if e["event"] == "summary"]
    # Exit 0 (no CEX) or 1 (CEX) with a summary; anything else is a failed run,
    # never a PASS_BOUNDED.
    if r.returncode not in (0, 1) or not summary or "P" not in summary[-1]["verdicts"]:
        return f"FLOW-ERROR (exit {r.returncode}): {(r.stdout + r.stderr)[-200:]!r}", None
    res = [e for e in ev if e["event"] == "result" and e["goal"] == "no-failure"]
    cex = [e["length"] for e in res if e["status"] == "CEX"]
    return summary[-1]["verdicts"]["P"], min(cex) if cex else None


def replay_on_text_monitor(tb, prop, disable, tmp):
    """The session's CEX must also fail the M1-validated text monitor."""
    dis = "disable iff (rst) " if disable else ""
    src = os.path.join(tmp, "rep.sv")
    open(src, "w").write(DESIGN.replace("endmodule", f"P: assert property (@(posedge clk) {dis}{prop});\nendmodule"))
    out = os.path.join(tmp, "rep_mon")
    if subprocess.run([QFV, "compile-sva", "--top", "t", "-o", out, src], capture_output=True).returncode:
        return "compile-error"
    obj = os.path.join(tmp, "rep_obj")
    shutil.rmtree(obj, ignore_errors=True)
    b = subprocess.run([os.path.join(BIN, "verilator"), "--binary", "--timing", "--assert", "-Wno-fatal",
                        "-Wno-lint", "-Wno-style", "--top-module", "qfv_replay", "-Mdir", obj, "-o", "sim",
                        tb, os.path.join(out, "rep.sv")], capture_output=True, text=True)
    if b.returncode:
        return "build-error"
    r = subprocess.run([os.path.join(obj, "sim"), "+verilator+error+limit+1000"], capture_output=True, text=True)
    return "confirmed" if "Assertion failed in qfv_replay.dut.P" in r.stdout + r.stderr else "NOT-REPRODUCED"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--filter", default="")
    args = ap.parse_args()
    fails = total = 0
    with tempfile.TemporaryDirectory() as tmp:
        design = os.path.join(tmp, "t.sv")
        open(design, "w").write(DESIGN)
        s = Serve(os.path.join(tmp, "session"))
        r, _, load_ms = s.call("load", files=[design], top="t", clock="clk")
        assert r["ok"], r
        print(f"session loaded once in {load_ms:.0f} ms\n")
        n = 0
        for prop in props():
            if args.filter not in prop:
                continue
            for disable in (False, True):
                n += 1
                total += 1
                label = f"P{n}"
                res, ev, ms = s.call("check_assertion", sva=prop, name=label,
                                     disable_iff="rst" if disable else "", budget_s=3)
                verdict = res.get("check", {}).get("verdicts", {}).get(label, "ERROR: " + json.dumps(res.get("lint"))[:200])
                cex = [e for e in ev if e.get("status") == "CEX" and e.get("goal") == "no-failure"]
                s_len = min((e["length"] for e in cex), default=None)
                f_verdict, f_len = flow(prop, disable, tmp)
                ok = verdict == f_verdict and (s_len is None or s_len == f_len - 1)
                rep = "-"
                if cex:
                    shortest = min(cex, key=lambda e: e["length"])
                    rep = replay_on_text_monitor(shortest["trace"]["testbench"], prop, disable, tmp)
                    ok = ok and rep == "confirmed" and shortest["certified"]["btorsim"] == "confirmed"
                fails += not ok
                name = ("disable iff (rst) " if disable else "") + prop
                print(f"{'PASS' if ok else 'FAIL'}  {name:58s} session={verdict}/{s_len} "
                      f"flow={f_verdict}/{f_len} replay={rep} ({ms:.0f} ms)")
        s.close()
    print(f"\n{total - fails}/{total} agree")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
