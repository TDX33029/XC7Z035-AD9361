// (c) Copyright 1995-2026 Xilinx, Inc. All rights reserved.
// 
// This file contains confidential and proprietary information
// of Xilinx, Inc. and is protected under U.S. and
// international copyright and other intellectual property
// laws.
// 
// DISCLAIMER
// This disclaimer is not a license and does not grant any
// rights to the materials distributed herewith. Except as
// otherwise provided in a valid license issued to you by
// Xilinx, and to the maximum extent permitted by applicable
// law: (1) THESE MATERIALS ARE MADE AVAILABLE "AS IS" AND
// WITH ALL FAULTS, AND XILINX HEREBY DISCLAIMS ALL WARRANTIES
// AND CONDITIONS, EXPRESS, IMPLIED, OR STATUTORY, INCLUDING
// BUT NOT LIMITED TO WARRANTIES OF MERCHANTABILITY, NON-
// INFRINGEMENT, OR FITNESS FOR ANY PARTICULAR PURPOSE; and
// (2) Xilinx shall not be liable (whether in contract or tort,
// including negligence, or under any other theory of
// liability) for any loss or damage of any kind or nature
// related to, arising under or in connection with these
// materials, including for any direct, or any indirect,
// special, incidental, or consequential loss or damage
// (including loss of data, profits, goodwill, or any type of
// loss or damage suffered as a result of any action brought
// by a third party) even if such damage or loss was
// reasonably foreseeable or Xilinx had been advised of the
// possibility of the same.
// 
// CRITICAL APPLICATIONS
// Xilinx products are not designed or intended to be fail-
// safe, or for use in any application requiring fail-safe
// performance, such as life-support or safety devices or
// systems, Class III medical devices, nuclear facilities,
// applications related to the deployment of airbags, or any
// other applications that could lead to death, personal
// injury, or severe property or environmental damage
// (individually and collectively, "Critical
// Applications"). Customer assumes the sole risk and
// liability of any use of Xilinx products in Critical
// Applications, subject only to applicable laws and
// regulations governing limitations on product liability.
// 
// THIS COPYRIGHT NOTICE AND DISCLAIMER MUST BE RETAINED AS
// PART OF THIS FILE AT ALL TIMES.
// 
// DO NOT MODIFY THIS FILE.


// IP VLNV: user.org:user:ad9361_lvds_phy:1.0
// IP Revision: 9

(* X_CORE_INFO = "ad9361_lvds_phy,Vivado 2020.2" *)
(* CHECK_LICENSE_TYPE = "design_1_ad9361_lvds_phy_0_0,ad9361_lvds_phy,{}" *)
(* IP_DEFINITION_SOURCE = "package_project" *)
(* DowngradeIPIdentifiedWarnings = "yes" *)
module design_1_ad9361_lvds_phy_0_0 (
  ref_clk,
  rst,
  rx_clk_in_p,
  rx_clk_in_n,
  rx_frame_in_p,
  rx_frame_in_n,
  rx_data_in_p,
  rx_data_in_n,
  tx_clk_out_p,
  tx_clk_out_n,
  tx_frame_out_p,
  tx_frame_out_n,
  tx_data_out_p,
  tx_data_out_n,
  data_clk,
  sample_clk,
  sample_locked,
  idelay_ready,
  adc_valid,
  adc_data_i0,
  adc_data_q0,
  adc_data_i1,
  adc_data_q1,
  dac_valid,
  dac_data_i0,
  dac_data_q0,
  dac_data_i1,
  dac_data_q1,
  delay_tap,
  delay_reload
);

(* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME ref_clk, FREQ_HZ 200000000, FREQ_TOLERANCE_HZ 0, PHASE 0.000, CLK_DOMAIN design_1_processing_system7_0_0_FCLK_CLK1, INSERT_VIP 0" *)
(* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 ref_clk CLK" *)
input wire ref_clk;
(* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME rst, POLARITY ACTIVE_HIGH, INSERT_VIP 0" *)
(* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 rst RST" *)
input wire rst;
input wire rx_clk_in_p;
input wire rx_clk_in_n;
input wire rx_frame_in_p;
input wire rx_frame_in_n;
input wire [5 : 0] rx_data_in_p;
input wire [5 : 0] rx_data_in_n;
output wire tx_clk_out_p;
output wire tx_clk_out_n;
output wire tx_frame_out_p;
output wire tx_frame_out_n;
output wire [5 : 0] tx_data_out_p;
output wire [5 : 0] tx_data_out_n;
(* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME data_clk, FREQ_HZ 100000000, FREQ_TOLERANCE_HZ 0, PHASE 0.000, CLK_DOMAIN design_1_ad9361_lvds_phy_0_0_data_clk, INSERT_VIP 0" *)
(* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 data_clk CLK" *)
output wire data_clk;
(* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME sample_clk, FREQ_HZ 100000000, FREQ_TOLERANCE_HZ 0, PHASE 0.000, CLK_DOMAIN design_1_ad9361_lvds_phy_0_0_sample_clk, INSERT_VIP 0" *)
(* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 sample_clk CLK" *)
output wire sample_clk;
output wire sample_locked;
output wire idelay_ready;
output wire adc_valid;
output wire [11 : 0] adc_data_i0;
output wire [11 : 0] adc_data_q0;
output wire [11 : 0] adc_data_i1;
output wire [11 : 0] adc_data_q1;
input wire dac_valid;
input wire [11 : 0] dac_data_i0;
input wire [11 : 0] dac_data_q0;
input wire [11 : 0] dac_data_i1;
input wire [11 : 0] dac_data_q1;
input wire [4 : 0] delay_tap;
input wire [6 : 0] delay_reload;

  ad9361_lvds_phy #(
    .RX_ENABLE(1)
  ) inst (
    .ref_clk(ref_clk),
    .rst(rst),
    .rx_clk_in_p(rx_clk_in_p),
    .rx_clk_in_n(rx_clk_in_n),
    .rx_frame_in_p(rx_frame_in_p),
    .rx_frame_in_n(rx_frame_in_n),
    .rx_data_in_p(rx_data_in_p),
    .rx_data_in_n(rx_data_in_n),
    .tx_clk_out_p(tx_clk_out_p),
    .tx_clk_out_n(tx_clk_out_n),
    .tx_frame_out_p(tx_frame_out_p),
    .tx_frame_out_n(tx_frame_out_n),
    .tx_data_out_p(tx_data_out_p),
    .tx_data_out_n(tx_data_out_n),
    .data_clk(data_clk),
    .sample_clk(sample_clk),
    .sample_locked(sample_locked),
    .idelay_ready(idelay_ready),
    .adc_valid(adc_valid),
    .adc_data_i0(adc_data_i0),
    .adc_data_q0(adc_data_q0),
    .adc_data_i1(adc_data_i1),
    .adc_data_q1(adc_data_q1),
    .dac_valid(dac_valid),
    .dac_data_i0(dac_data_i0),
    .dac_data_q0(dac_data_q0),
    .dac_data_i1(dac_data_i1),
    .dac_data_q1(dac_data_q1),
    .delay_tap(delay_tap),
    .delay_reload(delay_reload)
  );
endmodule
