# lvds timing — reconstructed from r10 timing summary (clocks identical)
create_clock -period 8.138 -name ad9361_data_clk [get_ports rx_clk_in_p]
# TX 反馈时钟：ODDR 输出反相沿（波形 {P/2, P}）
create_generated_clock -name ad9361_tx_fb_clk -source [get_ports rx_clk_in_p] -edges {2 3 4} [get_ports tx_clk_out_p]
# RX 数据为 LVDS 训练校准接口（lvds_calibration 运行时训练），数据按源同步标定处理
set_false_path -from [get_ports {rx_data_in_p[*]}]
set_false_path -from [get_ports {rx_data_in_n[*]}]
set_false_path -from [get_ports rx_frame_in_p]
set_false_path -from [get_ports rx_frame_in_n]
# TX 数据时钟由本设计产生并随位流标定，输出数据按源同步处理
set_false_path -to [get_ports {tx_data_out_p[*]}]
set_false_path -to [get_ports {tx_data_out_n[*]}]
set_false_path -to [get_ports tx_frame_out_p]
set_false_path -to [get_ports tx_frame_out_n]
