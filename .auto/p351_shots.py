import sys, time, json
from pathlib import Path
BASE = Path(r'D:/Document/tempPrj/XC7Z035-AD9361/P351/output/ui_source')
sys.path.insert(0, str(BASE / 'migration/p351/ui'))
sys.path.insert(0, str(BASE / 'ui'))
import tkinter as tk
from PIL import ImageGrab
import p351_ui

OUT = Path(r'D:/Document/tempPrj/XC7Z035-AD9361/.auto/unpack/ui_inspect')

app = p351_ui.P351ControlUI()
app.geometry('1456x1019+40+40')
app.update()
names = ['rf','bit','monitor','rx','system']
info = {}
for i, nm in enumerate(names):
    app.book.select(i)
    app.attributes('-topmost', True)
    app.lift()
    for _ in range(8):
        app.update_idletasks(); app.update(); time.sleep(0.04)
    # inner scroll height
    h = ''
    for w in app.winfo_children():
        pass
    x, y = app.winfo_rootx(), app.winfo_rooty()
    ww, wh = app.winfo_width(), app.winfo_height()
    ImageGrab.grab((x, y, x+ww, y+wh)).save(OUT / f'shot_{i}_{nm}.png')
    app.attributes('-topmost', False)
    info[nm] = True
app.withdraw()
json.dump(info, open(OUT/'shots.json','w'))
app.destroy()
print('shots saved')
