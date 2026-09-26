#!/usr/bin/env python3
"""Checks the bit-blaster operator by operator against btorsim (the Btor2Tools
reference simulator).

For each BTOR2 operator and several widths, a one-frame model computes
op(a, b) and exposes every result bit as a `bad` property. Random and corner
case operands are applied as a witness. `qfv sim` (our AIG) and `btorsim` must
report exactly the same set of true result bits.

Usage: tests/bitblast/run_ops.py [--vectors N]
"""
import argparse
import os
import random
import re
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
QFV = os.path.join(ROOT, "build", "qfv")
BTORSIM = os.path.join(ROOT, "tools", "oss-cad-suite", "bin", "btorsim")

BINARY = ["add", "sub", "mul", "udiv", "urem", "sdiv", "srem", "smod", "sll", "srl", "sra",
          "rol", "ror", "and", "or", "xor", "nand", "nor", "xnor"]
COMPARE = ["eq", "neq", "ult", "ulte", "ugt", "ugte", "slt", "slte", "sgt", "sgte"]
UNARY = ["not", "neg", "inc", "dec"]
REDUCE = ["redand", "redor", "redxor"]
BOOL = ["implies", "iff"]
WIDTHS = [1, 2, 3, 5, 8, 13]


def model(op, w):
    """Returns (BTOR2 text, result width, number of inputs)."""
    lines = [f"1 sort bitvec {w}", "2 sort bitvec 1"]
    rw = w
    if op in BINARY:
        lines += ["3 input 1 a", "4 input 1 b", f"10 {op} 1 3 4"]
    elif op in COMPARE:
        lines += ["3 input 1 a", "4 input 1 b", f"10 {op} 2 3 4"]
        rw = 1
    elif op in UNARY:
        lines += ["3 input 1 a", "4 input 1 b", f"10 {op} 1 3"]
    elif op in REDUCE:
        lines += ["3 input 1 a", "4 input 1 b", f"10 {op} 2 3"]
        rw = 1
    elif op in BOOL:
        lines = ["2 sort bitvec 1", "3 input 2 a", "4 input 2 b", f"10 {op} 2 3 4"]
        rw = 1
        w = 1
    elif op == "concat":
        lines += ["3 input 1 a", "4 input 1 b", f"5 sort bitvec {2 * w}", "10 concat 5 3 4"]
        rw = 2 * w
    elif op in ("uext", "sext"):
        lines += ["3 input 1 a", "4 input 1 b", f"5 sort bitvec {w + 3}", f"10 {op} 5 3 3"]
        rw = w + 3
    elif op == "slice":
        hi, lo = w - 1, w // 2
        lines += ["3 input 1 a", "4 input 1 b", f"5 sort bitvec {hi - lo + 1}", f"10 slice 5 3 {hi} {lo}"]
        rw = hi - lo + 1
    elif op == "ite":
        lines += ["3 input 1 a", "4 input 1 b", "6 input 2 c", "10 ite 1 6 3 4"]
    elif op == "negarg":  # negative argument ids mean bitwise NOT
        lines += ["3 input 1 a", "4 input 1 b", "10 add 1 -3 4"]
    n = 100
    for i in range(rw):
        lines += [f"{n} sort bitvec 1" if i == 0 else "",
                  f"{n + 1 + 2 * i} slice 100 10 {i} {i}", f"{n + 2 + 2 * i} bad {n + 1 + 2 * i}"]
    return "\n".join(l for l in lines if l) + "\n", rw, w


def const_models():
    """constd/consth/const parsing, including negative decimals."""
    cases = [("constd", 8, "-1"), ("constd", 8, "-128"), ("constd", 13, "4095"), ("constd", 5, "-7"),
             ("consth", 12, "abc"), ("consth", 7, "7f"), ("const", 6, "101100"), ("constd", 70, "-2")]
    for tag, w, value in cases:
        lines = [f"1 sort bitvec {w}", "2 sort bitvec 1", "3 input 2 dummy", f"10 {tag} 1 {value}"]
        for i in range(w):
            lines += [f"{101 + 2 * i} slice 2 10 {i} {i}", f"{102 + 2 * i} bad {101 + 2 * i}"]
        yield f"{tag} {value} (w={w})", "\n".join(lines) + "\n"


def witness(values):
    return "sat\nb0\n#0\n@0\n" + "".join(f"{i} {v}\n" for i, v in enumerate(values)) + ".\n"


def reached(cmd):
    out = subprocess.run(cmd, capture_output=True, text=True)
    text = out.stdout + out.stderr
    if "no bad state property reached" in text:
        return []
    m = re.search(r"reached bad state properties \{([^}]*)\}", text)
    if not m:
        return None
    return sorted(x for x in m.group(1).split() if x.endswith("@0"))


def run(tmp, name, btor, inputs_widths, vectors, rng):
    path = os.path.join(tmp, "m.btor")
    open(path, "w").write(btor)
    mism = 0
    for _ in range(vectors):
        vals = []
        for w in inputs_widths:
            choice = rng.random()
            if choice < 0.15:
                v = 0
            elif choice < 0.3:
                v = (1 << w) - 1
            elif choice < 0.4:
                v = 1 << (w - 1)
            else:
                v = rng.getrandbits(w)
            vals.append(format(v, f"0{w}b"))
        wit = os.path.join(tmp, "w.wit")
        open(wit, "w").write(witness(vals))
        ref = reached([BTORSIM, "-v", "-c", path, wit])
        ours = reached([QFV, "sim", "--btor", path, "--witness", wit])
        if ref is None or ours is None or ref != ours:
            mism += 1
            if mism == 1:
                print(f"  MISMATCH {name} inputs={vals}: btorsim={ref} qfv={ours}")
    return mism


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vectors", type=int, default=25)
    args = ap.parse_args()
    rng = random.Random(7)
    failures, total = 0, 0
    with tempfile.TemporaryDirectory() as tmp:
        ops = BINARY + COMPARE + UNARY + REDUCE + BOOL + ["concat", "uext", "sext", "slice", "ite", "negarg"]
        for op in ops:
            bad_widths = []
            for w in WIDTHS:
                if op == "slice" and w < 2:
                    continue
                btor, rw, iw = model(op, w)
                widths = [iw, iw] + ([1] if op == "ite" else [])
                total += 1
                if run(tmp, f"{op}/w{w}", btor, widths, args.vectors, rng):
                    failures += 1
                    bad_widths.append(w)
                if op in BOOL:
                    break
            print(f"{'PASS' if not bad_widths else 'FAIL'}  {op:8s} widths={WIDTHS if op not in BOOL else [1]}"
                  + (f"  failing widths={bad_widths}" if bad_widths else ""))
        for name, btor in const_models():
            total += 1
            bad = run(tmp, name, btor, [1], 1, rng)
            failures += bool(bad)
            print(f"{'PASS' if not bad else 'FAIL'}  {name}")
    print(f"\n{total - failures}/{total} operator/width combinations agree with btorsim "
          f"({args.vectors} vectors each)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
