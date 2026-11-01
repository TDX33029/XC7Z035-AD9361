import re, shutil, sys

base = r'D:\Document\tempPrj\XC7Z035-AD9361\P351\pl'
p = base + r'\source_bd.tcl'
d = open(p, encoding='utf-8').read()

# backup once
import os
bak = base + r'\source_bd.tcl.bak_sdpatch'
if not os.path.exists(bak):
    shutil.copyfile(p, bak)

subs = [
    ('CONFIG.PCW_EN_SDIO0 {0}', 'CONFIG.PCW_EN_SDIO0 {1}', 1),
    ('CONFIG.PCW_SD0_PERIPHERAL_ENABLE {0}', 'CONFIG.PCW_SD0_PERIPHERAL_ENABLE {1}', 1),
    ('CONFIG.PCW_SDIO_PERIPHERAL_VALID {0}', 'CONFIG.PCW_SDIO_PERIPHERAL_VALID {1}', 1),
    ('CONFIG.PCW_ACT_SDIO_PERIPHERAL_FREQMHZ {10.000000}', 'CONFIG.PCW_ACT_SDIO_PERIPHERAL_FREQMHZ {100.000000}', 1),
]
for old, new, cnt in subs:
    assert old in d, old
    d = d.replace(old, new, cnt)

m = re.search(r'CONFIG\.PCW_MIO_TREE_PERIPHERALS \{([^}]*)\}', d)
tree = m.group(1)
toks = tree.split('#')
assert len(toks) == 54, len(toks)
for i in range(40, 46):
    assert toks[i] == 'GPIO', (i, toks[i])
    toks[i] = 'SD 0'
d = d.replace(tree, '#'.join(toks), 1)

open(p, 'w', encoding='utf-8', newline='\n').write(d)
for pat in ['PCW_EN_SDIO0 {1}', 'PCW_SD0_PERIPHERAL_ENABLE {1}',
            'PCW_SDIO_PERIPHERAL_VALID {1}', 'ACT_SDIO_PERIPHERAL_FREQMHZ {100.000000}',
            'SD 0#SD 0#SD 0#SD 0#SD 0#SD 0']:
    print(pat, '->', pat in d)
print('tokens40-45:', tree and '#'.join(toks).split('#')[40:46])
