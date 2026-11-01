import sys, time
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
app.update_idletasks(); app.update()

def scroll_canvases(page_widget):
    out = []
    def rec(w):
        for c in w.winfo_children():
            if c.winfo_class() == 'Canvas' and any(c.type(it)=='window' for it in c.find_all()):
                out.append(c)
            rec(c)
    rec(page_widget)
    return out

app.attributes('-topmost', True); app.lift(); app.update()
# RF tab: scroll to bottom
rfc = scroll_canvases(app.nametowidget(app.book.tabs()[0]))[0]
rfc.yview_moveto(1.0); app.update(); time.sleep(0.3); app.update()
x,y = app.winfo_rootx(), app.winfo_rooty(); ww,wh = app.winfo_width(), app.winfo_height()
ImageGrab.grab((x,y,x+ww,y+wh)).save(OUT/'shot_0_rf_bottom.png')
# BIT tab: scroll to bottom
app.book.select(1); app.update()
bc = scroll_canvases(app.nametowidget(app.book.tabs()[1]))[0]
bc.yview_moveto(1.0); app.update(); time.sleep(0.3); app.update()
ImageGrab.grab((x,y,x+ww,y+wh)).save(OUT/'shot_1_bit_bottom.png')
app.attributes('-topmost', False)
app.withdraw(); app.destroy()
print('saved')
