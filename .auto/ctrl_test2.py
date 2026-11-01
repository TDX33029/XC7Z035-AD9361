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
        if t == text: w.invoke(); pump(0.3); return True
    print('  button not found:', text); return False

def log_tail(n=5):
    # 动态找 Text 控件（运行记录）
    txts = []
    def rec(w):
        for c in w.winfo_children():
            if c.winfo_class() == 'Text': txts.append(c)
            rec(c)
    rec(app)
    if not txts: return []
    return txts[0].get('1.0', 'end').strip().splitlines()[-n:]

pump(4)
print('1) discovered:', app.p351_network_confirmed, '|', app.board_ip.get())

# AD9361 直发配置事务：模式=不发射 + 89.75dB（无射频输出，纯配置验证）
click('应用并开始发射')
pump(12)
print('2) config_status:', app.config_status.get()[:90])
print('3) 运行记录尾部:')
for line in log_tail(5): print('   ', line[:110])

# 紧急关闭双TX
click('紧急关闭双TX')
pump(2)
print('4) 紧急关闭已发; 状态:', app.p351_status.get()[:60])
for line in log_tail(3): print('   ', line[:110])
app.withdraw(); app.destroy()
print('CTRL2_DONE')
