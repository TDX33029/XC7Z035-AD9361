from pywinauto import Desktop
import sys
app = Desktop(backend='uia').window(title='USART HMI', process=25856)
name = sys.argv[1]
btn = None
for w in app.descendants(control_type='Button'):
    if w.element_info.name == name:
        btn = w
        break
if btn is None:
    print('NOT FOUND', name); sys.exit(1)
patterns = btn.element_info._element.GetCurrentPatternAll if False else None
import comtypes.client
# list available patterns via LegacyIAccessible? just try invoke
try:
    btn.invoke()
    print('INVOKED', name)
except Exception as e:
    print('INVOKE FAILED:', e)
    try:
        btn.click()
        print('CLICKED(msg)', name)
    except Exception as e2:
        print('CLICK FAILED:', e2)
