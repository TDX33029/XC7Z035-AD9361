//////////////////////////////////////////////////////////////////////////////////
// Company: bochenjingxin
// Engineer: yang
// Create Date: 2024/05/16 12:35:02
// Design Name: 
// Module Name: ad936x_phy
// Project Name: 
// Target Devices: 
// Tool Versions: 
// Description: ad936x_top
// Dependencies: 
// Revision:
// Revision 0.01 - File Created
// Additional Comments:
//////////////////////////////////////////////////////////////////////////////////
module ad936x_phy #(
	parameter RX_2CH = 1    // 1=2R 双�?�道(RX1A+RX2A)�?0=1R1T 兼容旧行�?
)(
	input 	wire 			ref_clk 	,//200M IODELAY REF CLOCK
	input 	wire 			rst 		,//system reset
	//rx input
	input	wire          	rx_clk_in	,
  	input	wire          	rx_frame_in	,
  	input	wire  [11:0]  	rx_data_in	,
  	//tx output
  	output 	wire 			tx_clk_out 	,
  	output 	wire 			tx_frame_out,
  	output 	wire  [11:0]	tx_data_out ,
  	//get adc data output (ch1 = RX1A, ch2 = RX2A)
  	output	wire 			adc_valid	,
	output	wire 	[11:0]	adc_data_i0	,
	output	wire 	[11:0]	adc_data_q0	,
	output	wire 	[11:0]	adc_data_i1	,
	output	wire 	[11:0]	adc_data_q1	,
	//dac output to phy
	input 	wire 			dac_valid	,
	input 	wire 	[11:0]	dac_data_i0	,
	input 	wire 	[11:0]	dac_data_q0	,
	input 	wire 	[11:0]	dac_data_i1	,
	input 	wire 	[11:0]	dac_data_q1	,
	//ctrl & clock
	output 	wire 			data_clk ,//user_tx_clk
	input   wire    		fb_clk  ,
	input 	wire 	[4:0]	delay_tap,//delay_value
	input 	wire 			delay_reload//enable delay load
	);
//rx instance (RX_2CH 参数透传)
ad936x_cmos_rx #(.RX_2CH(RX_2CH)) ad936x_cmos_rx_inst
(
	.ref_clk       (ref_clk       ),
	.rst           (rst           ),
	//adc rx input from port
	.rx_clk_in     (rx_clk_in     ),
	.rx_frame_in   (rx_frame_in   ),
	.rx_data_in    (rx_data_in    ),
    //adc rx result &valid
	.adc_valid     (adc_valid     ),
	.adc_data_i0   (adc_data_i0   ),
	.adc_data_q0   (adc_data_q0   ),
	.adc_data_i1   (adc_data_i1   ),
	.adc_data_q1   (adc_data_q1   ),
    //clock & delay ctrl
	.data_clk      (data_clk      ),
	.delay_tap     (delay_tap     ),
	.delay_reload  (delay_reload  )
);
//tx instance
ad936x_cmos_tx ad936x_cmos_tx_inst
(
	.fb_clk       (fb_clk       ),
	.rst          (rst          ),
	//tx out to port
	.tx_clk_out   (tx_clk_out   ),
	.tx_frame_out (tx_frame_out ),
	.tx_data_out  (tx_data_out  ),
    //dac data in
	.dac_valid    (dac_valid    ),
	.dac_data_i0  (dac_data_i0  ),
	.dac_data_q0  (dac_data_q0  ),
	.dac_data_i1  (dac_data_i1  ),
	.dac_data_q1  (dac_data_q1  )
);

endmodule
