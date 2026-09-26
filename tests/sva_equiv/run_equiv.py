#!/usr/bin/env python3
"""Equivalence of qfv-generated monitors against the original SVA, using two
independent oracles.

1. EBMC (formal, bounded), which reads the ORIGINAL SVA with its own front end
   and checks both directions:
     A (no false alarms):    assume P           ; assert monitor(P)
     B (no missed failures): assume monitor(P)  ; assert P
   Both must hold up to the bound, and P and monitor(P) must get the same verdict
   on their own; that shows the test is not vacuous, since most properties are
   refutable over free inputs.
   EBMC 6.0 ignores a `disable iff` that becomes true in the middle of an
   attempt (it disagrees with IEEE 1800 16.12 and with Verilator; see
   tests/sva_equiv/README.md), so for `disable iff` variants the EBMC check
   is skipped.

2. Verilator (simulation), which runs the ORIGINAL SVA and the monitor side by
   side on random stimulus (rst ~5% of cycles). Verilator reports a failed
   attempt at the end of its longest possible window, while qfv reports it in
   the earliest cycle in which no thread can still match. So failures are
   matched within a window: every Verilator failure at t needs a monitor failure
   in [t-W, t], and every monitor failure at t needs a Verilator failure in
   [t, t+W], where W is the property's longest match length.
   Two Verilator 5.053 deviations were traced by hand against IEEE 1800
   (tests/sva_equiv/README.md; golden tests in directed/):
     - With `disable iff`, Verilator drops a failure when rst rises after the
       attempt has already failed but before its longest window ends. A
       monitor-only failure at t is accepted only when the trace has rst in
       (t, t+W].
     - With a ranged delay in the antecedent (a ##[m:n] b |-> ...), Verilator
       misses real failures, so it is not used as an oracle for those.

3. Directed golden tests (DIRECTED below): fixed stimulus, with expected failure
   cycles derived by hand from IEEE 1800. Only the monitor is simulated (plain
   Verilog), so they also cover the forms neither oracle can check.

Every line in unsupported.txt must be rejected by `qfv lint`.

Usage: tests/sva_equiv/run_equiv.py [--bound K] [--filter SUBSTR]
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
QFV = os.path.join(ROOT, "build", "qfv")
EBMC = os.path.join(ROOT, "tools", "hw-cbmc", "src", "ebmc", "ebmc")
HERE = os.path.dirname(os.path.abspath(__file__))

HEADER = "module t(input clk, input rst, input a, input b, input c, input d, input [2:0] x);\n"


def read_list(name):
    with open(os.path.join(HERE, name)) as f:
        return [l.strip() for l in f if l.strip() and not l.startswith("#")]


def directive(kind, label, prop, disable):
    dis = "disable iff (rst) " if disable else ""
    return f"{label}: {kind} property (@(posedge clk) {dis}{prop});\n"


def write(path, text):
    with open(path, "w") as f:
        f.write(text)


def compile_monitor(src, outdir):
    r = subprocess.run([QFV, "compile-sva", "--top", "t", "-o", outdir, src],
                       capture_output=True, text=True)
    res = json.loads(r.stdout)
    if not res["ok"]:
        raise RuntimeError(json.dumps(res["assertions"] + res["diagnostics"], indent=1))
    with open(res["files"][0]) as f:
        return f.read()


def ebmc(path, bound):
    r = subprocess.run([EBMC, "--systemverilog", "--top", "t", "--bound", str(bound), path],
                       capture_output=True, text=True)
    results = {}
    for line in r.stdout.splitlines():
        m = re.match(r"^\[t\.(\w+)\] .*: (PROVED up to bound \d+|REFUTED|.*)$", line)
        if m:
            results[m.group(1)] = "ok" if m.group(2).startswith("PROVED") else m.group(2)
    if not results:
        raise RuntimeError("ebmc produced no results:\n" + r.stdout[-2000:] + r.stderr[-2000:])
    return results


VERILATOR = os.path.join(ROOT, "tools", "oss-cad-suite", "bin", "verilator")
SIM_TB = """
module tb;
  reg clk = 0, rst = 0, a = 0, b = 0, c = 0, d = 0;
  reg [2:0] x = 0;
  integer i, seed;
  t dut(.clk(clk), .rst(rst), .a(a), .b(b), .c(c), .d(d), .x(x));
  initial begin
    seed = SEED;
    for (i = 0; i < CYCLES; i = i + 1) begin
      {a, b, c, d, x} = $random(seed);
      rst = ($random(seed) % 20) == 0;
      if (rst) $display("RST %0t", $time + 5);
      #5 clk = 1; #5 clk = 0;
    end
    $display("SIM_END");
    $finish;
  end
endmodule
"""


def verilator_diff(mon_text, prop, disable, tmp, window, cycles=3000, seed=1):
    """Runs original SVA (REF) and monitor (P) together; compares failure cycles."""
    mon = mon_text.replace(
        "!qfv_P_fail);", '!qfv_P_fail) else $display("FAIL P %0t", $time);')
    ref = directive("assert", "REF", prop, disable).replace(
        ");\n", ') else $display("FAIL REF %0t", $time);\n')
    write(os.path.join(tmp, "sim_t.sv"), mon.replace("endmodule", ref + "endmodule"))
    write(os.path.join(tmp, "sim_tb.sv"),
          SIM_TB.replace("SEED", str(seed)).replace("CYCLES", str(cycles)))
    obj = os.path.join(tmp, "obj")
    subprocess.run(["rm", "-rf", obj])
    r = subprocess.run([VERILATOR, "--binary", "--timing", "--assert", "-Wno-fatal", "-Wno-lint",
                        "-Wno-style", "--top-module", "tb", "-Mdir", obj, "-o", "sim",
                        os.path.join(tmp, "sim_tb.sv"), os.path.join(tmp, "sim_t.sv")],
                       capture_output=True, text=True)
    if r.returncode:
        if "Unsupported" in r.stderr:
            return "n/a (Verilator does not support this SVA)"
        raise RuntimeError("verilator build failed:\n" + r.stderr[-1500:])
    r = subprocess.run([os.path.join(obj, "sim")], capture_output=True, text=True)
    if "SIM_END" not in r.stdout:
        raise RuntimeError("simulation did not finish:\n" + r.stdout[-800:] + r.stderr[-800:])
    fails = {"P": [], "REF": []}
    resets = []
    for line in r.stdout.splitlines():
        m = re.match(r"FAIL (P|REF) (\d+)", line)
        if m:
            fails[m.group(1)].append(int(m.group(2)))
        m = re.match(r"RST (\d+)", line)
        if m:
            resets.append(int(m.group(1)))
    period = 10
    mon = sorted(set(fails["P"]))
    ref = sorted(set(fails["REF"]))
    lagged = sum(1 for t in ref if t not in mon)
    ref_unmatched = [t for t in ref if not any(t - window * period <= m <= t for m in mon)]
    mon_unmatched = [t for t in mon if not any(t <= r <= t + window * period for r in ref)]
    # Verilator deviation 1: failure dropped because rst rose before its window end.
    excused = [t for t in mon_unmatched
               if disable and any(t < r <= t + window * period for r in resets)]
    mon_unmatched = [t for t in mon_unmatched if t not in excused]
    if not ref_unmatched and not mon_unmatched:
        return (f"same {len(mon)} fail cycles ({lagged} reported later by Verilator"
                + (f", {len(excused)} dropped by Verilator on a later rst" if excused else "") + ")")
    cyc = lambda ts: [(t - 5) // period for t in ts[:3]]
    return (f"DIFF: Verilator-only fails at cycles {cyc(ref_unmatched)}, "
            f"monitor-only fails at cycles {cyc(mon_unmatched)}")


def max_len(src_text, tmp):
    """Longest match length of the property (antecedent + consequent), from qfv lint."""
    path = os.path.join(tmp, "len.sv")
    write(path, src_text)
    r = subprocess.run([QFV, "lint", "--top", "t", path], capture_output=True, text=True)
    a = json.loads(r.stdout)["assertions"][0]
    norm = a.get("normalized", "")
    total = 0
    for m in re.finditer(r"##(\d+)|##\[(\d+):(\d+)\]", norm):
        total += int(m.group(1) or m.group(3))
    return total + 1


def ranged_antecedent(src_text, tmp):
    path = os.path.join(tmp, "ra.sv")
    write(path, src_text)
    r = subprocess.run([QFV, "lint", "--top", "t", path], capture_output=True, text=True)
    norm = json.loads(r.stdout)["assertions"][0].get("normalized", "")
    return "|->" in norm and "##[" in norm.split("|->")[0]


def check(prop, disable, bound, tmp):
    base = os.path.join(tmp, "p")
    orig_assert = HEADER + directive("assert", "P", prop, disable) + "endmodule\n"
    orig_assume = HEADER + directive("assume", "P", prop, disable) + "endmodule\n"
    write(base + "_assert.sv", orig_assert)
    write(base + "_assume.sv", orig_assume)
    mon_assert = compile_monitor(base + "_assert.sv", os.path.join(tmp, "ma"))
    mon_assume = compile_monitor(base + "_assume.sv", os.path.join(tmp, "mu"))

    ref_assume = directive("assume", "REF", prop, disable)
    ref_assert = directive("assert", "REF", prop, disable)
    dir_a = mon_assert.replace("endmodule", ref_assume + "endmodule")
    dir_b = mon_assume.replace("endmodule", ref_assert + "endmodule")
    write(base + "_A.sv", dir_a)
    write(base + "_B.sv", dir_b)
    write(base + "_mon.sv", mon_assert)

    out = {}
    ok = True
    if not disable:
        out["A"] = ebmc(base + "_A.sv", bound).get("P", "missing")
        out["B"] = ebmc(base + "_B.sv", bound).get("REF", "missing")
        out["orig"] = ebmc(base + "_assert.sv", bound).get("P", "missing")
        out["mon"] = ebmc(base + "_mon.sv", bound).get("P", "missing")
        ok = out["A"] == "ok" and out["B"] == "ok" and out["orig"] == out["mon"]
    if ranged_antecedent(orig_assert, tmp):
        # Verilator deviation 2: misses failures of ranged antecedents.
        out["sim"] = "n/a (Verilator mis-evaluates ranged antecedents)"
    else:
        out["sim"] = verilator_diff(mon_assert, prop, disable, tmp,
                                    window=max_len(orig_assert, tmp))
    sim_checked = not out["sim"].startswith("n/a")
    if sim_checked:
        ok = ok and out["sim"].startswith("same")
    oracles = (0 if disable else 1) + (1 if sim_checked else 0)
    return ("PASS" if oracles else "UNVERIFIED") if ok else "FAIL", out, oracles


# (property, disable iff rst?, {signal: [cycles where it is 1]}, expected fail cycles, why)
DIRECTED = [
    ("a |-> ##[1:3] b", True, {"a": [1], "rst": [2]}, [],
     "rst mid-attempt disables it (EBMC gets this wrong)"),
    ("a |-> ##[1:3] b", True, {"a": [1], "rst": [4]}, [],
     "rst in the last window cycle still disables it"),
    ("a |-> ##[1:3] b", True, {"a": [1]}, [4], "no b in cycles 2..4: fails at 4"),
    ("a ##[1:2] b |=> c ##1 d", False, {"a": [7], "b": [8]}, [9],
     "antecedent matches at 8, c missing at 9 (Verilator misses this)"),
    ("a |-> b ##[1:2] c ##1 d", False, {"a": [25], "b": [25], "c": [26]}, [27],
     "both threads dead at 27: earliest failure, not window end (Verilator: 28)"),
    ("a |-> b ##[1:2] c ##1 d", True, {"a": [25], "b": [25], "c": [26], "rst": [28]}, [27],
     "already failed at 27, so a later rst does not disable it (Verilator drops it)"),
    ("a |-> ##[1:3] b ##[1:3] c", True, {"a": [0], "b": [2], "rst": [6]}, [5],
     "only thread b@2 survives; c missing in 3..5, so it fails at 5; rst at 6 is too late"),
    ("a |-> ##[1:3] b ##[1:3] c", True, {"a": [0], "b": [2], "rst": [4]}, [],
     "rst at 4 comes before the attempt completes, so it is disabled"),
    ("a |-> ##[1:3] b ##[1:3] c", True, {"a": [0], "b": [2], "c": [5]}, [],
     "c at 5 matches the last possible cycle"),
    ("a |-> ##[0:2] b ##[1:2] c", True, {"a": [0], "b": [0]}, [2],
     "b@0 opens c in 1..2; b@1 and b@2 are missing, so everything dies at 2"),
    ("a ##[1:2] b |=> c ##1 d", True, {"a": [3], "b": [4, 5], "c": [5, 6], "d": [6]}, [7],
     "two antecedent matches (4 and 5); the one from 5 needs d at 7"),
    ("a |-> ##[1:2] b", False, {"a": [0, 1], "b": [2]}, [],
     "one b at 2 satisfies both overlapping attempts"),
    ("a |-> ##[1:2] b", False, {"a": [0, 1], "b": [3]}, [2],
     "attempt 0 fails at 2 even though attempt 1 is still alive (per-attempt tracking)"),
]


def run_directed(tmp):
    failures = 0
    for prop, disable, ones, expected, why in DIRECTED:
        src = os.path.join(tmp, "dir.sv")
        write(src, HEADER + directive("assert", "P", prop, disable) + "endmodule\n")
        mon = compile_monitor(src, os.path.join(tmp, "dm")).replace(
            "!qfv_P_fail);", '!qfv_P_fail) else $display("FAIL %0d", cyc);')
        mon = mon.replace("module t(", "module t(input integer cyc, ")
        ncyc = max([c for v in ones.values() for c in v] + expected + [0]) + 4
        lines = []
        for sig in ("rst", "a", "b", "c", "d"):
            cond = " || ".join(f"cyc == {c}" for c in ones.get(sig, [])) or "0"
            lines.append(f"      {sig} = ({cond});")
        tb = ("module tb;\n  reg clk = 0, rst = 0, a = 0, b = 0, c = 0, d = 0;\n"
              "  reg [2:0] x = 0;\n  integer cyc;\n"
              "  t dut(.cyc(cyc), .clk(clk), .rst(rst), .a(a), .b(b), .c(c), .d(d), .x(x));\n"
              f"  initial begin\n    for (cyc = 0; cyc < {ncyc}; cyc = cyc + 1) begin\n"
              + "\n".join(lines) +
              "\n      #5 clk = 1; #5 clk = 0;\n    end\n    $display(\"SIM_END\");\n"
              "    $finish;\n  end\nendmodule\n")
        write(os.path.join(tmp, "dtb.sv"), tb)
        write(os.path.join(tmp, "dt.sv"), mon)
        obj = os.path.join(tmp, "dobj")
        subprocess.run(["rm", "-rf", obj])
        r = subprocess.run([VERILATOR, "--binary", "--timing", "--assert", "-Wno-fatal",
                            "-Wno-lint", "-Wno-style", "--top-module", "tb", "-Mdir", obj,
                            "-o", "sim", os.path.join(tmp, "dtb.sv"),
                            os.path.join(tmp, "dt.sv")], capture_output=True, text=True)
        if r.returncode:
            got = "build error: " + r.stderr[-300:]
        else:
            out = subprocess.run([os.path.join(obj, "sim")], capture_output=True, text=True).stdout
            got = sorted({int(m) for m in re.findall(r"FAIL (\d+)", out)})
        ok = got == expected
        failures += not ok
        name = ("disable iff (rst) " if disable else "") + prop
        print(f"{'PASS' if ok else 'FAIL':10s} golden: {name:42s} fails={got} expected={expected}  # {why}")
    return failures


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bound", type=int, default=10)
    ap.add_argument("--filter", default="")
    args = ap.parse_args()

    failures = 0
    unverified = 0
    two_oracles = 0
    total = 0
    with tempfile.TemporaryDirectory() as tmp:
        for prop in read_list("props.txt"):
            if args.filter not in prop:
                continue
            for disable in (False, True):
                total += 1
                name = ("disable iff (rst) " if disable else "") + prop
                try:
                    status, out, oracles = check(prop, disable, args.bound, tmp)
                except RuntimeError as e:
                    status, out, oracles = "FAIL", {"error": str(e)[:600]}, 0
                failures += status == "FAIL"
                unverified += status == "UNVERIFIED"
                two_oracles += status == "PASS" and oracles == 2
                print(f"{status:10s} {name:55s} {out}")

        if not args.filter:
            n = run_directed(tmp)
            failures += n
            total += len(DIRECTED)

        for prop in read_list("unsupported.txt"):
            if args.filter not in prop:
                continue
            total += 1
            src = os.path.join(tmp, "u.sv")
            write(src, HEADER + directive("assert", "U", prop, False) + "endmodule\n")
            r = subprocess.run([QFV, "lint", "--top", "t", src], capture_output=True, text=True)
            res = json.loads(r.stdout)
            sites = res["assertions"]
            errors = [d for d in res["diagnostics"] if d["severity"] == "error"]
            rejected = bool(errors) or (sites and sites[0]["status"] == "unsupported")
            reason = errors[0]["message"] if errors else (sites[0].get("reason") if sites else "?")
            failures += not rejected
            print(f"{'PASS' if rejected else 'FAIL':10s} reject: {prop:47s} {reason}")

    print(f"\n{total - failures - unverified}/{total} passed "
          f"({two_oracles} confirmed by both EBMC and Verilator), "
          f"{unverified} unverified (no oracle supports them), {failures} failed; bound {args.bound}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
