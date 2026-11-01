from pywinauto import Desktop
import sys
app = Desktop(backend='uia').window(title_re='USART HMI.*')
target = sys.argv[1]
for w in app.descendants():
    nm = w.element_info.name or ''
    if nm == target:
        print('found', w.element_info.control_type, nm)
        try:
            w.select()
            print('selected')
        except Exception as e1:
            print('select failed:', e1)
            try:
                w.invoke(); print('invoked')
            except Exception as e2:
                print('invoke failed:', e2)
        break
