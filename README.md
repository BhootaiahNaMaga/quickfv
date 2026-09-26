# QuickFV (`qfv`)

A fast, agent-first formal pre-check for SystemVerilog assertions, run **before** JasperGold:
- lint in milliseconds;
- a vacuity check (can the trigger ever fire?);
- a time-bounded bug hunt, with every counterexample independently certified.

The full design is in [`SPEC.md`](SPEC.md); a one-page summary of what was built and measured is in
[`docs/SUMMARY.md`](docs/SUMMARY.md), and the case-study report in
[`casestudy/m6_report`](casestudy/m6_report/README.md).

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
| `CEX` | Failing trace (shortest found), replayed by btorsim, and by Verilator on the original RTL with `--replay` |
| `VACUOUS` | The trigger provably never fires |
| `PROVEN` | Holds for all time; reported only when rIC3's witness circuit passes Certifaiger |
| `POSSIBLY_VACUOUS` | Trigger not reached within the budget |
| `PASS_BOUNDED` | No CEX within the budget, and the trigger is reachable. **Not a proof** (checkable with `--certify`) |
| `TRIVIALLY_TRUE` | Can't fail (folded to a constant during synthesis; one-shot `qfv bmc` only) |
| `COVERED` / `UNREACHABLE` / `NOT_COVERED` | Cover properties: witness found / provably unreachable / not reached in the budget |
| `UNSUPPORTED` / `ERROR` | Outside the v1 SVA subset, with a reason and location / has errors |

Priority when several apply: `CEX`, then `VACUOUS`, then `PROVEN`. A CEX that needs a particular X
value carries an `x_dependent` note.

## Commands and options

| Command | What |
|---|---|
| `qfv lint` | T0: parse, types, supported-SVA check |
| `qfv compile-sva` / `check-sva` | Emit the synthesizable monitors / lint plus compile |
| `qfv bmc` | Full check (vacuity, simulation, BMC, induction, rIC3), NDJSON events on stdout |
| `qfv sim` | Random simulation only |
| `qfv serve` | Persistent session: JSON requests on stdin, events on stdout (`tests/serve_client.py`) |
| `qfv mcp` | The same session as an MCP server |

Main options: `--top`, `--clock`, `--reset-expr`, `--reset-cycles N`, `--reset-free`, `--budget S`,
`--max-depth K`, `--replay` (Verilator RTL replay of each CEX), `--certify` (LIDRUP certificates for
bounded claims), `--witness FILE`, `--sim S` (random-simulation time), `--btor FILE`, `--work DIR`, `--vacuity-covers`, `--drop-unsupported`, `-D`, `-I`.

Setup files use a JasperGold Tcl subset (`analyze`, `elaborate`, `clock`, `reset`, `assert`/`assume`/`cover`,
`prove`, `set`); `reset -cycles N` is a QuickFV extension. Examples: `casestudy/m6_report/designs/*_setup*.tcl`.

## Layout

| Path | What |
|---|---|
| `SPEC.md` | Full design spec |
| `docs/SUMMARY.md` | One-page summary of what was built and measured |
| `src/sva/` | SVA → IR (`lower`), text monitors (`monitor`), direct AIG monitors (`aig_emit`) |
| `src/model/` | AIG, BTOR2 reader (bit-blaster) and writer |
| `src/engine/` | unroller, BMC/induction hunt, random simulation, traces, rIC3 portfolio, certificates |
| `src/session/` | persistent design session, setup-file parser, reset environment |
| `tools_src/` | `qfv` CLI, `serve`, `mcp` |
| `tcl/` | Tcl package |
| `scripts/setup_tools.sh` | Fetches and builds the open-source tools into `tools/` |
| `docker/` | Linux container (untested) |
| `casestudy/m0…m6/` | Milestone write-ups with results and lessons (M1 is in `tests/sva_equiv/README.md`); `m6_report` is the FVEval case study |
| `tests/` | SVA equivalence (3 oracles), bit-blaster vs btorsim, session vs flow, MCP agent loop, Tcl smoke test, portfolio and scaling benchmarks |

## Tests

```
python3 tests/sva_equiv/run_equiv.py           # SVA compiler vs EBMC, Verilator, golden traces
python3 tests/bitblast/run_ops.py              # bit-blaster vs btorsim, per operator
python3 tests/session_equiv/run_session_equiv.py
python3 tests/mcp_agent_loop.py                # end-to-end agent loop over MCP
tclsh tests/tcl_smoke.tcl
```

## License

MIT, see [`LICENSE`](LICENSE). Third-party tools fetched by `scripts/setup_tools.sh` keep their own licenses.
