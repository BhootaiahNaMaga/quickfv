# M6 — Case study: QuickFV on NVIDIA FVEval

**Question:** can an agent use QuickFV to write, debug and settle SystemVerilog assertions quickly
enough, and trustworthily enough, that JasperGold only sees what really needs it?

**Answer from this study:**
- **98% of the assertions checked were settled without JasperGold** (PROVEN or VACUOUS, both
  certified), plus CEXs whose root cause the agent identified.
- **Zero wrong verdicts reached a result.** Every CEX was independently certified, and every
  proof was checked by Certifaiger.
- The per-assertion feedback loop runs in about **1 ms** (lint + compile into the loaded design),
  and the first CEX typically arrives in **under 1 ms**.
- The study also found **one soundness bug in QuickFV** (fixed, and every result re-established)
  and **several problems in the benchmark itself**.

Everything here is reproducible: the scripts are in this directory, and the raw data is in
`*.json` / `agent_log.jsonl`.

## 1. How much real SVA does v1 cover?

All 79 expert-written NL2SVA-Human reference assertions were linted (`survey_nl2sva.py`):

| | Count |
|---|---|
| Supported by the v1 subset | **70 (89%)** |
| Rejected: liveness (`strong(##[0:$] …)`) | 9 |
| Rejected: anything else | 0 |

In session mode (`t0_latency.py`, 12 of 13 testbenches loaded), 2 more were rejected at AIG
emission. Both reference an element of an unpacked **wire** array (`match_tracker[0]`) that isn't
visible under that name in the synthesized model. The rejection is explicit, as designed.

**Hot-path latency** on this real SVA (add = T0 lint + direct AIG emission, design already loaded):
**p50 0.53 ms, p95 1.08 ms, max 2.9 ms.** Design load (Yosys, once): 30–150 ms.

## 2. The agent loop on NL2SVA designs (reference assertions + our DUTs)

FVEval's NL2SVA testbenches are reference models with no DUT, so we wrote DUTs from the NL prompts:
[`designs/arbiter_rr.sv`](designs/arbiter_rr.sv), [`designs/counter.sv`](designs/counter.sv), and the
M0 FIFO. That's **18 reference assertions**. The agent loop was then run through the session exactly
as an MCP agent would (logged in `agent_log.jsonl`).

### Round-robin arbiter (10 reference assertions)

| Step | Result | Time |
|---|---|---|
| load + check all | 7 PROVEN (certified), 3 CEX | 0.15 s + 1.4 s |
| read the 3 CEXs | all three are **environment** issues, not DUT bugs: `hold`+`busy` together (they're DUT inputs); `hold` before any grant; `hold` while the held client stops requesting. The references `arbiter_6` and `arbiter_3` actually **contradict** each other without such an assumption | – |
| add 2 assumptions (no reload) | `!(hold && busy)`, `hold \|-> \|(last_gnt & req)` | 0 ms each |
| re-check | **10/10 PROVEN (certified)** | 1.6 s |
| sanity covers (does the environment still allow grants, holds, blocking, rotation, wrap-around?) | 5/5 COVERED, witnesses certified | 0.2 s |

### Counter (5 reference assertions)

| Step | Result |
|---|---|
| first check | 1 PROVEN, 4 CEX |
| diagnosis | **two root causes**: (a) the environment jumps outside [min, max]; (b) three CEXs are in the first cycle after reset. The checker's delay registers have no reset and the checker masks that cycle assuming reset lasts ≥ 2 cycles, as JasperGold's reset analysis does. QuickFV held reset for one cycle, **a QuickFV semantics gap**, so `reset -cycles N` was added (a marked QuickFV extension) |
| after the fixes | 3 PROVEN, 2 CEX (`counter_0`, `counter_1`) |
| diagnosis | the prompts say "does not overflow/underflow", but the references encode that as "from max, the next count isn't ≤ min". That **false-alarms on a correct saturating counter** doing a legal large decrement (5-cycle CEX). **The reference assertions are too strong** |
| agent writes refined versions | "at max with a net increment and no jump, count stays max" (and the mirror): **PROVEN, non-vacuous**, and they catch the real overflow and floor bugs |

## 3. Mutation campaign (`campaign.py`)

Each design was checked clean, with every hand-written bug, and with automatic single-operator
mutants of the DUT, under the final environment.

| Design | Clean | Mutants killed | Hand bugs | Survivors |
|---|---|---|---|---|
| fifo_1r1w | 3 PROVEN | 11/12 | 4/4 | `wr_ready = (count == DEPTH)`: the FIFO never accepts data, so every **safety** assertion holds trivially. Only the liveness references (unsupported) or an "a push happens" cover would catch it |
| arbiter_rr | 10 PROVEN | 9/9 | 4/4 | – |
| counter | 5 PROVEN | 4/6 | 3/3 | 2 **equivalent** mutants (`ssum > max` → `>=`, `ssum < min` → `<=`: the boundary case gives the same value either way) |

- **Every verdict was certified:** 88/88 CEXs by btorsim, and 119/119 PROVEN by Certifaiger.
- **RTL replay** (Verilator on the mutated RTL + the original FVEval SVA + the agent's
  assertions): **22/24 confirmed.** The other 2 are flagged `x_dependent`. Those mutants read one
  bit past a vector (`last_gnt[N]`); the CEX needs that X to be 1, which is legitimate in formal
  semantics (and a real bug in the mutant), while 2-state simulation returns 0.
- **Time to first CEX:** p50 **0.1 ms**, p95 29 ms, max 0.9 s.

## 4. Design2SVA sample: real RTL, agent-written assertions (`design2sva.py`)

Design2SVA gives the RTL and asks for assertions. The agent wrote them from the RTL (one per FSM
transition branch plus legal-state invariants; valid latency for pipelines).

| | Designs | Assertions | PROVEN | VACUOUS | PASS_BOUNDED | Mutants killed | Check time |
|---|---|---|---|---|---|---|---|
| FSMs | 6 | 118 | **105** | **13** | 0 | 25/27 | 1.2–9.6 s |
| Pipelines | 4 (116–2100 lines) | 8 | 6 | 0 | 2 (the 50-stage one) | 6/7 | 1.1–10.2 s |

- **The 13 VACUOUS verdicts are dead branches in the benchmark RTL**, each proven, for example
  `!(in_B >= 'd0)` (an unsigned value is always ≥ 0) or guards that contradict themselves. Because
  the agent's assertions mirror the RTL, vacuity exposes RTL code that can never execute.
- **The pipeline survivor** is in the data path. The latency assertions deliberately don't check
  data, so the mutation score shows how weak a property set is.

## 5. Findings about the benchmark

| Finding | Where |
|---|---|
| **51 of 96 (53%) Design2SVA FSMs infer a latch** in `always_comb` (a state branch assigns nothing). QuickFV v1 rejects them with a clear message; the sample above uses latch-free FSMs | `design2sva_fsm.csv` |
| Two NL2SVA reference assertions are **too strong** (counter overflow/underflow) | §2 |
| Two arbiter references **contradict** each other without an environment assumption | §2 |
| The NL2SVA `ram` checker compares `re` (a 1-bit enable) with an address (`we && (re == symbolic_constant_a)`), which looks like a typo in the reference model | `annotated_tb/ram.sv` |
| The `multi_fifo` reference model has combinational feedback that Yosys rejects as a loop | `annotated_tb/multi_fifo.sv` |
| The NL2SVA testbenches have **no DUTs**, so "prove the reference" needs a DUT (written here for 3 designs) | all |

## 6. Findings about QuickFV (all fixed and re-validated)

1. **A soundness bug in the BTOR2 loader.** A BTOR2 state without `next` is unconstrained in
   every frame, but QuickFV held its value. Yosys writes every `$anyseq` (undriven wires, and X)
   this way, so QuickFV **under-approximated** such designs: CEXs stayed real, but PASS_BOUNDED,
   PROVEN or VACUOUS could have been wrong. btorsim and rIC3 exposed it on a 6-line model. Fixed
   (a fresh free input per frame), and **every earlier result was re-established**: no earlier
   verdict changed, because those designs had no such states.
2. **X semantics.** Yosys's optimiser used to pick a convenient constant for an X (an out-of-range
   read). X now becomes an explicit free input before optimisation, as in standard formal
   semantics, and CEXs that need a particular X value are flagged `x_dependent`.
3. **Register aliases.** With every wire exposed, one register can have several names (a DUT
   register and the checker port bound to it). Replay testbenches initialised the port. slang now
   picks the alias that is a variable.
4. **Reset length.** `reset -cycles N` (see §2).
5. **Latches.** The raw Yosys error is replaced by *"combinational loop … usually a latch inferred
   in always_comb"*.

## 7. Where the time goes, and what's left for JasperGold

| Metric | Value |
|---|---|
| Per-assertion lint + compile into the loaded design | p50 0.53 ms, p95 1.08 ms |
| Design load (Yosys, once per session) | 30–150 ms |
| Time to first CEX (campaign) | p50 0.1 ms, p95 29 ms |
| Whole-design check incl. certified proofs | 0.7–10 s |
| Assertions settled without JasperGold (Design2SVA + final NL2SVA environment) | 124/126 Design2SVA (98%); 18/18 NL2SVA + 2 refined |
| Wrong verdicts (certified results contradicted by an independent check) | **0** |
| Still for JasperGold | liveness assertions (9/79 in NL2SVA); PASS_BOUNDED results (e.g. the 50-stage pipeline); designs with latches |

## 8. What this means for a rollout at work

- The loop an agent needs works: lint in about 1 ms, then certified CEX or PROVEN in seconds, and
  environment assumptions added and removed without reloading.
- **Diagnosis is where the value is.** In this study, most "failures" were environment
  assumptions, reset length, or over-strong reference assertions, not DUT bugs. The QuickFV traces
  (filtered, a few cycles, certified) made each one a short investigation.
- **Watch-outs:**
  - latches aren't modelled (53% of one benchmark's FSMs have one);
  - liveness isn't supported;
  - 2-state vs X: CEXs that need a particular X are flagged, not hidden;
  - `reset -cycles` is a QuickFV extension, so check JasperGold's reset analysis on the handoff;
  - the Linux container is untested.
- **Cross-check QuickFV against JasperGold** on a few real blocks via `export_jaspergold`: same
  verdicts, and the same behaviour on `disable iff` and ranged sequences (where EBMC and
  Verilator were both found to deviate from IEEE 1800, see `tests/sva_equiv/README.md`).

## Reproduce

```
python3 casestudy/m6_report/survey_nl2sva.py     # §1 coverage
python3 casestudy/m6_report/t0_latency.py        # §1 hot-path latency
python3 casestudy/m6_report/make_props.py        # FVEval checker + references -> designs/*_props.sv
python3 casestudy/m6_report/campaign.py          # §3 mutation campaign
python3 casestudy/m6_report/design2sva.py        # §4 Design2SVA sample
```
(`agent_log.py` drives the logged agent loop of §2.)
