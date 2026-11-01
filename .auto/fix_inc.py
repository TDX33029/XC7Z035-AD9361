import shutil, os, glob
B = r'D:\Document\tempPrj\XC7Z035-AD9361\.auto\plbuild\app_bsp\ps7_cortexa9_0'
os.chdir(r'D:\Document\tempPrj\XC7Z035-AD9361\.auto\plbuild\appbuild')
# 1) 从 HSI 生成的 include 目录恢复（正确的 CortexA9 头）
inc = B + r'\include'
for f in glob.glob(inc + r'\*.h'):
    shutil.copyfile(f, os.path.basename(f))
# 2) 展平 libsrc 头文件，排除错误架构
bad = ('ARMv8', 'cortexr5', 'microblaze', 'profile')
copied = 0
for f in glob.glob(B + r'\libsrc\**\*.h', recursive=True):
    if any(b in f for b in bad):
        continue
    shutil.copyfile(f, os.path.basename(f))
    copied += 1
print('headers flattened:', copied)
# 3) stdout/stdin -> UART1 (JM1 调试口)
p = 'bspconfig.h'
d = open(p, encoding='utf-8', errors='replace').read()
d = d.replace('#define STDOUT_BASEADDRESS 0xE0000000', '#define STDOUT_BASEADDRESS 0xE0001000')
d = d.replace('#define STDIN_BASEADDRESS 0xE0000000', '#define STDIN_BASEADDRESS 0xE0001000')
open(p, 'w', encoding='utf-8', newline='\n').write(d)
# 4) xparameters.h: stdout + 缺失的 DEVICE_ID
p = 'xparameters.h'
d = open(p, encoding='utf-8', errors='replace').read()
d = d.replace('#define STDOUT_BASEADDRESS 0xE0000000', '#define STDOUT_BASEADDRESS 0xE0001000')
d = d.replace('#define STDIN_BASEADDRESS 0xE0000000', '#define STDIN_BASEADDRESS 0xE0001000')
need = {
 'XPAR_AXI_DMA_0_DEVICE_ID': 0,
 'XPAR_AXI_DMA_TX_0_DEVICE_ID': 1,
 'XPAR_PS7_GPIO_0_DEVICE_ID': 0,
 'XPAR_PS7_SPI_0_DEVICE_ID': 0,
 'XPAR_SPI_0_DEVICE_ID': 0,
 'XPAR_XUARTPS_0_DEVICE_ID': 0,
 'XPAR_XEMACPS_0_DEVICE_ID': 0,
 'XPAR_PS7_UART_0_DEVICE_ID': 0,
}
added = []
for k, v in need.items():
    if ('#define %s ' % k) not in d:
        d += '\n#define %s %dU\n' % (k, v)
        added.append(k)
open(p, 'w', encoding='utf-8', newline='\n').write(d)
print('DEVICE_ID added:', added)
print('xtime_l.h A53 leak:', 'CORTEXA53' in open('xtime_l.h', encoding='utf-8', errors='replace').read())
