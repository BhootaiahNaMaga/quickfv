// Simple valid/ready FIFO (circular buffer) used as the DUT for the FVEval
// fifo_1r1w reference model. Bugs are injected with +define+BUG_<NAME>.
module fifo #(
    parameter FIFO_DEPTH = 4,
    parameter DATA_WIDTH = 1
) (
    input                   clk,
    input                   reset_,
    input                   wr_vld,
    input  [DATA_WIDTH-1:0] wr_data,
    output                  wr_ready,
    output                  rd_vld,
    output [DATA_WIDTH-1:0] rd_data,
    input                   rd_ready
);
    localparam PW = $clog2(FIFO_DEPTH);

    reg [DATA_WIDTH-1:0] mem [FIFO_DEPTH-1:0];
    reg [PW-1:0]         wr_ptr, rd_ptr;
    reg [PW:0]           count;

`ifdef BUG_OVERFLOW
    assign wr_ready = (count <= FIFO_DEPTH);          // accepts a push when full
`else
    assign wr_ready = (count != FIFO_DEPTH);
`endif

`ifdef BUG_UNDERFLOW
    assign rd_vld = (count != 0) || wr_vld;           // claims data while empty
`else
    assign rd_vld = (count != 0);
`endif

    assign rd_data = mem[rd_ptr];

    wire push = wr_vld && wr_ready;
    wire pop  = rd_vld && rd_ready;

    always @(posedge clk) begin
        if (!reset_) begin
            wr_ptr <= 'd0;
            rd_ptr <= 'd0;
            count  <= 'd0;
        end else begin
            if (push) begin
                mem[wr_ptr] <= wr_data;
                wr_ptr      <= wr_ptr + 1'b1;
            end
`ifdef BUG_DATA
            // read pointer is not advanced on a simultaneous push+pop
            if (pop && !push) rd_ptr <= rd_ptr + 1'b1;
`else
            if (pop) rd_ptr <= rd_ptr + 1'b1;
`endif
`ifdef BUG_DEEP
            // count is corrupted only when the FIFO is exactly one short of
            // full and a push and pop happen together: needs a longer trace
            if (push && pop && count == FIFO_DEPTH - 1) count <= count + 1'b1;
            else
`endif
            count <= count + push - pop;
        end
    end
endmodule
