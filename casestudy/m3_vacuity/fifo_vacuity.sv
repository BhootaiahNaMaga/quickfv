// Vacuity test set for the M0 FIFO (casestudy/m0_fifo/rtl/fifo.sv).
// r*: triggers that can fire (expect REACHABLE, i.e. not vacuous).
// v*: injected vacuity; triggers that can never fire (expect VACUOUS), each
//     needing a different kind of proof.
module fifo_vacuity #(parameter FIFO_DEPTH = 4) (
    input clk, input reset_,
    input wr_vld, input wr_ready, input rd_vld, input rd_ready,
    input [2:0] count, input [1:0] rd_ptr, input [1:0] wr_ptr
);
    // --- reachable triggers ---
    r1_push_nonempty: assert property (@(posedge clk) disable iff (!reset_)
        wr_vld && wr_ready && !(rd_vld && rd_ready) |=> rd_vld);
    r2_full_blocks: assert property (@(posedge clk) disable iff (!reset_)
        (count == FIFO_DEPTH) |-> !wr_ready);
    r3_drain: assert property (@(posedge clk) disable iff (!reset_)
        rd_vld && rd_ready && !wr_vld ##1 !rd_vld |-> count == 0);

    // --- injected vacuity ---
    // combinational contradiction through the DUT's wires (wr_ready = count != DEPTH)
    v1_comb: assert property (@(posedge clk) disable iff (!reset_)
        wr_ready && (count == FIFO_DEPTH) |-> rd_vld);
    // width mistake: a 2-bit pointer never equals 7
    v2_width: assert property (@(posedge clk) disable iff (!reset_)
        (rd_ptr == 3'd7) |-> rd_vld);
    // the trigger is exactly the disable condition
    v3_disabled: assert property (@(posedge clk) disable iff (!reset_)
        !reset_ |-> rd_vld);
    // contradiction across cycles: count cannot drop from DEPTH to 0 in one cycle
    v4_sequence: assert property (@(posedge clk) disable iff (!reset_)
        (count == FIFO_DEPTH) ##1 (count == 0) |-> rd_vld);
    // state invariant: count never exceeds DEPTH (needs IC3; not k-inductive)
    v5_invariant: assert property (@(posedge clk) disable iff (!reset_)
        (count == 3'd5) |-> rd_vld);
    // state invariant across registers: wr_ptr - rd_ptr == count (mod 4)
    v6_pointers: assert property (@(posedge clk) disable iff (!reset_)
        (count == 0) && (wr_ptr != rd_ptr) |-> rd_vld);
    // contradiction with a sampled-value function
    v7_rose_const: assert property (@(posedge clk) disable iff (!reset_)
        $rose(count == 3'd6) |-> rd_vld);
endmodule

bind fifo fifo_vacuity #(.FIFO_DEPTH(FIFO_DEPTH)) fifo_vacuity_inst (.*);
