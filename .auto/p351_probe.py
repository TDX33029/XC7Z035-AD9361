"""P351 UI 插桩运行器：逐标签页导出控件几何/截图，并做无硬件功能探针。"""
import json
import sys
import time
from pathlib import Path

BASE = Path(r'D:/Document/tempPrj/XC7Z035-AD9361/P351/output/ui_source')
sys.path.insert(0, str(BASE / 'migration/p351/ui'))
sys.path.insert(0, str(BASE / 'ui'))

import tkinter as tk  # noqa: E402
from PIL import ImageGrab  # noqa: E402

import p351_ui  # noqa: E402

OUT = Path(r'D:/Document/tempPrj/XC7Z035-AD9361/.auto/unpack/ui_inspect')
OUT.mkdir(parents=True, exist_ok=True)


def widget_text(w):
    try:
        cls = w.winfo_class()
        if cls in ('TLabel', 'TButton', 'TCheckbutton', 'TRadiobutton', 'TNotebook.Tab'):
            return str(w.cget('text'))
        if cls == 'TEntry':
            try:
                return str(w.get())
            except Exception:
                return ''
        if cls == 'TCombobox':
            return str(w.get())
        if cls == 'TSpinbox':
            return str(w.get())
        if cls == 'TScale':
            return str(w.get())
    except Exception:
        pass
    return ''


def walk(w, depth=0, path=''):
    name = str(w)
    p = path + '/' + (name.rsplit('!', 1)[-1].split('.', 1)[-1] if '!' in name else name)
    try:
        item = {
            'path': p, 'cls': w.winfo_class(), 'depth': depth,
            'x': w.winfo_x(), 'y': w.winfo_y(), 'w': w.winfo_width(), 'h': w.winfo_height(),
            'reqw': w.winfo_reqwidth(), 'reqh': w.winfo_reqheight(),
            'text': widget_text(w),
            'state': str(w.cget('state')) if 'state' in w.keys() else '',
        }
    except Exception:
        return []
    out = [item]
    for c in w.winfo_children():
        out.extend(walk(c, depth + 1, p))
    return out


def dump_tab(app, index, name):
    nb = app.book
    nb.select(index)
    for _ in range(6):
        app.update_idletasks()
        app.update()
        time.sleep(0.05)
    rows = []
    for w in [app] + list(app.winfo_children()):
        rows.extend(walk(w))
    json.dump(rows, open(OUT / f'widgets_{index}_{name}.json', 'w', encoding='utf-8'),
              ensure_ascii=False, indent=1)
    x, y = app.winfo_rootx(), app.winfo_rooty()
    ww, wh = app.winfo_width(), app.winfo_height()
    img = ImageGrab.grab((x, y, x + ww, y + wh))
    img.save(OUT / f'tab_{index}_{name}.png')
    print(f'tab{index} {name}: {len(rows)} widgets, img {ww}x{wh}')


def main():
    app = p351_ui.P351ControlUI()
    app.geometry('1456x1019+40+40')
    app.update()
    tabs = [t.replace(' .!', '').strip() for t in app.book.tabs()]
    print('tabs:', tabs)
    names = ['rf', 'bit', 'monitor', 'rx', 'system']
    for i, nm in enumerate(names):
        dump_tab(app, i, nm)
    # 无硬件功能探针：只读地址测试（3秒超时）
    log0 = len(app.log_text.get('1.0', 'end')) if hasattr(app, 'log_text') else 0
    t0 = time.time()
    try:
        app.test_connection()
        for _ in range(60):
            app.update()
            time.sleep(0.1)
            if not getattr(app, 'network_busy', False):
                break
        print(f'test_connection done in {time.time()-t0:.1f}s busy={getattr(app, "network_busy", None)}')
        print('config_status:', app.config_status.get())
        print('connection_state:', app.connection_state.get())
        print('p351_status:', app.p351_status.get())
    except Exception as exc:
        print('probe error:', exc)
    json.dump({'log': app.log_text.get('1.0', 'end') if hasattr(app, 'log_text') else ''},
              open(OUT / 'after_probe.json', 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
    # 打开一个非法参数弹窗探针：设置非法衰减后触发_apply（如果存在）
    app.destroy()


if __name__ == '__main__':
    main()
