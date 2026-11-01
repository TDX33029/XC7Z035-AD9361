`timescale 1ns / 1ps

// AD9361 1R1T LVDS interface.
// At 61.44 MSPS the differential DATA_CLK is 122.88 MHz.  data_clk is the
// high-speed serializer clock; sample_clk is a synchronous divide-by-two
// clock used by waveform/modulator logic.
module ad9361_lvds_phy #(
    parameter integer RX_ENABLE = 1
)(
    input  wire        ref_clk,
    input  wire        rst,

    input  wire        rx_clk_in_p,
    input  wire        rx_clk_in_n,
    input  wire        rx_frame_in_p,
    input  wire        rx_frame_in_n,
    input  wire [5:0]  rx_data_in_p,
    input  wire [5:0]  rx_data_in_n,

    output wire        tx_clk_out_p,
    output wire        tx_clk_out_n,
    output wire        tx_frame_out_p,
    output wire        tx_frame_out_n,
    output wire [5:0]  tx_data_out_p,
    output wire [5:0]  tx_data_out_n,

    output wire        data_clk,
    output wire        sample_clk,
    output wire        sample_locked,
    output wire        idelay_ready,

    output wire        adc_valid,
    output wire [11:0] adc_data_i0,
    output wire [11:0] adc_data_q0,
    output wire [11:0] adc_data_i1,
    output wire [11:0] adc_data_q1,

    input  wire        dac_valid,
    input  wire [11:0] dac_data_i0,
    input  wire [11:0] dac_data_q0,
    input  wire [11:0] dac_data_i1,
    input  wire [11:0] dac_data_q1,

    input  wire [4:0]  delay_tap,
    input  wire [6:0]  delay_reload
);

wire rx_clk_ibuf;
wire rx_io_clk;
IBUFDS #(
    // Vendor P351 design requires Bank10=2.5V; verify actual rail before use.
    .DIFF_TERM("TRUE"),
    .IBUF_LOW_PWR("FALSE"),
    .IOSTANDARD("LVDS_25")
) rx_clk_ibufds (
    .I(rx_clk_in_p), .IB(rx_clk_in_n), .O(rx_clk_ibuf)
);

BUFG rx_clk_bufg (.I(rx_clk_ibuf), .O(data_clk));
BUFIO rx_clk_bufio (.I(rx_clk_ibuf), .O(rx_io_clk));

// 122.88 MHz * 8 / 16 = 61.44 MHz.  This MMCM intentionally targets the
// final-rate interface.  sample_locked remains low until AD9361 has been
// configured for the 122.88 MHz LVDS DATA_CLK.
wire mmcm_fb;
wire mmcm_fb_buf;
wire sample_clk_mmcm;
MMCME2_BASE #(
    .BANDWIDTH("OPTIMIZED"),
    .CLKIN1_PERIOD(8.138),
    .DIVCLK_DIVIDE(1),
    .CLKFBOUT_MULT_F(8.000),
    .CLKOUT0_DIVIDE_F(16.000),
    .STARTUP_WAIT("FALSE")
) sample_mmcm (
    .CLKIN1(data_clk),
    .RST(rst),
    .PWRDWN(1'b0),
    .CLKFBIN(mmcm_fb_buf),
    .CLKFBOUT(mmcm_fb),
    .CLKOUT0(sample_clk_mmcm),
    .LOCKED(sample_locked)
);
BUFG sample_fb_bufg (.I(mmcm_fb), .O(mmcm_fb_buf));
BUFG sample_clk_bufg (.I(sample_clk_mmcm), .O(sample_clk));

generate if (RX_ENABLE != 0) begin : gen_rx
ad9361_lvds_rx rx_path (
    .ref_clk(ref_clk),
    .rst(rst),
    .data_clk(data_clk),
    .capture_clk(rx_io_clk),
    .rx_frame_in_p(rx_frame_in_p),
    .rx_frame_in_n(rx_frame_in_n),
    .rx_data_in_p(rx_data_in_p),
    .rx_data_in_n(rx_data_in_n),
    .delay_tap(delay_tap),
    .delay_reload(delay_reload),
    .idelay_ready(idelay_ready),
    .adc_valid(adc_valid),
    .adc_data_i0(adc_data_i0),
    .adc_data_q0(adc_data_q0),
    .adc_data_i1(adc_data_i1),
    .adc_data_q1(adc_data_q1)
);
end else begin : gen_tx_only
    assign idelay_ready = 1'b0;
    assign adc_valid = 1'b0;
    assign adc_data_i0 = 12'd0;
    assign adc_data_q0 = 12'd0;
    assign adc_data_i1 = 12'd0;
    assign adc_data_q1 = 12'd0;
end endgenerate

ad9361_lvds_tx tx_path (
    .rst(rst),
    .data_clk(data_clk),
    .sample_clk(sample_clk),
    .sample_locked(sample_locked),
    .dac_valid(dac_valid),
    .dac_data_i0(dac_data_i0),
    .dac_data_q0(dac_data_q0),
    .dac_data_i1(dac_data_i1),
    .dac_data_q1(dac_data_q1),
    .tx_clk_out_p(tx_clk_out_p),
    .tx_clk_out_n(tx_clk_out_n),
    .tx_frame_out_p(tx_frame_out_p),
    .tx_frame_out_n(tx_frame_out_n),
    .tx_data_out_p(tx_data_out_p),
    .tx_data_out_n(tx_data_out_n)
);

endmodule
