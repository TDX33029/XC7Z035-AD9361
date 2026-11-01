import sys, time, socket
from pathlib import Path
BASE = Path(r'D:/Document/tempPrj/XC7Z035-AD9361/P351/output/ui_source')
sys.path.insert(0, str(BASE / 'migration/p351/ui'))
sys.path.insert(0, str(BASE / 'ui'))
import p351_final_rf as final_rf, p351_discovery

r = p351_discovery.choose_single('', '', timeout=3.0)
print('board:', r.board_ip, '| hw_qualified(CFGQ):', r.config.hardware_qualified)
st = final_rf.read_pc_status(r.board_ip, r.local_ip)
w = st.get
names = final_rf.NAMES
print('--- final RF status ---')
for n in ('version','flags','phase','fault','tfs_status','ref_source','board_flags','source','task','sequence','mode','rate'):
    print(' %-14s = %d' % (n, w(n)))
FLAGS = dict(VALID=1, OWNED=2, QUALIFIED=4, COMMITTED=8, REFERENCE_OK=16, TFS_LOCK=32, PLAN_KNOWN=64, MANUAL_TEST=128, DIRECT_ACTIVE=256)
fl = w('flags')
print('flags decoded:', [k for k,v in FLAGS.items() if fl & v])
bf = w('board_flags')
print('board_flags bit0(HW_QUAL)=', bf & 1, ' bit?RF_VALID=', bf & 2)
print('phase:', final_rf.PHASES[w('phase')] if w('phase') < len(final_rf.PHASES) else w('phase'))
print('fault:', final_rf.FAULTS[w('fault')] if w('fault') < len(final_rf.FAULTS) else w('fault'))
print('pc_text:', st.pc_text().replace(chr(10),' | '))
