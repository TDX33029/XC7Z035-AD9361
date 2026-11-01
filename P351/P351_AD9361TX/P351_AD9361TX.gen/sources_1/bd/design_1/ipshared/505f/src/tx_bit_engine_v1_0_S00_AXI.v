`timescale 1 ns / 1 ps

module tx_bit_engine_v1_0_S00_AXI #(
    parameter integer C_S_AXI_DATA_WIDTH = 32,
    parameter integer C_S_AXI_ADDR_WIDTH = 6
) (
    output reg  [31:0] cfg_bit_count,
    output reg  [31:0] cfg_repeat_count,
    output reg  [31:0] cfg_profile,
    output reg  [31:0] cfg_channel,
    output reg  [31:0] cfg_bit_rate,
    output reg  [31:0] cfg_rate_word,
    output reg         start_toggle,
    output reg         stop_toggle,
    output reg         clear_toggle,
    input  wire        status_busy,
    input  wire        status_done,
    input  wire        status_underrun,
    input  wire        status_error,
    input  wire [7:0]  status_error_code,
    input  wire [31:0] status_bits_sent,
    input  wire [31:0] status_frames_completed,

    input  wire                              S_AXI_ACLK,
    input  wire                              S_AXI_ARESETN,
    input  wire [C_S_AXI_ADDR_WIDTH-1:0]     S_AXI_AWADDR,
    input  wire [2:0]                        S_AXI_AWPROT,
    input  wire                              S_AXI_AWVALID,
    output wire                              S_AXI_AWREADY,
    input  wire [C_S_AXI_DATA_WIDTH-1:0]     S_AXI_WDATA,
    input  wire [(C_S_AXI_DATA_WIDTH/8)-1:0] S_AXI_WSTRB,
    input  wire                              S_AXI_WVALID,
    output wire                              S_AXI_WREADY,
    output wire [1:0]                        S_AXI_BRESP,
    output reg                               S_AXI_BVALID,
    input  wire                              S_AXI_BREADY,
    input  wire [C_S_AXI_ADDR_WIDTH-1:0]     S_AXI_ARADDR,
    input  wire [2:0]                        S_AXI_ARPROT,
    input  wire                              S_AXI_ARVALID,
    output wire                              S_AXI_ARREADY,
    output reg  [C_S_AXI_DATA_WIDTH-1:0]     S_AXI_RDATA,
    output wire [1:0]                        S_AXI_RRESP,
    output reg                               S_AXI_RVALID,
    input  wire                              S_AXI_RREADY
);

reg aw_hold;
reg [C_S_AXI_ADDR_WIDTH-1:0] awaddr_hold;
reg w_hold;
reg [C_S_AXI_DATA_WIDTH-1:0] wdata_hold;
reg [(C_S_AXI_DATA_WIDTH/8)-1:0] wstrb_hold;
integer byte_index;

wire aw_accept = S_AXI_AWVALID && S_AXI_AWREADY;
wire w_accept  = S_AXI_WVALID  && S_AXI_WREADY;
wire have_aw   = aw_hold || aw_accept;
wire have_w    = w_hold  || w_accept;
wire write_fire = !S_AXI_BVALID && have_aw && have_w;
wire [C_S_AXI_ADDR_WIDTH-1:0] wr_addr = aw_hold ? awaddr_hold : S_AXI_AWADDR;
wire [C_S_AXI_DATA_WIDTH-1:0] wr_data = w_hold ? wdata_hold : S_AXI_WDATA;
wire [(C_S_AXI_DATA_WIDTH/8)-1:0] wr_strb = w_hold ? wstrb_hold : S_AXI_WSTRB;

assign S_AXI_AWREADY = !aw_hold && !S_AXI_BVALID;
assign S_AXI_WREADY  = !w_hold  && !S_AXI_BVALID;
assign S_AXI_BRESP   = 2'b00;
assign S_AXI_ARREADY = !S_AXI_RVALID;
assign S_AXI_RRESP   = 2'b00;

function [31:0] apply_wstrb;
    input [31:0] old_value;
    input [31:0] new_value;
    input [3:0]  strb;
    integer i;
    begin
        apply_wstrb = old_value;
        for (i = 0; i < 4; i = i + 1)
            if (strb[i]) apply_wstrb[i*8 +: 8] = new_value[i*8 +: 8];
    end
endfunction

always @(posedge S_AXI_ACLK) begin
    if (!S_AXI_ARESETN) begin
        aw_hold          <= 1'b0;
        awaddr_hold      <= {C_S_AXI_ADDR_WIDTH{1'b0}};
        w_hold           <= 1'b0;
        wdata_hold       <= {C_S_AXI_DATA_WIDTH{1'b0}};
        wstrb_hold       <= {(C_S_AXI_DATA_WIDTH/8){1'b0}};
        S_AXI_BVALID     <= 1'b0;
        S_AXI_RVALID     <= 1'b0;
        S_AXI_RDATA      <= {C_S_AXI_DATA_WIDTH{1'b0}};
        cfg_bit_count    <= 32'd0;
        cfg_repeat_count <= 32'd0;
        cfg_profile      <= 32'd0;
        cfg_channel      <= 32'd0;
        cfg_bit_rate     <= 32'd0;
        cfg_rate_word    <= 32'd0;
        start_toggle     <= 1'b0;
        stop_toggle      <= 1'b0;
        clear_toggle     <= 1'b0;
    end else begin
        if (S_AXI_BVALID && S_AXI_BREADY)
            S_AXI_BVALID <= 1'b0;

        if (!write_fire) begin
            if (aw_accept) begin
                aw_hold     <= 1'b1;
                awaddr_hold <= S_AXI_AWADDR;
            end
            if (w_accept) begin
                w_hold     <= 1'b1;
                wdata_hold <= S_AXI_WDATA;
                wstrb_hold <= S_AXI_WSTRB;
            end
        end else begin
            aw_hold      <= 1'b0;
            w_hold       <= 1'b0;
            S_AXI_BVALID <= 1'b1;
            case (wr_addr[5:2])
                4'h0: begin
                    if (wr_strb[0] && wr_data[0]) start_toggle <= ~start_toggle;
                    if (wr_strb[0] && wr_data[1]) stop_toggle  <= ~stop_toggle;
                    if (wr_strb[0] && wr_data[2]) clear_toggle <= ~clear_toggle;
                end
                4'h2: cfg_bit_count    <= apply_wstrb(cfg_bit_count, wr_data, wr_strb);
                4'h3: cfg_repeat_count <= apply_wstrb(cfg_repeat_count, wr_data, wr_strb);
                4'h4: cfg_profile      <= apply_wstrb(cfg_profile, wr_data, wr_strb);
                4'h5: cfg_channel      <= apply_wstrb(cfg_channel, wr_data, wr_strb);
                4'h6: cfg_bit_rate     <= apply_wstrb(cfg_bit_rate, wr_data, wr_strb);
                4'hB: cfg_rate_word    <= apply_wstrb(cfg_rate_word, wr_data, wr_strb);
                default: begin end
            endcase
        end

        if (S_AXI_RVALID && S_AXI_RREADY)
            S_AXI_RVALID <= 1'b0;
        if (S_AXI_ARVALID && S_AXI_ARREADY) begin
            S_AXI_RVALID <= 1'b1;
            case (S_AXI_ARADDR[5:2])
                4'h0: S_AXI_RDATA <= 32'd0;
                4'h1: S_AXI_RDATA <= {16'd0, status_error_code, 4'd0,
                                      status_error, status_underrun,
                                      status_done, status_busy};
                4'h2: S_AXI_RDATA <= cfg_bit_count;
                4'h3: S_AXI_RDATA <= cfg_repeat_count;
                4'h4: S_AXI_RDATA <= cfg_profile;
                4'h5: S_AXI_RDATA <= cfg_channel;
                4'h6: S_AXI_RDATA <= cfg_bit_rate;
                4'h7: S_AXI_RDATA <= status_bits_sent;
                4'h8: S_AXI_RDATA <= status_frames_completed;
                4'h9: S_AXI_RDATA <= {24'd0, status_error_code};
                4'hA: S_AXI_RDATA <= 32'h0001_0200;
                4'hB: S_AXI_RDATA <= cfg_rate_word;
                default: S_AXI_RDATA <= 32'd0;
            endcase
        end
    end
end

wire unused = &{1'b0, S_AXI_AWPROT, S_AXI_ARPROT, byte_index[0]};

endmodule
