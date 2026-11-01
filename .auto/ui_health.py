import sys, time
from pathlib import Path
BASE = Path(r'D:/Document/tempPrj/XC7Z035-AD9361/P351/output/ui_source')
sys.path.insert(0, str(BASE / 'migration/p351/ui'))
sys.path.insert(0, str(BASE / 'ui'))
import p351_ui
app = p351_ui.P351ControlUI()
app.geometry('1456x1019+40+40')
for _ in range(30):
    app.update(); time.sleep(0.03)
print('sync_hint:', app.sync_hint.get())
print('follow:', app.system_panel.follow_screen.get())
print('sync_label:', app.system_panel.sync_label.cget('text'))
print('config_status:', app.config_status.get())
# 让自动发现跑两轮（无板卡，静默失败）
t0 = time.time()
while time.time()-t0 < 9:
    app.update(); time.sleep(0.05)
print('after auto-discover window: hint=', app.sync_hint.get(), '| confirmed=', app.p351_network_confirmed)
print('log lines:', app.log_text.count('1.0', 'end').split('.')[0])
app.withdraw(); app.destroy()
print('HEALTH_OK')
