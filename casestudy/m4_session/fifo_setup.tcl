# JasperGold-style setup for the M0 FIFO (the same commands a JasperGold run would use).
clear -all
set RTL ../m0_fifo/rtl
analyze -sv12 +define+BUG_DATA $RTL/fifo.sv
analyze -sv12 ../m0_fifo/sva/fifo_1r1w_props.sv
elaborate -top fifo
clock clk
reset -expression {!reset_}
# A property written directly in the setup file (top scope):
assert -name p_ready_when_empty {count == 0 |-> wr_ready}
set_prove_time_limit 30s
prove -all
