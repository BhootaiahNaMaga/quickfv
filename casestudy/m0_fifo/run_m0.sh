#!/usr/bin/env bash
# M0 reference flow, end to end:
#   1. every reference engine (via SymbiYosys) on the clean FIFO and each injected bug
#   2. every CEX replayed on the ORIGINAL RTL in Verilator (independent check)
#   3. EBMC on the original SVA as a second front end
# Usage: casestudy/m0_fifo/run_m0.sh        (results in work/m0/)
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
export PATH="$root/tools/oss-cad-suite/bin:$PATH"
ebmc="$root/tools/hw-cbmc/src/ebmc/ebmc"
work=${WORK:-$root/work/m0}
mkdir -p "$work/replay"

variants="CLEAN OVERFLOW UNDERFLOW DATA DEEP"
# smtbmc is left out: it slows down exponentially past depth ~15 on this design.
engines=${ENGINES:-"bmc_abc bmc_pono prove_ric3 prove_abc"}

printf "\n== 1+2. Reference engines, with CEX replay on the original RTL\n"
printf "%-10s %-11s %-5s %5s %4s  %-8s %s\n" variant engine result secs cex failed replay
for v in $variants; do
    def=""; [ "$v" != CLEAN ] && def="-D BUG_$v"
    sed "s/BUG_DEFINE/$def/" "$here/fifo.sby" > "$work/$v.sby"
    for e in $engines; do
        d="$work/${v}_$e"
        start=$(date +%s)
        (cd "$here" && sby -f -d "$d" "$work/$v.sby" "$e" >/dev/null 2>&1)
        secs=$(( $(date +%s) - start ))
        result=$(awk '{print $1}' "$d/status" 2>/dev/null)
        depth=$(grep -oE "Converted [0-9]+ time steps" "$d/logfile.txt" | grep -oE "[0-9]+" | head -1)
        failed="-"; replay="-"
        if [ -f "$d/engine_0/trace.yw" ]; then
            tb="$work/replay/${v}_$e.sv"
            python3 "$here/yw2tb.py" "$d/engine_0/trace.yw" fifo "$tb"
            obj="$work/replay/obj_${v}_$e"; rm -rf "$obj"
            if verilator --binary --timing --assert -Wno-fatal -Wno-lint -Wno-style \
                   --top-module qfv_replay -DNO_LIVENESS ${def//-D /-D} -Mdir "$obj" -o sim \
                   "$tb" "$here/rtl/fifo.sv" "$here/sva/fifo_1r1w_props.sv" \
                   "$here/env/reset_env.sv" > "$obj.log" 2>&1; then
                failed=$("$obj/sim" 2>&1 | grep -oE "Assertion failed in [^ :]*" | head -1 | grep -oE "fifo_[0-9]$")
                [ -n "$failed" ] && replay="CONFIRMED" || { replay="NOT-REPRODUCED"; failed="-"; }
            else
                replay="BUILD-ERROR"
            fi
        fi
        printf "%-10s %-11s %-5s %5s %4s  %-8s %s\n" "$v" "$e" "${result:-?}" "$secs" "${depth:--}" "$failed" "$replay"
    done
done

printf "\n== 3. EBMC on the original SVA (bound 20; wrapper top because EBMC lacks bind)\n"
for v in $variants; do
    def=""; [ "$v" != CLEAN ] && def="-D BUG_$v"
    res=$("$ebmc" --systemverilog -D NO_BIND $def --top ebmc_top --reset "ebmc_top.reset_==0" \
            --bound 20 "$here/rtl/fifo.sv" "$here/sva/fifo_1r1w_props.sv" "$here/env/ebmc_top.sv" 2>&1 \
          | grep -E "^\[" | sed -E 's/^\[ebmc_top.fifo_tb_inst.(fifo_[0-9])\].*: (PROVED up to bound 20|REFUTED)$/\1=\2/; s/PROVED up to bound 20/ok20/' | tr '\n' ' ')
    printf "%-10s %s\n" "$v" "$res"
done
