# 异步时钟组（重建）：AD9361 LVDS 域（含其生成时钟）与 PS FCLK 域经异步 FIFO 交接。
# 同组内相关时钟（data_clk 与 sample/tx_fb 生成时钟）保持时序分析。
set_clock_groups -asynchronous   -group [get_clocks {ad9361_data_clk ad9361_tx_fb_clk mmcm_fb sample_clk_mmcm}]   -group [get_clocks clk_fpga_0]   -group [get_clocks clk_fpga_1]   -group [get_clocks clk_fpga_2]
