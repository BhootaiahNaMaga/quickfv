#!/usr/bin/env python3
"""Builds <design>_props.sv: the FVEval reference model with its reference
assertions appended (labelled by task id), bound to our DUT."""
import csv, os, re
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FV = os.path.join(ROOT, "tools", "FVEval", "data_nl2sva")
OUT = os.path.join(os.path.dirname(__file__), "designs")
BINDS = {
    "arbiter_rr": ("arbiter_rr", "#(.NUM_OF_CLIENTS(NUM_OF_CLIENTS))"),
    "counter": ("counter", "#(.width(width), .min(min), .max(max))"),
}
rows = list(csv.DictReader(open(os.path.join(FV, "data", "nl2sva_human.csv"))))
for design, (dut, params) in BINDS.items():
    tb = open(os.path.join(FV, "annotated_tb", design + ".sv")).read()
    tbmod = re.search(r"module\s+(\w+)", tb).group(1)
    asserts = []
    for r in rows:
        if r["design_name"] != design:
            continue
        sva = re.sub(r"^\s*asrt\s*:", r["task_id"] + ":", r["ref_solution"].strip())
        asserts.append(f"// {r['task_id']}: {' '.join(r['prompt'].split())[:150]}\n{sva}")
    head, sep, tail = tb.rpartition("endmodule")
    text = (f"// FVEval NL2SVA-Human '{design}' reference model (Apache-2.0, NVIDIA) with its\n"
            f"// reference assertions, bound to our DUT '{dut}'.\n" + head +
            "\n// ---- FVEval reference assertions ----\n" + "\n".join(asserts) + "\n" + sep + tail +
            f"\n\nbind {dut} {tbmod} {params} tb_inst (.*);\n")
    open(os.path.join(OUT, design + "_props.sv"), "w").write(text)
    print(design, len(asserts), "assertions")
