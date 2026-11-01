import re, shutil, os

base = r'D:\Document\tempPrj\XC7Z035-AD9361\P351\pl'
p = base + r'\source_bd.tcl'
d = open(p, encoding='utf-8').read()

bak = base + r'\source_bd.tcl.bak_uart1'
if not os.path.exists(bak):
    shutil.copyfile(p, bak)

# 1) EMIO UART1 enable + IO switching
subs = [
    ('CONFIG.PCW_EN_EMIO_UART1 {0}', 'CONFIG.PCW_EN_EMIO_UART1 {1}', 1),
    ('CONFIG.PCW_UART1_UART1_IO {MIO 48 .. 49}', 'CONFIG.PCW_UART1_UART1_IO {EMIO}', 1),
]
for old, new, cnt in subs:
    assert old in d, old
    d = d.replace(old, new, cnt)

# 2) MIO tree tokens 48/49 -> GPIO
m = re.search(r'CONFIG\.PCW_MIO_TREE_PERIPHERALS \{([^}]*)\}', d)
tree = m.group(1)
toks = tree.split('#')
assert len(toks) == 54, len(toks)
assert toks[48] == 'UART 1' and toks[49] == 'UART 1', (toks[48], toks[49])
toks[48] = 'GPIO'
toks[49] = 'GPIO'
d = d.replace(tree, '#'.join(toks), 1)

# 3) BD ports for EMIO UART1 (connect right after the screen_uart connects)
anchor = 'connect_bd_net -net screen_uart_rx_1 [get_bd_ports screen_uart_rx] [get_bd_pins processing_system7_0/UART0_RX]'
assert anchor in d
add = (
    'connect_bd_net -net dbg_uart_tx_1 [get_bd_ports dbg_uart_tx] [get_bd_pins processing_system7_0/UART1_TX]\n'
    '  connect_bd_net -net dbg_uart_rx_1 [get_bd_ports dbg_uart_rx] [get_bd_pins processing_system7_0/UART1_RX]'
)
d = d.replace(anchor, anchor + '\n  ' + add, 1)

# 4) create the ports (after screen_uart port creation)
anchor2 = 'set screen_uart_tx [ create_bd_port -dir O screen_uart_tx ]'
assert anchor2 in d
portadd = ('set dbg_uart_rx [ create_bd_port -dir I dbg_uart_rx ]\n'
           '  set dbg_uart_tx [ create_bd_port -dir O dbg_uart_tx ]')
d = d.replace(anchor2, anchor2 + '\n  ' + portadd, 1)

open(p, 'w', encoding='utf-8', newline='\n').write(d)
for pat in ['PCW_EN_EMIO_UART1 {1}', 'PCW_UART1_UART1_IO {EMIO}',
            'dbg_uart_tx [ create_bd_port', 'processing_system7_0/UART1_TX']:
    print(pat, '->', pat in d)
