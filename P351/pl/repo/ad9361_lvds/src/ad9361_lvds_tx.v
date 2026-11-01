`timescale 1ns / 1ps

module ad9361_lvds_tx (
    input  wire        rst,
    input  wire        data_clk,
    input  wire        sample_clk,
    input  wire        sample_locked,
    input  wire        dac_valid,
    input  wire [11:0] dac_data_i0,
    input  wire [11:0] dac_data_q0,
    input  wire [11:0] dac_data_i1,
    input  wire [11:0] dac_data_q1,
    output wire        tx_clk_out_p,
    output wire        tx_clk_out_n,
    output wire        tx_frame_out_p,
    output wire        tx_frame_out_n,
    output wire [5:0]  tx_data_out_p,
    output wire [5:0]  tx_data_out_n
);

// One 24-bit TX1 sample crosses through a 16-entry elastic queue. Pointer
// synchronization must never restart a frame already on the LVDS pins.
// Loss of the sample MMCM lock must clear both domains even if one of the
// clocks has stopped.  Release is synchronized independently to each clock.
wire tx_reset_async = rst | ~sample_locked;
(* ASYNC_REG = "TRUE" *) reg [1:0] sample_release;
(* ASYNC_REG = "TRUE" *) reg [1:0] data_release;
wire sample_ready = sample_release[1];
wire data_ready = data_release[1];

always @(posedge sample_clk or posedge tx_reset_async) begin
    if (tx_reset_async)
        sample_release <= 2'b00;
    else
        sample_release <= {sample_release[0], 1'b1};
end

always @(posedge data_clk or posedge tx_reset_async) begin
    if (tx_reset_async)
        data_release <= 2'b00;
    else
        data_release <= {data_release[0], 1'b1};
end

(* ram_style = "distributed" *) reg [23:0] sample_mem [0:15];
reg [4:0] wr_bin, wr_gray, rd_bin, rd_gray;
(* ASYNC_REG = "TRUE" *) reg [4:0] rd_gray_meta, rd_gray_sync;
(* ASYNC_REG = "TRUE" *) reg [4:0] wr_gray_meta, wr_gray_sync;
(* MARK_DEBUG = "TRUE" *) reg overflow_sticky;
(* MARK_DEBUG = "TRUE" *) reg underflow_sticky;
wire queue_full = wr_gray == {~rd_gray_sync[4:3], rd_gray_sync[2:0]};
wire queue_empty = rd_gray == wr_gray_sync;
wire [4:0] wr_next = wr_bin + 5'd1;
wire [4:0] rd_next = rd_bin + 5'd1;
function [4:0] gray_to_bin;
    input [4:0] gray;
    integer b;
    begin
        gray_to_bin[4] = gray[4];
        for (b=3; b>=0; b=b-1)
            gray_to_bin[b] = gray_to_bin[b+1] ^ gray[b];
    end
endfunction
wire [4:0] queued_samples = gray_to_bin(wr_gray_sync) - rd_bin;
// Do not reset RAM contents: only pointers control validity. This also permits
// dual-clock distributed-RAM inference.
always @(posedge sample_clk) begin
    if (sample_ready && !tx_reset_async && dac_valid && !queue_full)
        sample_mem[wr_bin[3:0]] <= {dac_data_i0,dac_data_q0};
end

always @(posedge sample_clk or posedge tx_reset_async) begin
    if (tx_reset_async) begin
        wr_bin <= 0; wr_gray <= 0;
        rd_gray_meta <= 0; rd_gray_sync <= 0;
        overflow_sticky <= 0;
    end else if (!sample_ready) begin
        wr_bin <= 0; wr_gray <= 0;
        rd_gray_meta <= 0; rd_gray_sync <= 0;
        overflow_sticky <= 0;
    end else begin
        rd_gray_meta <= rd_gray;
        rd_gray_sync <= rd_gray_meta;
        if (dac_valid) begin
            if (queue_full) overflow_sticky <= 1;
            else begin
                wr_bin <= wr_next;
                wr_gray <= (wr_next >> 1) ^ wr_next;
            end
        end
    end
end

reg tx_count;
reg [11:0] active_i0;
reg [11:0] active_q0;
reg [5:0] tx_rise;
reg [5:0] tx_fall;
reg       tx_frame;
reg       tx_active;

always @(posedge data_clk or posedge tx_reset_async) begin
    if (tx_reset_async) begin
        wr_gray_meta <= 0; wr_gray_sync <= 0;
        rd_bin <= 0; rd_gray <= 0;
        underflow_sticky <= 0;
        tx_count    <= 1'b0;
        active_i0   <= 12'd0;
        active_q0   <= 12'd0;
        tx_active   <= 1'b0;
    end else if (!data_ready) begin
        wr_gray_meta <= 0; wr_gray_sync <= 0;
        rd_bin <= 0; rd_gray <= 0;
        underflow_sticky <= 0;
        tx_count    <= 1'b0;
        active_i0   <= 12'd0;
        active_q0   <= 12'd0;
        tx_active   <= 1'b0;
    end else begin
        wr_gray_meta <= wr_gray;
        wr_gray_sync <= wr_gray_meta;
        if (tx_active)
            tx_count <= ~tx_count;
        // Startup prefill absorbs pointer latency and a delayed sync edge.
        // In 1R1T the sample occupies two DATA_CLK slots; update after slot 1.
        if ((!tx_active && queued_samples >= 5'd4) ||
            (tx_active && tx_count == 1'b1)) begin
            if (!queue_empty) begin
                {active_i0,active_q0} <= sample_mem[rd_bin[3:0]];
                rd_bin <= rd_next;
                rd_gray <= (rd_next >> 1) ^ rd_next;
                tx_count <= 0;
                tx_active <= 1;
            end else begin
                // Preserve frame length and the last complete sample if the
                // producer stops; expose the fault instead of splicing words.
                underflow_sticky <= 1;
            end
        end
    end
end

always @(*) begin
    tx_rise  = 6'd0;
    tx_fall  = 6'd0;
    tx_frame = 1'b0;
    if (tx_active && data_ready && sample_locked) begin
        case (tx_count)
            1'b0: begin tx_frame = 1'b1; tx_rise = active_i0[11:6]; tx_fall = active_q0[11:6]; end
            1'b1: begin tx_frame = 1'b0; tx_rise = active_i0[5:0];  tx_fall = active_q0[5:0];  end
        endcase
    end
end

wire tx_clk_se;
wire tx_frame_se;
wire [5:0] tx_data_se;

(* IOB = "TRUE" *) ODDR #(
    .DDR_CLK_EDGE("SAME_EDGE"), .INIT(1'b0), .SRTYPE("SYNC")
) tx_clk_oddr (
    .Q(tx_clk_se), .C(data_clk), .CE(1'b1), .D1(1'b0), .D2(1'b1),
    .R(1'b0), .S(1'b0)
);

(* IOB = "TRUE" *) ODDR #(
    .DDR_CLK_EDGE("SAME_EDGE"), .INIT(1'b0), .SRTYPE("ASYNC")
) tx_frame_oddr (
    .Q(tx_frame_se), .C(data_clk), .CE(1'b1),
    .D1(tx_frame), .D2(tx_frame), .R(tx_reset_async), .S(1'b0)
);

OBUFDS #(.IOSTANDARD("LVDS_25"), .SLEW("FAST")) tx_clk_obufds (
    .I(tx_clk_se), .O(tx_clk_out_p), .OB(tx_clk_out_n)
);
OBUFDS #(.IOSTANDARD("LVDS_25"), .SLEW("FAST")) tx_frame_obufds (
    .I(tx_frame_se), .O(tx_frame_out_p), .OB(tx_frame_out_n)
);

genvar lane;
generate
    for (lane = 0; lane < 6; lane = lane + 1) begin : tx_lane
        (* IOB = "TRUE" *) ODDR #(
            .DDR_CLK_EDGE("SAME_EDGE"), .INIT(1'b0), .SRTYPE("ASYNC")
        ) tx_data_oddr (
            .Q(tx_data_se[lane]), .C(data_clk), .CE(1'b1),
            .D1(tx_rise[lane]), .D2(tx_fall[lane]),
            .R(tx_reset_async), .S(1'b0)
        );
        OBUFDS #(.IOSTANDARD("LVDS_25"), .SLEW("FAST")) tx_data_obufds (
            .I(tx_data_se[lane]), .O(tx_data_out_p[lane]),
            .OB(tx_data_out_n[lane])
        );
    end
endgenerate

endmodule
