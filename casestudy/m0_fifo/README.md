# M0 — FIFO through the open-source reference flow

M0 is the **reference flow** that QuickFV will be measured against, and a walkthrough of what a
formal tool does between "here are my SV+SVA files" and "here is a counterexample". None of
QuickFV's own code exists yet. Every step here uses existing open-source tools.

Run it: `casestudy/m0_fifo/run_m0.sh` (about 4.5 min, results in `work/m0/`; ~2.5 min is Pono on
the clean design). Tools: `tools/oss-cad-suite` (Yosys, slang, SymbiYosys, ABC, rIC3, Pono,
Verilator) and `tools/hw-cbmc` (EBMC 6.0, built from source).

## The design

| File | What it is |
|---|---|
| `rtl/fifo.sv` | The DUT, which we wrote: a 4-deep valid/ready circular-buffer FIFO. `+define+BUG_<X>` injects one of four bugs |
| `sva/fifo_1r1w_props.sv` | The FVEval NL2SVA-Human `fifo_1r1w` reference model plus its 5 reference assertions, attached with `bind`. This is the file that would go to JasperGold unchanged |
| `env/reset_env.sv` | Equivalent of JasperGold `reset -expression !reset_`: forces reset in cycle 0 |
| `env/ebmc_top.sv` | Explicit wrapper, because EBMC parses neither `bind` nor `.*` |

**Why we had to write a DUT.** FVEval's NL2SVA-Human "testbenches" contain no design. They are
reference models whose DUT-side signals (`rd_vld`, `wr_ready`, `rd_data`) are free inputs,
because FVEval uses them for assertion-equivalence checking, not for bug hunting. To hunt bugs we
need a real DUT under the model.

| Bug | Change | Caught by | Min. CEX depth |
|---|---|---|---|
| `OVERFLOW` | `wr_ready` stays high when full | fifo_1 (no overflow) | 7 |
| `UNDERFLOW` | `rd_vld` claims data while empty | fifo_0 (no underflow) | 3 |
| `DATA` | read pointer not advanced on simultaneous push+pop | fifo_2 (data integrity) | 5 |
| `DEEP` | count corrupted only on push+pop at DEPTH-1 | fifo_0 | 10 |

## The flow

```
 fifo.sv + fifo_1r1w_props.sv (+bind) + reset_env.sv
        │  slang parse (50 ms, full SVA OK)
        ▼
 Yosys + slang plugin ── elaborates design; lowers *simple* SVA (bool + disable iff)
        │                to $check cells; REJECTS |-> ##[0:$] strong(...) (liveness)
        ▼                → liveness hidden behind `ifndef NO_LIVENESS
 SymbiYosys prep (async2sync, chformal, memory_map, ...)
        ├── AIGER ─► ABC bmc3 / ABC pdr / rIC3
        └── BTOR2 ─► Pono
                 │
                 ▼
 CEX (Yosys witness .yw) ─► yw2tb.py ─► standalone SV testbench
                 │
                 ▼
 Verilator --assert on the ORIGINAL RTL + SVA  ─►  CONFIRMED / NOT-REPRODUCED

 Separately: EBMC reads the original SV+SVA itself (its own front end) — second opinion.
```

## Results

| Variant | ABC bmc3 (d=30) | Pono BMC (d=30) | rIC3 (IC3) | ABC pdr (IC3) | EBMC BMC (d=20) | Replay |
|---|---|---|---|---|---|---|
| CLEAN | PASS 7 s | PASS 153 s | **PROVEN <1 s** | **PROVEN <1 s** | ok to 20 | — |
| OVERFLOW | CEX d7 | CEX d7 | CEX d7 | CEX d9 | fifo_0,1,2 refuted | 4/4 ✔ |
| UNDERFLOW | CEX d3 | CEX d3 | CEX d3 | CEX d3 | fifo_0,2 refuted | 4/4 ✔ |
| DATA | CEX d5 | CEX d5 | CEX d5–7 | CEX d5 | fifo_2 refuted | 4/4 ✔ |
| DEEP | CEX d10 | CEX d10 | CEX d10–11 | CEX d10 | fifo_0,1,2 refuted | 4/4 ✔ |

**No engine disagreed with another, and all 16 CEXs reproduced on the original RTL. Each
CEX trace also passes when replayed on the clean RTL.**

## Lessons that change or confirm the QuickFV design

1. **For a small design, IC3 is faster than BMC even for a pass.** rIC3 and ABC `pdr` *prove* the
   clean FIFO in under 1 s, while BMC to depth 30 takes 7 s (ABC) to 153 s (Pono). SymbiYosys's
   default `smtbmc` slows down exponentially (35 s for step 19 alone). **→ The portfolio should race IC3 from
   t=0, not only on long budgets.** A fast `PROVEN` is the best possible "fail fast" outcome, because
   it ends the question.
2. **IC3 CEXs are not minimal, and their length varies between runs** (rIC3: DATA 5–7, DEEP 10–11;
   ABC pdr: OVERFLOW 9 vs minimal 7). BMC always gives the shortest trace. **→ When an IC3 engine
   finds a CEX first, re-run BMC to that depth to minimize the trace before showing it to an agent.**
3. **Bounded "pass" on liveness is meaningless and dangerous.** EBMC reports `fifo_3`/`fifo_4` as
   "PROVED up to bound 20", even with `--liveness-to-safety`, yet both are **false**: the environment
   can hold `rd_ready=0` forever. **→ This confirms the v1 decision to return `UNSUPPORTED` for liveness
   instead of a misleading bounded result.**
4. **The name-mapping risk (SPEC §13) is real.** Yosys-internal registers (`$auto$async2sync...`)
   appear in witness traces with no RTL counterpart. The replay has to skip them and still reproduce
   the failure, which it does. **→ QuickFV's Model IR must tag every state element as RTL or
   synthetic.**
5. **Yosys+slang lowers more SVA than documented.** Simple boolean properties with
   `disable iff` elaborate to `$check` cells today. Only sequences, implication and liveness are
   rejected. **→ The QuickFV SVA compiler only has to handle what sits above that line.**
6. **EBMC's parser is fragile on idiomatic SV** (`bind`, `.*`), as the research predicted.
   **→ EBMC stays a semantic oracle for the SVA compiler, never a front end.**
7. **Don't-care bits.** Witnesses contain `?` for values that don't matter. The replay picks 0, and
   the CEX still reproduces. A future certifier could check both 0 and 1.
8. **FVEval NL2SVA-Human has no DUTs.** For the case study (SPEC §10) we write DUTs for its
   reference models, as here, or use Design2SVA, which has real RTL but no reference assertions.

## Files produced per run (`work/m0/`)
- `<VARIANT>_<engine>/`: SymbiYosys workdir (`model/` has the AIGER/BTOR2, `engine_0/trace.{yw,vcd}` the CEX)
- `replay/<VARIANT>_<engine>.sv`: the generated replay testbench (readable; worth opening)
