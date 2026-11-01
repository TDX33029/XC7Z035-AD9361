`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company: bochenjingxin
// Engineer: yang
// Create Date: 2024/05/16 12:35:02
// Design Name:
// Module Name: ad936x_cmos_rx
// Project Name:
// Target Devices:
// Tool Versions:
// Description: ad936x rx data module
// Dependencies:
// Revision:
// Revision 0.01 - File Created
// Additional Comments:
//////////////////////////////////////////////////////////////////////////////////
module ad936x_cmos_rx #(
	parameter RX_2CH = 0    // 1=2R 双通道解交织(I1,Q1,I2,Q2 交织在单 RX 口)
)(
	input 	wire 			ref_clk 	,
	input 	wire 			rst 		,
	//adc rx input from port
	input 	wire			rx_clk_in 	,
	input 	wire 			rx_frame_in ,
	input 	wire 	[11:0]	rx_data_in 	,
    //adc rx result &valid (ch1 = RX1A 主天线, ch2 = RX2A 参考天线)
	output	wire 			adc_valid	,
	output	wire 	[11:0]	adc_data_i0	,
	output	wire 	[11:0]	adc_data_q0	,
	output	wire 	[11:0]	adc_data_i1	,//ch2 I (RX_2CH=1 时有效)
	output	wire 	[11:0]	adc_data_q1	,//ch2 Q
    //clock & delay ctrl
	output 	wire 			data_clk 	,//ad936x data clk to user logical
	input 	wire 	[4:0]	delay_tap	,//delay_value  tap
	input 	wire 			delay_reload//1 reload
	);
	
//delay the input clock
wire 			rx_clk_delay 	;//
wire 	[11:0]	rx_data_delay	;//
wire 			rx_frame_delay 	;//
//iddr
wire 	[1:0] 	rx_frame 		;//
wire 	[11:0]	rx_data_i 	;//
wire 	[11:0]	rx_data_q 	;//
//data & valid output reg
reg 		valid_reg ;//
reg 	[11:0]	adc_data_i0_reg 	;//
reg 	[11:0]	adc_data_q0_reg 	;//
reg 	[11:0]	adc_data_i1_reg 	;//
reg 	[11:0]	adc_data_q1_reg 	;//
reg 		ch1_flag, ch2_flag 	;//2R 组内标志
//assign to output
assign adc_valid = valid_reg;//
assign adc_data_i0 = adc_data_i0_reg;//
assign adc_data_q0 = adc_data_q0_reg;//
assign adc_data_i1 = adc_data_i1_reg;//
assign adc_data_q1 = adc_data_q1_reg;//
// IDELAY CTRL
IDELAYCTRL IDELAYCTRL_inst (
  	.RDY(),       // 1-bit output: Ready output
  	.REFCLK(ref_clk),// 1-bit input: Reference clock input
  	.RST(rst)        // 1-bit input: Active high reset input
);
//input phy clock buf
BUFG BUFG_inst (
  .O(data_clk), // 1-bit output: Clock output
  .I(rx_clk_in)  // 1-bit input: Clock input
);
//input delay 
IDELAYE2 #(
  	.CINVCTRL_SEL("FALSE"),          // Enable dynamic clock inversion (FALSE, TRUE)
  	.DELAY_SRC("IDATAIN"),            // Delay input (IDATAIN, DATAIN)
  	.HIGH_PERFORMANCE_MODE("FALSE"), // Reduced jitter ("TRUE"), Reduced power ("FALSE")
  	.IDELAY_TYPE("VAR_LOAD"),        // FIXED, VARIABLE, VAR_LOAD, VAR_LOAD_PIPE
  	.IDELAY_VALUE(6),                // Input delay tap setting (0-31)
  	.PIPE_SEL("FALSE"),              // Select pipelined mode, FALSE, TRUE
  	.REFCLK_FREQUENCY(200.0),        // REF input frequency in MHz(190.0-210.0, 290.0-310.0).
  	.SIGNAL_PATTERN("DATA")          // DATA, CLOCK input signal
)
//frame delay
IDELAYE2_inst_frame_delay (
  	.CNTVALUEOUT(), 			// 5-bit output: Counter value output
  	.DATAOUT(rx_frame_delay),	// 1-bit output: Delayed data output
  	.C(ref_clk),	        	// 1-bit input: Clock input
  	.CE(1'b0),                 	// 1-bit input: Active high enable increment/decrement input
  	.CINVCTRL(1'b0),           	// 1-bit input: Dynamic clock inversion input
  	.CNTVALUEIN(delay_tap),  	// 5-bit input: Counter value input
  	.DATAIN(1'b0),       		// 1-bit input: Internal delay data input
  	.IDATAIN(rx_frame_in),    	// 1-bit input: Data input from the I/O
  	.INC(1'b0),                	// 1-bit input: Increment / Decrement tap delay input
  	.LD(delay_reload),     	// 1-bit input: Load IDELAY_VALUE input
  	.LDPIPEEN(1'b0),           	// 1-bit input: Enable PIPELINE register to load data input
  	.REGRST(1'b0)              	// 1-bit input: Active-high reset tap-delay input
);
//data delay
genvar i;
generate
	for (i = 0; i < 12; i = i + 1) begin
	IDELAYE2 #(
	.CINVCTRL_SEL("FALSE"),// Enable dynamic clock inversion (FALSE, TRUE)
	.DELAY_SRC("IDATAIN"),// Delay input (IDATAIN, DATAIN)
	.HIGH_PERFORMANCE_MODE("FALSE"),// Reduced jitter ("TRUE"), Reduced power ("FALSE")
	.IDELAY_TYPE("VAR_LOAD"),// FIXED, VARIABLE, VAR_LOAD, VAR_LOAD_PIPE
	.IDELAY_VALUE(0),// Input delay tap setting (0-31)
	.PIPE_SEL("FALSE"),// Select pipelined mode, FALSE, TRUE
	.REFCLK_FREQUENCY(200.0),// REF input frequency in MHz(190.0-210.0, 290.0-310.0).
	.SIGNAL_PATTERN("DATA")          // DATA, CLOCK input signal
	)
	IDELAYE2_inst_frame_delay (
	.CNTVALUEOUT(), 			// 5-bit output: Counter value output
	.DATAOUT(rx_data_delay[i]),	// 1-bit output: Delayed data output
	.C(ref_clk),	        	// 1-bit input: Clock input
	.CE(1'b0),          // 1-bit input: Active high enable increment/decrement input
	.CINVCTRL(1'b0),           // 1-bit input: Dynamic clock inversion input
	.CNTVALUEIN(delay_tap),   // 5-bit input: Counter value input
	.DATAIN(1'b0),     			// 1-bit input: Internal delay data input
	.IDATAIN(rx_data_in[i]),    // 1-bit input: Data input from the I/O
	.INC(1'b0),          // 1-bit input: Increment / Decrement tap delay input
	.LD(delay_reload),  // 1-bit input: Load IDELAY_VALUE input
	.LDPIPEEN(1'b0),  // 1-bit input: Enable PIPELINE register to load data input
	.REGRST(1'b0)              	// 1-bit input: Active-high reset tap-delay input
	);
	end
endgenerate
//SDR TO DDR FRAME
IDDR #(
	//"OPPOSITE_EDGE","SAME_EDGE"//or"SAME_EDGE_PIPELINED" 
  	.DDR_CLK_EDGE("SAME_EDGE_PIPELINED"), 
  	.INIT_Q1(1'b0), // Initial value of Q1: 1'b0 or 1'b1
  	.INIT_Q2(1'b0), // Initial value of Q2: 1'b0 or 1'b1
  	.SRTYPE("SYNC") // Set/Reset type: "SYNC" or "ASYNC" 
) 	IDDR_inst_frame (
  	.Q1(rx_frame[1]), 	// 1-bit output for positive edge of clock
  	.Q2(rx_frame[0]), 	// 1-bit output for negative edge of clock
  	.C(data_clk),   	// 1-bit clock input
  	.CE(1'b1), 			// 1-bit clock enable input
  	.D(rx_frame_delay), // 1-bit DDR data input
  	.R(1'b0),   		// 1-bit reset
  	.S(1'b0)    		// 1-bit set
);	
//SDR TO DDR DATA
generate
	for (i = 0; i < 12; i = i + 1)
	begin:data_iddr
	IDDR #(
	   //"OPPOSITE_EDGE","SAME_EDGE"or"SAME_EDGE_PIPELINED" 
		.DDR_CLK_EDGE("SAME_EDGE_PIPELINED"), 
		.INIT_Q1(1'b0), // Initial value of Q1: 1'b0 or 1'b1
		.INIT_Q2(1'b0), // Initial value of Q2: 1'b0 or 1'b1
		.SRTYPE("SYNC") // Set/Reset type: "SYNC" or "ASYNC" 
	) 	IDDR_inst_data (
		.Q1(rx_data_i[i]), 		// 1-bit output for positive edge of clock
		.Q2(rx_data_q[i]), 		// 1-bit output for negative edge of clock
		.C(data_clk),   		// 1-bit clock input
		.CE(1'b1), 				// 1-bit clock enable input
		.D(rx_data_delay[i]),  	// 1-bit DDR data input
		.R(1'b0),   			// 1-bit reset
		.S(1'b0)    			// 1-bit set
	);			
	end
endgenerate	
//rx fram & data shift output
// 2R 解交织：AD9361 双收时 I1,Q1,I2,Q2 在单 RX 口按 2 时钟一组交织输出。
//   FRAME 高(整拍) 的那 1 拍 → ch1 (RX1A)，FRAME 低(整拍) 的那 1 拍 → ch2 (RX2A)。
//   SAME_EDGE_PIPELINED 下 rx_data_i(正沿)=I、rx_data_q(负沿)=Q 同周期对齐。
//   帧低拍发 valid（ch1 已由前一帧高拍锁存）→ 配对 {ch1(n),ch2(n)} 同时刻、2 拍/组。
//   注意：帧极性以 ILA 实测为准，若 ch1/ch2 或 I/Q 对调，把 rx_frame[1] 的判断取反
//   或交换锁存即可（调试项）。
always @(posedge data_clk) begin
	if (rst==1'b1) begin
		adc_data_i0_reg <= 'd0;
		adc_data_q0_reg <= 'd0;
		adc_data_i1_reg <= 'd0;
		adc_data_q1_reg <= 'd0;
		ch1_flag <= 1'b0;
		ch2_flag <= 1'b0;
		valid_reg <= 1'b0;
	end
	else if (RX_2CH == 1'b1) begin
		/* 修复 2026-08-12：旧版用 ch1_flag&&ch2_flag 判定，非阻塞语义读到的是本拍前旧值，
		 * 使 valid 滞后 1 拍、落在下一组 ch1 覆盖拍 → valid 每 3 拍才发一组（丢 ~⅓ 样本），
		 * 且配对交替错位（BIST 实测组内相位差 22.5°/0° 交替 + 双通道各自 +45° 跳变）。
		 * 新逻辑：帧高拍锁存 ch1 置 ch1_flag；帧低拍（ch1 已就绪）锁存 ch2 并当拍发 valid，
		 * 配对 {ch1(n), ch2(n)} 同时刻、恒 2 data_clk 拍/组 = 20M 组/s，无丢样。 */
		valid_reg <= 1'b0;
		if (rx_frame[1] == 1'b1) begin     // 帧高拍 → ch1
			adc_data_i0_reg <= rx_data_i;
			adc_data_q0_reg <= rx_data_q;
			ch1_flag <= 1'b1;
		end else if (ch1_flag) begin       // 帧低拍且 ch1 已锁存 → ch2 + 同时刻配对
			adc_data_i1_reg <= rx_data_i;
			adc_data_q1_reg <= rx_data_q;
			valid_reg <= 1'b1;
			ch1_flag <= 1'b0;
		end
	end
	else if(rx_frame==2'b10)begin          // 原 1R1T 行为
        adc_data_i0_reg <= rx_data_i;
        adc_data_q0_reg <= rx_data_q;
        valid_reg <= 1'b1;
	end
end

endmodule