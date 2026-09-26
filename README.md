# QuickFV (`qfv`)

A fast, agent-first formal pre-check for SystemVerilog assertions, run **before** JasperGold:
- lint in milliseconds;
- a vacuity check (can the trigger ever fire?);
- a time-bounded bug hunt, with every counterexample independently certified.

The full design is in [`SPEC.md`](SPEC.md).

## Build and tools

```
scripts/setup_tools.sh          # OSS CAD Suite (Yosys, slang, rIC3, Verilator, btorsim, ...), EBMC, FVEval
cmake -S . -B build -G Ninja && cmake --build build
export PATH=$PWD/tools/oss-cad-suite/bin:$PATH
```

## Use it

**As an agent tool (MCP):**
```
claude mcp add quickfv -- $PWD/build/qfv mcp --work /tmp/qfv_session
```
Then: `load_design` (a JasperGold-style setup file) and `check_assertion` for each assertion.

**One-shot from the shell:**
```
qfv lint --top fifo rtl.sv props.sv                       # T0: syntax, types, supported SVA
qfv bmc  --top fifo --clock clk --reset-expr '!reset_' \
         --budget 600 --replay rtl.sv props.sv            # T1 + T2, NDJSON events
```

**From Tcl** (tclsh, or an EDA tool's Tcl shell): `source tcl/qfv.tcl`; `qfv::start`;
`qfv::load setup.tcl`; `qfv::check_assertion {req |=> ack}`.

## Verdicts

| Verdict | Meaning |
|---|---|
| `CEX` | Failing trace, replayed by btorsim (and by Verilator on the original RTL with `--replay`) |
| `VACUOUS` | The trigger provably never fires |
| `POSSIBLY_VACUOUS` | Trigger not reached within the budget |
| `PASS_BOUNDED` | No CEX within the budget, and the trigger is reachable. **Not a proof** |
| `TRIVIALLY_TRUE` | Can't fail (folded to a constant) |
| `UNSUPPORTED` / `ERROR` | Outside the v1 SVA subset, with a reason and location / has errors |

## Layout

| Path | What |
|---|---|
| `src/sva/` | SVA → IR (`lower`), text monitors (`monitor`), direct AIG monitors (`aig_emit`) |
| `src/model/` | AIG, BTOR2 reader (bit-blaster) and writer |
| `src/engine/` | unroller, BMC/induction hunt, random simulation, traces, external provers |
| `src/session/` | persistent design session, setup-file parser |
| `tools_src/` | `qfv` CLI, `serve`, `mcp` |
| `tcl/` | Tcl package |
| `casestudy/m0…m4/` | milestone write-ups with results and lessons |
| `tests/` | SVA equivalence (3 oracles), bit-blaster vs btorsim, session vs flow, MCP agent loop |
