`timescale 1ns / 1ps
// P351 carrier adaptation; the signal/DMA/AXI BD remains unchanged.
// Physical RX2/TX2 use the first logical I/Q slot in the 1R1T interface.
module p351_top (
    inout [14:0] DDR_addr, inout [2:0] DDR_ba,
    inout DDR_cas_n, DDR_ck_n, DDR_ck_p, DDR_cke, DDR_cs_n,
    inout [3:0] DDR_dm, inout [31:0] DDR_dq,
    inout [3:0] DDR_dqs_n, DDR_dqs_p,
    inout DDR_odt, DDR_ras_n, DDR_reset_n, DDR_we_n,
    inout FIXED_IO_ddr_vrn, FIXED_IO_ddr_vrp,
    inout [53:0] FIXED_IO_mio,
    inout FIXED_IO_ps_clk, FIXED_IO_ps_porb, FIXED_IO_ps_srstb,
    input rx_clk_in_p, rx_clk_in_n, rx_frame_in_p, rx_frame_in_n,
    input [5:0] rx_data_in_p, rx_data_in_n,
    output tx_clk_out_p, tx_clk_out_n, tx_frame_out_p, tx_frame_out_n,
    output [5:0] tx_data_out_p, tx_data_out_n,
    output spi_clk, spi_csn, spi_mosi, input spi_miso,
    output txnrx, enable, resetb, en_agc, output [3:0] ctrl_in,
    output tfs_sck, tfs_mosi, tfs_le, input tfs_miso, tfs_ld,
    output [6:0] tfs_atten_ttl,
    output screen_uart_tx, input screen_uart_rx,
    output dbg_uart_tx, input dbg_uart_rx,
    output rf_trx2_tx_nrx, rf_rx2_ntrx_rx,
    output clk_ref_select, clk_pll_sck, clk_pll_data, clk_pll_le,
    input clk_pll_lock, clk_ref_sample,
    output clk_dac_cs_n
);
wire [31:0] gpio, gpio_i;
wire board_clk100, board_resetn;
wire ref_present, ref_frequency_ok, ref_qualified;
wire safety_ready, safety_fault, safety_force_atten, safety_rf_allow;
wire [6:0] raw_atten_ttl;
p351_rf_safety rf_safety(
    .clk(board_clk100), .resetn(board_resetn), .heartbeat(gpio[8]), .arm(gpio[9]),
    .rf_request(gpio[12]), .require_tfs(gpio[10]), .tfs_ld(tfs_ld),
    .reference_ok(~gpio[24] | ref_qualified), .ready(safety_ready), .fault(safety_fault),
    .force_atten_safe(safety_force_atten), .rf_allow(safety_rf_allow));
assign tfs_atten_ttl=safety_force_atten ? 7'b0 : raw_atten_ttl;
p351_ref_monitor reference_monitor(
    .clk(board_clk100), .resetn(board_resetn), .ref_in(clk_ref_sample),
    .pll_lock(clk_pll_lock), .require_external(gpio[24]),
    .arm_external(gpio[25]),
    .ref_present(ref_present), .frequency_ok(ref_frequency_ok), .qualified(ref_qualified));
assign txnrx = gpio[0];
assign enable = gpio[1];
assign resetb = gpio[2];
assign en_agc = gpio[3];
assign ctrl_in = gpio[7:4];
// 外部参考模式失锁/缺参考时，PL不等PS主循环，直接关板载TX路径。
assign rf_trx2_tx_nrx = gpio[12] & (~gpio[24] | ref_qualified) & safety_rf_allow;
assign rf_rx2_ntrx_rx = gpio[13];
// These are independent of the old EXT_EN/VCTCXO_DISABLE GPIOs. P351's
// 10 MHz goes to ADF4002; its 40 MHz VCTCXO stays running, feeding AD9361.
assign clk_pll_sck = gpio[16];
assign clk_pll_data = gpio[17];
assign clk_pll_le = gpio[18];
assign clk_ref_select = gpio[19];
assign clk_dac_cs_n = 1'b1; // ADF与DAC共用SCK/DATA；未使用的DAC必须取消片选。
// bit31 = V8 continuous-timing capability; bit30 = P351 identity.
// bit29为参考监督/DAC隔离能力；bit28为已修正RX I/Q+PN解包能力。
// 能力只声明数字版本，不代表射频/电气验收。新PS拒绝混用旧PL。
// bit27=单槽连续采集能力；V6 PS要求该位，避免误配旧双槽DMA。
// bit26=实时DSP；bit25/24分别由PS驱动外部参考ARM/REQUIRE，不能复用为能力输入。
assign gpio_i = {2'b11, 6'b111100, ref_qualified, ref_frequency_ok, ref_present, clk_pll_lock, 4'b0,
                rf_rx2_ntrx_rx, rf_trx2_tx_nrx, 2'b0, 1'b1, 1'b1, safety_ready, safety_fault, 8'b0};
design_1_wrapper system (
    .DDR_addr(DDR_addr), .DDR_ba(DDR_ba), .DDR_cas_n(DDR_cas_n),
    .DDR_ck_n(DDR_ck_n), .DDR_ck_p(DDR_ck_p), .DDR_cke(DDR_cke),
    .DDR_cs_n(DDR_cs_n), .DDR_dm(DDR_dm), .DDR_dq(DDR_dq),
    .DDR_dqs_n(DDR_dqs_n), .DDR_dqs_p(DDR_dqs_p), .DDR_odt(DDR_odt),
    .DDR_ras_n(DDR_ras_n), .DDR_reset_n(DDR_reset_n), .DDR_we_n(DDR_we_n),
    .FIXED_IO_ddr_vrn(FIXED_IO_ddr_vrn), .FIXED_IO_ddr_vrp(FIXED_IO_ddr_vrp),
    .FIXED_IO_mio(FIXED_IO_mio), .FIXED_IO_ps_clk(FIXED_IO_ps_clk),
    .FIXED_IO_ps_porb(FIXED_IO_ps_porb), .FIXED_IO_ps_srstb(FIXED_IO_ps_srstb),
    .gpio(gpio), .gpio_i(gpio_i),
    .board_clk100(board_clk100), .board_resetn(board_resetn),
    .rx_clk_in_p(rx_clk_in_p), .rx_clk_in_n(rx_clk_in_n),
    .rx_frame_in_p(rx_frame_in_p), .rx_frame_in_n(rx_frame_in_n),
    .rx_data_in_p(rx_data_in_p), .rx_data_in_n(rx_data_in_n),
    .tx_clk_out_p(tx_clk_out_p), .tx_clk_out_n(tx_clk_out_n),
    .tx_frame_out_p(tx_frame_out_p), .tx_frame_out_n(tx_frame_out_n),
    .tx_data_out_p(tx_data_out_p), .tx_data_out_n(tx_data_out_n),
    .spi_clk(spi_clk), .spi_csn(spi_csn), .spi_mosi(spi_mosi), .spi_miso(spi_miso),
    .tfs_sck(tfs_sck), .tfs_mosi(tfs_mosi), .tfs_le(tfs_le),
    .tfs_miso(tfs_miso), .tfs_ld(tfs_ld), .tfs_atten_ttl(raw_atten_ttl),
    .screen_uart_tx(screen_uart_tx), .screen_uart_rx(screen_uart_rx),
    .dbg_uart_tx(dbg_uart_tx), .dbg_uart_rx(dbg_uart_rx)
);
endmodule
