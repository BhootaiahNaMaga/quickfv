# QuickFV — Agent-First Formal Pre-Check Engine

**Status:** Draft v0.5 · 2026-09-25 (M0–M3 done; see `casestudy/m0_fifo/README.md`, `tests/sva_equiv/README.md`, `casestudy/m2_engine/README.md`, `casestudy/m3_vacuity/README.md`)
**Working name:** `qfv` (CLI / daemon: `qfvd`)

---

## 1. Problem

Agents (and humans) writing SystemVerilog Assertions today get feedback from JasperGold. That
loop is slow and built for humans: `analyze → elaborate → clock/reset → prove`, a GUI-oriented
workflow, a reload on every assertion edit, and an expensive license. An agent that writes
twenty assertions and iterates on each one spends most of its time waiting on tool reloads.

**QuickFV** is a fast, open-source formal tool for the question *"is this assertion worth taking
to JasperGold?"* It answers in milliseconds for syntax and semantic errors, in about a second for
vacuity, and within a time budget (default 10 min) for shallow bugs. Only assertions that survive
go to JasperGold, using the same source files.

The project has two goals, and both count:
1. **Tool:** a fast feedback loop for agentic formal verification.
2. **Learning:** build the core engines (SVA→automata, bit-blasting, incremental BMC, simulation)
   by hand to understand what commercial engines do internally.

## 2. Goals and non-goals

### Goals (v1)
- A tiered, streaming verdict for each assertion (§4) with the pass criterion:
  **lint-clean ∧ reachable (non-vacuous) ∧ no CEX within the time budget.**
- **The same SV+SVA sources go to JasperGold unchanged.** Unsupported SVA is *rejected
  explicitly*, never silently ignored.
- **A persistent session:** the design is elaborated once, and adding, editing or removing an
  assertion never reloads it.
- Every result is **certified** by an independent checker (§8).
- It runs on an open-source stack on macOS (dev) and Linux x86-64 servers (deployment).

### Non-goals (v1)
- Being a JasperGold plug-in. JasperGold has no third-party engine API, so QuickFV sits upstream of it.
- Unbounded proof as a goal. `PROVEN` is an opportunistic bonus (v2 k-induction).
- Liveness (`s_eventually`, `s_until`, fairness). Reported as `UNSUPPORTED`.
- Multi-clock designs and properties. Reported as `UNSUPPORTED` in v1.
- Formally verifying the engine's source code (see §8: we certify results instead).
- Writing an SV parser or a SAT solver.

## 3. Users and use cases

**Primary user: an LLM agent** that writes or repairs SVA for an RTL block.

| Use case | Flow |
|---|---|
| Author an assertion | agent writes SVA → `check` → T0 lint error in <100 ms → fix → re-check |
| Catch vacuity | T1 reports `VACUOUS` (antecedent unreachable) with the reason → agent fixes the trigger |
| Hunt shallow bugs | T2 streams a `CEX` with a replayable trace → agent classifies it as an RTL bug or a missing assumption → adds an `assume` → re-check (no reload) |
| Hand off | survivors → `qfv export-jg` → JasperGold Tcl for sign-off |

**Secondary user:** an engineer learning FV internals, who reads the code, inspects the
unrolled CNF and monitors, and compares results with rIC3 and ABC.

## 4. Verdict model

### 4.1 Tiers
Each assertion moves through the tiers in order and stops at the first failure. Results are
streamed as they are produced: the agent never waits for the full budget to learn about an
early failure.

| Tier | Check | Target latency | Failure verdicts |
|---|---|---|---|
| **T0 Lint** | Parse and type-check the SVA against the *loaded* design: unknown signals, width mismatches, clocking, unsupported constructs | < 100 ms | `SYNTAX_ERROR`, `TYPE_ERROR`, `UNSUPPORTED` |
| **T1 Sanity** | The trigger (antecedent) is reachable, via a goal with a witness trace, or proven unreachable (synthesis folding, k-induction, rIC3 IC3); the property is not constant-folded away | < 1 ms for constant/local vacuity; ~0.3 s for invariant-based (M3) | `VACUOUS`, `POSSIBLY_VACUOUS`, `TRIVIALLY_TRUE` |
| **T2 Hunt** | Incremental BMC (depth grows until the time runs out) plus random simulation, raced from t=0 against IC3 engines (rIC3, ABC `pdr`), which often prove small designs in under 1 s (M0) | ≤ budget (default 600 s) | `CEX` (or early `PROVEN`) |
| **T3 Handoff** | Generate JasperGold Tcl for the survivors | — | — |

### 4.2 Final verdicts

```
SYNTAX_ERROR | TYPE_ERROR | UNSUPPORTED        (T0)
VACUOUS | POSSIBLY_VACUOUS | TRIVIALLY_*       (T1)  POSSIBLY_VACUOUS = no witness within budget
CEX                                            (T2)  always carries a replay-verified trace
PASS_BOUNDED                                   (T2)  lint-clean ∧ reachable ∧ no CEX in budget
PROVEN                                         (v2)  k-induction / portfolio IC3, certificate checked
ERROR                                                internal failure; never reported as a pass
```

`PASS_BOUNDED` always records **the depth reached** and **the wall-clock time used**, for example
`no CEX; depth 147; 600 s; engines: qfv-bmc, rIC3, abc-bmc`. It is not a proof.

### 4.3 Budget
- It is time-based, not depth-based. The default is **10 min per assertion**, configurable per call
  (for example a `30s` smoke check or a `10m` pre-JasperGold run).
- Assertions run in parallel across cores. Assumptions are shared by the whole session.

## 5. Supported SVA subset (v1)

| Supported | Rejected with `UNSUPPORTED` (v1) |
|---|---|
| `assert/assume/cover property`, `@(posedge clk)`, `disable iff` | liveness: `s_eventually`, `eventually`, `s_until`, `until`, strong sequences |
| boolean expressions over design signals, parameters, functions that are constant-foldable | unbounded ranges `##[a:$]` in the consequent (v2 via liveness/safety split) |
| `\|->`, `\|=>` | `[*]`, `[->]`, `[=]` repetition (v1.1) |
| `##N`, `##[a:b]` (finite) | `intersect`, `within`, `throughout`, `first_match` (v1.1) |
| `$past(e[,n])`, `$rose`, `$fell`, `$stable`, `$changed` | multi-clock, local variables, `sequence` args with side effects |
| `$onehot`, `$onehot0`, `$countones`, `$isunknown` (always false; 2-state) | `expect`, procedural (concurrent-in-always) assertions (v1.1) |
| named `property`/`sequence` declarations without local vars; `bind` | `$fell`/`$rose` on multi-bit sampled expressions (width-1 only) |

Semantics follow IEEE 1800-2017 §16. Any construct we don't support is **rejected with a precise
source location**, never approximated.

## 6. Architecture

```
            ┌──────────────── agent / human ────────────────┐
            │   MCP server     JSON CLI (qfv)     Tcl pkg    │
            └──────────┬───────────┬─────────────┬───────────┘
                       └───── JSON-RPC over unix socket ──────┐
                                                               ▼
┌──────────────────────────── qfvd (session daemon, C++17) ───────────────────────────┐
│                                                                                     │
│  Design load (once per session)                    Per assertion (hot path)         │
│  ─────────────────────────────                     ───────────────────────          │
│  setup.tcl ─► slang: parse all sources             SVA text                          │
│               ├─ extract SVA items ─────────────►  ├─ slang parse+bind (T0)         │
│               └─ design w/o SVA ─► Yosys+sv-elab   ├─ SVA→monitor compiler          │
│                                   (flatten)        │    (NFA → flops + bad/cover)   │
│                                     ─► BTOR2       └─ emit into Model IR ──┐         │
│                                         │                                  │         │
│                              Btor2Tools parser                             │         │
│                                         ▼                                  ▼         │
│                              ┌──────── Model IR (word-level) ───────────────┐        │
│                              │ states, inputs, init, next, constraints,     │        │
│                              │ per-assertion bad/cover nodes, name map      │        │
│                              └───────────────┬──────────────────────────────┘        │
│                                              │                                      │
│           ┌──────────────────┬───────────────┼────────────────┬──────────────┐       │
│           ▼                  ▼               ▼                ▼              ▼       │
│      Bit-blaster      Random simulator   Portfolio mgr   Trace/CEX mgr   Certifier   │
│           │           (bit-parallel,     (rIC3, ABC      (VCD, JSON,     (replay,   │
│           ▼            64 lanes/word)     &bmc, Pono     SV testbench)   LIDRUP/     │
│   Incremental BMC                         via BTOR2/                     LRAT)       │
│   (1 CaDiCaL inst,                        AIGER export)                              │
│    activation lit                                                                    │
│    per assertion)                                                                    │
└─────────────────────────────────────────────────────────────────────────────────────┘
```

### 6.1 Components

| Component | Build or reuse | Notes |
|---|---|---|
| Setup-file interpreter | **Build** | An embedded Tcl interpreter (libtcl) implementing a JasperGold-style command subset (§7.2) |
| SV/SVA parsing | **Reuse:** slang (MIT) | Full SV-2017 parse and elaboration; used for T0 and to extract SVA items |
| Design elaboration → BTOR2 | **Reuse:** Yosys + sv-elab (ISC) | Runs once per design load. Concurrent SVA is **stripped before Yosys** (sv-elab rejects it) and routed to our compiler |
| BTOR2 parsing | **Reuse:** Btor2Tools (MIT) | |
| **SVA→monitor compiler** | **Build** | Scope: what Yosys+slang can't lower. It already lowers boolean properties with `disable iff` to `$check` (M0); implication, sequences and delays are ours. slang AST → sequence NFA → synthesizable monitor (flops plus `bad`/`cover`/`constraint` nodes) written directly into the Model IR. Reference semantics: EBMC (`temporal-logic/`), CIRCT LTL dialect |
| Model IR | **Build** | Word-level transition system; stable name map from RTL hierarchical names to IR nodes; every state element is tagged **RTL** or **synthetic** (Yosys `$auto$` flops, monitor flops), so traces and replays show and set only RTL state (M0) |
| **Bit-blaster** | **Build** | Word ops → AIG → CNF (Tseitin) with structural hashing. Target of the later Lean verification |
| **Incremental BMC** | **Build** | One CaDiCaL instance per session. Frame k is added once; each assertion's bad literal is gated by an activation literal and solved under assumptions. Learned clauses are kept across assertions and edits |
| **Random simulator** | **Build** | Bit-parallel (64 traces/word) over the AIG; honors `assume` via rejection; also seeds T1 cover witnesses |
| Portfolio | **Reuse:** rIC3*, ABC `&bmc`/`pdr`, Pono | Started at t=0 alongside our BMC (M0: IC3 proved the FIFO in <1 s, while BMC to depth 30 took 7–153 s). Export BTOR2/AIGER, race the engines; the first CEX or proof wins. An IC3 CEX is **re-minimized** with BMC to its depth before it is reported (IC3 traces are not shortest). *Check the rIC3 license (BSD-3 vs GPL-3) before bundling* |
| SAT solver | **Reuse:** CaDiCaL (MIT) | Also used for LIDRUP/LRAT proof output |
| Certifiers | **Reuse:** btorsim, Yosys `sim`, Verilator (`--assert`), lidrup-check, cake_lpr, Certifaiger/Cerbtora | See §8 |

### 6.2 Key design rules
1. **No reload per assertion.** Yosys runs only in `load`. Assertions compile straight into the IR
   and the solver incrementally.
2. **Explicit rejection beats approximation.** Unsupported SVA, multi-clock logic, X semantics and
   black boxes all produce a typed error.
3. **2-state semantics.** X/Z are handled per the Yosys `setundef` policy (documented and
   configurable). This is a known divergence from JasperGold and is noted in every result.
4. **Deterministic by default.** Seeds are fixed and recorded in every result.
5. **Every CEX is independent evidence.** No CEX is reported until it has been replayed on the
   original RTL (§8).

## 7. Interfaces

### 7.1 JSON CLI (`qfv`)
```
qfv session start  setup.tcl                  → {"session":"s1","load_ms":8123,"signals":1412}
qfv check   -s s1  --file props.sv [--budget 10m] [--stream]
qfv check   -s s1  --sva 'assert property (@(posedge clk) disable iff(!rst_n) push |-> ##1 !empty);'
qfv assume  -s s1  --sva '...'                → adds constraint; invalidates prior PASS_BOUNDED results
qfv remove  -s s1  --id a7
qfv status  -s s1  [--id a7]
qfv trace   -s s1  --id a7  --format vcd|json|tb
qfv export-jg -s s1 [--only pass_bounded] > handoff.tcl
qfv session stop   -s s1
```
Streamed result (one JSON object per line, NDJSON):
```json
{"id":"a7","tier":"T0","status":"ok","ms":14}
{"id":"a7","tier":"T1","status":"reachable","ms":210,"witness":"traces/a7_cover.vcd","depth":3}
{"id":"a7","tier":"T2","status":"CEX","ms":4210,"depth":9,"engine":"qfv-bmc",
 "trace":{"vcd":"traces/a7.vcd","json":"traces/a7.json","tb":"traces/a7_tb.sv"},
 "certified":{"rtl_replay":"pass","btorsim":"pass"},
 "summary":"cycle 9: push=1 while full=1; count wraps 7→0; empty asserted"}
```
Exit codes: 0 = everything checked has `PASS_BOUNDED` or `PROVEN`, 1 = at least one assertion failed, 2 = tool error.

### 7.2 Setup file (a JasperGold-style Tcl subset)
```tcl
analyze -sv12 rtl/fifo.sv
analyze -sv12 sva/fifo_props.sv        ;# the same file you'd give JasperGold
elaborate -top fifo -parameter DEPTH 8
clock clk
reset -expression !rst_n               ;# initial state = state after reset sequence
assume -name a_no_push_full {@(posedge clk) full |-> !push}
set_prove_time_limit 10m               ;# maps to the qfv budget
prove -all
```
Supported commands: `analyze`, `elaborate`, `clock`, `reset`, `assume`, `assert`, `cover`,
`prove`, `set_prove_time_limit`, `get_property_list`, `get_property_info`. Unknown commands →
error, never ignored. We use a subset of the JasperGold command *names* for familiarity. We make
no claim of full compatibility.

### 7.3 Tcl package
`package require qfv` provides `qfv::check`, `qfv::status` and so on. It wraps the CLI with `exec`
and parses the JSON, so it can be `source`d in tclsh, JasperGold's Tcl shell, or any other Tcl-based EDA tool.

### 7.4 MCP server
A thin wrapper over the daemon socket. Tools: `load_design`, `check_assertion`, `add_assumption`,
`remove`, `get_status`, `get_trace` (with signal filtering and a cycle window, so agents don't take in whole
VCDs), `explain_cex` (a structured diff of the signals relevant to the assertion's cone), `export_jaspergold`.

### 7.5 JasperGold handoff (`export-jg`)
Emits a Tcl script with the original `analyze`/`elaborate`/`clock`/`reset`, all assumptions, and
the assertions still open. Each is annotated with the QuickFV result (depth reached, time,
witness) so the JasperGold run can skip what is already known.

## 8. Trust: certify results, don't verify code

The engine code is fast and not formally verified. **Every reported result carries evidence that
a small, independent checker validates.**

| Claim | Certificate | Checker | When |
|---|---|---|---|
| `CEX` | Input trace from reset | (1) `btorsim` on the BTOR2; (2) **replay on the original RTL** with the generated SV testbench run in Verilator `--assert` (Icarus has no concurrent SVA) plus the assertion. (2) is ground truth and also catches bugs in elaboration and the SVA compiler | Always, before reporting |
| `REACHABLE` (T1) | Cover witness trace | Same replay | Always |
| No CEX to depth k | CaDiCaL **LIDRUP** incremental proof (fast) or per-depth CNF + **LRAT** | `lidrup-check` (default); `cake_lpr` (CakeML, verified) or Lean's LRAT checker (`--certify=verified`) | Opt-in (`--certify`) |
| `PROVEN` (v2) | Inductive invariant / witness circuit | Certifaiger (AIGER) / Cerbtora (BTOR2) | Always, for PROVEN |

**The remaining gap:** an UNSAT proof shows the *CNF* has no solution, not that the CNF encodes the
RTL faithfully. We close it in layers:
1. **Differential testing:** every case-study property runs through both QuickFV and rIC3/EBMC.
   Any disagreement is a P0 bug.
2. **Replay:** random-simulation traces are checked against Yosys `sim` for per-cycle equality of the IR model.
3. **(Later) Lean-verified bit-blaster/unroller.** Following Lean's `bv_decide`, we prove that the
   BTOR2→CNF unrolling is correct, so certified LRAT proofs become end-to-end guarantees for
   the post-elaboration model.

The trusted base in v1 is slang, Yosys/sv-elab elaboration, and the SVA compiler. The trusted base
for CEX results is only the RTL simulator.

## 9. Performance targets

| Metric | Target |
|---|---|
| Design load (≤50k flops after flattening) | ≤ 30 s |
| T0 lint per assertion | p95 < 100 ms |
| T1 reachability, shallow trigger (≤10 cycles) | p95 < 1 s |
| Incremental re-check after an `assume` edit | no re-elaboration; solver reused |
| Time to first CEX vs rIC3 BMC on the case study | within 2× (qfv-bmc alone); ≤ 1× with portfolio |
| Wrong verdicts vs oracle | **0** |
| Parallelism | 1 BMC solver per assertion group + simulation lanes; scales to server core count |

## 10. Case study (validation vehicle)

**Designs:** NVIDIA FVEval (Apache-2.0)
- **NL2SVA-Human:** 13 hand-written testbenches, 79 assertions. FIFOs (1R1W ×4, multi-port),
  arbiters (×4), FSMs, counters, RAM. **These are reference models with no DUT** (the DUT signals
  are free inputs; FVEval uses them for assertion equivalence). For bug hunting we write a small DUT
  per model and `bind` the model to it, as in `casestudy/m0_fifo`.
- **Design2SVA sample:** about 20 of the 192 parameterized pipelines and random FSMs.

**Method**
1. Run the reference SVA on the clean RTL. Expect `PASS_BOUNDED`/`PROVEN` and agreement with the oracles.
2. **Bug injection:** mutate the RTL (flipped conditions, off-by-one pointers, dropped resets,
   stuck bits), 5–10 mutants per design. Expect a `CEX` on the assertions that cover each mutation.
3. **Vacuity injection:** make the antecedents unreachable. Expect `VACUOUS`.
4. **Agent loop:** an agent writes SVA from the NL2SVA descriptions using QuickFV feedback. Measure
   iterations and wall time to a lint-clean, reachable result with no CEX.

**Oracles** (in place of JasperGold, which we can't access): rIC3 (BMC and IC3) and EBMC (BMC and SVA
semantics). JasperGold validation happens later at work, via `export-jg`.

**Metrics:** wrong verdicts (must be 0), p50/p95 latency per tier, time to first CEX
vs the oracles, mutation kill rate, and the share of assertions settled without JasperGold.

**Stretch:** NL2SVA-Machine, checking two SVA properties for equivalence with bidirectional BMC
(assume A ⊢ assert B, and the reverse), as an agent tool: "is my assertion equivalent to the reference?"

## 11. Milestones

Each milestone ends with a check against the oracles before the next one starts.

| # | Deliverable | Exit criteria |
|---|---|---|
| **M0** ✅ | OSS CAD Suite (Yosys, slang, SymbiYosys, ABC, rIC3, Pono, Verilator) + EBMC 6.0 built from source. FVEval `fifo_1r1w` model on a hand-written DUT with 4 injected bugs, through 5 engines, with CEX replay (`casestudy/m0_fifo`) | Done: 0 disagreements; 16/16 CEXs replay-confirmed on the original RTL; walkthrough in `casestudy/m0_fifo/README.md`. Linux container deferred to M5 |
| **M1** ✅ | Session skeleton plus **SVA compiler** (slang → NFA → monitor, emitted as Verilog for inspection) plus **T0 lint** | Done: `qfv lint`/`compile-sva`/`check-sva`. 68/71 equivalence cases pass and 0 fail, against three oracles (EBMC, Verilator, hand-derived golden tests), because EBMC and Verilator each deviate from IEEE 1800 on `disable iff` and ranged sequences (`tests/sva_equiv/README.md`). T0 check of a new assertion: 0.2–3 ms. Compiled FIFO monitors reproduce the M0 results exactly |
| **M2** ✅ | Model IR (AIG), BTOR2 loading, bit-blaster, incremental BMC (`qfv bmc`) | Done: shortest CEX = rIC3 BMC on all FIFO bugs; 9/9 CEXs certified by btorsim; bit-blaster matches btorsim on 261/261 operator×width cases; shallow bugs in ms of solver time (rIC3: 30–140 ms per process run). *Moved to M4: "adding an assertion never reloads the design"* (needs direct monitor→AIG emission plus the session daemon) |
| **M3** ✅ | **T1** vacuity (trigger goals; k-induction + rIC3 IC3 for proofs), bit-parallel random simulator, CEX outputs (JSON/VCD/BTOR2 witness/SV testbench), **RTL replay certification** | Done: 7/7 injected vacuities → VACUOUS (synthesis folding, induction k≤2, rIC3); 3/3 real triggers reachable; 17/17 FIFO CEXs confirmed by btorsim **and** Verilator replay on the original RTL+SVA; simulation finds 16- and 32-deep bugs in 3–90 ms where BMC and rIC3 timed out |
| **M4** | JSON CLI with streaming, Tcl setup interpreter, Tcl package, MCP server, `export-jg`; **persistent session**: monitors emitted straight into the AIG (SV expression → AIG over a Yosys name map), so adding or editing an assertion never reruns Yosys | An agent completes the case-study loop only through MCP; re-checking an edited assertion needs no Yosys run |
| **M5** | Portfolio (rIC3/ABC/Pono), LIDRUP/LRAT certification, container build | Time to first CEX at or below the oracles; `--certify` passes on all PASS_BOUNDED results |
| **M6** | Case-study report (§10) | Metrics published; the handoff Tcl is ready to try on JasperGold at work |
| **v2** | k-induction (`PROVEN`) with Certifaiger, simulation-seeded BMC, SVA v1.1 constructs, liveness via liveness-to-safety, Lean-verified unroller, word-level solving (Bitwuzla) | — |

## 12. Tech stack and packaging
- **Language:** C++20 core (CMake; slang requires C++20). Tcl 8.6 embedded for setup files. MCP server in Python (thin) or C++.
- **Dependencies:** slang (MIT), CaDiCaL (MIT), Btor2Tools (MIT), Yosys + sv-elab (ISC), Tcl (BSD),
  nlohmann/json (MIT). Oracles and portfolio run as external processes, so their licenses stay separate.
- **Packaging:** an OCI container (Docker/Apptainer) for the work server; a static `qfvd` binary
  plus a Yosys build as fallback. CI builds linux-x86_64 and macOS-arm64.
- **Deployment target:** a many-core Linux server with container support.

## 13. Risks and open questions

| Risk | Mitigation |
|---|---|
| Name mapping: the SVA references hierarchical RTL names that Yosys flattening may rename or optimize away | Keep names in Yosys (`-keep` attributes, `write_btor -s`); T0 reports a signal as missing post-elaboration explicitly |
| Stripping concurrent SVA before Yosys while keeping `bind` and in-module assertion scope | slang produces the design text minus SVA plus a scope map; tested in M1 |
| 2-state vs JasperGold 4-state/X semantics give different verdicts | Documented; each result is flagged; `$isunknown` handled conservatively; validated at work via `export-jg` |
| Memories bit-blast too large | M2 maps memories to registers (`memory_map`) and rejects BTOR2 arrays; lazy array encoding in v1.1; size limits reported |
| BMC stalls past depth ~20 on data-path properties (M2: every BMC tested, rIC3's included) | Random simulation first (M3: 16/32-deep FIFO bugs in 3–90 ms); IC3 portfolio from t=0; simulation-seeded BMC in v2 |
| Simulation CEXs are long (hundreds of cycles) and BMC cannot shorten deep ones in time | v2: trace shortening (drop or merge cycles and re-check by simulation), then BMC from a late state of the trace |
| Random simulation is useless if reset is left unconstrained (it resets constantly) | Default is JasperGold-style reset held inactive after cycle 0; `--reset-free` is opt-in (M3) |
| A bounded "pass" read as meaningful for liveness (M0: EBMC reports false liveness properties as "PROVED up to bound 20") | Liveness is `UNSUPPORTED` in v1; v2 requires lasso or liveness-to-safety with a CEX-capable engine |
| rIC3 license ambiguity (BSD-3 vs GPL-3) | Invoke it only as an external process; resolve before any bundling |
| SVA semantic bugs in our compiler | Three oracles: EBMC bidirectional bounded equivalence, Verilator random differential, hand-derived golden tests; RTL replay on every trace. **The reference tools themselves deviate from the LRM** (M1: EBMC on mid-attempt `disable iff`; Verilator on failure timing and ranged antecedents), so disagreements are resolved against the LRM, not against a tool |
| `PASS_BOUNDED` read as "proven" by agents | The verdict name, depth and the "not a proof" flag are always in the JSON; the MCP tool description states it |

## 14. Glossary
- **BMC:** bounded model checking. The design is unrolled k cycles and SAT searches for a violation.
- **Activation literal:** a SAT variable that switches an assertion's clauses on or off through solver
  assumptions, so assertions can be added or removed without rebuilding the solver.
- **Vacuity:** an implication that passes only because its antecedent never fires.
- **LRAT / LIDRUP:** SAT UNSAT-proof formats checkable by small or verified checkers.
- **Certifaiger / Cerbtora:** certificate checkers for AIGER / BTOR2 model-checking proofs.

## 15. References
- FVEval: https://github.com/NVlabs/FVEval · arXiv:2410.23299
- slang: https://github.com/MikePopoloski/slang · sv-elab: https://github.com/povik/sv-elab
- Yosys: https://github.com/YosysHQ/yosys · Btor2Tools: https://github.com/hwmcc/btor2tools
- CaDiCaL: https://github.com/arminbiere/cadical · lidrup-check: https://github.com/arminbiere/lidrup-check
- rIC3: https://github.com/gipsyh/rIC3 · ABC: https://github.com/berkeley-abc/abc · Pono: https://github.com/stanford-centaur/pono
- EBMC: https://github.com/diffblue/hw-cbmc · CIRCT: https://github.com/llvm/circt
- Certifaiger: https://github.com/Froleyks/certifaiger · Cerbtora: https://github.com/Froleyks/cerbtora
- cake_lpr: https://github.com/tanyongkiam/cake_lpr · HWMCC'25: https://hwmcc.github.io/2025/
- Related agentic tools: https://github.com/zesun33/mcp-formal · https://github.com/saintbate/FormalSynapse
