P351 SD 卡部署说明（2026-11-02：GCC 9.2.1 功能镜像待硬件验收）
====================================================

当前状态（重要）
  - 已知可用基线仍是 v3：屏幕和上位机都能识别，但 v3 没有 DHCP、没有
    RDFQ/RDFA，因此“任何一端修改，另一端同步显示”在 v3 上不可能实现。
    v3 上屏幕点“应用并发射”被拒后显示“未就绪”是原有保护行为，不是新故障。
  - SD 卡当前记录：2026-11-02 已写入 DMA TX 修复镜像（1D4F…）并刷新缓存，
    等待上电验收。回退镜像 BOOT_SAFE_v3.BIN 同时保留在卡内。
    目标是验证“保护拒绝时仍双向同步请求值”。
  - 该功能镜像尚未做硬件启动验收。它与此前失败的 GCC 12.3.1/UARTFIX 镜像
    不同：控制台地址、IRQ VFP、启动代码均已重新核对，且 _boot 指令与原始
    GCC 9.2.0 已知可用 ELF 逐字节一致。
  - 若功能镜像仍不能启动，先用同工具链编译的纯净 v3 对照镜像
    BOOT_GCC921_V3SRC_CONTROL.BIN 隔离“构建链问题”还是“功能代码问题”。

本目录文件
  BOOT.BIN                           待验收功能镜像（GCC 9.2.1，人工RF联调 + DHCP + RDFQ/RDFA）
  BOOT_SAFE_v3.BIN                   已知可用回退镜像（v3，静态 192.168.1.2，无 DHCP/RDFQ）
  BOOT_GCC921_MANUAL.BIN             与 BOOT.BIN 相同，功能镜像原始文件
  BOOT_GCC921_V3SRC_CONTROL.BIN      同工具链/BSP 的纯净 v3 对照镜像（用于隔离启动问题）
  BOOT_ATTEMPT_UARTFIX_MANUAL.BIN    GCC 12 失败尝试，仅留档，不得写卡
  BOOT_ATTEMPT_UARTFIX_PROTECTED.BIN GCC 12 失败尝试，仅留档，不得写卡
  SHA256SUMS.txt                     全部镜像的 SHA256

当前 SHA256（完整值以 SHA256SUMS.txt 为准）
  1d4fdd9175f14e9ffaf5901607f3308cfa1174312db5f8c17b7f2b5fd1123070  BOOT_GCC921_MANUAL.BIN（功能镜像，DMA TX 配置已修，已写卡待上电）
  16ae4c729e8a1e841446b4b47505ee7648d5e65f0ab7b8bf646708af2f9b5e08  BOOT_GCC921_V3SRC_CONTROL.BIN（纯净对照）
  c4e7ef89a503746a70bcf92a2ddd49c44c1dbf35b1a88b2711edcb760501e1a1  BOOT_SAFE_v3.BIN（v3 回退）
  705bb8588214c04cab118b7486023dd363176a6aca1b745b3ebc27530418662d  BOOT_ATTEMPT_UARTFIX_MANUAL.BIN（失败留档）
  0eeae3698afccf4e4e686a71b9b4d4ba2b168de723ad081b6e38a66c4922d989  BOOT_ATTEMPT_UARTFIX_PROTECTED.BIN（失败留档）

GCC 9.2.1 功能镜像的构建与静态核对
  - 工具链：xpack arm-none-eabi-gcc 9.2.1；BSP：HSI 2021.2 生成的
    standalone_v7_6 + axidma_v9_1 / emacps_v3_15 / gpiops_v3_5 /
    scugic_v4_6 / uartps_v3_11。
  - xpack 9.2.1 没有 hard-float multilib，因此 BSP 与 app 全部用
    -mfloat-abi=softfp -mfpu=vfpv3 重编；不要混入旧的 hard-float 目标文件，
    否则会出现 “uses VFP register arguments, libc.a does not”。
  - 功能 app 以 -O0 -g3 重编（与原厂已知可用 app 的优化级别一致），
    text 464,828 / data 15,798 / bss 5,093,872；比 -O2 版大约 0x20000 B，
    但仍远低于 0x200000 的 BSS 起始地址。
  - 已核对：入口 0x100000；控制台 outbyte 写 0xE0001000（PS UART1）；
    IRQ/FIQ 保存 VFP（vpush d0-d7/d16-d31 与 FPSCR/FPEXC）；
    0x10012c 起的 _boot 指令与原始 GCC 9.2.0 ELF 完全一致；
    app 分区文件偏移 0xCC9C00，长度 0x7800C，内容与 ELF 首个 LOAD 逐字节一致。
  - FSBL 与位流区域沿用 v3 的同一份；与 v3 镜像相比只有 app 分区头里的
    长度/校验和 5 个字节不同，FSBL/位流字节本身一致。

为什么 v3 不能同步（重要）
  - v3 的 PS 固件没有 RDFQ/RDFA 请求值快照，也没有“PC 请求回写屏幕草稿”。
    上位机 r12 读 RDFQ 超时后自动回退到 FRFA，只能显示板端已应用状态；
    屏幕也只显示板端已提交状态。所以 v3 上两端不同步是固件能力缺失，
    不是串口/网络配置问题。
  - 屏幕“未就绪：请查询标定、参考及故障”来自最终 RF 路径的硬保护：
    标定表为空、整机资格标志为 0、需要外 10 MHz 与 TFS 状态 0x300。
    保护拒绝时不会发射，也不会把请求值伪装成已应用。

功能镜像上电后应看到的行为
  0. 上电后屏幕当前页应先短暂出现红字“P351: BOOT OK”。这是 app 启动后
     直接发的 TJC xstr，不依赖 v2 握手，用来证明镜像已被加载且 UART0 可写。
     之后按顺序显示阶段名：board init → SPI init → AD9361 init →
     AD9361 ok → LVDS cal → LVDS ok → DMA RX → GIC init → DMA TX →
     DMA ok → NET init → NET ok → LOOP。停在哪一条，就是该阶段没返回；
     tx_boot_fail 会显示“P351: <失败阶段>”并保持 RF OFF。
     这样即使 UART1 没接，也能区分“镜像没启动”与“启动后哪个硬件阶段失败”。
  1. 屏幕结束“等待板端确认”，正常页面可操作；上位机 r12 能自动发现板卡。
  2. 屏幕修改参数并点“应用并发射”：保护仍拒绝，屏幕仍显示“未就绪”；
     上位机整机页应在下一次轮询后显示屏幕刚提交的请求值，并标注
     “板端未应用 · 未就绪（保护/标定/参考）”，不会显示已发射。
  3. 上位机修改整机页并提交：保护仍拒绝；固件把 PC 请求值写入屏幕草稿
     （dm/dr/dc/dfr/db/dn/di/dsl/dsh/dsx/dsb/df/dpo），dirty=1；
     屏幕在 1~2 秒内重绘为 PC 请求值，同时保留未应用提示，不会发射。
  4. 上述同步只改变显示/草稿，不改变 FRFQ 已应用状态，不访问 SPI，
     不绕过标定/参考/TFS。

已定位的 DMA TX 失败（2026-11-02）
  - 现场屏幕停在“P351: DMA TX FAILED”。根因在 BSP 的 AXI DMA 配置表：
    HSI 2021.2 生成的 xaxidma_g.c 只有 RX 实例（0x40400000），
    没有 TX 实例（0x40410000），所以 XAxiDma_LookupConfig(1) 返回 NULL，
    dma_tx_bit_init() 在第一步就失败，与 RF/同步代码无关。
  - 已按原厂 v3 位流实际参数补齐两个实例：RX=0x40400000(S2MM)、
    TX=0x40410000(MM2S)，burst=16、SG length width=26。
    HSI 生成的 burst=64/SG=14 与实际位流不符，已一并纠正。
  - 修复后镜像 SHA256 1d4fdd91…，app 分区已核对与 ELF 一致；
    已写入 SD 卡并刷新缓存，尚未上电验证。
  - BSP 配置表的修复文件已入库：
    P351/output/firmware/bsp_patches/（含 apply_axidma_patch.ps1）。
    BSP 重新生成后先应用该补丁，否则会再次出现 DMA TX FAILED。

写卡步骤（当前卡已写好，通常不需要再写）
  0. 当前 SD 卡已写入 DMA TX 修复镜像 1D4F…（BOOT.BIN），
     BOOT_SAFE_v3.BIN=C4E7… 保持在卡内。
  1. 若要重写：板卡断电取卡，插入电脑（FAT32）。
  2. 在仓库根目录运行（把 E 换成实际盘符）：
       功能镜像：
         powershell -ExecutionPolicy Bypass -File P351/output/tools/write_sd_boot.ps1 -Drive E -Image BOOT_GCC921_MANUAL.BIN
       纯净 v3 对照：
         powershell -ExecutionPolicy Bypass -File P351/output/tools/write_sd_boot.ps1 -Drive E -Image BOOT_GCC921_V3SRC_CONTROL.BIN
       回退 v3：
         powershell -ExecutionPolicy Bypass -File P351/output/tools/write_sd_boot.ps1 -Drive E -Image BOOT_SAFE_v3.BIN
     脚本只接受可移动磁盘，会备份旧 BOOT.BIN，复制后校验 SHA256。
  3. 写完后刷新缓存并安全弹出再插回板卡：
       powershell -NoProfile -Command "Write-VolumeCache -DriveLetter E"
  4. 上电等待约 15 秒；屏幕应结束等待，上位机应发现板卡。

上位机连接方式
  - 功能镜像：DHCP 优先；没有 DHCP 时回退 192.168.1.2/24。
    当前 PC 有线网卡为 192.168.1.111/24，直连或同网段路由器都可发现。
    若路由器下发其它网段，PC 与板卡在同一 LAN 时 r12 会自动发现。
  - v3 回退镜像：固定 192.168.1.2，PC 有线网卡需为 192.168.1.x。
  - 使用含 RDFQ 回填的 r12：
      P351/output/ui/P351_Control_r12/P351_Control_r12.exe
      SHA256 6178d03889b44685de65a8ce786af5b5fe7e30ea8431adae6b0f48149c339d08
    旧版 UI 仍可控制板卡，但不会回填被保护拒绝的请求值。
  - 首次运行如出现 Windows 防火墙弹窗，请允许专用/公用网络。

UART1 启动日志（启动失败时必做）
  - 调试串口 UART1：JM1 pin37 = W29 = IO_L17P_12 为板端 TX，
    pin39 = W30 为 RX，pin33/35 = GND，115200 8N1。
    USB-TTL 的 RX 接 W29；不要接屏幕 UART0 的 AJ28（pin17）。
  - 运行（不需要管理员）：
      powershell -ExecutionPolicy Bypass -File P351/output/tools/serial_capture.ps1
    默认 COM15、115200、采集 20 秒，日志写到 P351/output/logs/。
  - v3 上电约 1 秒应出现“=== AD9361TX 1R1T LVDS ... ===”横幅；
    功能镜像同样应先出现该横幅。无横幅说明停在 FSBL/加载阶段，
    与 RDFQ/DHCP 功能无关。

若功能镜像仍不能启动
  1. 先写 BOOT_GCC921_V3SRC_CONTROL.BIN（同工具链、纯净 v3 源码）：
     - 对照镜像能启动 → 构建链可用，问题在功能代码，回退后继续定位；
     - 对照镜像也不能启动 → 仍是工具链/BSP 与原始 GCC 9.2.0/v7_3 的差异，
       需要取得原版 Vitis 2020.2 GCC 9.2.0 + standalone_v7_3 或改用
       原始 app 二进制打补丁的方案。
  2. 无论哪种情况，先写回 BOOT_SAFE_v3.BIN 恢复屏幕/上位机可用。

回退
  1. 断电取卡，把 BOOT_SAFE_v3.BIN 复制为 BOOT.BIN，刷新缓存后插回上电。
  2. v3 固定 192.168.1.2；PC 有线网卡为 192.168.1.111/24 时可直接发现。

注意
  - BootROM 只读卡根 BOOT.BIN；换版本必须把所选镜像改名为 BOOT.BIN，
    或使用脚本 -Image。
  - 功能镜像仍是人工RF联调版：PC 独立 AD9361 页可直发，默认 89.75 dB、
    无自动发射；屏幕最终 RF 路径仍受标定/参考/TFS 保护。
    上电测试仍须确认实体负载/仪器保护，不能把“未就绪”当作硬件验收。
  - RDFQ/RDFA 只表示“用户请求值”，不代表已应用或正在发射；
    FRFQ/FRFA 仍是唯一已应用状态。
