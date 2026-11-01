import sys, time
from pathlib import Path
BASE = Path(r'D:/Document/tempPrj/XC7Z035-AD9361/P351/output/ui_source')
sys.path.insert(0, str(BASE / 'migration/p351/ui'))
sys.path.insert(0, str(BASE / 'ui'))
import p351_ui
app = p351_ui.P351ControlUI()
app.geometry('1456x1019+40+40')
app.withdraw()
t0 = time.time()
mirror_seen = False
while time.time()-t0 < 14:
    app.update(); time.sleep(0.05)
    if app.screen_mirror is not None and not mirror_seen:
        mirror_seen = True
        print('MIRROR at %.1fs: edit=%d vars[0:6]=' % (time.time()-t0, app.screen_mirror['edit']), app.screen_mirror['vars'][:6])
print('hint:', app.sync_hint.get())
print('整机草稿:', {k: app.system_panel.variables[k].get() for k in ('frequency','power','rate','mode','framed','bytes','repeat')})
app.withdraw(); app.destroy()
print('MIRROR_TEST_DONE mirror_seen=', mirror_seen)
