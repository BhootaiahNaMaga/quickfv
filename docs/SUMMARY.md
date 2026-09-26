# QuickFV — What Was Built

QuickFV (`qfv`) is a working, open-source, agent-first formal pre-check for SystemVerilog
assertions. It gives a verdict per assertion before anything goes to JasperGold. On NVIDIA
FVEval it settled 98% of checked assertions without JasperGold, with zero wrong verdicts.

JasperGold is built for engineers at a GUI: it reloads the design per edit and is expensive.
QuickFV loads the design once and answers each new or edited assertion in about 1 ms (lint +
compile into the loaded design). Bugs usually surface in under 1 ms of solver time. Every CEX and
proof verdict is independently certified: counterexamples are replayed by btorsim before they are
reported, and proofs are checked by separate tools; bounded "no CEX" answers are checkable with
`--certify`. Certificates are about the emitted model: that the SVA and RTL were translated
correctly is tested separately (the equivalence and regression suites).

Full design: [`SPEC.md`](../SPEC.md). Case-study report: [`casestudy/m6_report`](../casestudy/m6_report/README.md).

## Architecture and flow

Yosys runs once per design; after that, every assertion goes straight into an in-memory circuit.

1. **Load (once):** slang parses the sources and strips all assertions. Yosys elaborates the
   design with every named wire kept, and writes BTOR2. QuickFV bit-blasts it into an AIG with a
   name map.
2. **Add an assertion (no Yosys):** slang binds the SVA in its module. QuickFV's own compiler
   lowers it to a monitor and emits it directly into the AIG.
3. **Check:** one time budget, several engines racing:
    - random simulation (64 traces per machine word) for deep-but-likely bugs;
    - incremental BMC on CaDiCaL for the shortest counterexample;
    - k-induction for vacuity proofs;
    - rIC3 (IC3, HWMCC winner) per property, as a subprocess, for proofs and deep bugs.
4. **Certify** every result (see below), then report as streamed JSON events.

The code is C++20, about 6,900 lines: `src/sva` (SVA compiler), `src/model` (AIG, BTOR2
read/write), `src/engine` (unroller, hunt, simulation, portfolio), `src/session` (persistent
session, setup files).

## What was built, by milestone

| Milestone | What it delivered | Exit evidence |
| --- | --- | --- |
| M0 Reference flow | Open-source toolchain (Yosys, slang, rIC3, ABC, Pono, EBMC, Verilator) run on an FVEval FIFO with 4 injected bugs | 16/16 counterexamples replay on the RTL; 0 tool disagreements |
| M1 SVA compiler | `qfv lint` / `compile-sva` / `check-sva`: SVA lowered to synthesizable monitors; unsupported SVA rejected with a reason | 68/71 equivalence cases pass, 0 fail, against EBMC, Verilator and hand-derived golden tests |
| M2 Engine | AIG bit-blaster and incremental BMC on CaDiCaL (`qfv bmc`) | 261/261 operator tests match btorsim; CEX lengths match rIC3 |
| M3 Vacuity + traces | Trigger-reachability checks, random simulation, JSON/VCD/witness/testbench traces, RTL replay | 7/7 injected vacuities found; 17/17 CEXs confirmed two ways |
| M4 Agent interface | Persistent session, direct SVA-to-AIG emission (no Yosys per assertion), MCP server, Tcl package, JasperGold-style setup files, JasperGold export | Agent loop over MCP 14/14; Yosys runs once per session |
| M5 Portfolio + certificates | rIC3 per property; LIDRUP certificates for bounded claims; Certifaiger-checked proofs; Dockerfile | First CEX at or below rIC3 in every benchmark case; all certificates verify |
| M6 Case study | FVEval run with real DUTs, mutation campaign, Design2SVA sample, report | 98% settled without JasperGold; 0 wrong verdicts |

## Verdicts and certification

| Verdict | Meaning | Independent check |
| --- | --- | --- |
| CEX | Failing trace, shortest found | btorsim reaches the exact property at the exact cycle; Verilator fails the original SVA on the original RTL (`--replay`) |
| VACUOUS | The trigger can never fire | Synthesis folding, k-induction (optionally LIDRUP-certified), or an rIC3 proof checked by Certifaiger |
| PROVEN | Holds for all time | rIC3 witness circuit verified by Certifaiger (9 obligations, each UNSAT); an uncertified proof is never reported |
| PASS_BOUNDED | No CEX within the time budget; not a proof | Every "no CEX at depth k" answer is checkable with `--certify` (CaDiCaL LIDRUP proof + lidrup-check) |
| POSSIBLY_VACUOUS | Trigger not reached within the budget | — |
| UNSUPPORTED / ERROR | Outside the v1 SVA subset / has errors, with reason and location | — |

Priority when several apply: CEX, then VACUOUS, then PROVEN. A CEX that needs a particular X value
is flagged `x_dependent` (real in formal semantics, not reproducible in 2-state simulation). Every
checker was shown to reject tampered input. The remaining trusted base is slang and Yosys
elaboration plus QuickFV's own RTL-to-CNF encoding.

## Interfaces

| Interface | For | What it does |
| --- | --- | --- |
| `qfv mcp` | LLM agents (Claude Code, any MCP host) | MCP tools: `load_design`, `check_assertion`, `add_assumption`, `check_all`, `get_trace`, `list_assertions`, `remove`, `export_jaspergold`. Accepts bare properties like `req \|=> ack` |
| `qfv serve` | Scripts | JSON requests on stdin, streamed JSON events on stdout |
| `tcl/qfv.tcl` | Engineers in tclsh or an EDA tool's Tcl shell | Pipe to `qfv serve`: `qfv::load`, `qfv::check_assertion`, `qfv::assume`, `qfv::export_jg` |
| `qfv lint`, `qfv bmc` | One-shot shell use | Lint; full check with `--replay`, `--certify`, `--budget` |
| Setup files | Design setup | JasperGold Tcl subset: `analyze`, `elaborate`, `clock`, `reset`, `assert`/`assume`/`cover`, `prove`, `set`. Unknown commands are errors. `reset -cycles N` is a QuickFV extension |
| `export_jaspergold` | Handoff | Tcl script with sources, clock/reset, session assertions rewritten to top scope, verdicts as comments, and `prove` on what is still open |

To use from Claude Code: `claude mcp add quickfv -- <repo>/build/qfv mcp --work <session dir>`.

## Case-study results (NVIDIA FVEval)

| Metric | Result |
| --- | --- |
| Expert assertions in the v1 SVA subset | 70 of 79 (the 9 rejected are all liveness) |
| Lint + compile of one assertion into the loaded design | 0.53 ms median, 1.08 ms at p95 |
| Design load (Yosys, once per session) | 30–150 ms |
| Time to first counterexample | 0.1 ms median, 29 ms at p95 |
| Whole-design check including certified proofs | 0.7–10 s |
| Injected bugs caught (mutation campaign) | 24 of 27 (all 11 hand-written); 2 survivors undetectable, 1 needs liveness |
| Certified results | 88/88 counterexamples, 119/119 proofs |
| Counterexamples replayed on the RTL | 22 of 24 confirmed; 2 flagged X-dependent |

The study found problems on both sides:
- **In QuickFV (fixed):** a soundness bug in the BTOR2 loader (states without `next` must be
  free every cycle). All earlier results were re-run and none changed.
- **In the benchmark:** 53% of Design2SVA FSMs infer a latch; two counter references are too
  strong; two arbiter references contradict each other without an assumption; the RAM checker
  has a likely typo; 13 dead branches in the FSM RTL were proven VACUOUS.

## Limitations and next steps

The biggest open item is a cross-check against JasperGold on real blocks; nothing has been run on
JasperGold yet.

- **Not supported in v1:** liveness properties, latches (rejected with a clear message),
  multi-clock designs, SVA repetition (`[*]`), property arguments.
- **Untested:** the Linux container (`docker/Dockerfile`).
- **Trusted, not proven:** the RTL-to-CNF encoding (tested by 261 operator checks and cross-checks;
  a Lean-verified unroller is planned).
- **Semantics to confirm on JasperGold:** `disable iff` and ranged sequences (EBMC and Verilator
  both deviate from IEEE 1800 there), X handling, and reset length.
- **Not integrated:** ABC and Pono in the portfolio (rIC3 covers IC3, BMC and k-induction).

Next steps:
- [ ] Run `export_jaspergold` on 2–3 real blocks and compare verdicts
- [ ] Build and test the container on a Linux server
- [ ] Add latch modeling and liveness (v1.1 / v2)
