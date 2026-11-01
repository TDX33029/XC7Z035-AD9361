from pywinauto import Desktop
import sys
h = int(sys.argv[1], 16)
dlg = Desktop(backend='uia').window(handle=h)
def walk(elem, depth, maxdepth):
    try:
        info = elem.element_info
        print('  '*depth + f"{info.control_type}|{info.name!r:.45}|rect={info.rectangle}")
    except Exception:
        return
    if depth >= maxdepth: return
    try:
        for c in elem.children():
            walk(c, depth+1, maxdepth)
    except Exception:
        pass
walk(dlg, 0, 4)
