"""补齐 xparameters.h 的 AXI DMA 驱动参数（值从 r10 ELF 配置表精确提取）。"""
p = r'D:\Document\tempPrj\XC7Z035-AD9361\.auto\plbuild\appbuild\xparameters.h'
d = open(p, encoding='utf-8', errors='replace').read()
block = '''
/* AXI DMA 驱动参数（值自 r10 P351_APP.elf 配置表提取，2026-11-02） */
#define XPAR_XAXIDMA_NUM_INSTANCES 2U
#define XPAR_AXIDMA_0_DEVICE_ID 0U
#define XPAR_AXIDMA_0_BASEADDR 0x40400000U
#define XPAR_AXIDMA_0_SG_INCLUDE_STSCNTRL_STRM 0U
#define XPAR_AXIDMA_0_INCLUDE_MM2S 0U
#define XPAR_AXIDMA_0_INCLUDE_MM2S_DRE 0U
#define XPAR_AXIDMA_0_M_AXI_MM2S_DATA_WIDTH 32U
#define XPAR_AXIDMA_0_INCLUDE_S2MM 1U
#define XPAR_AXIDMA_0_INCLUDE_S2MM_DRE 0U
#define XPAR_AXIDMA_0_M_AXI_S2MM_DATA_WIDTH 64U
#define XPAR_AXIDMA_0_INCLUDE_SG 0U
#define XPAR_AXIDMA_1_DEVICE_ID 1U
#define XPAR_AXIDMA_1_BASEADDR 0x40410000U
#define XPAR_AXIDMA_1_SG_INCLUDE_STSCNTRL_STRM 0U
#define XPAR_AXIDMA_1_INCLUDE_MM2S 1U
#define XPAR_AXIDMA_1_INCLUDE_MM2S_DRE 0U
#define XPAR_AXIDMA_1_M_AXI_MM2S_DATA_WIDTH 64U
#define XPAR_AXIDMA_1_INCLUDE_S2MM 0U
#define XPAR_AXIDMA_1_INCLUDE_S2MM_DRE 0U
#define XPAR_AXIDMA_1_M_AXI_S2MM_DATA_WIDTH 32U
#define XPAR_AXIDMA_1_INCLUDE_SG 0U
'''
if 'XPAR_AXIDMA_0_DEVICE_ID' not in d:
    d += block
open(p, 'w', encoding='utf-8', newline='\n').write(d)
print('axidma params added')
