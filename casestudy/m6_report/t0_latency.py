#!/usr/bin/env python3
"""Hot-path latency on real SVA: each FVEval NL2SVA-Human testbench is loaded
once, then each reference assertion is added (T0 lint + direct AIG emission)."""
import collections, csv, json, os, re, statistics, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tests")); from serve_client import Serve
FV = os.path.join(ROOT, "tools", "FVEval", "data_nl2sva")
rows = list(csv.DictReader(open(os.path.join(FV, "data", "nl2sva_human.csv"))))
by = collections.defaultdict(list)
for r in rows: by[r["design_name"]].append(r)
adds, loads, statuses = [], [], collections.Counter()
with tempfile.TemporaryDirectory() as tmp:
    for design, rs in by.items():
        tbfile = os.path.join(FV, "annotated_tb", design + ".sv")
        top = re.search(r"module\s+(\w+)", open(tbfile).read()).group(1)
        s = Serve(os.path.join(tmp, design))
        r, _, ms = s.call("load", files=[tbfile], top=top, clock="clk", reset="!reset_")
        if not r.get("ok"):
            print(design, "load failed:", json.dumps(r)[:300]); s.close(); continue
        loads.append(ms)
        for x in rs:
            sva = re.sub(r"^\s*asrt\s*:", x["task_id"] + ":", x["ref_solution"].strip())
            rr, _, ams = s.call("add", module=top, sva=sva)
            adds.append(ams)
            for a in rr.get("added", []): statuses[a["status"]] += 1
        s.close()
q = lambda v, p: sorted(v)[min(len(v) - 1, int(p * len(v)))]
print(f"designs loaded: {len(loads)}  load ms p50={statistics.median(loads):.0f} max={max(loads):.0f}")
print(f"assertions added: {len(adds)}  statuses={dict(statuses)}")
print(f"add (T0 + AIG emission) ms: p50={statistics.median(adds):.2f} p95={q(adds, .95):.2f} max={max(adds):.2f}")
json.dump({"load_ms": loads, "add_ms": adds, "statuses": statuses}, open(os.path.join(HERE, "t0_latency.json"), "w"))
