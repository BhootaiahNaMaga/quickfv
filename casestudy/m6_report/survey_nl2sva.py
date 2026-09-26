#!/usr/bin/env python3
"""Lint all FVEval NL2SVA-Human reference assertions with qfv: how much of
real, expert-written SVA falls in QuickFV's v1 subset?"""
import collections, csv, json, os, re, subprocess, sys, tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
QFV = os.path.join(ROOT, "build", "qfv")
FVEVAL = os.path.join(ROOT, "tools", "FVEval", "data_nl2sva")

rows = list(csv.DictReader(open(os.path.join(FVEVAL, "data", "nl2sva_human.csv"))))
by_status = collections.Counter()
reasons = collections.Counter()
per_design = collections.defaultdict(collections.Counter)
details = []
with tempfile.TemporaryDirectory() as tmp:
    for r in rows:
        tb = open(os.path.join(FVEVAL, "annotated_tb", r["design_name"] + ".sv")).read()
        top = re.search(r"module\s+(\w+)", tb).group(1)
        sva = r["ref_solution"].strip()
        # FVEval appends the assertion inside the testbench module (as its harness does).
        head, sep, tail = tb.rpartition("endmodule")
        src = os.path.join(tmp, r["design_name"] + ".sv")
        open(src, "w").write(head + "\n" + sva + "\n" + sep + tail)
        out = subprocess.run([QFV, "lint", "--top", top, src], capture_output=True, text=True).stdout
        res = json.loads(out)
        errs = [d["message"] for d in res["diagnostics"] if d["severity"] == "error"]
        a = res["assertions"][0] if res["assertions"] else None
        status = "error" if errs else (a["status"] if a else "no assertion found")
        reason = errs[0] if errs else (a.get("reason", "") if a else "")
        by_status[status] += 1
        per_design[r["design_name"]][status] += 1
        if status != "supported":
            key = re.sub(r"'[^']*'", "'…'", reason)[:90]
            reasons[key] += 1
        details.append({"task": r["task_id"], "design": r["design_name"], "status": status,
                        "reason": reason, "sva": " ".join(sva.split())})
json.dump(details, open(os.path.join(os.path.dirname(__file__), "survey_nl2sva.json"), "w"), indent=1)
print(f"{len(rows)} reference assertions: " + ", ".join(f"{k} {v}" for k, v in by_status.most_common()))
print("\nwhy not supported:")
for k, v in reasons.most_common():
    print(f"  {v:3d}  {k}")
print("\nper design (supported/total):")
for d, c in sorted(per_design.items()):
    print(f"  {d:26s} {c['supported']}/{sum(c.values())}")
