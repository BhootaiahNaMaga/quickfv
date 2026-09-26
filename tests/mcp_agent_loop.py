#!/usr/bin/env python3
"""M4 exit test: an agent's case-study loop, only through MCP (`qfv mcp`).

Plays the part of an agent working on the M0 FIFO (DATA bug): load the design,
write assertions (one with a typo), read a counterexample, add an environment
assumption, re-check, and export the JasperGold handoff. Checks the answers
and that Yosys runs exactly once (at load): editing assertions never
re-synthesizes the design.

Usage: tests/mcp_agent_loop.py
"""
import json
import os
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
WORK = os.path.join(ROOT, "work", "m4", "mcp")


class Mcp:
    def __init__(self):
        env = {**os.environ, "PATH": os.path.join(ROOT, "tools", "oss-cad-suite", "bin") + ":" + os.environ["PATH"]}
        self.p = subprocess.Popen([os.path.join(ROOT, "build", "qfv"), "mcp", "--work", WORK],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=env)
        self.n = 0

    def rpc(self, method, params=None, notify=False):
        msg = {"jsonrpc": "2.0", "method": method, "params": params or {}}
        if not notify:
            self.n += 1
            msg["id"] = self.n
        self.p.stdin.write(json.dumps(msg) + "\n")
        self.p.stdin.flush()
        if notify:
            return None
        return json.loads(self.p.stdout.readline())

    def tool(self, tool_name, **args):
        t0 = time.time()
        r = self.rpc("tools/call", {"name": tool_name, "arguments": args})["result"]
        return json.loads(r["content"][0]["text"]), r["isError"], (time.time() - t0) * 1000


checks = []


def expect(cond, what):
    checks.append((bool(cond), what))
    print(f"  {'ok ' if cond else 'FAIL'} {what}")


def main():
    m = Mcp()
    init = m.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                "clientInfo": {"name": "loop-test", "version": "1"}})
    m.rpc("notifications/initialized", notify=True)
    tools = [t["name"] for t in m.rpc("tools/list")["result"]["tools"]]
    print("server:", init["result"]["serverInfo"], "tools:", tools)

    print("\n1. load the design once")
    r, err, ms = m.tool("load_design", setup_file=os.path.join(ROOT, "casestudy", "m4_session", "fifo_setup.tcl"))
    expect(r["ok"] and not err, f"loaded in {ms:.0f} ms (Yosys {r['yosys_ms']:.0f} ms)")
    yosys_log = os.path.join(WORK, "yosys.log")
    yosys_stamp = os.path.getmtime(yosys_log)

    print("\n2. agent writes an assertion with a typo -> T0 error in ms")
    r, err, ms = m.tool("check_assertion", module="fifo_1r1w_tb", name="a_push_nonempty",
                        sva="wr_push && !rd_pop |=> !fifo_emtpy", disable_iff="tb_reset", budget_s=5)
    msgs = [d["message"] for d in r["lint"]["diagnostics"]]
    expect(any("fifo_empty" in x for x in msgs), f"typo reported ({ms:.0f} ms): {msgs[0] if msgs else '?'}")

    print("\n3. agent fixes it -> not vacuous, no CEX in budget")
    r, err, ms = m.tool("check_assertion", module="fifo_1r1w_tb", name="a_push_nonempty_v2",
                        sva="wr_push && !rd_pop |=> !fifo_empty", disable_iff="tb_reset", budget_s=5)
    v = r["check"]["verdicts"].get("a_push_nonempty_v2")
    expect(v == "PASS_BOUNDED", f"verdict {v} ({ms:.0f} ms incl. 5 s budget)")
    trig = [e for e in r["results"] if e["goal"] == "trigger-reachable"]
    expect(trig and trig[0]["status"] == "REACHABLE", "trigger reachable (assertion is not vacuous)")

    print("\n4. agent writes a vacuous assertion -> VACUOUS")
    r, err, ms = m.tool("check_assertion", module="fifo_1r1w_tb", name="a_vacuous",
                        sva="fifo_full && fifo_empty |-> rd_pop", disable_iff="tb_reset", budget_s=5)
    v = r["check"]["verdicts"].get("a_vacuous")
    expect(v == "VACUOUS" and ms < 1000, f"verdict {v} ({ms:.0f} ms; stops as soon as vacuity is proven)")

    print("\n5. check the file assertions -> fifo_2 fails (the DATA bug)")
    r, err, ms = m.tool("check_all", ids=["fifo_0", "fifo_1", "fifo_2"], budget_s=5)
    expect(r["verdicts"].get("fifo_2") == "CEX", f"verdicts {r['verdicts']} ({ms:.0f} ms)")

    print("\n6. agent reads the counterexample, filtered")
    r, err, ms = m.tool("get_trace", id="fifo_2", signals=["rd_ptr", "wr_ptr", "count", "wr_vld", "rd_ready"])
    expect(r.get("ok") and r["frames"], f"trace of {len(r.get('frames', []))} cycles ({ms:.0f} ms)")
    for f in r.get("frames", []):
        print("     cycle", f["cycle"], f["inputs"], f["registers"])

    print("\n7. agent adds an environment assumption (no simultaneous push and pop) and re-checks")
    r, err, ms = m.tool("add_assumption", name="env_no_push_pop", sva="!(wr_vld && rd_ready)")
    expect(r["ok"], f"assumption added ({ms:.0f} ms)")
    r, err, ms = m.tool("list_assertions")
    stale = [a["id"] for a in r["assertions"] if a.get("last", {}).get("stale")]
    expect("fifo_2" in stale, f"earlier results marked stale: {stale}")
    r, err, ms = m.tool("check_all", ids=["fifo_2"], budget_s=5)
    expect(r["verdicts"].get("fifo_2") == "PASS_BOUNDED",
           f"fifo_2 under the assumption: {r['verdicts'].get('fifo_2')} (the bug needs a push+pop) ({ms:.0f} ms)")

    print("\n8. agent removes the assumption; the bug is back")
    r, err, ms = m.tool("remove", id="env_no_push_pop")
    r, err, ms = m.tool("check_all", ids=["fifo_2"], budget_s=5)
    expect(r["verdicts"].get("fifo_2") == "CEX", f"fifo_2 again: {r['verdicts'].get('fifo_2')} ({ms:.0f} ms)")

    print("\n9. JasperGold handoff")
    path = os.path.join(WORK, "handoff.tcl")
    r, err, ms = m.tool("export_jaspergold", path=path)
    tcl = open(path).read()
    line = next((l for l in tcl.splitlines() if l.startswith("assert -name a_push_nonempty_v2")), "")
    expect("fifo_tb_inst.wr_push" in line and "fifo_tb_inst.fifo_empty" in line,
           f"session assertion exported in top scope: {line}")
    expect("fifo_2: CEX" in tcl and "prove -property" in tcl, "verdicts and remaining proofs in the script")

    print("\n10. no re-synthesis after load")
    expect(os.path.getmtime(yosys_log) == yosys_stamp, "Yosys ran once (its log was not rewritten)")

    m.p.stdin.close()
    m.p.wait()
    failed = [w for ok, w in checks if not ok]
    print(f"\n{len(checks) - len(failed)}/{len(checks)} checks passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
