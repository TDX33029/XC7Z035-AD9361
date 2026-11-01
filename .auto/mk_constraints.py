"""从 io_placed 报告重建 p351_pins.xdc（含每端口 PACKAGE_PIN/IOSTANDARD），并生成
lvds_timing.xdc / lvds_clock_groups_impl.xdc（按 r10 时序报告的时钟定义重建），
追加 JM1 调试串口 dbg_uart_tx/rx（W29/W30, LVCMOS33）。"""
import re, json, os

BASE = r'D:\Document\tempPrj\XC7Z035-AD9361'
RPT = BASE + r'\P351\P351_AD9361TX\P351_AD9361TX.runs\impl_1\p351_top_io_placed.rpt'
OUT = BASE + r'\P351\constraints'
os.makedirs(OUT, exist_ok=True)

txt = open(RPT, encoding='utf-8', errors='replace').read()
rows = []
for line in txt.splitlines():
    if not line.startswith('|'):
        continue
    cells = [c.strip() for c in line.strip('|').split('|')]
    if len(cells) < 12:
        continue
    pin, port, site, iostd, bank = cells[0], cells[1], cells[3], cells[5], cells[6]
    if not pin or not port or pin == 'Package Pin':
        continue
    rows.append((port, pin, site, iostd, bank))
print('io report ports:', len(rows))

out = ['# p351 pins — reconstructed from impl_1/p351_top_io_placed.rpt (r10 build)',
       '# + JM1 debug UART (W29/W30) added 2026-11-01', '']
lvds, missing_std = [], []
for port, pin, site, iostd, bank in sorted(rows, key=lambda r: r[1]):
    if not iostd or iostd in ('GND', 'VCC', 'NC'):
        continue
    out.append('set_property PACKAGE_PIN %s [get_ports {%s}]' % (pin, port))
    if iostd not in ('NONE', ''):
        out.append('set_property IOSTANDARD %s [get_ports {%s}]' % (iostd, port))
    if 'LVDS' in iostd:
        lvds.append(port)
# dbg uart on JM1 (bank 12, LVCMOS33)
out += ['', '# JM1 debug UART (PS UART1 via EMIO)',
        'set_property PACKAGE_PIN W29 [get_ports dbg_uart_tx]',
        'set_property IOSTANDARD LVCMOS33 [get_ports dbg_uart_tx]',
        'set_property PACKAGE_PIN W30 [get_ports dbg_uart_rx]',
        'set_property IOSTANDARD LVCMOS33 [get_ports dbg_uart_rx]']
open(OUT + r'\p351_pins.xdc', 'w', encoding='utf-8', newline='\n').write('\n'.join(out) + '\n')
print('p351_pins.xdc written:', len(rows), 'ports; LVDS ports:', len(lvds))

# lvds_timing.xdc — clocks measured from r10 timing summary
t = open(BASE + r'\P351\P351_AD9361TX\P351_AD9361TX.runs\impl_1\p351_top_timing_summary_routed.rpt',
         encoding='utf-8', errors='replace').read()
m = re.search(r'ad9361_data_clk\s+\{([\d.]+)\s+([\d.]+)\}\s+([\d.]+)', t)
period = m.group(3)
timing = ['# lvds timing — reconstructed from r10 timing summary (clocks identical)',
          'create_clock -period %s -name ad9361_data_clk [get_ports rx_clk_in_p]' % period,
          '# TX 反馈时钟：ODDR 输出反相沿（波形 {P/2, P}）',
          'create_generated_clock -name ad9361_tx_fb_clk -source [get_ports rx_clk_in_p] -edges {2 3 4} [get_ports tx_clk_out_p]',
          '# RX 数据为 LVDS 训练校准接口（lvds_calibration 运行时训练），数据按源同步标定处理',
          'set_false_path -from [get_ports {rx_data_in_p[*]}]',
          'set_false_path -from [get_ports {rx_data_in_n[*]}]',
          'set_false_path -from [get_ports rx_frame_in_p]',
          'set_false_path -from [get_ports rx_frame_in_n]',
          '# TX 数据时钟由本设计产生并随位流标定，输出数据按源同步处理',
          'set_false_path -to [get_ports {tx_data_out_p[*]}]',
          'set_false_path -to [get_ports {tx_data_out_n[*]}]',
          'set_false_path -to [get_ports tx_frame_out_p]',
          'set_false_path -to [get_ports tx_frame_out_n]']
open(OUT + r'\lvds_timing.xdc', 'w', encoding='utf-8', newline='\n').write('\n'.join(timing) + '\n')
print('lvds_timing.xdc written, period =', period)

groups = ['# 异步时钟组：PS FCLK 域与 AD9361 LVDS 域经异步 FIFO 交接（重建）',
          'set_clock_groups -asynchronous -group [get_clocks ad9361_data_clk] -group [get_clocks clk_fpga_0] -group [get_clocks clk_fpga_1] -group [get_clocks clk_fpga_2]']
open(OUT + r'\lvds_clock_groups_impl.xdc', 'w', encoding='utf-8', newline='\n').write('\n'.join(groups) + '\n')
print('lvds_clock_groups_impl.xdc written')
