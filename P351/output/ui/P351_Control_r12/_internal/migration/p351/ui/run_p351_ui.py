"""Reuse the existing UI with P351 reference semantics, no automatic RF start."""
from pathlib import Path
import sys

HERE = (Path(sys._MEIPASS)/'migration/p351/ui' if getattr(sys,'frozen',False)
        else Path(__file__).resolve().parent)
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT/'ui'))
sys.path.insert(0, str(HERE))
from p351_ui import main
if __name__=='__main__':
    raise SystemExit(main())
