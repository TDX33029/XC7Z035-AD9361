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


// IP VLNV: xilinx.com:user:dds_inc:1.12
// IP Revision: 3

(* X_CORE_INFO = "dds_inc,Vivado 2020.2" *)
(* CHECK_LICENSE_TYPE = "design_1_dds_inc_0_0,dds_inc,{}" *)
(* CORE_GENERATION_INFO = "design_1_dds_inc_0_0,dds_inc,{x_ipProduct=Vivado 2020.2,x_ipVendor=xilinx.com,x_ipLibrary=user,x_ipName=dds_inc,x_ipVersion=1.12,x_ipCoreRevision=3,x_ipLanguage=VERILOG,x_ipSimLanguage=MIXED}" *)
(* IP_DEFINITION_SOURCE = "package_project" *)
(* DowngradeIPIdentifiedWarnings = "yes" *)
module design_1_dds_inc_0_0 (
  clk,
  rst,
  phase_inc,
  bit_data,
  bit_valid,
  bit_phase,
  frame_start,
  frame_end,
  bit_ready,
  pcm_deviation_word,
  pcm_frequency_offset,
  pcm_amplitude,
  pcm_filter_profile,
  pcm_control,
  soqpsk_bit_rate_word,
  artm_bit_rate_word,
  pcm_iq_valid,
  pcm_accepted_bits,
  pcm_debug_nrz,
  pcm_debug_filter,
  pcm_debug_phase,
  dac_valid,
  dac_data_i0,
  dac_data_q0,
  dac_data_i1,
  dac_data_q1
);

(* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME clk, ASSOCIATED_RESET rst, FREQ_HZ 100000000, FREQ_TOLERANCE_HZ 0, PHASE 0.000, CLK_DOMAIN design_1_ad9361_lvds_phy_0_0_sample_clk, INSERT_VIP 0" *)
(* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 clk CLK" *)
input wire clk;
(* X_INTERFACE_PARAMETER = "XIL_INTERFACENAME rst, POLARITY ACTIVE_HIGH, INSERT_VIP 0" *)
(* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 rst RST" *)
input wire rst;
input wire [31 : 0] phase_inc;
input wire bit_data;
input wire bit_valid;
input wire [31 : 0] bit_phase;
input wire frame_start;
input wire frame_end;
output wire bit_ready;
input wire [31 : 0] pcm_deviation_word;
input wire [31 : 0] pcm_frequency_offset;
input wire [15 : 0] pcm_amplitude;
input wire [3 : 0] pcm_filter_profile;
input wire [31 : 0] pcm_control;
input wire [31 : 0] soqpsk_bit_rate_word;
input wire [31 : 0] artm_bit_rate_word;
output wire pcm_iq_valid;
output wire [31 : 0] pcm_accepted_bits;
output wire [15 : 0] pcm_debug_nrz;
output wire [15 : 0] pcm_debug_filter;
output wire [31 : 0] pcm_debug_phase;
output wire dac_valid;
output wire [11 : 0] dac_data_i0;
output wire [11 : 0] dac_data_q0;
output wire [11 : 0] dac_data_i1;
output wire [11 : 0] dac_data_q1;

  dds_inc inst (
    .clk(clk),
    .rst(rst),
    .phase_inc(phase_inc),
    .bit_data(bit_data),
    .bit_valid(bit_valid),
    .bit_phase(bit_phase),
    .frame_start(frame_start),
    .frame_end(frame_end),
    .bit_ready(bit_ready),
    .pcm_deviation_word(pcm_deviation_word),
    .pcm_frequency_offset(pcm_frequency_offset),
    .pcm_amplitude(pcm_amplitude),
    .pcm_filter_profile(pcm_filter_profile),
    .pcm_control(pcm_control),
    .soqpsk_bit_rate_word(soqpsk_bit_rate_word),
    .artm_bit_rate_word(artm_bit_rate_word),
    .pcm_iq_valid(pcm_iq_valid),
    .pcm_accepted_bits(pcm_accepted_bits),
    .pcm_debug_nrz(pcm_debug_nrz),
    .pcm_debug_filter(pcm_debug_filter),
    .pcm_debug_phase(pcm_debug_phase),
    .dac_valid(dac_valid),
    .dac_data_i0(dac_data_i0),
    .dac_data_q0(dac_data_q0),
    .dac_data_i1(dac_data_i1),
    .dac_data_q1(dac_data_q1)
  );
endmodule
