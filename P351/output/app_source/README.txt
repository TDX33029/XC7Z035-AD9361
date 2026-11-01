P351 PS权威源码目录（2026-10-18，V9平台/固定70MHz屏幕保护候选）

本目录独立于原7020 Vitis工程；修改原vitis目录不会更新P351候选程序。
main.c负责板级初始化、物理RX2/TX2控制、任务及接收监视。
lvds_calibration.c负责RX2的I/Q双视图device PN严格训练。
p351_board.c/.h负责板型、PL修复能力及射频开关保护。
board_config.h与UDP的CFGQ快照为V9、38字；RXSQ是只读RX配置/源状态。
screen_v2.h解析历史v2/v3并新增v4/op12/63B固定IF原子APPLY。
旧v1/v2/v3屏幕写命令仅返回拒绝，不再改变LO、参考、TFS或外部衰减。
screen_controller.inc负责固定70MHz、逐调制功率资格、本地PN11/同步字及事务状态。
UART接收仅校验并排队，不在接收/中断上下文发射；独立STOP不依赖页面/参数/握手。
p351_tx_system.h是历史双频段候选规划，已不适用于最新固定70MHz需求，保持未放行。
当前初始化LO=70MHz、TRX2目标=-30dBm；未标定时参数使用89.75dB保护，不代表已达到-30dBm。
PCM/FM标定入口为boot_pcm_fm_profile.h；SOQPSK标定入口为p351_if70_profile.h。
两种调制均使用0.8数字幅度、56MHz模拟滤波、70MHz中心和-30dBm目标，但必须分别实测。
56MHz不是B99；高码率不能直接套用20MHz滤波。标定数据必须对应此固定带宽。
标定有效时AD9361衰减=实测零衰减功率-目标功率，范围0..89.75dB、步进0.25dB。
未标定时APPLY返回NOT_READY，不把最大衰减保护误称-30dBm。
屏幕不提供CW：历史CW诊断为LO+2MHz，不能把72MHz标作70MHz。
其他射频变频和最终频率/衰减交给外部上变频器；固定IF事务不操作TFS。
p351_clock_profile.h定义默认外10MHz驯服；AD9361 REFCLK仍40MHz。

匹配PL：migration/p351/pl及其Vivado工程。不能只更新ELF沿用旧RX位流。
本轮最终ELF：../build_fixed_if70_screen_bw56_20261018/P351_OFFLINE_CANDIDATE.elf。
本轮构建BSP：migration/p351/bsp_confirmed_r2；必须配套核验P351 V9位流及PL能力位。
UI入口：migration/p351/ui/启动P351_UI.bat。
本轮说明：migration/p351/P351_固定70MHz中频与功率标定_20261017.md。
本轮核验：migration/p351/logs/screen_if70_20261018/及build_fixed_if70_screen_20261018.log。
本轮屏幕：migration/p351/screen/fixed_if70_v5_20261018，传输协议v4/op12。
工程版本v5与线协议版本v4不是同一个编号；旧最终频段工程不能混用。

保持P351_RF_HARDWARE_QUALIFIED=0、BOOT_PCM_FM_ENABLED=0。
TRX2的70MHz模拟通路、实际功率、外10M锁定/失锁、频谱仍需上板验收，资格宏未放行。
当前为离线数字验证成果，不是实板验收结论；源代码与TFT必须和同批ELF配套。
默认外10MHz经ADF4002驯服40MHz；AD9361仍声明40MHz REFCLK，不是PL直接跑10MHz。
缺参考或失锁时PL关物理TX路径、PS关RF/LO/BIT，不自动回退或自动重开发射。
用户再次点击APPLY才可显式重新执行外10MHz资格；无需时钟设置页。
不要把旧板20dBm基准当作P351实测功率。
上板前须确认实际电压，PA到RX自收必须使用合适的外部衰减器。
