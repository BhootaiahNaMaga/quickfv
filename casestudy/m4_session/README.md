# M4 — Agent interface: persistent session, MCP, Tcl, JasperGold handoff

The design is loaded **once**. After that, adding, editing or removing an assertion or an
assumption **never re-runs Yosys**: each assertion is compiled straight into the loaded circuit.

## Three front doors, one session

| Front end | For | Transport |
|---|---|---|
| `qfv mcp` | LLM agents (Claude Code, any MCP host) | MCP, JSON-RPC 2.0 over stdio |
| `qfv serve` | scripts, and the Tcl package | newline-delimited JSON over stdio, events streamed |
| `tcl/qfv.tcl` | engineers in tclsh or an EDA tool's Tcl shell (e.g. JasperGold) | a pipe to `qfv serve` |

**MCP tools:** `load_design`, `check_assertion` (lint + add + check in one call), `add_assumption`,
`check_all`, `get_trace` (filter by signal names and cycle window), `list_assertions`, `remove`,
`export_jaspergold`. Assertions can be full directives or bare properties
(`wr_push |=> !fifo_empty`, optionally with `disable_iff`), clocked on the design clock.

To use it from Claude Code:
```
claude mcp add quickfv -- /path/to/FV_engines/build/qfv mcp --work /path/to/session_dir
```
(`rIC3`, `btorsim`, `yosys` must be on `PATH`, or set `QFV_RIC3`/`QFV_BTORSIM`/`QFV_YOSYS`.)

## Setup files: a JasperGold Tcl subset

```tcl
set RTL ../m0_fifo/rtl
analyze -sv12 +define+BUG_DATA $RTL/fifo.sv
analyze -sv12 ../m0_fifo/sva/fifo_1r1w_props.sv
elaborate -top fifo            ;# -parameter NAME VALUE supported
clock clk
reset -expression {!reset_}
assert -name p_ready_when_empty {count == 0 |-> wr_ready}
set_prove_time_limit 30s
prove -all
```
Supported: `analyze`, `elaborate`, `clock`, `reset`, `assert`/`assume`/`cover`, `prove`,
`set_prove_time_limit`, `clear`, `set`/`$var`, `{}`/`""` quoting, comments. **Anything else is an
error with a line number**, never ignored.

## How "no re-synthesis" works

```
load:  sources ─► slang ─► strip ALL assertions ─► Yosys ONCE (every named wire kept + exposed)
                                                        │
                                            BTOR2 ─► AIG + name map ("fifo_tb_inst.wr_push" → bits)
add:   SVA text ─► insert into its module, re-parse THAT file only ─► slang binds it ─► lower (M1)
       ─► SV expression compiler: slang's typed AST → AIG gates over the name map
       ─► monitor registers become new latches; properties get appended
check: hunt (M3) on the chosen properties, reusing the session's unrollers
```

- **Assumptions** use activation literals, so `remove` switches one off without rebuilding the
  solver. Earlier results are marked **stale**.
- The session rebuilds its SAT encoding when it passes 2M clauses. The model stays loaded; see
  the lessons below for why.
- Traces from the in-memory model are certified with btorsim. For that, the AIG (monitors
  included) is written out as BTOR2 (`src/model/btor2_write.cpp`). rIC3 uses the same file for
  vacuity proofs.

## Results

**Direct AIG emission vs the Yosys flow** (`tests/session_equiv/run_session_equiv.py`, all 46
properties from M1's list, with and without `disable iff`):
- **46/46 agree.** Verdicts are the same, and CEXs are exactly one frame shorter, because Yosys
  registers a clocked assertion's condition (M2 lesson 3).
- Every session CEX is btorsim-confirmed **and** fails the M1-validated text monitor in Verilator.
- Each check takes 20–90 ms end to end: lint, emit, hunt and certify.

**The agent loop through MCP only** (`tests/mcp_agent_loop.py`, 14/14 checks):

| Step | Result | Time |
|---|---|---|
| load the FIFO (setup file) | Yosys once | 126 ms |
| assertion with a typo | *"undeclared identifier 'fifo_emtpy'; did you mean 'fifo_empty'?"* | 1 ms |
| fixed assertion | trigger reachable, PASS_BOUNDED | 5 s (the budget) |
| vacuous assertion | VACUOUS; the search stops as soon as it's proven | 4 ms |
| file assertions | fifo_2: CEX (the DATA bug) | budget |
| read the CEX | filtered trace, 4 cycles | 0 ms |
| add assumption "no push+pop" | fifo_2 → PASS_BOUNDED; earlier results marked stale | 1 ms + budget |
| remove the assumption | fifo_2 → CEX again | 67 ms |
| JasperGold handoff | session assertions rewritten into top scope, verdicts as comments, `prove -property` on what is open | – |
| Yosys runs after load | **none** | – |

**M3's vacuity set through the session:** the same verdicts as M3 (7/7 VACUOUS, 3/3 reachable).
Induction needs one step less (k=0/1), because direct emission has no extra sampling frame.

**Tcl package** (`tests/tcl_smoke.tcl`, tclsh 8.5): load, check, vacuity, CEX and export all work.

## Lessons

1. **The Yosys recipe order silently changed the model.** Flattening *before* mapping memories
   merged a register array into one misnamed 4-bit vector. The session then produced a CEX that
   btorsim confirmed on our model but that **did not reproduce on the RTL**. RTL replay caught it.
   The fix: keep all wires, map memories, then flatten. The emitter now also **refuses** a name
   whose width disagrees with the RTL, instead of truncating it.
2. **`expose` moves names from states to outputs.** With every wire exposed, Yosys names the
   *output*, and every state comes out anonymous. The loader now names a state from the output
   that reads it.
3. **btorsim crashes on a negated `constraint` argument.** That's valid BTOR2, but btorsim
   segfaults (exit 139) *after* printing its verdict, so a quick look at its output misses it. The
   writer now emits explicit `not` nodes. Worth reporting upstream.
4. **CaDiCaL's "lucky phases" ignore the terminator.** On a large incremental unrolling, that
   preprocessing ran for 10+ minutes past a 5 s budget. Disabled for BMC.
5. **Persistence needs a size limit.** 20k frames left over from one deep check cut the next check
   to depth 3. Rebuilding the encoding past 2M clauses brought it back to depth 843.
6. **slang takes each buffer path once.** Every re-parse of an edited file needs a unique path.

## Not in M4

- In-session **RTL replay** (Verilator on the original sources plus session assertions). Session
  traces are btorsim-certified, and the replay testbench is written, but automatic replay stays in
  the M3 CLI flow (`qfv bmc --replay`).
- **Parameter-dependent assertions** in modules instantiated with different parameters (one text,
  several lowerings) are rejected, as in M1.
- **Setup-file `assert` in a non-top scope:** JasperGold evaluates setup properties at the top, and
  so does QuickFV.
