//////////////////////////////////////////////////////////////////////////////////
// AD9361 CMOS full-port 2T2R transmitter.
// TX_FRAME high selects TX1 and low selects TX2. I is launched on the rising
// edge and Q on the falling edge of the 40 MHz feedback clock.
//////////////////////////////////////////////////////////////////////////////////
module ad936x_cmos_tx(
	input  wire        fb_clk,
	input  wire        rst,
	output wire        tx_clk_out,
	output wire        tx_frame_out,
	output wire [11:0] tx_data_out,
	input  wire        dac_valid,
	input  wire [11:0] dac_data_i0,
	input  wire [11:0] dac_data_q0,
	input  wire [11:0] dac_data_i1,
	input  wire [11:0] dac_data_q1
);

reg channel_select;
reg [11:0] tx_data_i_reg;
reg [11:0] tx_data_q_reg;
reg tx_frame_reg;

always @(posedge fb_clk) begin
	if (rst) begin
		channel_select <= 1'b0;
		tx_data_i_reg <= 12'd0;
		tx_data_q_reg <= 12'd0;
		tx_frame_reg <= 1'b1;
	end else if (dac_valid) begin
		if (channel_select == 1'b0) begin
			tx_data_i_reg <= dac_data_i0;
			tx_data_q_reg <= dac_data_q0;
			tx_frame_reg <= 1'b1;
		end else begin
			tx_data_i_reg <= dac_data_i1;
			tx_data_q_reg <= dac_data_q1;
			tx_frame_reg <= 1'b0;
		end
		channel_select <= ~channel_select;
	end
end

ODDR #(.DDR_CLK_EDGE("SAME_EDGE"), .INIT(1'b0), .SRTYPE("SYNC"))
tx_frame_oddr (
	.Q(tx_frame_out), .C(fb_clk), .CE(1'b1),
	.D1(tx_frame_reg), .D2(tx_frame_reg), .R(1'b0), .S(1'b0)
);

ODDR #(.DDR_CLK_EDGE("SAME_EDGE"), .INIT(1'b0), .SRTYPE("SYNC"))
tx_clock_oddr (
	.Q(tx_clk_out), .C(fb_clk), .CE(1'b1),
	.D1(1'b1), .D2(1'b0), .R(1'b0), .S(1'b0)
);

genvar i;
generate
	for (i = 0; i < 12; i = i + 1) begin : tx_data_oddr_gen
		ODDR #(.DDR_CLK_EDGE("SAME_EDGE"), .INIT(1'b0), .SRTYPE("SYNC"))
		tx_data_oddr (
			.Q(tx_data_out[i]), .C(fb_clk), .CE(1'b1),
			.D1(tx_data_i_reg[i]), .D2(tx_data_q_reg[i]),
			.R(1'b0), .S(1'b0)
		);
	end
endgenerate

endmodule
