"""Load a named shared UI module without ambiguous sys.path shadowing."""
from pathlib import Path
import importlib.util
import sys

ROOT=Path(sys._MEIPASS) if getattr(sys,'frozen',False) else Path(__file__).resolve().parents[3]

def load_shared(name):
    if name not in ('board_config','tfs_control'):
        raise ValueError('Unsupported shared module')
    alias='_p351_shared_'+name
    if alias not in sys.modules:
        spec=importlib.util.spec_from_file_location(alias,ROOT/'ui'/(name+'.py'))
        module=importlib.util.module_from_spec(spec)
        sys.modules[alias]=module
        spec.loader.exec_module(module)
    return sys.modules[alias]
