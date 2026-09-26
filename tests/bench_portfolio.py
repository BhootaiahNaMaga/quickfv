#!/usr/bin/env python3
"""M5 benchmark: the full QuickFV stack vs rIC3 alone, same model.

For FIFOs of several depths (8-bit data) with the DEEP bug, the DATA bug, and
clean, compare:
  qfv:  simulation + BMC + induction + rIC3 in the portfolio, every CEX
        certified (btorsim), every PROVEN certified (Certifaiger).
        Reported: time to the first verdict for the failing/provable property
        (solver time after load), and end to end including Yosys.
  rIC3: its default portfolio on qfv's own BTOR2 (Yosys excluded).

Usage: tests/bench_portfolio.py [--depths 4 8 16 32] [--timeout 60]
"""
import argparse
import json
import os
import signal
import subprocess
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BIN = os.path.join(ROOT, "tools", "oss-cad-suite", "bin")
PATH = ":".join([BIN, os.path.join(ROOT, "tools", "lidrup-check"), os.path.join(ROOT, "tools", "certifaiger", "build"),
                 os.environ["PATH"]])
QFV = os.path.join(ROOT, "build", "qfv")
CASE = os.path.join(ROOT, "casestudy", "m0_fifo")


def run(cmd, timeout):
    t0 = time.time()
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
                         start_new_session=True, env={**os.environ, "PATH": PATH})
    try:
        out, _ = p.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        out, _ = p.communicate()
        out = out or ""
        out += "\nTIMEOUT"
    return out, time.time() - t0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--depths", type=int, nargs="+", default=[4, 8, 16, 32])
    ap.add_argument("--timeout", type=int, default=60)
    a = ap.parse_args()
    work = os.path.join(ROOT, "work", "m5", "bench")
    os.makedirs(work, exist_ok=True)
    print(f"{'FIFO':6s} {'variant':7s} | {'qfv first verdict':38s} {'e2e':>6s} | {'rIC3 alone':>16s}")
    for d in a.depths:
        src = open(os.path.join(CASE, "rtl", "fifo.sv")).read()
        src = src.replace("parameter FIFO_DEPTH = 4", f"parameter FIFO_DEPTH = {d}")
        src = src.replace("parameter DATA_WIDTH = 1", "parameter DATA_WIDTH = 8")
        rtl = os.path.join(work, f"fifo_d{d}.sv")
        open(rtl, "w").write(src)
        for variant in ["DEEP", "DATA", "CLEAN"]:
            w = os.path.join(work, f"d{d}_{variant}")
            defs = [] if variant == "CLEAN" else ["-D", f"BUG_{variant}"]
            out, e2e = run([QFV, "bmc", "--top", "fifo", *defs, "--clock", "clk", "--reset-expr", "!reset_",
                            "--budget", str(a.timeout), "--work", w, rtl,
                            os.path.join(CASE, "sva", "fifo_1r1w_props.sv")], a.timeout + 60)
            ev = [json.loads(l) for l in out.splitlines() if l.startswith("{")]
            model = next((e for e in ev if e["event"] == "model"), {})
            res = [e for e in ev if e["event"] == "result" and e["goal"] == "no-failure"]
            final = [e for e in res if e["status"] in ("CEX", "PROVEN")]
            if final:
                first = min(final, key=lambda e: e["ms"])
                cert = first.get("certified", {}).get("btorsim") if first["status"] == "CEX" else \
                    ("certified" if first.get("proof_certified") else "UNCERTIFIED")
                q = f"{first['status']} {first['assertion']} {first['ms']/1000:6.2f}s ({first['engine']}, {cert})"
            else:
                q = "no verdict (PASS_BOUNDED)"
            btor = os.path.join(w, "model.btor")
            r_out, r_t = run([os.path.join(BIN, "rIC3"), btor], a.timeout)
            r_first = r_out.strip().split("\n")[0] if "TIMEOUT" not in r_out else "timeout"
            print(f"{d:<6d} {variant:7s} | {q:38s} {e2e:5.1f}s | {r_first:>7s} {r_t:6.2f}s", flush=True)


if __name__ == "__main__":
    main()
