// FVEval NL2SVA-Human 'arbiter_rr' reference model (Apache-2.0, NVIDIA) with its
// reference assertions, bound to our DUT 'arbiter_rr'.
module arbiter_rr_tb (
clk, reset_, req, gnt, gnt_id, busy, hold, cont_gnt);

    parameter       NUM_OF_CLIENTS = 6;

localparam tb_num_client_req_width = $clog2(NUM_OF_CLIENTS); 
localparam busy_latency_width = $clog2(NUM_OF_CLIENTS);
localparam cont_gnt_latency_width = $clog2(NUM_OF_CLIENTS);
localparam hold_latency_width = $clog2(NUM_OF_CLIENTS);

input clk;
input reset_;//clock and reset
input busy; //busy 
input hold;//hold
input [NUM_OF_CLIENTS-1 : 0] req;
input [NUM_OF_CLIENTS-1 : 0] gnt;//request and grant. grant is assumed to be one hot.
input [tb_num_client_req_width-1:0] gnt_id;
input cont_gnt; //same as cont_gnt from arbgen


wire tb_reset;
assign tb_reset = (reset_ == 1'b0);

wire [NUM_OF_CLIENTS-1 : 0] tb_req;
wire [NUM_OF_CLIENTS-1 : 0] tb_gnt;
wire [NUM_OF_CLIENTS-1 : 0] tb_req_for_starvation;
wire [NUM_OF_CLIENTS-1 : 0] tb_hold;

genvar a;
assign tb_req = req;
assign tb_gnt = gnt;
assign tb_req_for_starvation = req;

reg [NUM_OF_CLIENTS-1 : 0] last_gnt;
always @(posedge clk) begin
    if (!reset_) begin 
        last_gnt <= 0; 
   end else if (|tb_gnt && !cont_gnt) begin 
        last_gnt <= tb_gnt; 
    end
end

genvar i;
reg [NUM_OF_CLIENTS-1 : 0] req_seen_flag;
for (i = 0; i < NUM_OF_CLIENTS; i++) begin
    always @(posedge clk) begin
        if (!reset_) begin req_seen_flag[i] <= 0; end
        else if (tb_req_for_starvation[i] && tb_gnt[i]) begin req_seen_flag[i] <= 0; end
        else if (tb_req_for_starvation[i]) begin req_seen_flag[i] <= 1; end
        else if (tb_gnt[i]) begin req_seen_flag[i] <= 0; end
    end
end

wire [NUM_OF_CLIENTS-1 : 0] valid_request_mask_rr = (last_gnt < tb_gnt) ? (tb_gnt - last_gnt - last_gnt) : (
                                                (last_gnt > tb_gnt) ? ~(last_gnt - tb_gnt | last_gnt) :
                                                ~tb_gnt);


// ---- FVEval reference assertions ----
// arbiter_0: that the arbiter grant signal is 0-1-hot. Use the signal 'tb_gnt'.
arbiter_0: assert property (@(posedge clk) disable iff (tb_reset)
    !($onehot0(tb_gnt)) !== 1'b1
);
// arbiter_3: that if there is a req, there will be a grant except when busy. Use the signals 'tb_req', 'busy', and 'tb_gnt'.
arbiter_3: assert property (@(posedge clk) disable iff (tb_reset)
    (!busy && |tb_req && (tb_gnt == 'd0)) !== 1'b1
);
// arbiter_4: that each grant must be to a requesting client only. Use the signals 'tb_req' and 'tb_gnt'.
arbiter_4: assert property (@(posedge clk) disable iff (tb_reset)
    (|tb_gnt && ((tb_gnt & tb_req) == 'd0)) !== 1'b1
);
// arbiter_5: that no grants are made when the arbiter downstream is busy. Use the signals 'tb_gnt', 'hold', 'busy', and 'last_gnt'.
arbiter_5: assert property (@(posedge clk) disable iff (tb_reset)
    (|tb_gnt && busy) !== 1'b1
);
// arbiter_6: that the arbiter holds onto grants when there is hold. Use the signals 'tb_gnt', 'hold', and 'tb_req'.
arbiter_6: assert property (@(posedge clk) disable iff (tb_reset)
    (hold && !busy && (tb_gnt != last_gnt)) !== 1'b1 
);
// arbiter_7: that the given arbiter follows the Round Robin policy: start with 0th client and keep granting from 0 to n clients one by one except those that are no
arbiter_7: assert property (@(posedge clk) disable iff (tb_reset)
    (|last_gnt && |tb_gnt && !hold && (|(valid_request_mask_rr & tb_req))) !== 1'b1
);
// arbiter_8: that the first grant of the arbiter follows the Round Robin policy: start with 0th client and keep granting from 0 to n clients one by one except thos
arbiter_8: assert property (@(posedge clk) disable iff (tb_reset)
    ((last_gnt === 'd0) && |tb_gnt && ( ((tb_gnt-'d1) & tb_req) !== 'd0)) !== 1'b1
);
// arbiter_9: that the arbiter is never on hold or busy or on continued grant at the same time. Use the signals 'tb_req', 'tb_gnt', 'hold', 'valid_request_mask_rr',
arbiter_9: assert property (@(posedge clk) disable iff (tb_reset)
    !$onehot0({hold,busy,cont_gnt}) !== 1'b1
);
// arbiter_10: that each grant id is proper. Use the signals 'tb_gnt' and 'gnt_id'.
arbiter_10: assert property (@(posedge clk) disable iff (tb_reset)
    (|tb_gnt && (tb_gnt[gnt_id] != 1'b1)) !== 1'b1
);
// arbiter_11: that each grant id is proper, for the case where there no grants yet.Use the signals 'tb_gnt' and 'gnt_id'.
arbiter_11: assert property (@(posedge clk) disable iff (tb_reset)
    ((tb_gnt == 0) && (gnt_id != 0)) !== 1'b1
);
endmodule

bind arbiter_rr arbiter_rr_tb #(.NUM_OF_CLIENTS(NUM_OF_CLIENTS)) tb_inst (.*);
