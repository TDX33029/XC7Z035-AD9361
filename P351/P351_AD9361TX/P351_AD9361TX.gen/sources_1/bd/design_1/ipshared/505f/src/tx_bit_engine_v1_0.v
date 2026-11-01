`timescale 1 ns / 1 ps

module tx_bit_engine_v1_0 #(
    parameter integer C_S00_AXI_DATA_WIDTH = 32,
    parameter integer C_S00_AXI_ADDR_WIDTH = 6,
    parameter integer AXIS_CLK_HZ = 40000000,
    parameter integer BUFFER_WORDS = 4096
) (
    input  wire                                  s00_axi_aclk,
    input  wire                                  s00_axi_aresetn,
    input  wire [C_S00_AXI_ADDR_WIDTH-1:0]       s00_axi_awaddr,
    input  wire [2:0]                            s00_axi_awprot,
    input  wire                                  s00_axi_awvalid,
    output wire                                  s00_axi_awready,
    input  wire [C_S00_AXI_DATA_WIDTH-1:0]       s00_axi_wdata,
    input  wire [(C_S00_AXI_DATA_WIDTH/8)-1:0]   s00_axi_wstrb,
    input  wire                                  s00_axi_wvalid,
    output wire                                  s00_axi_wready,
    output wire [1:0]                            s00_axi_bresp,
    output wire                                  s00_axi_bvalid,
    input  wire                                  s00_axi_bready,
    input  wire [C_S00_AXI_ADDR_WIDTH-1:0]       s00_axi_araddr,
    input  wire [2:0]                            s00_axi_arprot,
    input  wire                                  s00_axi_arvalid,
    output wire                                  s00_axi_arready,
    output wire [C_S00_AXI_DATA_WIDTH-1:0]       s00_axi_rdata,
    output wire [1:0]                            s00_axi_rresp,
    output wire                                  s00_axi_rvalid,
    input  wire                                  s00_axi_rready,

    input  wire                                  axis_clk,
    input  wire                                  axis_aresetn,
    input  wire [31:0]                           s_axis_tdata,
    input  wire [3:0]                            s_axis_tkeep,
    input  wire                                  s_axis_tvalid,
    output wire                                  s_axis_tready,
    input  wire                                  s_axis_tlast,
    output wire                                  bit_data,
    output wire                                  bit_valid,
    input  wire                                  bit_ready,
    output wire                                  frame_start,
    output wire                                  frame_end,
    output wire                                  busy,
    output wire                                  done,
    output wire                                  underrun,
    output wire                                  error,
    output wire [31:0]                           bits_sent,
    output wire [31:0]                           bit_phase,
    output wire                                  underrun_irq
);

wire [31:0] cfg_bit_count_axi;
wire [31:0] cfg_repeat_count_axi;
wire [31:0] cfg_profile_axi;
wire [31:0] cfg_channel_axi;
wire [31:0] cfg_bit_rate_axi;
wire [31:0] cfg_rate_word_axi;
wire start_toggle_axi;
wire stop_toggle_axi;
wire clear_toggle_axi;

(* ASYNC_REG = "TRUE" *) reg [31:0] bit_count_meta, bit_count_sync;
(* ASYNC_REG = "TRUE" *) reg [31:0] bit_rate_meta, bit_rate_sync;
(* ASYNC_REG = "TRUE" *) reg [31:0] rate_word_meta, rate_word_sync;
(* ASYNC_REG = "TRUE" *) reg [31:0] repeat_meta, repeat_sync;
(* ASYNC_REG = "TRUE" *) reg [2:0] start_sync, stop_sync, clear_sync;
reg [2:0] command_cdc_armed;
wire start_pulse_axis = command_cdc_armed[2] && (start_sync[2] ^ start_sync[1]);
wire stop_pulse_axis  = command_cdc_armed[2] && (stop_sync[2]  ^ stop_sync[1]);
wire clear_pulse_axis = command_cdc_armed[2] && (clear_sync[2] ^ clear_sync[1]);

wire busy_axis, done_axis, underrun_axis, error_axis;
wire [7:0] error_code_axis;
wire [31:0] bits_sent_axis;
wire [31:0] frames_completed_axis;
// Gray码在源时钟域寄存后再过同步器，避免二进制进位产生组合毛刺。
reg [31:0] bits_sent_gray_axis;
reg [31:0] frames_completed_gray_axis;
always @(posedge axis_clk) begin
    if (!axis_aresetn) begin
        bits_sent_gray_axis <= 32'd0;
        frames_completed_gray_axis <= 32'd0;
    end else begin
        bits_sent_gray_axis <= bits_sent_axis ^ (bits_sent_axis >> 1);
        frames_completed_gray_axis <= frames_completed_axis ^ (frames_completed_axis >> 1);
    end
end
(* ASYNC_REG = "TRUE" *) reg [1:0] busy_cdc, done_cdc, underrun_cdc, error_cdc;
(* ASYNC_REG = "TRUE" *) reg [7:0] error_code_meta, error_code_sync;
(* ASYNC_REG = "TRUE" *) reg [31:0] bits_sent_gray_meta, bits_sent_gray_sync;
(* ASYNC_REG = "TRUE" *) reg [31:0] frames_completed_gray_meta, frames_completed_gray_sync;
wire [31:0] bits_sent_sync;
wire [31:0] frames_completed_sync;

function [31:0] gray_to_binary;
    input [31:0] gray;
    integer gray_bit;
    begin
        gray_to_binary[31] = gray[31];
        for (gray_bit = 30; gray_bit >= 0; gray_bit = gray_bit - 1)
            gray_to_binary[gray_bit] = gray_to_binary[gray_bit+1] ^ gray[gray_bit];
    end
endfunction

assign bits_sent_sync = gray_to_binary(bits_sent_gray_sync);
assign frames_completed_sync = gray_to_binary(frames_completed_gray_sync);

always @(posedge axis_clk) begin
    if (!axis_aresetn) begin
        bit_count_meta <= 32'd0;
        bit_count_sync <= 32'd0;
        bit_rate_meta  <= 32'd0;
        bit_rate_sync  <= 32'd0;
        rate_word_meta <= 32'd0;
        rate_word_sync <= 32'd0;
        repeat_meta    <= 32'd0;
        repeat_sync    <= 32'd0;
        start_sync <= 3'd0;
        stop_sync  <= 3'd0;
        clear_sync <= 3'd0;
        command_cdc_armed <= 3'd0;
    end else begin
        bit_count_meta <= cfg_bit_count_axi;
        bit_count_sync <= bit_count_meta;
        bit_rate_meta  <= cfg_bit_rate_axi;
        bit_rate_sync  <= bit_rate_meta;
        rate_word_meta <= cfg_rate_word_axi;
        rate_word_sync <= rate_word_meta;
        repeat_meta    <= cfg_repeat_count_axi;
        repeat_sync    <= repeat_meta;
        // Three stages intentionally give the static configuration bus time
        // to settle before the command toggle is observed in this domain.
        start_sync <= {start_sync[1:0], start_toggle_axi};
        stop_sync  <= {stop_sync[1:0],  stop_toggle_axi};
        clear_sync <= {clear_sync[1:0], clear_toggle_axi};
        // Suppress a false command if only the AXIS domain is reset while an
        // AXI-domain toggle is high. The synchronizers settle before arming.
        command_cdc_armed <= {command_cdc_armed[1:0], 1'b1};
    end
end

always @(posedge s00_axi_aclk) begin
    if (!s00_axi_aresetn) begin
        busy_cdc       <= 2'd0;
        done_cdc       <= 2'd0;
        underrun_cdc   <= 2'd0;
        error_cdc      <= 2'd0;
        error_code_meta <= 8'd0;
        error_code_sync <= 8'd0;
        bits_sent_gray_meta <= 32'd0;
        bits_sent_gray_sync <= 32'd0;
        frames_completed_gray_meta <= 32'd0;
        frames_completed_gray_sync <= 32'd0;
    end else begin
        busy_cdc     <= {busy_cdc[0], busy_axis};
        done_cdc     <= {done_cdc[0], done_axis};
        underrun_cdc <= {underrun_cdc[0], underrun_axis};
        error_cdc    <= {error_cdc[0], error_axis};
        error_code_meta <= error_code_axis;
        error_code_sync <= error_code_meta;
        bits_sent_gray_meta <= bits_sent_gray_axis;
        bits_sent_gray_sync <= bits_sent_gray_meta;
        frames_completed_gray_meta <= frames_completed_gray_axis;
        frames_completed_gray_sync <= frames_completed_gray_meta;
    end
end

tx_bit_engine_v1_0_S00_AXI #(
    .C_S_AXI_DATA_WIDTH(C_S00_AXI_DATA_WIDTH),
    .C_S_AXI_ADDR_WIDTH(C_S00_AXI_ADDR_WIDTH)
) axi_regs (
    .cfg_bit_count(cfg_bit_count_axi),
    .cfg_repeat_count(cfg_repeat_count_axi),
    .cfg_profile(cfg_profile_axi),
    .cfg_channel(cfg_channel_axi),
    .cfg_bit_rate(cfg_bit_rate_axi),
    .cfg_rate_word(cfg_rate_word_axi),
    .start_toggle(start_toggle_axi),
    .stop_toggle(stop_toggle_axi),
    .clear_toggle(clear_toggle_axi),
    .status_busy(busy_cdc[1]),
    .status_done(done_cdc[1]),
    .status_underrun(underrun_cdc[1]),
    .status_error(error_cdc[1]),
    .status_error_code(error_code_sync),
    .status_bits_sent(bits_sent_sync),
    .status_frames_completed(frames_completed_sync),
    .S_AXI_ACLK(s00_axi_aclk), .S_AXI_ARESETN(s00_axi_aresetn),
    .S_AXI_AWADDR(s00_axi_awaddr), .S_AXI_AWPROT(s00_axi_awprot),
    .S_AXI_AWVALID(s00_axi_awvalid), .S_AXI_AWREADY(s00_axi_awready),
    .S_AXI_WDATA(s00_axi_wdata), .S_AXI_WSTRB(s00_axi_wstrb),
    .S_AXI_WVALID(s00_axi_wvalid), .S_AXI_WREADY(s00_axi_wready),
    .S_AXI_BRESP(s00_axi_bresp), .S_AXI_BVALID(s00_axi_bvalid),
    .S_AXI_BREADY(s00_axi_bready), .S_AXI_ARADDR(s00_axi_araddr),
    .S_AXI_ARPROT(s00_axi_arprot), .S_AXI_ARVALID(s00_axi_arvalid),
    .S_AXI_ARREADY(s00_axi_arready), .S_AXI_RDATA(s00_axi_rdata),
    .S_AXI_RRESP(s00_axi_rresp), .S_AXI_RVALID(s00_axi_rvalid),
    .S_AXI_RREADY(s00_axi_rready)
);

tx_bit_engine_axis #(
    .AXIS_CLK_HZ(AXIS_CLK_HZ), .BUFFER_WORDS(BUFFER_WORDS)
) engine (
    .axis_clk(axis_clk), .axis_aresetn(axis_aresetn),
    .start_pulse(start_pulse_axis), .stop_pulse(stop_pulse_axis),
    .clear_pulse(clear_pulse_axis), .cfg_bit_count(bit_count_sync),
    .cfg_repeat_count(repeat_sync), .cfg_bit_rate(bit_rate_sync),
    .cfg_rate_word(rate_word_sync),
    .s_axis_tdata(s_axis_tdata), .s_axis_tkeep(s_axis_tkeep),
    .s_axis_tvalid(s_axis_tvalid), .s_axis_tready(s_axis_tready),
    .s_axis_tlast(s_axis_tlast), .bit_data(bit_data),
    .bit_valid(bit_valid), .bit_ready(bit_ready),
    .frame_start(frame_start), .frame_end(frame_end),
    .busy(busy_axis), .done(done_axis), .underrun(underrun_axis),
    .error(error_axis), .error_code(error_code_axis),
    .bits_sent(bits_sent_axis), .frames_completed(frames_completed_axis),
    .bit_phase(bit_phase)
);

assign busy          = busy_axis;
assign done          = done_axis;
assign underrun      = underrun_axis;
assign error         = error_axis;
assign bits_sent     = bits_sent_axis;
assign underrun_irq  = underrun_cdc[1] | error_cdc[1];

wire unused_cfg = &{1'b0, cfg_profile_axi, cfg_channel_axi};

endmodule
