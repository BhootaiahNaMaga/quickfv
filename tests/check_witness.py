#!/usr/bin/env python3
"""Independently confirms a qfv CEX: btorsim (Btor2Tools) replays the BTOR2
witness on the model and must report reaching exactly bad property b<P> at
frame <K>, which is stricter than btorsim's exit code (that only checks the
property is reached at some frame).

Usage: check_witness.py model.btor witness.wit
Exit 0 = confirmed, 1 = not reproduced.
"""
import os
import re
import subprocess
import sys

BTORSIM = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "oss-cad-suite",
                       "bin", "btorsim")


def check(btor, wit):
    lines = open(wit).read().split("\n")
    prop = lines[1].strip()                       # e.g. "b2"
    depth = max(int(l[1:]) for l in lines if l.startswith("@"))
    r = subprocess.run([BTORSIM, "-v", "-c", btor, wit], capture_output=True, text=True)
    out = r.stdout + r.stderr
    m = re.search(r"reached bad state properties \{([^}]*)\}", out)
    reached = m.group(1).split() if m else []
    ok = f"{prop}@{depth}" in reached and "constraints always satisfied" in out
    return ok, f"claimed {prop}@{depth}, btorsim reached {reached or 'nothing'}"


if __name__ == "__main__":
    ok, msg = check(sys.argv[1], sys.argv[2])
    print(("CONFIRMED " if ok else "NOT REPRODUCED ") + msg)
    sys.exit(0 if ok else 1)
