// EBMC parses neither `bind` nor `.*` port connections, so for EBMC runs the DUT
// and the FVEval checker are instantiated side by side here (compile with
// -D NO_BIND).
module ebmc_top (input clk, input reset_, input wr_vld, input wr_data, input rd_ready);
    wire wr_ready, rd_vld, rd_data;
    fifo dut (.clk(clk), .reset_(reset_), .wr_vld(wr_vld), .wr_data(wr_data),
              .wr_ready(wr_ready), .rd_vld(rd_vld), .rd_data(rd_data), .rd_ready(rd_ready));
    fifo_1r1w_tb fifo_tb_inst (.clk(clk), .reset_(reset_), .wr_vld(wr_vld), .wr_data(wr_data),
              .wr_ready(wr_ready), .rd_vld(rd_vld), .rd_data(rd_data), .rd_ready(rd_ready));
endmodule
