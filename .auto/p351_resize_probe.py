import sys, time
from pathlib import Path
BASE = Path(r'D:/Document/tempPrj/XC7Z035-AD9361/P351/output/ui_source')
sys.path.insert(0, str(BASE / 'migration/p351/ui'))
sys.path.insert(0, str(BASE / 'ui'))
import tkinter as tk
import p351_ui

app = p351_ui.P351ControlUI()
app.geometry('1456x1019+40+40')
app.update_idletasks(); app.update()

# locate the three scroll canvases: children of notebook pages
nb = app.book
pages = nb.tabs()  # rf, bit, monitor, rx, system
def canvases_of(page):
    pf = app.nametowidget(page)
    out = []
    def rec(w):
        for c in w.winfo_children():
            if c.winfo_class() == 'Canvas':
                out.append(c)
            rec(c)
    rec(pf)
    # prefer canvas containing a window item (scroll container)
    out.sort(key=lambda c: 0 if any(c.type(it)=='window' for it in c.find_all()) else 1)
    return out

canvas_map = {}
for i, p in enumerate(pages):
    cs = canvases_of(p)
    if cs:
        canvas_map[i] = cs[0]

def item_width(c):
    for it in c.find_all():
        if c.type(it) == 'window':
            return c.itemcget(it, 'width')
    return '?'

print('initial item widths (before visiting):')
for i, c in canvas_map.items():
    print('  tab%d canvas item width=%s  canvas winfo=%d' % (i, item_width(c), c.winfo_width()))

log = []
names = ['rf','bit','monitor','rx','system']
for step, i in enumerate([1, 4, 1, 4, 0]):  # visit bit, system, revisit bit, revisit system, back to rf
    nb.select(i)
    # sample the item width over the next frames WITHOUT extra update pumping beyond normal
    for f in range(6):
        app.update_idletasks(); app.update()
        row = {}
        for j, c in canvas_map.items():
            row['tab%d' % j] = item_width(c)
        log.append((step, names[i], f, row))
        time.sleep(0.05)
print('\nwidth timeline (tab switches):')
prev = None
for step, vis, f, row in log:
    key = tuple(sorted(row.items()))
    if key != prev:
        print('  step%d visible=%-7s frame%d: %s' % (step, vis, f, row))
        prev = key
app.destroy()
