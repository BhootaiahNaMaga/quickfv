#!/usr/bin/env python3
"""Scaling benchmark: qfv bmc vs rIC3 (BMC and default IC3 portfolio) on the
same BTOR2, for larger FIFOs (DEPTH x 8-bit). Every CEX is checked with btorsim.

Usage: tests/bench_fifo_scale.py [--depths 8 16 32] [--timeout 60]
"""
import argparse, json, os, signal, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(ROOT, "tests"))
from check_witness import check  # noqa: E402

BIN = os.path.join(ROOT, "tools", "oss-cad-suite", "bin")
QFV = os.path.join(ROOT, "build", "qfv")
CASE = os.path.join(ROOT, "casestudy", "m0_fifo")


def timed(cmd, timeout):
    """Runs cmd in its own session and kills the whole process group on timeout:
    bin/rIC3 is a wrapper script, and killing only it orphans the real prover."""
    t0 = time.time()
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                         start_new_session=True,
                         env={**os.environ, "PATH": BIN + ":" + os.environ["PATH"]})
    try:
        out, _ = p.communicate(timeout=timeout)
        return out, time.time() - t0
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.communicate()
        return "TIMEOUT", timeout


def ric3_result(out):
    if out == "TIMEOUT":
        return "timeout"
    first = out.split("\n")[0].strip()
    if first.startswith("sat") or "\nsat" in out:
        frames = [l for l in out.split("\n") if l.startswith("@")]
        return f"CEX len {len(frames)}"
    return first or "?"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--depths", type=int, nargs="+", default=[8, 16, 32])
    ap.add_argument("--timeout", type=int, default=60)
    a = ap.parse_args()
    work = os.path.join(ROOT, "work", "m2", "scale")
    os.makedirs(work, exist_ok=True)
    for D in a.depths:
        src = open(os.path.join(CASE, "rtl", "fifo.sv")).read()
        src = src.replace("parameter FIFO_DEPTH = 4", f"parameter FIFO_DEPTH = {D}")
        src = src.replace("parameter DATA_WIDTH = 1", "parameter DATA_WIDTH = 8")
        rtl = os.path.join(work, f"fifo_d{D}.sv")
        open(rtl, "w").write(src)
        for variant in ["DEEP", "DATA", "CLEAN"]:
            w = os.path.join(work, f"d{D}_{variant}")
            defs = [] if variant == "CLEAN" else ["-D", f"BUG_{variant}"]
            out, qt = timed([QFV, "bmc", "--top", "fifo", *defs, "--clock", "clk", "--reset-expr", "!reset_",
                             "--reset-free", "--budget", str(a.timeout), "--work", w, rtl,
                             os.path.join(CASE, "sva", "fifo_1r1w_props.sv")], a.timeout + 30)
            ev = [json.loads(l) for l in out.splitlines() if l.startswith("{")]
            model = next(e for e in ev if e["event"] == "model")
            res = [e for e in ev if e["event"] == "result"]
            cex = [e for e in res if e["status"] == "CEX"]
            btor = os.path.join(w, "model.btor")
            if cex:
                first = min(cex, key=lambda e: e["ms"])
                ok = all(check(btor, e["witness"])[0] for e in cex)
                q = f"CEX len {first['length']} in {first['ms'] / 1000:.2f}s ({'certified' if ok else 'NOT CERTIFIED'})"
            else:
                q = f"clean to depth {min(e['depth_reached'] for e in res)} in {a.timeout}s"
            rb, rbt = timed([os.path.join(BIN, "rIC3"), "--engine", "bmc", "--end", "60", btor, "--witness"],
                            a.timeout)
            rp, rpt = timed([os.path.join(BIN, "rIC3"), btor], a.timeout)
            print(f"D={D:<3} {variant:6s} latches={model['latches']:4d} ands={model['ands']:6d} | "
                  f"qfv: {q:44s} | rIC3 bmc: {ric3_result(rb):12s} {rbt:6.2f}s | "
                  f"rIC3 portfolio: {(rp.split(chr(10))[0] if rp != 'TIMEOUT' else 'timeout'):8s} {rpt:6.2f}s",
                  flush=True)


if __name__ == "__main__":
    main()
