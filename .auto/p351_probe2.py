import sys, time, json
from pathlib import Path
BASE = Path(r'D:/Document/tempPrj/XC7Z035-AD9361/P351/output/ui_source')
sys.path.insert(0, str(BASE / 'migration/p351/ui'))
sys.path.insert(0, str(BASE / 'ui'))
import tkinter as tk
import tkinter.messagebox as mb
import p351_ui, ad9361tx_ui as shared

dialogs = []
def fake_dialog(kind):
    def f(title=None, message=None, **kw):
        dialogs.append((kind, str(title), str(message)[:120]))
        return True if kind=='askyesno' else 'ok'
    return f
shared.messagebox.askyesno = fake_dialog('askyesno')
shared.messagebox.showerror = fake_dialog('showerror')
shared.messagebox.showwarning = fake_dialog('showwarning')

app = p351_ui.P351ControlUI()
app.geometry('1456x1019+40+40'); app.update()

report = {}
# 1) 未连接时点击 射频与时钟->应用时钟
try:
    for w in app.winfo_children(): pass
    app.apply_clock_command() if hasattr(app,'apply_clock_command') else None
except Exception as e:
    report['apply_clock_direct'] = repr(e)
# 找应用时钟按钮并 invoke
def find_buttons(w, acc):
    for c in w.winfo_children():
        if isinstance(c, tk.ttk.Widget if False else object) and c.winfo_class()=='TButton':
            acc.append((c, str(c.cget('text'))))
        find_buttons(c, acc)
btns = []
find_buttons(app, btns)
report['button_count'] = len(btns)
def click(text):
    for w,t in btns:
        if t == text:
            w.invoke(); app.update(); return True
    return False
click('应用时钟')
app.update()
report['after_apply_clock_dialogs'] = list(dialogs); dialogs.clear()
# 2) 整机应用并发射（system panel）
click('整机应用并发射')
app.update(); time.sleep(0.3); app.update()
report['after_system_apply_dialogs'] = list(dialogs); dialogs.clear()
report['system_status'] = getattr(app, 'final_rf_text', None) and app.final_rf_text.get()
# 3) 紧急停止所有发射
try:
    app.emergency_off(); app.update()
    report['emergency_off'] = 'ok, no exception'
except Exception as e:
    report['emergency_off'] = repr(e)
report['dialogs_total'] = list(dialogs)
# 4) 模式切换
app.tx1_mode.set('SOQPSK-TG（PL）'); app.update()
report['power_summary_after_mode'] = app.tx1_power_summary.get()
# 5) 非法衰减
app.tx1_attenuation_db.set('100'); app.update()
report['power_summary_bad_att'] = app.tx1_power_summary.get()
# 6) RX页应用（未确认地址）
app.book.select(3); app.update()
click('应用并开始RX2解调')
app.update()
report['rx_apply_dialogs'] = list(dialogs)
print(json.dumps(report, ensure_ascii=False, indent=1))
app.destroy()
