from pywinauto import Desktop
import sys
app = Desktop(backend='uia').window(title='USART HMI', process=25856) if len(sys.argv)<2 else Desktop(backend='uia').window(best_match=sys.argv[1])
def walk(elem, depth, maxdepth):
    try:
        info = elem.element_info
        print('  '*depth + f"{info.control_type}|{info.name!r:.60}|{info.class_name!r:.40}|{info.rectangle}")
    except Exception as e:
        print('  '*depth + f"<err {e}>")
        return
    if depth >= maxdepth: return
    try:
        for c in elem.children():
            walk(c, depth+1, maxdepth)
    except Exception:
        pass
walk(app, 0, 2)
