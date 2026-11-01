`timescale 1ns / 1ps

module ad9361_lvds_rx (
    input  wire        ref_clk,
    input  wire        rst,
    input  wire        data_clk,
    input  wire        capture_clk,
    input  wire        rx_frame_in_p,
    input  wire        rx_frame_in_n,
    input  wire [5:0]  rx_data_in_p,
    input  wire [5:0]  rx_data_in_n,
    input  wire [4:0]  delay_tap,
    input  wire [6:0]  delay_reload,
    output wire        idelay_ready,
    output wire        adc_valid,
    output wire [11:0] adc_data_i0,
    output wire [11:0] adc_data_q0,
    output wire [11:0] adc_data_i1,
    output wire [11:0] adc_data_q1
);

wire frame_ibuf;
wire frame_delayed;
wire frame_rise;
wire frame_fall;
wire [5:0] data_ibuf;
wire [5:0] data_delayed;
wire [5:0] data_rise;
wire [5:0] data_fall;

// PS/AXI写入来自100MHz域；IDELAY的C端在200MHz ref_clk域。
// tap先同步两拍，加载掩码再多等一拍，保证LD发生时tap已稳定。
// PS每次加载保持掩码10us后清零；目标域只在上升沿产生一个LD脉冲。
(* ASYNC_REG = "TRUE" *) reg [4:0] delay_tap_meta, delay_tap_ref;
(* ASYNC_REG = "TRUE" *) reg [6:0] reload_meta, reload_sync, reload_settle;
reg [6:0] reload_previous;
wire [6:0] delay_load_ref = reload_settle & ~reload_previous;
always @(posedge ref_clk or posedge rst) begin
    if (rst) begin
        delay_tap_meta <= 5'd0; delay_tap_ref <= 5'd0;
        reload_meta <= 7'd0; reload_sync <= 7'd0; reload_settle <= 7'd0;
        reload_previous <= 7'd0;
    end else begin
        delay_tap_meta <= delay_tap; delay_tap_ref <= delay_tap_meta;
        reload_meta <= delay_reload; reload_sync <= reload_meta;
        reload_settle <= reload_sync; reload_previous <= reload_settle;
    end
end

IDELAYCTRL delay_ctrl (
    .REFCLK(ref_clk), .RST(rst), .RDY(idelay_ready)
);

IBUFDS #(
    // Match vendor P351 2.5V HR-bank design; not authorization to program.
    .DIFF_TERM("TRUE"), .IBUF_LOW_PWR("FALSE"), .IOSTANDARD("LVDS_25")
) frame_ibufds (
    .I(rx_frame_in_p), .IB(rx_frame_in_n), .O(frame_ibuf)
);

IDELAYE2 #(
    .CINVCTRL_SEL("FALSE"), .DELAY_SRC("IDATAIN"),
    .HIGH_PERFORMANCE_MODE("TRUE"), .IDELAY_TYPE("VAR_LOAD"),
    // Start near the centre of the input eye before PS PRBS training.
    // At 200 MHz REFCLK this gives roughly 1.5 ns initial data delay;
    // boot calibration subsequently loads the measured centre via IDELAY.
    .IDELAY_VALUE(20), .PIPE_SEL("FALSE"),
    .REFCLK_FREQUENCY(200.0), .SIGNAL_PATTERN("DATA")
) frame_idelay (
    .CNTVALUEOUT(), .DATAOUT(frame_delayed), .C(ref_clk), .CE(1'b0),
    .CINVCTRL(1'b0), .CNTVALUEIN(delay_tap_ref), .DATAIN(1'b0),
    .IDATAIN(frame_ibuf), .INC(1'b0), .LD(delay_load_ref[6]),
    .LDPIPEEN(1'b0), .REGRST(1'b0)
);

(* IOB = "TRUE" *) IDDR #(
    .DDR_CLK_EDGE("SAME_EDGE_PIPELINED"), .INIT_Q1(1'b0),
    .INIT_Q2(1'b0), .SRTYPE("SYNC")
) frame_iddr (
    .Q1(frame_rise), .Q2(frame_fall), .C(capture_clk), .CE(1'b1),
    .D(frame_delayed), .R(1'b0), .S(1'b0)
);

genvar lane;
generate
    for (lane = 0; lane < 6; lane = lane + 1) begin : rx_lane
        IBUFDS #(
            .DIFF_TERM("TRUE"), .IBUF_LOW_PWR("FALSE"),
            .IOSTANDARD("LVDS_25")
        ) data_ibufds (
            .I(rx_data_in_p[lane]), .IB(rx_data_in_n[lane]),
            .O(data_ibuf[lane])
        );

        IDELAYE2 #(
            .CINVCTRL_SEL("FALSE"), .DELAY_SRC("IDATAIN"),
            .HIGH_PERFORMANCE_MODE("TRUE"), .IDELAY_TYPE("VAR_LOAD"),
            // Match FRAME's startup tap to the data lanes. The PS calibration
            // may later select an independent centre for this lane.
            .IDELAY_VALUE(20), .PIPE_SEL("FALSE"),
            .REFCLK_FREQUENCY(200.0), .SIGNAL_PATTERN("DATA")
        ) data_idelay (
            .CNTVALUEOUT(), .DATAOUT(data_delayed[lane]), .C(ref_clk),
            .CE(1'b0), .CINVCTRL(1'b0), .CNTVALUEIN(delay_tap_ref),
            .DATAIN(1'b0), .IDATAIN(data_ibuf[lane]), .INC(1'b0),
            .LD(delay_load_ref[lane]), .LDPIPEEN(1'b0), .REGRST(1'b0)
        );

        (* IOB = "TRUE" *) IDDR #(
            .DDR_CLK_EDGE("SAME_EDGE_PIPELINED"), .INIT_Q1(1'b0),
            .INIT_Q2(1'b0), .SRTYPE("SYNC")
        ) data_iddr (
            .Q1(data_rise[lane]), .Q2(data_fall[lane]),
            .C(capture_clk), .CE(1'b1), .D(data_delayed[lane]),
            .R(1'b0), .S(1'b0)
        );
    end
endgenerate

reg [5:0]  data_fall_d;
reg        frame_fall_d;
reg [3:0]  frame_shift;
reg [23:0] data_shift;
reg [11:0] i0_reg;
reg [11:0] q0_reg;
reg [11:0] i1_reg;
reg [11:0] q1_reg;
reg        valid_pending;
reg        valid_reg;

assign adc_valid   = valid_reg;
assign adc_data_i0 = i0_reg;
assign adc_data_q0 = q0_reg;
assign adc_data_i1 = i1_reg;
assign adc_data_q1 = q1_reg;

always @(posedge data_clk) begin
    if (rst) begin
        data_fall_d   <= 6'd0;
        frame_fall_d  <= 1'b0;
        frame_shift   <= 4'd0;
        data_shift    <= 24'd0;
        i0_reg        <= 12'd0;
        q0_reg        <= 12'd0;
        i1_reg        <= 12'd0;
        q1_reg        <= 12'd0;
        valid_pending <= 1'b0;
        valid_reg     <= 1'b0;
    end else begin
        data_fall_d  <= data_fall;
        frame_fall_d <= frame_fall;
        frame_shift  <= {frame_shift[1:0], frame_rise, frame_fall_d};
        data_shift   <= {data_shift[11:0], data_rise, data_fall_d};

        // I/Q保持两拍。valid延后一拍，供同域打包器/PN检查器读取完整复样本。
        valid_reg     <= valid_pending;
        valid_pending <= 1'b0;

        // UG570 Fig.79：源时钟上升沿之后发送I，下降沿之后发送Q。
        // IDDR采样的是该沿之前的稳定值：Q1(data_rise)取得上一下降沿
        // 发出的Q，Q2(data_fall)取得上一上升沿发出的I。fall额外延一拍，
        // 才与rise属于同一upper/lower槽。不能仅凭Q1/Q2名称判断I/Q。
        // frame_shift=1100时，data_shift={Q上6,I上6,Q下6,I下6}。
        // 修复前反向拼接，导致输出I/Q互换、复数频谱正负频偏反向。
        if (frame_shift == 4'b1100) begin
            i0_reg <= {data_shift[17:12], data_shift[5:0]};
            q0_reg <= {data_shift[23:18], data_shift[11:6]};
            valid_pending <= 1'b1;
        end
    end
end

endmodule
