analyze -sv12 arbiter_rr.sv
analyze -sv12 arbiter_rr_props.sv
elaborate -top arbiter_rr
clock clk
reset -expression {!reset_}
