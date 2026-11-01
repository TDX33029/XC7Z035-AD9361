import sys, time
from pathlib import Path
BASE = Path(r'D:/Document/tempPrj/XC7Z035-AD9361/P351/output/ui_source')
sys.path.insert(0, str(BASE / 'migration/p351/ui'))
sys.path.insert(0, str(BASE / 'ui'))
import p351_ui
app = p351_ui.P351ControlUI()
app.geometry('1456x1019+40+40')
app.withdraw()
t0 = time.time(); confirmed = None; state_seen = None
while time.time()-t0 < 20:
    app.update(); time.sleep(0.05)
    if confirmed is None and app.p351_network_confirmed:
        confirmed = (round(time.time()-t0,1), app.board_ip.get(), app.local_ip.get())
    if app.final_rf_state is not None and state_seen is None:
        state_seen = round(time.time()-t0,1)
    if confirmed and state_seen and time.time()-t0 > 6:
        break
print('auto-confirm:', confirmed, '| state_read at:', state_seen)
print('board_ip:', app.board_ip.get(), '| sync_hint:', app.sync_hint.get())
print('整机草稿(同步自板端):', {k: app.system_panel.variables[k].get() for k in ('frequency','power','rate','mode','framed','bytes','count','sync','bits','repeat')})
print('final_rf_text:', app.final_rf_text.get().replace(chr(10),' ⏎ ')[:150])
print('config_status:', app.config_status.get()[:80])
app.withdraw(); app.destroy()
print('LIVE_OK')
