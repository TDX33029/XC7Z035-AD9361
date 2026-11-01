import sys, time
from pathlib import Path
BASE = Path(r'D:/Document/tempPrj/XC7Z035-AD9361/P351/output/ui_source')
sys.path.insert(0, str(BASE / 'migration/p351/ui'))
sys.path.insert(0, str(BASE / 'ui'))
import tkinter as tk
import p351_ui

app = p351_ui.P351ControlUI()
app.geometry('1456x1019+40+40')
app.withdraw()

def pump(sec):
    t0 = time.time()
    while time.time()-t0 < sec:
        app.update(); time.sleep(0.03)

def click(text):
    btns = []
    def rec(w):
        for c in w.winfo_children():
            if c.winfo_class() == 'TButton': btns.append((c, str(c.cget('text'))))
            rec(c)
    rec(app)
    for w, t in btns:
        if t == text:
            w.invoke(); pump(0.3); return True
    print('  button not found:', text); return False

# 1) 等自动发现
pump(4)
print('1) discovered:', app.p351_network_confirmed, '| board:', app.board_ip.get())
print('   p351_status:', app.p351_status.get()[:70])

# 2) 写控制测试A：应用参考时钟（IO PLL→外10M驯服配置，无射频输出）
ok = click('应用时钟')
pump(4)
print('2) 应用时钟 clicked:', ok, '| config_status:', app.config_status.get()[:80])

# 3) 写控制测试B：AD9361 直发配置应用（模式=不发射，安全）——通过射频参数事务
print('3) tx1_mode:', app.tx1_mode.get(), '| 衰减:', app.tx1_attenuation_db.get())

# 4) 写控制测试C：紧急停止（STOP，安全指令）
app.emergency_off()
pump(2)
print('4) 紧急停止 sent; log tail:')
log = app.log_text.get('1.0', 'end').strip().splitlines()
for line in log[-4:]: print('   ', line[:100])
app.withdraw(); app.destroy()
print('CTRL_TEST_DONE')
