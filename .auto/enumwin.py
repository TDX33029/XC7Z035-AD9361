import ctypes
from ctypes import wintypes
import sys

u32 = ctypes.windll.user32
u32.SetProcessDPIAware()

WM_GETTEXT = 0x000D
WM_GETTEXTLENGTH = 0x000E

def get_text(h):
    n = u32.SendMessageW(h, WM_GETTEXTLENGTH, 0, 0)
    if n == 0: return ''
    buf = ctypes.create_unicode_buffer(n+2)
    u32.SendMessageW(h, WM_GETTEXT, n+1, buf)
    return buf.value

def get_class(h):
    b = ctypes.create_unicode_buffer(256)
    u32.GetClassNameW(h, b, 256)
    return b.value

EnumWindowsProc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
EnumChildProc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

results = []
def dump_tree(h, depth, out):
    r = wintypes.RECT()
    u32.GetWindowRect(h, ctypes.byref(r))
    cls = get_class(h); txt = get_text(h)
    out.append('%s hwnd=%x class=%-28s rect=(%d,%d,%d,%d) %dx%d text=%r' % (
        '  '*depth, h, cls, r.left, r.top, r.right, r.bottom, r.right-r.left, r.bottom-r.top, txt[:60]))
    kids = []
    def cb(ch, lp):
        kids.append(ch); return True
    u32.EnumChildWindows(h, EnumChildProc(cb), 0)
    for ch in kids:
        dump_tree(ch, depth+1, out)

target = None
def find_top(h, lp):
    if u32.IsWindowVisible(h):
        t = get_text(h)
        if 'USART' in t:
            global target
            target = h
            return False
    return True
u32.EnumWindows(EnumWindowsProc(find_top), 0)
print('TOP hwnd=%x' % target)
out = []
dump_tree(target, 0, out)
print('\n'.join(out))
print('TOTAL %d' % len(out))
