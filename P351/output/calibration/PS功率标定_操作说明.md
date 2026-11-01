# P351 PS功率标定：填数值后同步修改两份工程

## 最简单的用法

双击本目录的 **`配置PS功率标定.cmd`**，按提示填写：

1. PCM/FM 在 **TRX2、70 MHz、AD9361衰减0 dB** 时的实测平均功率，单位 **dBm**。
2. SOQPSK 对应的实测平均功率；暂时没有独立数据可回车共用模型。共用不等于已完成该模式实测标定。
3. 后级选择一种填写方式：
   - **方式1：整链净增益(dB)**。已包含功放、混频器、滤波器、分路器和线缆的全部增益/损耗。
   - **方式2：功放增益(dB)＋其余路径总损耗(dB)**。脚本计算净增益＝功放增益−路径损耗。
4. 查看计算预览，输入 **`YES`** 才写入；输入其他内容即取消。

脚本需要 Python 3.9或更新版本（本机使用3.12），无第三方库依赖；不会联网、控制板卡、下载、烧录、开启发射或改UI。

“0dBm衰减功率”应理解为 **0 dB衰减时的实测功率**。0 dB是衰减档位，不是输出0 dBm。
“功放功率”不能直接填进增益栏：额定输出瓦数、P1dB或饱和输出dBm均不等于增益。若只有输入/输出的实际平均功率，则增益(dB)＝输出(dBm)−输入(dBm)，且须处于正常线性工作区。

## 脚本究竟改哪里

同时生成完全相同的两份文件：

```text
C:\Users\Jump\Desktop\AD9361TX\migration\p351\ps\src\p351_up70_calibration_data.h
C:\Users\Jump\Desktop\AD9361TX\migration\p351\vitis_offline\P351_APP\src\p351_up70_calibration_data.h
```

生成表中的 `zero_mdbm` 是TRX2零衰减基准，`net_gain_mdb` 是后级净增益，数值以千分之一dB/dBm存储。例如5 dBm写成5000，18 dB写成18000。

计算逻辑在 `ps/src/p351_up70_calibration.h`，正常情况下不用手工修改。
`boot_pcm_fm_profile.h` 中的 `BOOT_PCM_FM_ZERO_MDBM` 属于独立的“上电自动发射”方案，**不是当前屏幕整机标定入口**。本工具不修改它，也不启用自动发射或硬件资格许可。

PC UI仍是独立AD9361控制，不把这份屏幕整机功放增益用于PC功率换算。

## 写入后还要做什么

写入前可先运行 `simulate_screen_control.py --config 自己的标定.json --with-pl`，对原生屏幕指令、PS业务、状态回读及PL SPI轨迹做隔离仿真，检查可达性；不会写入生产标定。完整流程见同目录 `显示屏控制_仿真与标定说明_20261020.md`。

1. 运行脚本前先在Vitis保存文件；脚本运行时不要同时编辑标定头文件或Build。
2. 写入成功后，在Vitis对 `P351_APP` 执行 **Refresh/F5 → Build Project**。
3. 新参数进入 `P351_APP/Debug/P351_APP.elf` 后，用户后续下载此新ELF才在板上生效。
4. 若使用Flash启动，还需重新生成包含新ELF的BOOT.BIN并由用户烧录；旧BOOT不会自动变化。
5. 屏幕仍须满足外部10 MHz资格、TFS锁定、外部衰减回读、PL/PS匹配等条件，标定表不是绕过这些检查的开关。

本次交付工具时没有真实测量数值，因此**没有给生产头文件填入演示值，也没有重建带假标定的ELF**。

## 计算例子——仅说明，不能照抄到实物

假设TRX2零衰减实测为 **+5 dBm**，功放增益 **30 dB**，其余路径损耗 **12 dB**：

- AD9361固定IF目标为−30 dBm，所需片内衰减＝5−(−30)＝**35 dB**。
- 后级净增益＝30−12＝**18 dB**。
- 后级零衰减时，预测最终功率＝−30＋18＝**−12 dBm**。
- 若屏幕最终目标为−30 dBm，所需后级衰减＝−12−(−30)＝**18 dB**。

片内衰减按0.25 dB、外部衰减按0.5 dB向上取整；目标超出实际可达范围时PS拒绝，不默默限幅或假装已经达到目标。向上取整只约束模型预测，不消除测量误差、功放压缩和温漂。

## 命令行用法

先进入目录：

```powershell
cd C:\Users\Jump\Desktop\AD9361TX\migration\p351
```

交互写入：

```powershell
python .\configure_ps_power.py --interactive --apply
```

只有功放增益与损耗时，先预览（以下仍为演示数值）：

```powershell
python .\configure_ps_power.py --p351-zero-dbm 5 --amplifier-gain-db 30 --path-loss-db 12
```

确认是自己的真实数据后，在原命令末尾加 `--apply` 即同步写入。
若PCM/FM与SOQPSK零衰减功率不同，增加 `--soqpsk-zero-dbm`，例如：

```powershell
python .\configure_ps_power.py --p351-zero-dbm 5 --soqpsk-zero-dbm 6.25 --net-gain-db 18 --apply
```

也可填写 `PS功率标定_填写模板.json` 的两个 `null` 为真实数字：

```powershell
python .\configure_ps_power.py --config .\PS功率标定_填写模板.json
python .\configure_ps_power.py --config .\PS功率标定_填写模板.json --apply
```

`--frequency-mhz`、`--target-dbm`、`--rate-hz` 只改变此次预览点，不修改屏幕草稿或上电频率。

## 测量条件和精度边界

- 使用本P351的TRX2，不沿用旧7020的“零衰减功率”数字。
- 按屏幕实际波形测量：70 MHz、数字幅度26214、模拟带宽56 MHz，记录调制方式和码率。不要未经核实把CW功率当成两种调制的平均功率。
- 对调制信号采用适合带宽的平均/信道功率测量，不拿单个频谱点的峰值当总平均功率。补偿实体衰减器和测量线缆损耗，并核对仪器输入限值。
- 整链净增益为后级零衰减时“最终输出口实际平均功率−实际IF输入平均功率”。实际IF输入可能因0.25 dB量化而略低于−30 dBm，精细标定应使用实测输入值。
- 快速填写几个数得到的是**统一线性模型**，不是100k～20M、全频段、全模式都已经逐点实测。工具生成的快速表 `confidence=1`，不能标成完整实测标定。
- 正式精细标定可用原 `calibrate_up70.py` 的 `if_profiles/rf_profiles` JSON格式，通过本工具 `--config` 同步写入；支持分别限定模式、频率、码率与实测依据。格式见 `上变频功率配置_使用说明.md`。不要将一处测量冒充整个范围的实测。

## 备份与错误处理

备份位于 `logs/power_calibration_updates/北京时间时间戳/`：

- `canonical_before.h`：规范PS原标定。
- `vitis_before.h`：Vitis原标定。
- `input_config.json`：本次输入。
- `transaction.json`：两份文件的原/新散列和执行/回滚状态。

写入前检查两份原标定一致；如果不一致则拒绝覆盖，避免丢失未同步的测量记录。第二份写入失败会回滚第一份；并发修改时不覆盖他人数据，保留备份并明确报错。掉电或进程被强制终止仍可能中断两文件事务，不能保证操作系统级跨文件原子性；重新使用前核对两份头文件一致。

恢复时将两个 `*_before.h` 分别复制回上述对应路径，然后重新Build。不要同时运行多个配置窗口。

## 本次工具验证

12项隔离工程测试通过，覆盖预览不写文件、双副本一致、备份、重复配置、第二份写入失败回滚、暂存失败、Ctrl+C中断、并发编辑保护、错误输入、分模式基准和真实生产C计算兼容性。验证未把演示数值写入真实PS工程。证据为 `logs/power_wizard_20261020/results.json`。
