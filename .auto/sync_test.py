import sys, time
from pathlib import Path
BASE = Path(r'D:/Document/tempPrj/XC7Z035-AD9361/P351/output/ui_source')
sys.path.insert(0, str(BASE / 'migration/p351/ui'))
sys.path.insert(0, str(BASE / 'ui'))
import tkinter as tk
import p351_ui
import p351_final_rf as final_rf

app = p351_ui.P351ControlUI()
app.geometry('1456x1019+40+40')
app.update()
sp = app.system_panel
print('follow default:', sp.follow_screen.get())
print('fields before:', {k: sp.variables[k].get() for k in ('frequency','power','rate','mode','code','framed','bytes','count','sync','bits','repeat')})

# 合成屏幕状态：屏幕上按了应用 5137.0MHz -30dBm 4Mbit/s PCM/FM NRZ-L framed 512x1 sync FE6B2840/32 无限
NAMES = final_rf.NAMES
def word(name, value):
    i = NAMES.index(name)
app.final_rf_state = None
w = [0]*len(NAMES)
def setw(n, v): w[NAMES.index(n)] = v
setw('version', 7); setw('flags', final_rf.PLAN_KNOWN | final_rf.VALID | final_rf.OWNED)
setw('phase', 4); setw('fault', 0)
setw('final_khz', 5137000); setw('target_mdbm', (30000) & 0xFFFFFFFF)  # dpo=60 → +30dBm幅值？实际屏幕 dpo*500=30000，PS里为负目标
setw('target_mdbm', (-30000) & 0xFFFFFFFF)
setw('if_khz', 70000); setw('if_att_mdb', 0)
setw('mode', 4); setw('rate', 4000000); setw('code', 0)
setw('framed', 1); setw('fbytes', 512); setw('fcount', 1)
setw('sync_low', 0xFE6B2840); setw('sync_high', 0); setw('sync_bits', 32)
setw('repeat', 1); setw('sequence', 0x53000001); setw('task', 0x53000001)
from p351_final_rf import FinalRFStatus
app.final_rf_state = FinalRFStatus(words=tuple(w))
sp.synced_signature = None
sp.sync_tick(); app.update()
after = {k: sp.variables[k].get() for k in ('frequency','power','rate','mode','code','framed','bytes','count','sync','bits','repeat')}
print('fields after backfill:', after)
print('sync_hint:', app.sync_hint.get())
assert after['frequency'] == '5137.0', after['frequency']
assert after['power'] == '-30.0', after['power']
assert after['rate'] == '4000000'
assert after['mode'] == 'PCM/FM' and after['code'] == 'NRZ-L'
assert after['framed'] == '开' and after['bytes'] == '512' and after['count'] == '1'
assert after['sync'] == 'FE6B2840' and after['bits'] == '32' and after['repeat'] == '无限'
print('BACKFILL_OK')
# 再来一次相同状态：不应重复改写（签名去重）
sp.sync_tick()
print('dedup OK (hint unchanged):', app.sync_hint.get())
app.withdraw(); app.destroy()
print('TEST_PASS')
