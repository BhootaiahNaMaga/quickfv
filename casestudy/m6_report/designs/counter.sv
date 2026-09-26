// Counter DUT for the FVEval counter reference model: saturating counter in
// [min, max] with increment, decrement and jump (jump wins).
// Bugs: +define+BUG_<NAME>.
module counter #(parameter width = 4, parameter min = 2, parameter [width:0] max = 12) (
    input clk, input reset_,
    input incr_vld, input [width-1:0] incr_value,
    input decr_vld, input [width-1:0] decr_value,
    input jump_vld, input [width-1:0] jump_value,
    output reg [width-1:0] count
);
    wire [width-1:0] eff_incr = {width{incr_vld}} & incr_value;
    wire [width-1:0] eff_decr = {width{decr_vld}} & decr_value;
    wire signed [width+2:0] ssum = $signed({3'b0, count}) + $signed({3'b0, eff_incr}) - $signed({3'b0, eff_decr});
    always @(posedge clk) begin
        if (!reset_) count <= min;
        else if (jump_vld) count <= jump_value;
`ifdef BUG_WRAP
        else if (ssum > $signed({2'b0, max})) count <= min;     // wraps instead of saturating
`else
        else if (ssum > $signed({2'b0, max})) count <= max[width-1:0];
`endif
`ifdef BUG_FLOOR
        else if (ssum < min - 1) count <= min;                  // off by one at the floor
`else
        else if (ssum < min) count <= min;
`endif
`ifdef BUG_IDLE
        else if (!incr_vld && !decr_vld) count <= count + (count == 7); // glitch at 7 when idle
`endif
        else count <= ssum[width-1:0];
    end
endmodule
