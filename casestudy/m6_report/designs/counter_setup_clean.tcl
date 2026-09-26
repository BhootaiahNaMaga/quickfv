analyze -sv12 counter.sv
analyze -sv12 counter_props.sv
elaborate -top counter
clock clk
reset -expression {!reset_} -cycles 2
