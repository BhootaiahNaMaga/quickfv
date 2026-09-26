// Equivalent of JasperGold `reset -expression !reset_`: reset is asserted in the
// first cycle, so the search starts from the reset state instead of an
// arbitrary one. QuickFV's `reset` setup command will generate this.
module reset_env (input clk, input reset_);
    reg first_cycle = 1'b1;
    always @(posedge clk) first_cycle <= 1'b0;
    always @(*) if (first_cycle) assume (!reset_);
endmodule

bind fifo reset_env reset_env_inst (.clk(clk), .reset_(reset_));
