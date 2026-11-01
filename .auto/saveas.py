from pywinauto import Desktop
import sys
path = sys.argv[1]
dlg = Desktop(backend='uia').window(title='另存为', visible_only=True)
edits = dlg.descendants(control_type='Edit')
fn = None
for e in edits:
    try:
        nm = e.element_info.name or ''
    except Exception:
        nm = ''
    if '文件名' in nm or nm == '':
        # the filename combo edit; pick the one whose value pattern exists and is near bottom
        fn = e
        break
if fn is None:
    print('NO EDIT FOUND'); print([e.element_info.name for e in edits]); sys.exit(1)
fn.set_edit_text(path)
print('SET', path)
dlg.child_window(title='保存(&S)', control_type='Button').invoke() if False else None
# click Save
for b in dlg.descendants(control_type='Button'):
    n = b.element_info.name or ''
    if '保存' in n:
        b.invoke(); print('CLICKED SAVE'); break
