from pywinauto import Desktop
import sys
h = int(sys.argv[1], 16)
panel_name = sys.argv[2]
dlg = Desktop(backend='uia').window(handle=h)
def find(elem, name):
    if (elem.element_info.name or '') == name:
        return elem
    for c in elem.children():
        r = find(c, name)
        if r: return r
    return None
panel = find(dlg, panel_name)
def walk(elem, depth, maxdepth):
    try:
        info = elem.element_info
        print('  '*depth + f"{info.control_type}|{info.name!r:.50}|rect={info.rectangle}")
    except Exception:
        return
    if depth >= maxdepth: return
    try:
        for c in elem.children():
            walk(c, depth+1, maxdepth)
    except Exception:
        pass
walk(panel, 0, 3)
