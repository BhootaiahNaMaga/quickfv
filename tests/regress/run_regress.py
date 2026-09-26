"""Regression tests for the soundness and robustness findings of PROJECT_REVIEW.md.

Each case is a minimal reproduction of a wrong verdict or a crash; the suite
fails (non-zero exit) on any mismatch, missing result or server death.
Needs build/qfv and tools/oss-cad-suite/bin (yosys, btorsim).

    python3 tests/regress/run_regress.py [-k name-substring]
"""
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
QFV = os.environ.get("QFV_BIN") or os.path.join(ROOT, "build", "qfv")  # QFV_BIN: test another build
TOOLS = os.path.join(ROOT, "tools", "oss-cad-suite", "bin")


class Serve:
    def __init__(self, work, mode="serve", env=None, ric3=False):
        e = {**os.environ, "PATH": TOOLS + ":" + os.environ["PATH"], **({} if ric3 else {"QFV_NO_RIC3": "1"}),
             **(env or {})}
        self.p = subprocess.Popen([QFV, mode, "--work", work], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True, env=e)
        self.n = 0

    def raw(self, line):
        """Sends one raw line; returns the next non-event message (None if the server died)."""
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        while True:
            out = self.p.stdout.readline()
            if not out:
                return None
            msg = json.loads(out)
            if "event" not in msg:
                return msg

    def call(self, method, **params):
        self.n += 1
        msg = self.raw(json.dumps({"id": self.n, "method": method, "params": params}))
        if msg is None:
            raise AssertionError(f"server died during {method} (exit {self.p.wait()})")
        return msg["result"]

    def close(self):
        self.p.stdin.close()
        return self.p.wait()


def sv(tmp, name, text):
    path = os.path.join(tmp, name)
    with open(path, "w") as f:
        f.write(text)
    return path


def expect(cond, what):
    if not cond:
        raise AssertionError(what)


FAST = {"budget_s": 2, "sim_s": 0}


# --- finding 1: vacuity pre-pass must not skip a SAT base case -------------------
def t_initial_state_failure(tmp):
    f = sv(tmp, "t.sv", """
module t(input clk);
  reg q = 1'b1;
  always @(posedge clk) q <= 0;
  P: assert property (@(posedge clk) q |-> 1'b0);
endmodule
""")
    s = Serve(tmp)
    expect(s.call("load", files=[f], top="t")["ok"], "load failed")
    r = s.call("check_all", budget_s=0.2, sim_s=0)
    expect(r["verdicts"] == {"P": "CEX"}, f"want P:CEX, got {r['verdicts']}")
    s.close()


def t_initial_only_cover(tmp):
    f = sv(tmp, "t.sv", """
module t(input clk);
  reg q = 1'b1;
  always @(posedge clk) q <= 0;
  C: cover property (@(posedge clk) q);
endmodule
""")
    s = Serve(tmp)
    expect(s.call("load", files=[f], top="t")["ok"], "load failed")
    r = s.call("check_all", budget_s=0.2, sim_s=0)
    expect(r["verdicts"] == {"C": "COVERED"}, f"want C:COVERED, got {r['verdicts']}")
    s.close()


# --- finding 2: every instance of a module assertion is checked --------------------
def t_repeated_instances(tmp):
    f = sv(tmp, "t.sv", """
module child(input clk, input a);
  P: assert property (@(posedge clk) a);
endmodule
module t(input clk);
  child good(clk, 1'b1);
  child bad (clk, 1'b0);
endmodule
""")
    s = Serve(tmp)
    ld = s.call("load", files=[f], top="t")
    expect(ld["ok"], f"load failed: {ld}")
    ids = sorted(a["id"] for a in ld["assertions"])
    expect(len(ids) == 2, f"want 2 instance assertions, got {ids}")
    r = s.call("check_all", **FAST)["verdicts"]
    expect(sorted(r.values()) == ["CEX", "PASS_BOUNDED"], f"want one CEX + one PASS_BOUNDED, got {r}")
    bad = [k for k, v in r.items() if v == "CEX"]
    expect(bad and "bad" in bad[0], f"the failing instance should be 'bad', got {r}")
    s.close()


def t_repeated_instances_oneshot(tmp):
    # One-shot `qfv bmc`: one verdict per instance, whichever instance fails.
    for a, b in [(1, 0), (0, 1)]:
        f = sv(tmp, "t.sv", f"""
module child(input clk, input a);
  P: assert property (@(posedge clk) a);
endmodule
module t(input clk);
  child u1(clk, 1'b{a});
  child u2(clk, 1'b{b});
endmodule
""")
        r = oneshot(tmp, f)
        want = {"u1.P": "PASS_BOUNDED" if a else "CEX", "u2.P": "PASS_BOUNDED" if b else "CEX"}
        expect(r["summary"] == want and r["exit"] == 1, f"want {want} (exit 1), got {r['summary']} (exit {r['exit']})")


def oneshot(tmp, f, *extra):
    """Runs `qfv bmc` on one file; returns the summary verdicts and the exit code."""
    work = tempfile.mkdtemp(dir=tmp)
    p = subprocess.run([QFV, "bmc", "--top", "t", "--max-depth", "5", "--budget", "3", "--no-ric3",
                        "--work", work, *extra, f],
                       capture_output=True, text=True, env={**os.environ, "PATH": TOOLS + ":" + os.environ["PATH"]})
    summary = [json.loads(l) for l in p.stdout.splitlines() if '"summary"' in l]
    expect(summary, f"no summary from qfv bmc (exit {p.returncode}):\n{p.stdout[-2000:]}{p.stderr[-2000:]}")
    return {"summary": summary[-1]["verdicts"], "exit": p.returncode, "stdout": p.stdout}


# --- finding 3: out-of-range selects are unconstrained, as in the Yosys flow -------
def t_out_of_range_select(tmp):
    f = sv(tmp, "t.sv", """
module t(input clk, input [1:0] v, input [1:0] idx);
endmodule
""")
    s = Serve(tmp)
    expect(s.call("load", files=[f], top="t")["ok"], "load failed")
    r = s.call("check_assertion", sva="idx == 2 |-> v[idx] == 0", name="P", **FAST)
    expect(r["check"]["verdicts"] == {"P": "CEX"}, f"want P:CEX, got {r.get('check', r)}")
    # The one-shot flow must agree.
    prop = sv(tmp, "p.sv", """
module t(input clk, input [1:0] v, input [1:0] idx);
  P: assert property (@(posedge clk) idx == 2 |-> v[idx] == 0);
endmodule
""")
    one = oneshot(tmp, prop)
    expect(one["summary"] == {"P": "CEX"}, f"one-shot bmc should find a CEX: {one['summary']}")
    s.close()


def t_in_range_select(tmp):
    # Control for finding 3: an in-range dynamic select must still be exact.
    f = sv(tmp, "t.sv", """
module t(input clk, input [3:0] v, input [1:0] idx);
endmodule
""")
    s = Serve(tmp)
    expect(s.call("load", files=[f], top="t")["ok"], "load failed")
    r = s.call("check_assertion", sva="v == 4'b0100 && idx == 2 |-> v[idx] == 1", name="P", **FAST)
    expect(r["check"]["verdicts"] == {"P": "PASS_BOUNDED"}, f"want PASS_BOUNDED, got {r.get('check', r)}")
    s.close()


def t_narrow_selector(tmp):
    # A selector narrower than the range: index 4 is not 0 (no aliasing).
    f = sv(tmp, "t.sv", """
module t(input clk, input [7:0] v, input [1:0] idx, input [1:0] p, input [2:0] wa, output [7:0] o);
  logic [7:0] m [0:7];
  always_ff @(posedge clk) m[wa] <= v;
  assign o = m[wa];
endmodule
""")
    s = Serve(tmp)
    expect(s.call("load", files=[f], top="t")["ok"], "load failed")
    r = s.call("check_assertion", sva="v == 8'h01 && idx == 0 |-> v[idx] == 1", name="P", **FAST)
    expect(r["check"]["verdicts"] == {"P": "PASS_BOUNDED"}, f"packed: want PASS_BOUNDED, got {r.get('check', r)}")
    r = s.call("check_assertion", sva="p == 0 |-> m[p] == m[0]", name="Q", **FAST)
    expect(r["check"]["verdicts"] == {"Q": "PASS_BOUNDED"}, f"array: want PASS_BOUNDED, got {r.get('check', r)}")
    r = s.call("check_assertion", sva="v[3:0] == 4'h5 |-> v[p+2] == v[p+2]", name="R", **FAST)
    expect(r["check"]["verdicts"] == {"R": "PASS_BOUNDED"}, f"want PASS_BOUNDED, got {r.get('check', r)}")
    s.close()


# --- finding 4: a failed reload leaves no usable half-state --------------------------
def t_failed_reload(tmp):
    # Load is transactional: after a failed reload the previous design, its
    # sources and its circuit, are still loaded together, unchanged.
    good = sv(tmp, "good.sv", "module t(input clk, output a); assign a = 1'b1; endmodule\n")
    for bad_text in ["module t(input clk, output a, output b); assign a = 1'b0; assign b = 0; wire w = ; endmodule\n",
                     "module t(input clk, output a, output b); assign a = 1'b0; assign b = nope; endmodule\n"]:
        bad = sv(tmp, "bad.sv", bad_text)
        s = Serve(tmp)
        expect(s.call("load", files=[good], top="t")["ok"], "first load failed")
        r = s.call("load", files=[bad], top="t")
        expect(not r["ok"], "broken reload should fail")
        expect(r.get("previous_design") == "still loaded (unchanged)", f"reload should report the old design: {r}")
        r = s.call("check_assertion", sva="a", name="P", **FAST)
        expect(r["check"]["verdicts"] == {"P": "PASS_BOUNDED"}, f"old design (a=1) should hold: {r}")
        r = s.call("check_assertion", sva="b", name="Q", **FAST)
        expect("check" not in r and not r["lint"]["ok"], f"'b' exists only in the rejected sources: {r}")
        s.close()


# --- finding 5: an unsupported assumption blocks the load ------------------------------
def t_unsupported_assumption(tmp):
    f = sv(tmp, "t.sv", """
module t(input clk, input a);
  A: assume property (@(posedge clk) a[*2]);
  P: assert property (@(posedge clk) a);
endmodule
""")
    s = Serve(tmp)
    ld = s.call("load", files=[f], top="t")
    expect(ld["ok"] is False, f"load with an unsupported assumption must fail, got ok={ld.get('ok')}")
    expect("A" in json.dumps(ld.get("error", "")), f"error should name the assumption: {ld.get('error')}")
    expect(s.call("check_all", **FAST).get("ok") is False, "no checks on a refused environment")
    s.close()


def t_unsupported_assumption_oneshot(tmp):
    f = sv(tmp, "t.sv", """
module t(input clk, input a);
  A: assume property (@(posedge clk) a[*2]);
  P: assert property (@(posedge clk) a);
endmodule
""")
    work = tempfile.mkdtemp(dir=tmp)
    p = subprocess.run([QFV, "bmc", "--top", "t", "--budget", "2", "--no-ric3", "--work", work, f],
                       capture_output=True, text=True, env={**os.environ, "PATH": TOOLS + ":" + os.environ["PATH"]})
    expect(p.returncode == 2 and '"stage":"environment"' in p.stdout and '"summary"' not in p.stdout,
           f"qfv bmc must refuse an unsupported assumption (exit {p.returncode}):\n{p.stdout[-1500:]}")


def t_unsupported_session_assumption(tmp):
    f = sv(tmp, "t.sv", "module t(input clk, input a); endmodule\n")
    s = Serve(tmp)
    expect(s.call("load", files=[f], top="t")["ok"], "load failed")
    r = s.call("add_assumption", sva="a[*2]", name="A")
    expect(r["ok"] is False, f"adding an unsupported assumption should fail: {r}")
    # It is not part of the environment: nothing is left behind silently.
    expect(all(a["id"] != "A" for a in s.call("list")["assertions"]), "unsupported assumption kept in the session")
    s.close()


# --- finding 6: an unconfirmed trace is not a CEX verdict -------------------------------
def t_uncertified_cex(tmp):
    f = sv(tmp, "t.sv", "module t(input clk, input a); endmodule\n")
    s = Serve(tmp, env={"QFV_BTORSIM": "/nonexistent/review-btorsim"})
    expect(s.call("load", files=[f], top="t")["ok"], "load failed")
    r = s.call("check_assertion", sva="a", name="P", **FAST)
    v = r["check"]["verdicts"]["P"]
    expect(v != "CEX", f"an unreplayed trace must not be reported as CEX, got {v}")
    expect(v == "CEX_UNCONFIRMED", f"want CEX_UNCONFIRMED, got {v}")
    s.close()


def t_certified_cex(tmp):
    # Control for finding 6: with btorsim available the CEX is confirmed.
    f = sv(tmp, "t.sv", "module t(input clk, input a); endmodule\n")
    s = Serve(tmp)
    expect(s.call("load", files=[f], top="t")["ok"], "load failed")
    r = s.call("check_assertion", sva="a", name="P", **FAST)
    expect(r["check"]["verdicts"] == {"P": "CEX"}, f"want P:CEX, got {r['check']['verdicts']}")
    s.close()


# --- finding 7: an uncertified external proof settles nothing ----------------------------
def t_uncertified_external_proof(tmp):
    # A stub rIC3 claims every goal unreachable, without a checkable certificate.
    stub = sv(tmp, "fake_ric3", "#!/bin/sh\necho UNSAT\n")
    os.chmod(stub, 0o755)
    f = sv(tmp, "t.sv", """
module t(input clk);
  reg [15:0] c = 0;
  always @(posedge clk) c <= c + 1;
endmodule
""")
    s = Serve(tmp, ric3=True, env={"QFV_RIC3": stub})
    expect(s.call("load", files=[f], top="t")["ok"], "load failed")
    # The trigger c == 60000 is reachable (far beyond BMC in 2 s).
    r = s.call("check_assertion", sva="c == 60000 |-> c != 0", name="P", **FAST)
    v = r["check"]["verdicts"]["P"]
    expect(v in ("POSSIBLY_VACUOUS", "PASS_BOUNDED"), f"uncertified 'unreachable' must not settle P: got {v}")
    s.close()


# --- finding 9: paths are arguments, never shell or Yosys syntax ------------------------
ODD_DIR = "dir with space;$HOME 'q'"


def t_odd_paths_session(tmp):
    d = os.path.join(tmp, ODD_DIR)
    os.makedirs(os.path.join(d, "inc dir"))
    sv(os.path.join(d, "inc dir"), "defs.svh", "`define ONE 1'b1\n")
    f = sv(d, "t 1.sv", '`include "defs.svh"\nmodule t(input clk, input a); wire one = `ONE; endmodule\n')
    s = Serve(os.path.join(d, "work dir"))
    ld = s.call("load", setup_file=sv(d, "setup.tcl", f"analyze -sv12 {{+incdir+{d}/inc dir}} {{{f}}}\n"
                                                          "elaborate -top t\nclock clk\n"))
    expect(ld["ok"], f"load with odd paths failed: {json.dumps(ld)[:800]}")
    r = s.call("check_assertion", sva="a", name="P", **FAST)
    expect(r["check"]["verdicts"] == {"P": "CEX"}, f"want P:CEX, got {r.get('check', r)}")
    tcl = s.call("export_jg")["tcl"]
    expect("{" + f + "}" in tcl, f"export must brace-quote the path:\n{tcl}")
    s.close()


def t_odd_paths_oneshot(tmp):
    d = os.path.join(tmp, ODD_DIR)
    os.makedirs(d)
    f = sv(d, "t 1.sv", "module t(input clk, input a);\n  P: assert property (@(posedge clk) a);\nendmodule\n")
    replay = os.path.exists(os.path.join(TOOLS, "verilator"))
    r = oneshot(d, f, *(["--replay"] if replay else []))  # the work dir is under d, too
    expect(r["summary"] == {"P": "CEX"}, f"want P:CEX, got {r['summary']}")
    if replay:
        expect('"rtl_replay":"confirmed"' in r["stdout"], f"replay with odd paths failed:\n{r['stdout'][-1500:]}")


def t_bad_top_name(tmp):
    f = sv(tmp, "t.sv", "module t(input clk); endmodule\n")
    s = Serve(tmp)
    r = s.call("load", files=[f], top="t; shell", clock="clk")
    expect(r["ok"] is False and "identifier" in r.get("error", ""), f"a non-identifier top must be refused: {r}")
    s.close()


# --- handoff: unique names, stale results stay open, environment differences are flagged
def t_export_handoff(tmp):
    f = sv(tmp, "t.sv", """
module child(input clk, input a);
endmodule
module t(input clk, input a, input b);
  child u1(clk, a);
  child u2(clk, b);
  A: assume property (@(posedge clk) b);
endmodule
""")
    s = Serve(tmp)
    expect(s.call("load", files=[f], top="t")["ok"], "load failed")
    r = s.call("check_assertion", module="child", sva="a", name="Q", **FAST)
    expect(r["check"]["verdicts"] == {"u1.Q": "CEX", "u2.Q": "PASS_BOUNDED"}, f"per instance: {r.get('check', r)}")
    s.call("remove", id="A")
    tcl = s.call("export_jg")["tcl"]
    expect("-name u1_Q " in tcl and "-name u2_Q " in tcl, f"instance properties need unique names:\n{tcl}")
    expect("WARNING: the environments differ" in tcl and "#   A" in tcl, f"removed source assumption not flagged:\n{tcl}")
    expect("*u1_Q" in tcl.split("prove -property")[1], f"stale CEX must stay open:\n{tcl}")
    s.close()


# --- finding 10: malformed envelopes do not kill the server ------------------------------
def t_malformed_serve(tmp):
    s = Serve(tmp)
    for line in ["[]", "7", '{"id":1,"method":7}', '{"id":1,"method":"load","params":[]}',
                 '{"id":1,"method":"check_all","params":{"budget_s":"x"}}']:
        msg = s.raw(line)
        expect(msg is not None, f"serve died on {line!r}")
    expect(s.call("list").get("ok") is False, "server unusable after malformed input")
    expect(s.close() == 0, "serve exited abnormally")


def t_malformed_mcp(tmp):
    s = Serve(tmp, mode="mcp")
    for line in ['{"jsonrpc":"2.0","id":1,"method":7}', "[]", '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":7}',
                 '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"check_all","arguments":[]}}']:
        msg = s.raw(line)
        expect(msg is not None, f"mcp died on {line!r}")
    msg = s.raw('{"jsonrpc":"2.0","id":9,"method":"ping"}')
    expect(msg is not None and msg.get("id") == 9, f"mcp unusable after malformed input: {msg}")
    expect(s.close() == 0, "mcp exited abnormally")


TESTS = [v for k, v in sorted(globals().items()) if k.startswith("t_")]


def main():
    sel = sys.argv[2] if len(sys.argv) > 2 and sys.argv[1] == "-k" else ""
    if not os.path.exists(QFV):
        sys.exit(f"missing {QFV}: build first")
    missing = [t for t in ("yosys", "btorsim") if not os.path.exists(os.path.join(TOOLS, t))]
    if missing:  # 77: skipped (CTest SKIP_RETURN_CODE), not passed
        print(f"SKIP: {', '.join(missing)} not in {TOOLS} (run scripts/setup_tools.sh)")
        sys.exit(77)
    failed = 0
    for t in TESTS:
        if sel not in t.__name__:
            continue
        with tempfile.TemporaryDirectory(prefix="qfv_regress_") as tmp:
            try:
                t(tmp)
                print(f"PASS {t.__name__}")
            except Exception as e:
                failed += 1
                print(f"FAIL {t.__name__}: {e}")
    print(f"{'FAILED' if failed else 'OK'}: {failed} failure(s)")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
