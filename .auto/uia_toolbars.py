from pywinauto import Desktop
app = Desktop(backend='uia').window(title='USART HMI', process=25856)
def walk(elem, depth, maxdepth):
    try:
        info = elem.element_info
        print('  '*depth + f"{info.control_type}|{info.name!r:.50}|rect={info.rectangle}")
    except Exception as e:
        return
    if depth >= maxdepth: return
    try:
        for c in elem.children():
            walk(c, depth+1, maxdepth)
    except Exception:
        pass
# top-level children windows, find the one at (0,23)-(1920,100) containing menubar+toolbars
for w in app.children():
    r = w.element_info.rectangle
    if r.top == 23 and r.bottom == 100:
        walk(w, 0, 3)
