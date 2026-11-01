import sys, ctypes
from comtypes import CoInitialize
try:
    import comtypes
except ImportError:
    print('NO_COMTYPES'); sys.exit(2)
