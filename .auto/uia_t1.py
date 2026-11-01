from pywinauto import Desktop
import time, sys
h = int('d0b76', 16)
dlg = Desktop(backend='uia').window(handle=h)
def find(elem, name):
    if (elem.element_info.name or '') == name:
        return elem
    for c in elem.children():
        r = find(c, name)
        if r: return r
    return None
p1 = find(dlg, 'itemPanel1')
tabs = [c for c in p1.children()]
t1 = tabs[1]
t1.invoke()
time.sleep(1)
p4 = find(dlg, 'itemPanel4')
def walk(elem, depth, maxdepth):
    try:
        info = elem.element_info
        print('  '*depth + f"{info.control_type}|rect={info.rectangle}")
        nm = info.name or ''
        if nm and depth >= 2:
            print('  '*depth + 'NAME:' + nm.replace('\n', ' ')[:400])
    except Exception:
        return
    if depth >= maxdepth: return
    try:
        for c in elem.children():
            walk(c, depth+1, maxdepth)
    except Exception:
        pass
walk(p4, 0, 2)
