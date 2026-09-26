// Round-robin arbiter DUT for the FVEval arbiter_rr reference model.
// Written from the NL2SVA prompts: one-hot grant to a requester, none while
// busy, hold keeps the previous grant, round robin starting at client 0.
// cont_gnt (continued grant) is not implemented and tied to 0.
// Bugs for the case study: +define+BUG_<NAME>.
module arbiter_rr #(parameter NUM_OF_CLIENTS = 6) (
    input clk, input reset_,
    input [NUM_OF_CLIENTS-1:0] req,
    input busy, input hold,
    output reg [NUM_OF_CLIENTS-1:0] gnt,
    output reg [$clog2(NUM_OF_CLIENTS)-1:0] gnt_id,
    output cont_gnt
);
    localparam N = NUM_OF_CLIENTS;
    reg [N-1:0] last_gnt;
    assign cont_gnt = 1'b0;

    // Round robin: the first requester after the last granted client.
    integer i, last_idx;
    reg found;
    always @(*) begin
        last_idx = N - 1;           // nothing granted yet: start at client 0
        for (i = 0; i < N; i = i + 1)
            if (last_gnt[i]) last_idx = i;
        gnt = '0;
        found = 1'b0;
        if (busy) begin
`ifdef BUG_BUSY
            gnt = req & -req;       // ignores busy: grants the lowest requester
`endif
        end else if (hold && |last_gnt) begin
            gnt = last_gnt;
        end else begin
            for (i = 1; i <= N; i = i + 1)
                if (!found && req[(last_idx + i) % N]) begin
`ifdef BUG_RR
                    gnt[(last_idx + i + ((last_idx + i) % N == 0 ? 1 : 0)) % N] = 1'b1; // skips client 0 on wrap
`else
                    gnt[(last_idx + i) % N] = 1'b1;
`endif
                    found = 1'b1;
                end
        end
        gnt_id = '0;
        for (i = 0; i < N; i = i + 1)
`ifdef BUG_ID
            if (gnt[i] && i != 3) gnt_id = i;  // wrong id for client 3
`else
            if (gnt[i]) gnt_id = i;
`endif
    end

    always @(posedge clk) begin
        if (!reset_) last_gnt <= '0;
`ifdef BUG_LAST
        else if (|gnt && !hold) last_gnt <= gnt & {N{~req[0]}}; // forgets grants while client 0 requests
`else
        else if (|gnt) last_gnt <= gnt;
`endif
    end
endmodule
