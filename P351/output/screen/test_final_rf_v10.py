"""解释执行TJC事件，逐字节核验最终射频请求；不连接硬件。"""
import json
import random
import re
import struct
from pathlib import Path
import screen_design as sd
from demo_full import Demo

BASE = Path(__file__).resolve().parent
checks = 0


def check(condition, message='check failed'):
    global checks
    assert condition, message
    checks += 1


def parse(source):
    source = source.replace('}else if', '}\nelse if').replace('}else', '}\nelse')
    lines = [s.strip() for s in source.splitlines() if s.strip() and not s.lstrip().startswith('//')]
    def block(index):
        nodes = []
        while index < len(lines) and lines[index] != '}':
            if lines[index].startswith(('if(', 'else if(')):
                result, index = branch(index)
                nodes.extend(result)
            else:
                nodes.append(('line', lines[index])); index += 1
        return nodes, index
    def branch(index):
        condition = lines[index][lines[index].index('(')+1:-1]
        check(lines[index+1] == '{')
        yes, index = block(index+2)
        check(lines[index] == '}'); index += 1
        no = []
        if index < len(lines) and lines[index] == 'else':
            check(lines[index+1] == '{')
            no, index = block(index+2)
            check(lines[index] == '}'); index += 1
        elif index < len(lines) and lines[index].startswith('else if('):
            no, index = branch(index)
        return [('if', condition, yes, no)], index
    nodes, end = block(0)
    check(end == len(lines))
    return nodes


PRESS = parse((BASE/'shouye.press.txt').read_text(encoding='utf-8'))
SEND = parse((BASE/'shouye.t3.press.txt').read_text(encoding='utf-8'))
TIMER = parse((BASE/'shouye.tm0.timer.txt').read_text(encoding='utf-8'))
DRAW = parse((BASE/'shouye.p1.press.txt').read_text(encoding='utf-8'))
VALIDATE = parse('\n'.join(sd.native_validation()))


def state(**values):
    result = {}
    for line in (BASE/'Program.s.txt').read_text(encoding='utf-8').splitlines():
        if line.startswith('int '):
            for item in line[4:].split(','):
                name, value = item.split('=')
                result[name.strip()] = int(value, 0)
    result.update(values)
    return result


def expr(source, values):
    source = source.replace('&&', ' and ').replace('||', ' or ')
    source = re.sub(r'(?<!/)/(?!/)', '//', source)
    if not re.fullmatch(r'[A-Za-z0-9_ +*/%<>=!&|().-]+', source):
        raise ValueError(source)
    return int(eval(source, {'__builtins__': {}}, values))


def execute(nodes, values, wire=None, crc=None, rendered=None):
    wire = bytearray() if wire is None else wire
    crc = bytearray() if crc is None else crc
    for node in nodes:
        if node[0] == 'if':
            execute(node[2] if expr(node[1], values) else node[3], values, wire, crc, rendered)
            continue
        line = node[1]
        if line == 'click t3,1':
            execute(SEND, values, wire, crc, rendered)
        elif line in ('click p1,1', 'sendme'):
            pass
        elif line.startswith('crcrest '):
            crc.clear()
        elif line.startswith('crcputh '):
            crc.extend(bytes.fromhex(line[8:]))
        elif line.startswith('crcputs '):
            name, size = line[8:].split(',')
            crc.extend(values[name].to_bytes(4, 'little')[:int(size)])
        elif line.startswith('printh '):
            wire.extend(bytes.fromhex(line[7:]))
        elif line.startswith('prints '):
            name, size = line[7:].split(',')
            value = sd.crc16(crc) if name == 'crcval' else values[name]
            wire.extend(value.to_bytes(4, 'little')[:int(size)])
        elif line.startswith(('cls ', 'fill ')):
            pass
        elif line.startswith('covx '):
            name, dest, length, fmt = line[5:].split(',')
            values[dest] = format(values[name], 'X' if int(fmt) == 2 else 'd').zfill(int(length))
        elif line.startswith('xstr '):
            tokens = line[5:].split(',', 10)
            value = tokens[-1]
            text = json.loads(value) if value.startswith('"') else values[value]
            if rendered is not None:
                rendered.append((tuple(map(int, tokens[:4])), text))
        else:
            name, value = line.split('=', 1)
            if name.endswith('.txt'):
                values[name] = ''.join(json.loads(part) if part.startswith('"') else values[part]
                                       for part in value.split('+'))
            else:
                values[name] = expr(value, values)
    return bytes(wire)


def tap(values, x, y):
    values.update(tch0=x, tch1=y)
    return execute(PRESS, values)


def native_packet(d, sequence=0):
    values = state(**d, op=14, seq=sequence)
    return values, execute(SEND, values)


check(sd.crc16(b'123456789') == 0x4B37)
check(sd.TABS == ['发射', '数据帧', '同步字', '状态'])
check(sd.OPTIONS['dm'] == ['PCM/FM', 'SOQPSK-TG'])
check({'df', 'dpo'} <= sd.LIMITS.keys())
check(not ({'da', 'ds', 'dtf'} & sd.LIMITS.keys()))
check(sd.label_value('df', sd.DEFAULTS) == '5137.0 MHz')
check(sd.label_value('dpo', sd.DEFAULTS) == '-30 dBm')
check(not any(name in ('applyref', 'applytfs', 'applyrx', 'stoprx') for page in sd.PAGES for _, name, *_ in page))
for page in range(4):
    for edge in (0, 119):
        values = state()
        tap(values, page*120+edge, 260)
        check(values['pg'] == page and not values['edit'] and not values['pick'])
for page, items in enumerate(sd.PAGES):
    for kind, name, label, x, y, w, h in items:
        check(0 <= x < x+w <= 480 and 31 <= y < y+h <= 220, name)
        if kind == 'group':
            for bx, by, bw, bh, j, title in sd.selection_boxes(name, x, y, w, h):
                check(bh >= 28)
                values = state(pg=page)
                tap(values, bx+bw//2, by+bh//2)
                check(values[name] == j and values['dirty'] == 1)
for j, (x, y, w, h, _, _) in enumerate(sd.PICK_BOXES):
    values = state(pg=1)
    tap(values, 120, 60)
    check(values['pick'] == 1)
    tap(values, x+w//2, y+h//2)
    check(values['dc'] == j and not values['pick'])
for overlay in ('edit', 'pick'):
    values = state(pending=1, **{overlay: 1})
    check(tap(values, 430, 15) == bytes.fromhex('A5 5A 01 02 FF FF FF'))
    check(not values['pending'] and not values['edit'] and not values['pick'])
    values = state(**{overlay: 1})
    tap(values, 470, 260)
    check(values['pg'] == 3 and not values['edit'] and not values['pick'])
manifest = json.loads((BASE/'control_manifest.json').read_text(encoding='utf-8'))
for name in sd.HEX_FIELDS:
    field_id = manifest['field_ids'][name]
    values = state(edit=field_id, entry=0)
    for digit in 'FE6B':
        x, y, w, h, _ = next(k for k in sd.HEX_KEYS if k[4] == digit)
        tap(values, x+w//2, y+h//2)
    check(values['entry'] == 0xFE6B)
    x, y, w, h, _ = next(k for k in sd.HEX_KEYS if k[4] == '确定')
    tap(values, x+w//2, y+h//2)
    check(values[name] == 0xFE6B and not values['edit'])
    values = state(edit=field_id, entry=65535)
    x, y, w, h, _ = next(k for k in sd.HEX_KEYS if k[4] == '1')
    tap(values, x+w//2, y+h//2)
    check(values['badkey'] == 1 and values['entry'] == 65535)
    x, y, w, h, _ = next(k for k in sd.HEX_KEYS if k[4] == '确定')
    tap(values, x+w//2, y+h//2)
    check(values['edit'] == field_id)

rng = random.Random(20261018)
cases = []
# 每个0.5dB档位经真实按键事件→报文→完整快照回填；键盘按整dB输入。
for units in range(20, 81):
    values = state(dpo=units)
    tap(values, 350, 55)
    check(values['entry'] == units//2 and values['phalf'] == units%2)
    for title in ('清空', *str(units//2)):
        x,y,w,h,_ = next(k for k in sd.keys_for('dpo') if k[-1] == title)
        tap(values, x+w//2, y+h//2)
    if units%2:
        tap(values, 380, 72)
    tap(values, 380, 126)
    check(not values['edit'] and values['dpo'] == units)
    wire = tap(values, 100, 205)
    check(struct.unpack_from('<I', wire, 52)[0] == units*500)
    values.update(rev=1, sysvalid=1, route=2, outkhz=5137000, rfpmag=units*500, dirty=0)
    execute(TIMER, values)
    check(values['dpo'] == units)
    cases.append(dict(sd.DEFAULTS, dpo=units))
values = state(dpo=80)
tap(values, 350, 55);tap(values, 380, 72);tap(values, 380, 126)
check(values['edit'] != 0 and values['dpo'] == 80, '-40.5 must be rejected')
tap(values, 230, 216)
check(values['phalf'] == 0 and values['entry'] == 40, 'backspace removes .5 first')
for bits in range(16, 34):
    for word in (0, 1, (1 << bits)-1, 1 << bits):
        if word >= 1 << 33:
            continue
        cases.append(dict(sd.DEFAULTS, dsb=bits, dsl=word & 65535, dsh=(word >> 16) & 65535, dsx=word >> 32))
for _ in range(400):
    bits, word = rng.randrange(16, 34), rng.randrange(1 << 33)
    cases.append(dict(sd.DEFAULTS, dsb=bits, dsl=word & 65535, dsh=(word >> 16) & 65535, dsx=word >> 32))
for framed in (0, 1):
    for count in (1, 2, 8, 256):
        for length in (1, 2, 3, 4, 5, 6, 64, 512, 2048):
            for bits in (16, 32, 33):
                cases.append(dict(sd.DEFAULTS, dfr=framed, dn=count, db=length, dsb=bits, dsl=0, dsh=0, dsx=0))
for mode in (-1, 0, 1, 2):
    for rate in (99999, 100000, 100001, 4000001, 16777217, 19999999, 20000000, 20000001):
        cases.append(dict(sd.DEFAULTS, dm=mode, dr=rate))
for frequency in (70000, 5090900, 5091000, 5091100, 5137000, 5150000, 5150100,
                  5500000, 5924900, 5925000, 6125000, 6125100, 5137001):
    for power in (0, 19, 20, 21, 40, 60, 79, 80, 81):
        cases.append(dict(sd.DEFAULTS, df=frequency, dpo=power))
for d in cases:
    try:
        sd.validate(d); accepted = True
    except ValueError:
        accepted = False
    values = state(**d)
    execute(VALIDATE, values)
    check((values['bad'] == 0) == accepted, str(d))
    values, wire = native_packet(d)
    check(bool(wire) == accepted)
    if accepted:
        op, args = sd.request('applytx', d)
        check(wire == sd.packet(1, op, args))
        check(len(wire) == 63 and wire[:4] == bytes.fromhex('A5 5A 06 0E'))
        check(struct.unpack_from('<I', wire, 56)[0] == sd.crc16(wire[:56]))
        check(struct.unpack_from('<I', wire, 36)[0] == d['dsl'])
        check(struct.unpack_from('<I', wire, 40)[0] == d['dsh'])
        check(struct.unpack_from('<I', wire, 44)[0] == d['dsb']+d['dsx']*256+(d['dr']%1000)*1024)
        check(struct.unpack_from('<I', wire, 12)[0]*1000+(struct.unpack_from('<I',wire,44)[0]>>10) == d['dr'])
        check(struct.unpack_from('<II', wire, 48) == (d['df'], d['dpo']*500))
for mode in (0, 1):
    for code in range(7):
        values, wire = native_packet(dict(sd.DEFAULTS, dm=mode, dc=code), 65535)
        check(values['seq'] == 1 and struct.unpack_from('<I', wire, 16)[0] == (code if mode == 0 else 0))
for action in ('queryst', 'readtx'):
    op, args = sd.request(action, sd.DEFAULTS)
    values = state(op=op, seq=65534)
    check(execute(SEND, values) == sd.packet(65535, op, args))
check(execute(SEND, state(op=14, pending=1)) == b'')
for rc in (0, 1, 3, 4, 8):
    values = state(rev=1, seenrev=0, dirty=1, pending=1, seq=22, waitop=14, ackseq=22, ackop=14, ackrc=rc)
    execute(TIMER, values)
    check(values['pending'] == (1 if rc == 4 else 0))
    if rc == 8:
        check(values['msg'] == 8)
values = state(rev=2, seenrev=1, dirty=1, pending=1, seq=22, waitop=14, ackseq=21, ackop=14, ackrc=0)
execute(TIMER, values)
check(values['pending'] == 1, 'stale ACK must not complete new request')
check(not sd.path_ready(dict(valid=True, ifvalid=1, ifkhz=72000)))
check(sd.path_ready(dict(valid=True, ifvalid=1, ifkhz=70000)))
check('未校准' in sd.actual_text('power', dict(valid=True, pcal=0)))
check('未锁定' in sd.actual_text('reference', dict(valid=True, refsrc=1, refok=0)))
check('已锁定' in sd.actual_text('reference', dict(valid=True, refsrc=1, refok=1)))


def paint(values):
    # 差分刷新不重复输出已显示文本；观察值须保留上次绘制历史。
    drawn = values.setdefault('_paint_history', [])
    execute(DRAW, values, rendered=drawn)
    return drawn


def header(values):
    return next(text for rect, text in reversed(paint(values)) if rect == (284, 3, 96, 25))


good = dict(valid=True, rev=1, sysvalid=1, route=2, rfvalid=1, rfphase=4, rffault=0,
            outkhz=5137000, rfpmag=30000, rfexpect=30000, rfmodel=1, rfmode=4,
            ifvalid=1, ifkhz=70000, pcal=1, refsrc=1, refok=1)
for key, bad in [('sysvalid', 0), ('route', 1), ('rfvalid', 0), ('rfphase', 2),
                 ('rffault', 4), ('outkhz', 5500000), ('rfpmag', 45000), ('rfpmag', 30001)]:
    s = dict(good, **{key: bad})
    check(sd.rf_status(s) != 'ON', key)
    values = state(**{k:v for k,v in s.items() if k != 'valid'}, dirty=1)
    execute(TIMER, values)
    check(header(values) != 'RF: ON', key)
check(sd.rf_status(good) == 'ON')
check(sd.rf_status(dict(good, valid=False)) == '?')
check(sd.rf_status(dict(good, sysvalid=0, rfmode=0, rfphase=5, rffault=3)) == 'OFF')
check('非实测' in sd.actual_text('power', good))
check('增益模型' in sd.actual_text('power', good))
check('整链标定' in sd.actual_text('power', dict(good, rfmodel=2)))
check('安全停发' in sd.actual_text('source', dict(good, rffault=4)))
check('5137.0' in sd.actual_text('tx', good))
for phase in range(6):
    check(sd.phase_text(dict(rfphase=phase)) != '状态未知')
values = state(**{k:v for k,v in good.items() if k != 'valid'}, dirty=1)
execute(TIMER, values)
check(header(values) == 'RF: ON')
# 部分未提交快照绝不能污染已提交状态/草稿。
values.update(rfphase=5, rffault=3, sysvalid=0, outkhz=6125000, rfpmag=10000)
check(header(values) == 'RF: ON', 'uncommitted incoming fields leaked')
values['rev'] = 2
execute(TIMER, values)
check(header(values) != 'RF: ON')
check(values['crffault'] == 3 and values['coutkhz'] == 6125000)
# 新任务提交、STOP、过期立即撤销旧ON，不等待板端回执。
values = state(**{k:v for k,v in good.items() if k != 'valid'}, dirty=1)
execute(TIMER, values)
tap(values, 100, 200)
check(header(values) == 'RF: ?', 'APPLY kept stale ON')
values = state(**{k:v for k,v in good.items() if k != 'valid'}, dirty=1)
execute(TIMER, values)
tap(values, 430, 15)
check(header(values) == 'RF: ?', 'STOP kept stale ON')
values = state(**{k:v for k,v in good.items() if k != 'valid'}, dirty=1)
execute(TIMER, values)
values['stale'] = 16
check(header(values) == 'RF: ?')
# 只有完整有效最终计划可回填；0.5dB目标精确往返，不静默取整。
for target, frequency, valid, expected in [(20000,6125000,1,(40,6125000)),
                                         (20500,6125000,1,(41,6125000)),
                                         (20001,6125000,1,(60,5137000)),
                                         (20000,5500000,1,(60,5137000)),
                                         (20000,6125000,0,(60,5137000))]:
    values = state(rev=1, sysvalid=valid, route=2, outkhz=frequency, rfpmag=target)
    execute(TIMER, values)
    check((values['dpo'], values['df']) == expected)
# 20s超时不自动重新提交发射；一次查询只读。
values = state(pending=1, age=79, waitop=14, seq=4)
wire = execute(TIMER, values)
check(not values['pending'] and values['msg'] == 5 and not wire)
# “读取板端设置”必须等待本次QUERY的新快照，而不是立即复制旧缓存。
values = state(rev=1, seenrev=1, crev=1, snapshotok=1, dirty=1,
               coutkhz=5137000, crfpmag=30000, csysvalid=1, croute=2)
tap(values, 350, 205)
check(values['readpending'] == 1 and values['pending'] == 1 and values['dpo'] == 60)
values.update(rev=2, ackseq=values['seq'], ackop=3, ackrc=0, sysvalid=1,
              route=2, outkhz=6125000, rfpmag=20000)
execute(TIMER, values)
check(not values['readpending'] and not values['pending'] and values['dpo'] == 40)
check(values['df'] == 6125000 and values['dirty'] == 0)


class Root:
    def __init__(self):
        self.queue = []
    def after(self, ms, function):
        self.queue.append(function)


u = Demo.__new__(Demo)
u.root, u.d, u.s = Root(), sd.DEFAULTS.copy(), {}
u.page, u.entry, u.picker = 0, None, False
u.power_half = 0
u.inputbad, u.seq, u.notice = False, 0, ''
u.pending, u.last_packet, u.fail_next, u.link, u.stopped = False, None, False, True, False
u.render = lambda: None
for page in range(4):
    u.tap(page*120+60, 260)
    check(u.page == page)
u.page = 0
u.action('applytx')
check(u.pending and len(u.last_packet) == 63)
u.root.queue.pop(0)()
check(not u.pending and u.s['ackrc'] == 8 and not u.s['ifvalid'] and not u.s['pcal'])
u.action('queryst')
u.tap(430, 15)
u.root.queue.pop(0)()
check(not u.pending and not u.s['valid'])
u.synthetic_model = True
u.stopped = False
u.action('applytx')
u.root.queue.pop(0)()
check(u.s['ackrc'] == 0 and sd.rf_status(u.s) == 'ON')
check(u.s['rfmodel'] == 1 and '离线合成' in u.notice)
u.inject_failure()
u.action('queryst')
u.root.queue.pop(0)()
check(u.s['rffault'] == 3 and sd.rf_status(u.s) == 'OFF')
for page in range(4):
    check(sd.draw(page).size == (480, 272))
for filename in ('shouye.press.txt', 'shouye.p1.press.txt', 'shouye.t3.press.txt', 'shouye.tm0.timer.txt'):
    source = (BASE/filename).read_text(encoding='utf-8')
    check(source.count('{') == source.count('}'))
    for line in source.splitlines():
        check(not ('if(' in line and '&&' in line and '||' in line), 'TJC condition mixes && and ||')
check('tmp=dr%1000\na9=tmp*1024\ntmp=dsx*256\na9=a9+tmp\na9=a9+dsb' in (BASE/'shouye.t3.press.txt').read_text(encoding='utf-8'))
result = dict(passed=True, checks=checks, parameter_cases=len(cases), hardware_tested=False,
              native_compilation_checked_by_this_test=False,
              tests=['four-page navigation', 'seven PCM codes', 'two modulation modes/no CW',
                     'final RF request distinct from fixed IF/no raw LO writes', 'HEX keypad and overflow',
                     '16..33-bit sync, frame capacity, positive payload', '63B v6/op14 byte-exact CRC',
                     'query51B/STOP priority/pending/timeout/oldACK', 'uncalibrated/unlocked UI honesty',
                     'rev-last coherent shadow snapshot; partial writes cannot contaminate committed state',
                     'fresh RF ON/OFF/unknown and invalidation after APPLY/STOP/stale',
                     'final plan/model/estimate/fault rendering and exact target backfill',
                     'offline demo and PNG dimensions'])
(BASE/'verification').mkdir(exist_ok=True)
(BASE/'verification/offline_tests.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
print('PASS', checks, 'offline checks;', len(cases), 'parameter cases; hardware_tested=False')
