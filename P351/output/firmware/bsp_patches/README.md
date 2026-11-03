# BSP 补丁：AXI DMA 配置表（axidma_v9_1）

## 问题
HSI 2021.2 生成的
`.auto/plbuild/app_bsp/ps7_cortexa9_0/libsrc/axidma_v9_1/src/xaxidma_g.c`
只有 RX 一个实例，缺少 TX 实例（DeviceId 1 / 0x40410000）。应用调用
`XAxiDma_LookupConfig(XPAR_AXI_DMA_TX_0_DEVICE_ID)` 时返回 NULL，
`dma_tx_bit_init()` 直接失败，屏幕停在 `P351: DMA TX FAILED`。

同时，生成器给出的 RX 参数 burst=64 / SG length width=14 与本板 BOOT.BIN
中的原厂 v3 位流不一致。

## 修复
按原始 GCC 9.2.0 已知可用 ELF 中的 `XAxiDma_ConfigTable`（2 × 68 B）恢复：

| DeviceId | 地址 | 方向 | Mm2sWidth | S2mmWidth | Burst | SG length |
|---|---|---|---|---|---|---|
| 0 | 0x40400000 | S2MM (RX) | 32 | 64 | 16 | 26 |
| 1 | 0x40410000 | MM2S (TX) | 64 | 32 | 16 | 26 |

本目录的 `axidma_v9_1/xaxidma_g.c` 就是修复后的完整文件。

## 应用
BSP 重新生成后，用以下命令覆盖（默认路径为当前自动化构建 BSP）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File P351/output/firmware/bsp_patches/apply_axidma_patch.ps1
```

如 BSP 在其它位置，传 `-BspRoot <...>/ps7_cortexa9_0`。

应用后必须重编 BSP 与 app。核对：

```bash
arm-none-eabi-nm -S <elf> | grep XAxiDma_ConfigTable   # 大小应为 0x88
arm-none-eabi-objdump -s -j .rodata <elf> | grep -A8 40410000
```

并确认启动日志不再出现 `[txbit] init failed`，屏幕阶段诊断能走过
`DMA TX` → `DMA ok`。

## 说明
- 应用构建目录里的 `xparameters.h` 需包含
  `XPAR_AXI_DMA_TX_0_DEVICE_ID 1U` 与 `XPAR_AXI_DMA_TX_0_BASEADDR
  0x40410000U`；当前 `.auto/plbuild/appbuild_gcc921/xparameters.h`
  已包含，重新生成 XSA 后要重新核对。
- 该补丁只改 BSP 配置表，不动位流、不改 RF/保护逻辑。
