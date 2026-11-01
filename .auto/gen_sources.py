"""生成显式源文件清单 sources.mk（全部排除规则集中于此）。"""
import os

BASE = 'D:/Document/tempPrj/XC7Z035-AD9361/.auto/plbuild'
LIB = BASE + '/app_bsp/ps7_cortexa9_0/libsrc'
BAD = ('ARMv8', 'cortexr5', 'microblaze', 'profile', 'armcc', 'iccarm', 'tmp_src', 'standalone_v7_6/src/clocking')

csrc, ssrc, incs = [], [], []
for root, dirs, files in os.walk(LIB):
    norm = root.replace('\\', '/')
    rel = norm.split('/libsrc/')[-1]
    if any(b in rel for b in BAD):
        dirs[:] = []
        continue
    # standalone 源码根级文件（与子目录版本重复）排除
    if '/standalone_v' in norm and norm.endswith('/src'):
        for f in list(files):
            # inbyte/outbyte 是根级独有文件（子目录无副本），必须保留
            if f.endswith(('.c', '.S')) and f not in ('inbyte.c', 'outbyte.c'):
                files.remove(f)
    for f in files:
        p = norm + '/' + f
        if f.endswith('.c'):
            csrc.append(p)
        elif f.endswith('.S'):
            ssrc.append(p)
        elif f.endswith('.h') and '/src/' in p:
            incs.append(norm)

csrc = sorted(set(csrc))
ssrc = sorted(set(ssrc))
incs = sorted(set(incs))

with open(BASE + '/appbuild/sources.mk', 'w', newline='\n') as f:
    f.write('# 自动生成：显式源文件清单（排除错误架构/armcc变体/tmp_src/standalone根级重复）\n')
    f.write('LIB_CSRC := \\\n' + ' \\\n'.join(' ' + x for x in csrc) + '\n')
    f.write('\nLIB_SSRC := \\\n' + ' \\\n'.join(' ' + x for x in ssrc) + '\n')
    f.write('\nINCLUDES := -I. ' + ' '.join('-I' + x for x in incs) + '\n')
print('csrc:', len(csrc), 'ssrc:', len(ssrc), 'incdirs:', len(incs))
