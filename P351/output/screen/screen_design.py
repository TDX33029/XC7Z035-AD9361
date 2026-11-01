"""P351 最终射频屏幕 v10；1bit/s码率、0.5dB功率、四页、差分防闪。

发射参数通过 v6/op14 一次提交。发射频率/功率是后级上变频后的设置目标。
9361内部仍固定70MHz/-30dBm；根据板端完整快照显示执行阶段和安全停发。
生成事件不等于原生 HMI/TFT 编译，更不代表硬件功率已经标定。
"""
from pathlib import Path
import json
import struct
from PIL import Image, ImageDraw, ImageFont

BASE = Path(__file__).resolve().parent
CODES = ['NRZ-L', 'NRZ-M', 'NRZ-S', 'Biphase-L', 'Biphase-M', 'Biphase-S', 'RNRZ-L']
TABS = ['发射', '数据帧', '同步字', '状态']
NAV_WIDTH = 120
OPTIONS = {'dm': ['PCM/FM', 'SOQPSK-TG'], 'dc': CODES,
           'dfr': ['关', '开'], 'di': ['单次', '无限循环'], 'dsx': ['0', '1']}
DEFAULTS = dict(dm=0, dr=4000000, dc=0, dfr=1, db=512, dn=1, di=1,
                dsl=0x2840, dsh=0xFE6B, dsx=0, dsb=32, df=5137000, dpo=60)
LIMITS = dict(dr=(100000, 20000000), db=(1, 2048), dn=(1, 256),
              dsl=(0, 65535), dsh=(0, 65535), dsb=(16, 33),
              df=(5091000, 6125000), dpo=(20, 80))
HEX_FIELDS = {'dsl', 'dsh'}
ACTUAL = ['ackseq', 'ackop', 'ackrc', 'rfmode', 'flo', 'rate', 'att', 'code', 'framed',
          'fbytes', 'fcount', 'runflag', 'refsrc', 'refok', 'clkstate', 'tfsstat', 'tfkhz',
          'extatt', 'tempmc', 'tempok', 'rxon', 'rxf', 'rxrate', 'rxbw', 'rxgain', 'rxmode',
          'rxcode', 'rxstat', 'rxbits', 'rxerrs', 'rxframes', 'countsat', 'qualified',
          'rfvalid', 'source', 'outkhz', 'route', 'sysvalid', 'adfix', 'syncbits',
          'synclo', 'synchi', 'syncbit', 'totalatt', 'rev', 'pcal', 'pcmpcal', 'soqpcal',
          'ifvalid', 'ifkhz', 'iftarget', 'ifatten',
          'rfpmag', 'rfexpect', 'rfmodel', 'rffault', 'rfphase']
PAGES = [
    [('field', 'df', '发射频率：', 12, 38, 260, 44),
     ('field', 'dpo', '发射功率：', 280, 38, 188, 44),
     ('group', 'dm', '调制方式', 12, 85, 456, 52),
     ('field', 'dr', '码率 / bit/s', 12, 147, 220, 38),
     ('text', 'pn', '数据源  PN11', 254, 156, 214, 24),
     ('button', 'applytx', '应用并发射', 12, 192, 286, 28),
     ('button', 'readtx', '读取板端设置', 306, 192, 162, 28)],
    [('choice', 'dc', 'PCM 码型', 12, 40, 456, 42),
     ('group', 'dfr', '帧同步', 12, 88, 220, 50),
     ('group', 'di', '重复方式', 240, 88, 228, 50),
     ('field', 'db', '小帧 / byte', 12, 146, 220, 38),
     ('field', 'dn', '复帧小帧数', 240, 146, 228, 38),
     ('text', 'pn', 'PN11 / 7FF / 2047 bit', 12, 196, 286, 20),
     ('button', 'navsync', '同步字设置', 306, 192, 162, 28)],
    [('field', 'dsl', '同步字低16位 / HEX', 12, 40, 220, 46),
     ('field', 'dsh', '同步字高16位 / HEX', 240, 40, 228, 46),
     ('field', 'dsb', '同步字长度 / bit', 12, 100, 220, 42),
     ('group', 'dsx', '第33位', 240, 94, 228, 50),
     ('summary', 'sync', '', 12, 153, 456, 22),
     ('button', 'checkframe', '检查同步字与帧容量', 12, 192, 456, 28)],
    [('actual', 'tx', '', 12, 40, 456, 27),
     ('actual', 'power', '', 12, 76, 456, 27),
     ('actual', 'reference', '', 12, 112, 456, 27),
     ('actual', 'source', '', 12, 148, 456, 27),
     ('actual', 'temp', '', 12, 190, 286, 27),
     ('button', 'queryst', '刷新状态', 306, 190, 162, 30)]
]
NAVY, BG, INK, MUTED, BLUE, RED = '#142D43', '#F1F4F8', '#20374A', '#667D90', '#176BC0', '#BC3A38'
PICK_BOXES = [(12+(j % 2)*232, 66+(j//2)*36, 224, 32, j, title) for j, title in enumerate(CODES)]
PICK_CANCEL = (244, 174, 224, 32)
KEYS = [(12+90*(i % 3), 96+35*(i//3), 84, 32, str(i+1)) for i in range(9)]+[
    (12, 201, 84, 32, '清空'), (102, 201, 84, 32, '0'), (192, 201, 84, 32, '退格'),
    (302, 96, 164, 60, '确定'), (302, 168, 164, 60, '取消')]
HEX_KEYS = [(12+(i % 4)*66, 94+(i//4)*34, 60, 30, title) for i, title in enumerate('0123456789ABCDEF')]+[
    (290, 94, 178, 36, '确定'), (290, 136, 178, 36, '退格'),
    (290, 178, 178, 24, '清空'), (290, 210, 178, 24, '取消')]


def font(n=13):
    return ImageFont.truetype('C:/Windows/Fonts/msyh.ttc', n)


def rgb565(color):
    r, g, b = tuple(bytes.fromhex(color[1:]))
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def keys_for(name):
    return (HEX_KEYS if name in HEX_FIELDS else
            KEYS+[(300, 60, 168, 28, '切换 .0/.5')] if name == 'dpo' else KEYS)


def crc16(data):
    crc = 0xffff
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xa001 if crc & 1 else 0)
    return crc


def sync_word(d):
    return d['dsl'] | (d['dsh'] << 16) | (d['dsx'] << 32)


def sync_text(d):
    return f"SYNC 0x{sync_word(d):0{(d['dsb']+3)//4}X} / {d['dsb']}bit"


def valid_frequency(value):
    return value % 100 == 0 and (5091000 <= value <= 5150000 or 5925000 <= value <= 6125000)


def validate(d):
    for key, (lo, hi) in LIMITS.items():
        if not isinstance(d[key], int) or not lo <= d[key] <= hi:
            raise ValueError(f'{key}超出范围 {lo}..{hi}')
    for key, count in [('dm', 2), ('dc', 7), ('dfr', 2), ('di', 2), ('dsx', 2)]:
        if not isinstance(d[key], int) or not 0 <= d[key] < count:
            raise ValueError(f'{key}选项无效')
    if not valid_frequency(d['df']):
        raise ValueError('最终频率须在两个工作段内且为100kHz步进')
    if sync_word(d) >= (1 << d['dsb']):
        raise ValueError('同步字超出所选位宽')
    if d['db'] * d['dn'] > 16384:
        raise ValueError('复帧缓存超过16KB')
    if d['dfr']:
        if d['db'] * 8 <= d['dsb'] + (8 if d['dn'] > 1 else 0):
            raise ValueError('帧内无有效载荷')
    return True


def label_value(name, d):
    if name == 'df':
        return f"{d[name]//1000}.{d[name] % 1000//100} MHz"
    if name == 'dpo':
        return f"-{d[name]/2:g} dBm"
    if name in OPTIONS:
        return OPTIONS[name][d[name]]
    if name in HEX_FIELDS:
        return f'{d[name]:04X}'
    return str(d.get(name, 0))


def selection_boxes(name, x, y, w, h):
    unit = w // len(OPTIONS[name])
    return [(x+j*unit, y+18, (unit if j < len(OPTIONS[name])-1 else w-j*unit)-4,
             h-18, j, title) for j, title in enumerate(OPTIONS[name])]


def path_ready(s):
    """只表示板端固定中频配置有效，不把配置回读当成 RF 实测。"""
    return bool(s.get('valid') and s.get('ifvalid') == 1 and s.get('ifkhz') == 70000)


def final_plan_valid(s):
    return bool(s.get('valid') and s.get('sysvalid') == 1 and s.get('route') == 2
                and valid_frequency(s.get('outkhz', 0))
                and 10000 <= s.get('rfpmag', 0) <= 40000 and s.get('rfpmag', 0) % 500 == 0)


def rf_status(s):
    """ON是板端有效执行状态，并非射频功率传感器的测量结果。"""
    if not s.get('valid') or s.get('rfvalid') != 1:
        return '?'
    if final_plan_valid(s) and s.get('rfphase') == 4 and not s.get('rffault'):
        return 'ON'
    if s.get('rfmode') == 0 and s.get('rfphase') in (0, 5):
        return 'OFF'
    return '?'


def phase_text(s):
    if s.get('rffault'):
        return f"安全停发 / 故障 {s['rffault']}"
    return {0: '空闲', 1: '等待外部参考', 2: '等待二级本振', 3: '启动70MHz中频',
            4: '任务执行中', 5: '已停止'}.get(s.get('rfphase'), '状态未知')


def actual_text(name, s):
    if not s.get('valid'):
        return '等待板端状态'
    if name == 'tx':
        if not final_plan_valid(s):
            return '最终射频设置尚未提交 · ' + phase_text(s)
        return f"已应用 {s['outkhz']/1000:.1f} MHz / -{s['rfpmag']/1000:g} dBm"
    if name == 'power':
        model = s.get('rfmodel', 0)
        if not final_plan_valid(s) or model not in (1, 2):
            return '输出功率：未校准 / 无有效估算'
        label = '增益模型估算' if model == 1 else '整链标定估算'
        if 0 < s.get('rfexpect', 0) <= 100000:
            return f"{label} -{s['rfexpect']/1000:g} dBm · 非实测"
        return f'{label} · 无有效功率值'
    if name == 'reference':
        return ('外部10 MHz · 已锁定' if s.get('refsrc') == 1 and s.get('refok') == 1
                else '外部10 MHz · 未锁定')
    if name == 'source':
        return phase_text(s) + (' · 中频已标定' if s.get('pcal') else ' · 中频未标定')
    if name == 'temp':
        return f"AD9361  {s.get('tempmc', 0)/1000:.1f} °C" if s.get('tempok') else 'AD9361  -- °C'
    return ''


def draw(page=0, d=None, s=None, notice='等待板端状态', keypad=None, picker=False):
    d = DEFAULTS if d is None else d
    s = {} if s is None else s
    im = Image.new('RGB', (480, 272), BG)
    g = ImageDraw.Draw(im)
    def text(x, y, value, size=13, color=INK):
        g.text((x, y), value, font=font(size), fill=color)
    g.rectangle((0, 0, 479, 30), fill=NAVY)
    text(12, 5, '校验仿真器 P351 · TX2', 15, 'white')
    rf = 'RF: ' + rf_status(s)
    text(284, 7, rf, 12, 'white')
    g.rectangle((384, 3, 470, 27), fill=RED)
    text(399, 5, '停止发射', 13, 'white')
    for kind, name, label, x, y, w, h in PAGES[page]:
        if kind in ('text', 'actual', 'summary'):
            value = actual_text(name, s) if kind == 'actual' else sync_text(d) if kind == 'summary' else label
            text(x, y, value, 22 if name in ('if', 'power') and kind == 'text' else 12,
                 INK if kind == 'text' and name in ('if', 'power') else MUTED)
        elif kind == 'button':
            g.rounded_rectangle((x, y, x+w-1, y+h-1), 4, fill=BLUE)
            width = g.textlength(label, font=font(14))
            text(x+(w-width)//2, y+(h-18)//2, label, 14, 'white')
        elif kind == 'group':
            text(x, y, label, 11, MUTED)
            for bx, by, bw, bh, j, title in selection_boxes(name, x, y, w, h):
                selected = d[name] == j
                g.rounded_rectangle((bx, by, bx+bw-1, by+bh-1), 4,
                                    fill=BLUE if selected else 'white', outline='#D7E0E9')
                width = g.textlength(title, font=font(13))
                text(bx+(bw-width)//2, by+(bh-17)//2, title, 13, 'white' if selected else INK)
        else:
            g.rounded_rectangle((x, y, x+w-1, y+h-1), 4, fill='white', outline='#D7E0E9')
            text(x+8, y+2, label if name != 'dc' or d['dm'] == 0 else 'SOQPSK-TG 不使用 PCM 码型', 11, MUTED)
            text(x+8, y+18, label_value(name, d)+('    选择 >' if kind == 'choice' else ''),
                 15, MUTED if name == 'dc' and d['dm'] != 0 else INK)
    text(12, 224, notice[:36], 12, RED if any(t in notice for t in ('失败', '错误', '超时', '未标定', '未就绪')) else MUTED)
    for i, title in enumerate(TABS):
        g.rectangle((i*NAV_WIDTH, 244, (i+1)*NAV_WIDTH-1, 271), fill=BLUE if i == page else NAVY)
        text(i*NAV_WIDTH+(NAV_WIDTH-g.textlength(title, font=font(13)))//2, 250, title, 13, 'white')
    if picker:
        g.rectangle((0, 31, 479, 243), fill=BG)
        text(12, 38, 'PCM 码型', 14)
        for x, y, w, h, j, title in PICK_BOXES:
            g.rounded_rectangle((x, y, x+w-1, y+h-1), 4, fill=BLUE if d['dc'] == j else 'white')
            text(x+16, y+6, title, 14, 'white' if d['dc'] == j else INK)
        x, y, w, h = PICK_CANCEL
        g.rounded_rectangle((x, y, x+w-1, y+h-1), 4, fill=NAVY)
        text(x+80, y+6, '取消', 14, 'white')
    if keypad:
        name, value, *fraction = keypad
        g.rectangle((0, 31, 479, 243), fill=BG)
        prompt = ('HEX: 0000..FFFF' if name in HEX_FIELDS else
                  '最终频率 / kHz；两个工作频段，100 kHz步进' if name == 'df' else
                  '发射功率 / dBm（-40 至 -10）' if name == 'dpo' else
                  f'{LIMITS[name][0]}..{LIMITS[name][1]}')
        text(12, 36, prompt, 12)
        text(12, 59, f'{value:04X}' if name in HEX_FIELDS else
             f'-{value}.{5 if fraction and fraction[0] else 0} dBm' if name == 'dpo' else str(value), 22, BLUE)
        for x, y, w, h, title in keys_for(name):
            g.rounded_rectangle((x, y, x+w-2, y+h-2), 3, fill=BLUE if title == '确定' else 'white')
            text(x+16, y+4, title, 15, 'white' if title == '确定' else INK)
    return im


def packet(seq, op, args):
    if op not in (3, 14):
        raise ValueError('unsupported screen operation')
    count = 12 if op == 14 else 9
    if len(args) > count or not 1 <= seq <= 65535:
        raise ValueError('sequence/argument count')
    args = list(args)+[0]*(count-len(args))
    if any(not isinstance(a, int) or not 0 <= a <= 0xffffff for a in args):
        raise ValueError('word range')
    prefix = bytes((0xa5, 0x5a, 6 if op == 14 else 2, op))+struct.pack(f'<{count+1}I', seq, *args)
    return prefix+struct.pack('<I', crc16(prefix))+b'\xff'*3


def request(action, d):
    if action == 'applytx':
        validate(d)
        return 14, [d['dm'], d['dr']//1000, d['dc'] if d['dm'] == 0 else 0, d['dfr'],
                    d['db'], d['dn'], d['di'], d['dsl'], d['dsh'], d['dsb']+d['dsx']*256+(d['dr']%1000)*1024,
                    d['df'], d['dpo']*500]
    if action in ('queryst', 'readtx'):
        return 3, []
    raise ValueError(action)


def native_validation():
    lines = ['bad=0']
    for name, (lo, hi) in LIMITS.items():
        lines += [f'if({name}<{lo}||{name}>{hi})', '{', 'bad=1', '}']
    for name, maximum in [('dm', 1), ('dc', 6), ('dfr', 1), ('di', 1), ('dsx', 1)]:
        lines += [f'if({name}<0||{name}>{maximum})', '{', 'bad=1', '}']
    lines += ['if(df>5150000&&df<5925000)', '{', 'bad=1', '}',
              'tmp=df%100', 'if(tmp!=0)', '{', 'bad=1', '}']
    lines += ['if(dsb<=32&&dsx!=0)', '{', 'bad=1', '}']
    for bits in range(16, 32):
        lines += [f'if(dsb=={bits}&&dsh>={1 << (bits-16)})', '{', 'bad=1', '}']
    lines += ['tmp=db*dn', 'if(tmp>16384)', '{', 'bad=1', '}', 'if(dfr==1)', '{',
              'tmp=db*8-dsb', 'if(dn>1)', '{', 'tmp=tmp-8', '}',
              'if(tmp<=0)', '{', 'bad=1', '}', '}']
    return lines


def copy_snapshot_lines():
    # 只读rev最后提交，先复制到c*影子变量；禁止把局部更新当完整快照。
    return ['if(crate>=100000&&crate<=20000000)', '{', 'dr=crate', '}',
            'if(csource==1)', '{', 'dc=ccode', 'dfr=cframed', 'db=cfbytes', 'dn=cfcount', 'di=crunflag', '}',
            'if(csyncbits>=16&&csyncbits<=33&&csynclo<=65535&&csynchi<=65535&&csyncbit<=1)',
            '{', 'dsb=csyncbits', 'dsl=csynclo', 'dsh=csynchi', 'dsx=csyncbit', '}',
            'if(crfmode==3)', '{', 'dm=1', '}else if(crfmode==4)', '{', 'dm=0', '}'] + native_final_check() + [
            'tmp=crfpmag%500', 'if(finalok==1&&tmp==0)',
            '{', 'df=coutkhz', 'dpo=crfpmag/500', '}']


def native_final_check():
    return ['finalok=0', 'finalfreq=0',
            'if(coutkhz>=5091000&&coutkhz<=5150000)', '{', 'finalfreq=1', '}',
            'if(coutkhz>=5925000&&coutkhz<=6125000)', '{', 'finalfreq=1', '}',
            'tmp=coutkhz%100', 'part=crfpmag%500',
            'if(csysvalid==1&&croute==2&&finalfreq==1&&tmp==0&&part==0&&crfpmag>=10000&&crfpmag<=40000)',
            '{', 'finalok=1', '}']


def hit(x, y, w, h):
    return f'if(ix>={x}&&ix<{x+w}&&iy>={y}&&iy<{y+h})'


def write(name, lines):
    (BASE/name).write_text('\n'.join(lines)+'\n', encoding='utf-8')


def native_events():
    BASE.mkdir(exist_ok=True)
    variables = dict(pg=0, edit=0, pick=0, entry=0, phalf=0, dirty=0, seq=0, op=3,
                     pending=0, age=0, seenrev=0, stale=0, msg=0, ix=0, iy=0,
                     tmp=0, part=0, waitop=3, bad=0, badkey=0, routeok=0,
                     finalok=0, finalfreq=0, snapshotok=0, readpending=0,
                     paintpg=255, paintedit=255, paintpick=255, redraw=0,
                     rfview=0, oldrf=255, viewok=0, foot=0, keyerr=0, olderr=255,
                     **DEFAULTS, **{name: 0 for name in ACTUAL},
                     **{'c'+name: 0 for name in ACTUAL}, **{f'a{i}': 0 for i in range(12)})
    # 原 HMI 的非活动物理页仍需变量声明；运行时只导航 shouye 画布。
    variables.update(da=359, dtf=3637000, ds=1, dta=40,
                     rm=0, rf=70000, rr=4000, rb=20000, rg=20, rc=0)
    program = ['// P351最终射频v9；dpo单位0.5dB；固定中频，差分刷新；功率非实测。', 'int sys0=0,sys1=0,sys2=0']
    program += [f'int {name}={value}' for name, value in variables.items()]
    program += ['int txseq=0,txmode=0,txlo=0,txatt=20,txrate=4',
                'int txcode=0,txframed=1,txflen=512,txcount=1,txinf=1',
                'int txsum=0,txok=0,txsel=0', 'baud=115200', 'dim=85', 'recmod=0', 'bkcmd=0',
                'printh 00 00 00 FF FF FF 88 FF FF FF', 'page 0']
    fg, bg, navy, blue, red, muted = map(rgb565, (INK, BG, NAVY, BLUE, RED, MUTED))
    lines = []
    caches = {}
    def line(value):
        lines.append(value)
    def begin_region(*dependencies):
        # 每个区域独立比较，不能用sum/CRC代替，避免碰撞漏刷新。
        pairs = []
        for dependency in dependencies:
            cache = f'pv{len(caches)}'
            caches[cache] = 0
            pairs.append((dependency, cache))
        condition = '||'.join(['redraw==1'] + [f'{a}!={b}' for a,b in pairs])
        line(f'if({condition})'); line('{')
        return pairs
    def end_region(pairs):
        for dependency, cache in pairs:
            line(f'{cache}={dependency}')
        line('}')
    def text(x, y, w, h, value, fc=fg, bc=bg):
        line(f'xstr {x},{y},{w},{h},5,{fc},{bc},0,1,1,"{value}"')
    def val(name, x, y, w, h, hex16=False, quarter=False, final_frequency=False, negative_power=False, negative_mdbm=False):
        if final_frequency:
            line(f'tmp={name}/1000'); line('covx tmp,t5.txt,0,0'); line('t5.txt=t5.txt+"."')
            line(f'part={name}%1000'); line('part=part/100'); line('covx part,t0.txt,0,0')
            line('t5.txt=t5.txt+t0.txt'); line('t5.txt=t5.txt+" MHz"')
        elif negative_mdbm:
            line(f'tmp={name}/1000'); line('covx tmp,t0.txt,0,0'); line('t5.txt="-"')
            line('t5.txt=t5.txt+t0.txt'); line('t5.txt=t5.txt+"."')
            line(f'part={name}%1000'); line('covx part,t0.txt,3,0')
            line('t5.txt=t5.txt+t0.txt'); line('t5.txt=t5.txt+" dBm"')
        elif negative_power:
            # 草稿dpo用半dB整数；键盘entry用整dB，phalf保存小数位。
            line(f'tmp={name}' if name == 'entry' else f'tmp={name}/2')
            line('covx tmp,t0.txt,0,0'); line('t5.txt="-"'); line('t5.txt=t5.txt+t0.txt')
            line('t5.txt=t5.txt+"."')
            line('part=phalf*5' if name == 'entry' else f'part={name}%2')
            if name != 'entry':
                line('part=part*5')
            line('covx part,t0.txt,0,0'); line('t5.txt=t5.txt+t0.txt'); line('t5.txt=t5.txt+" dBm"')
        elif quarter:
            line(f'tmp={name}/4'); line('covx tmp,t5.txt,0,0'); line('t5.txt=t5.txt+"."'); line(f'part={name}%4')
            for value in range(4):
                line(f'if(part=={value})'); line('{'); line(f't5.txt=t5.txt+"{value*25:02d}"'); line('}')
        else:
            line(f'covx {name},t5.txt,{4 if hex16 else 0},{2 if hex16 else 0}')
        line(f'xstr {x},{y},{w},{h},5,{fg},65535,0,1,1,t5.txt')
    # 只在物理页初次装入时清屏。普通触摸/新快照不清屏。
    line('if(paintpg==255)'); line('{')
    line(f'cls {bg}'); line(f'fill 0,0,480,31,{navy}')
    text(12, 3, 260, 25, '校验仿真器 P351 TX2', 65535, navy)
    line(f'fill 384,3,87,25,{red}'); text(396, 3, 75, 25, '停止发射', 65535, red); line('}')
    line('redraw=0')
    line('if(pg!=paintpg||edit!=paintedit||pick!=paintpick)'); line('{')
    line('redraw=1'); line(f'fill 0,31,480,213,{bg}'); line('}')
    line('viewok=0'); line('if(crev>0&&stale<16&&snapshotok==1)'); line('{'); line('viewok=1'); line('}')
    line('routeok=0'); line('if(cifvalid==1&&cifkhz==70000)'); line('{'); line('routeok=1'); line('}')
    lines.extend(native_final_check())
    line('rfview=0')
    line('if(snapshotok==1&&crev>0&&stale<16&&crfvalid==1)'); line('{')
    line('if(finalok==1&&crfphase==4&&crffault==0)'); line('{')
    line('rfview=2'); line('}')
    line('if(crfmode==0)'); line('{'); line('if(crfphase==0||crfphase==5)'); line('{')
    line('rfview=1'); line('}'); line('}'); line('}')
    line('if(rfview!=oldrf)'); line('{')
    for value, title in enumerate(('RF: ?', 'RF: OFF', 'RF: ON')):
        line(f'if(rfview=={value})'); line('{'); text(284,3,96,25,title,65535,navy); line('}')
    line('oldrf=rfview'); line('}')
    for page, items in enumerate(PAGES):
        line(f'if(pg=={page}&&edit==0&&pick==0)'); line('{')
        for kind, name, label, x, y, w, h in items:
            dependencies = {'field':[name], 'group':[name], 'choice':['dc','dm'],
                            'summary':['dsl','dsh','dsx','dsb']}.get(kind, [])
            if kind == 'actual':
                dependencies = ['viewok'] + {
                    'tx':['finalok','coutkhz','crfpmag'],
                    'power':['finalok','crfmodel','crfexpect'],
                    'reference':['crefsrc','crefok'],
                    'source':['crfphase','crffault'],
                    'temp':['ctempmc','ctempok']}[name]
            region = begin_region(*dependencies)
            if kind == 'text':
                text(x, y, w, h, label, fg if name in ('if', 'power') else muted)
            elif kind == 'summary':
                text(x, y, 112, h, 'SYNC HEX:'); val('dsx', 124, y, 20, h)
                val('dsh', 148, y, 78, h, hex16=True); val('dsl', 230, y, 78, h, hex16=True)
                text(326, y, 54, h, '/bit:'); val('dsb', 388, y, 64, h)
            elif kind == 'actual':
                line(f'fill {x},{y},{w},{h},{bg}')
                line('if(crev==0||stale>=16||snapshotok==0)'); line('{'); text(x, y, w, h, '等待板端状态')
                line('}else'); line('{')
                if name == 'tx':
                    text(x, y, w, h, '最终RF设置尚未提交', muted)
                    line('if(finalok==1)'); line('{')
                    text(x, y, 64, h, '已应用')
                    val('coutkhz', x+64, y, 156, h, final_frequency=True)
                    val('crfpmag', x+228, y, 216, h, negative_mdbm=True); line('}')
                elif name == 'power':
                    text(x, y, w, h, '输出功率：未校准 / 无有效估算', red)
                    line('if(finalok==1)'); line('{')
                    line('if(crfmodel==1||crfmodel==2)'); line('{')
                    text(x, y, w, h, '功率模型有效 / 无有效功率值', muted)
                    line('if(crfexpect>0&&crfexpect<=100000)'); line('{')
                    text(x, y, 148, h, '增益模型估算')
                    line('if(crfmodel==2)'); line('{'); text(x, y, 148, h, '整链标定估算'); line('}')
                    val('crfexpect', x+148, y, 192, h, negative_mdbm=True)
                    text(x+348, y, 108, h, '非实测', muted); line('}'); line('}'); line('}')
                elif name == 'reference':
                    text(x, y, w, h, '外部10MHz  未锁定', red)
                    line('if(crefsrc==1&&crefok==1)'); line('{'); text(x, y, w, h, '外部10MHz  已锁定'); line('}')
                elif name == 'source':
                    text(x, y, w, h, '执行阶段未知', muted)
                    for phase, title in [(0,'空闲'),(1,'等待外部参考'),(2,'等待二级本振'),(3,'启动70MHz中频'),(4,'任务执行中'),(5,'已停止')]:
                        line(f'if(crfphase=={phase})'); line('{'); text(x, y, w, h, title); line('}')
                    line('if(crffault>0)'); line('{')
                    text(x, y, 176, h, '安全停发 / 故障', red); val('crffault', x+176, y, 112, h); line('}')
                elif name == 'temp':
                    text(x, y, 120, h, 'AD9361 /mC:'); val('ctempmc', x+124, y, w-124, h)
                    line('if(ctempok==0)'); line('{'); text(x+124, y, w-124, h, '--'); line('}')
                line('}')
            elif kind == 'group':
                text(x, y, w, 16, label, muted)
                for bx, by, bw, bh, value, title in selection_boxes(name, x, y, w, h):
                    line(f'if({name}=={value})'); line('{'); line(f'fill {bx},{by},{bw},{bh},{blue}')
                    text(bx+5, by, bw-10, bh, title, 65535, blue)
                    line('}else'); line('{'); line(f'fill {bx},{by},{bw},{bh},65535')
                    text(bx+5, by, bw-10, bh, title, fg, 65535); line('}')
            elif kind == 'button':
                line(f'fill {x},{y},{w},{h},{blue}'); text(x+8, y, w-16, h, label, 65535, blue)
            else:
                if kind == 'field':
                    line('if(redraw==1)'); line('{')
                line(f'fill {x},{y},{w},{h},65535'); text(x+6, y, w-12, 16, label, muted, 65535)
                if kind == 'field':
                    line('}')
                    val(name, x+6, y+16, w-12, h-16, hex16=name in HEX_FIELDS,
                        final_frequency=name == 'df', negative_power=name == 'dpo')
                else:
                    for value, title in enumerate(CODES):
                        line(f'if(dc=={value})'); line('{'); text(x+6, y+15, w-12, h-15, title+'  选择 >', fg, 65535); line('}')
                    line('if(dm!=0)'); line('{'); text(x+6, y, w-12, h, 'SOQPSK-TG 不使用PCM码型', muted, 65535); line('}')
            end_region(region)
        line('}')
    field_ids = {name: i+1 for i, name in enumerate(LIMITS)}
    prompts = {name: label for items in PAGES for kind, name, label, *_ in items if kind == 'field'}
    line('if(edit>0)'); line('{')
    for name, (lo, hi) in LIMITS.items():
        line(f'if(edit=={field_ids[name]})'); line('{')
        prompt = ('同步字HEX 0000-FFFF' if name in HEX_FIELDS else
                  '最终频率/kHz，100kHz步进' if name == 'df' else
                  '发射功率/dBm (-40至-10)' if name == 'dpo' else f'{prompts[name]} ({lo}-{hi})')
        line('if(redraw==1)'); line('{'); text(12, 36, 456, 23, prompt); line('}')
        region = begin_region('entry', 'phalf') if name == 'dpo' else begin_region('entry')
        val('entry', 12, 60, 270, 28, hex16=name in HEX_FIELDS, negative_power=name == 'dpo')
        end_region(region)
        line('if(redraw==1)'); line('{')
        for x, y, w, h, title in keys_for(name):
            line(f'fill {x},{y},{w-2},{h-2},65535'); text(x+12, y, w-16, h-2, title, fg, 65535)
        line('}'); line('}')
    line('keyerr=0'); line('if(msg==3||badkey==1)'); line('{'); line('keyerr=1'); line('}')
    line('if(redraw==1||keyerr!=olderr)'); line('{')
    line(f'if(edit!={field_ids["dpo"]})'); line('{')
    line('if(keyerr==1)'); line('{'); text(290,60,178,28,'参数越界',red)
    line('}else'); line('{'); text(290,60,178,28,''); line('}'); line('}')
    line('olderr=keyerr'); line('}'); line('}')
    line('if(pick==1)'); line('{'); region = begin_region('dc'); text(12, 36, 456, 24, 'PCM码型')
    for x, y, w, h, value, title in PICK_BOXES:
        line(f'fill {x},{y},{w},{h},65535'); text(x+16, y, w-20, h, title, fg, 65535)
        line(f'if(dc=={value})'); line('{'); line(f'fill {x},{y},{w},{h},{blue}')
        text(x+16, y, w-20, h, title, 65535, blue); line('}')
    x, y, w, h = PICK_CANCEL
    line(f'fill {x},{y},{w},{h},{navy}'); text(x+80, y, w-80, h, '取消', 65535, navy)
    end_region(region); line('}')
    line('if(pg!=paintpg)'); line('{')
    for i, title in enumerate(TABS):
        line(f'if(pg=={i})'); line('{'); line(f'fill {i*NAV_WIDTH},244,{NAV_WIDTH},28,{blue}')
        text(i*NAV_WIDTH+12, 244, NAV_WIDTH-24, 28, title, 65535, blue)
        line('}else'); line('{'); line(f'fill {i*NAV_WIDTH},244,{NAV_WIDTH},28,{navy}')
        text(i*NAV_WIDTH+12, 244, NAV_WIDTH-24, 28, title, 65535, navy); line('}')
    line('}')
    line('if(edit==0&&pick==0)'); line('{')
    notices = [('rev==0||stale>=16', '等待板端状态'), ('pending==1', '等待板端确认'),
                        ('msg==1', '操作完成'), ('msg==2', '操作失败，请查看状态'),
                        ('msg==3', '参数错误，请检查同步字和帧容量'), ('msg==4', '停止请求已发送'),
                        ('msg==5', '确认超时'), ('msg==6', '参数检查通过'),
                        ('msg==8', '未就绪：请查询标定、参考及故障')]
    line('foot=0')
    for index,(condition,title) in enumerate(notices,1):
        line(f'if({condition})'); line('{'); line(f'foot={index}'); line('}')
    region = begin_region('foot')
    for index,title in enumerate(['参数草稿']+[t for _,t in notices]):
        line(f'if(foot=={index})'); line('{')
        text(12,220,456,24,title,muted if index in (0,3,8) else red); line('}')
    end_region(region); line('}')
    line('paintpg=pg'); line('paintedit=edit'); line('paintpick=pick')
    write('shouye.p1.press.txt', lines)
    program[2:2] = [f'int {name}={value}' for name,value in caches.items()]
    write('Program.s.txt', program)
    press = ['ix=tch0', 'iy=tch1', 'if(ix>=0&&ix<480&&iy>0&&iy<272)', '{',
             'if(ix>=384&&iy<=30)', '{', 'printh A5 5A 01 02 FF FF FF',
             'pending=0', 'snapshotok=0', 'readpending=0', 'msg=4', 'edit=0', 'pick=0', '}else', '{',
             'if(iy>=244)', '{', f'pg=ix/{NAV_WIDTH}', 'edit=0', 'pick=0', '}else if(pick==1)', '{']
    for x, y, w, h, value, title in PICK_BOXES:
        press += [hit(x, y, w, h), '{', f'dc={value}', 'pick=0', 'dirty=1', '}']
    press += [hit(*PICK_CANCEL), '{', 'pick=0', '}', '}else if(edit>0)', '{']
    for name, field_id in field_ids.items():
        press += [f'if(edit=={field_id})', '{']
        for x, y, w, h, title in keys_for(name):
            press += [hit(x, y, w, h), '{']
            if title in '0123456789ABCDEF' and len(title) == 1:
                radix = 16 if name in HEX_FIELDS else 10
                press += [f'if(entry<={4095 if radix == 16 else 9999999})', '{', f'entry=entry*{radix}+{int(title, radix)}',
                          '}else', '{', 'badkey=1', 'msg=3', '}']
            elif title == '清空':
                press += ['entry=0', 'badkey=0', 'msg=0']
                if name == 'dpo':
                    press += ['phalf=0']
            elif title == '退格':
                if name == 'dpo':
                    press += ['if(phalf==1)', '{', 'phalf=0', '}else', '{', 'entry=entry/10', '}']
                else:
                    press += [f'entry=entry/{16 if name in HEX_FIELDS else 10}']
                press += ['badkey=0', 'msg=0']
            elif title == '切换 .0/.5':
                press += ['phalf=1-phalf', 'badkey=0', 'msg=0']
            elif title == '取消':
                press += ['edit=0']
            else:
                lo, hi = LIMITS[name]
                press += ['bad=badkey']
                if name == 'df':
                    press += ['tmp=entry%100', 'if(tmp!=0)', '{', 'bad=1', '}',
                              'if(entry>5150000&&entry<5925000)', '{', 'bad=1', '}']
                value = 'entry'
                if name == 'dpo':
                    press += ['tmp=entry*2+phalf']
                    value = 'tmp'
                press += [f'if({value}>={lo}&&{value}<={hi}&&bad==0)', '{', f'{name}={value}', 'edit=0', 'dirty=1', 'msg=0',
                          '}else', '{', 'msg=3', '}']
            press += ['}']
        press += ['}']
    press += ['}else', '{']
    for page, items in enumerate(PAGES):
        press += [f'if(pg=={page})', '{']
        for kind, name, label, x, y, w, h in items:
            if kind == 'group':
                for bx, by, bw, bh, value, title in selection_boxes(name, x, y, w, h):
                    press += [hit(bx, by, bw, bh), '{', f'{name}={value}', 'dirty=1', 'msg=0', '}']
                continue
            if kind not in ('field', 'choice', 'button'):
                continue
            press += [hit(x, y, w, h), '{']
            if kind == 'field':
                press += [f'edit={field_ids[name]}', f'entry={name}', 'badkey=0', 'msg=0']
                if name == 'dpo':
                    press += ['entry=dpo/2', 'phalf=dpo%2']
            elif kind == 'choice':
                press += ['if(dm==0)', '{', 'pick=1', 'msg=0', '}']
            elif name == 'navsync':
                press += ['pg=2']
            elif name == 'checkframe':
                press += native_validation()+['if(bad==0)', '{', 'msg=6', '}else', '{', 'msg=3', '}']
            elif name == 'readtx':
                press += ['if(pending==0)', '{', 'readpending=1', 'op=3', 'click t3,1', '}']
            else:
                press += [f'op={14 if name == "applytx" else 3}', 'click t3,1']
            press += ['}']
        press += ['}']
    press += ['}', '}', '}', 'click p1,1']
    write('shouye.press.txt', press)
    send = ['if(pending==0)', '{', 'bad=0', 'if(op==14)', '{']+native_validation()+['}']
    send += ['if(bad==0)', '{']+[f'a{i}=0' for i in range(12)]
    fields = ['dm', 'dr/1000', 'dc', 'dfr', 'db', 'dn', 'di', 'dsl', 'dsh', 'dsb+dsx*256+(dr%1000)*1024', 'df', 'dpo*500']
    send += ['if(op==14)', '{']
    for i, value in enumerate(fields):
        # TJC逐步运算，不依赖复杂括号/优先级；最高字节仍为0，不破坏STOP扫描。
        send += ['tmp=dr%1000', 'a9=tmp*1024', 'tmp=dsx*256', 'a9=a9+tmp', 'a9=a9+dsb'] if i == 9 else [f'a{i}={value}']
    send += ['if(dm!=0)', '{', 'a2=0', '}', '}',
             'if(seq>=65535)', '{', 'seq=0', '}', 'seq=seq+1', 'waitop=op',
             'pending=1', 'age=0', 'msg=0', 'if(op==14)', '{', 'snapshotok=0', 'readpending=0', '}', 'sendme', 'crcrest 1,0xffff',
             'if(op==14)', '{', 'printh A5 5A 06', 'crcputh A5 5A 06',
             '}else', '{', 'printh A5 5A 02', 'crcputh A5 5A 02', '}',
             'prints op,1', 'crcputs op,1', 'prints seq,4', 'crcputs seq,4']
    for i in range(9):
        send += [f'prints a{i},4', f'crcputs a{i},4']
    send += ['if(op==14)', '{']
    for i in range(9, 12):
        send += [f'prints a{i},4', f'crcputs a{i},4']
    send += ['}', 'prints crcval,4', 'printh FF FF FF', '}else', '{', 'msg=3', '}', '}']
    write('shouye.t3.press.txt', send)
    timer = ['if(stale<1000)', '{', 'stale=stale+1', '}', 'if(rev>0&&rev!=seenrev)', '{']
    timer += [f'c{name}={name}' for name in ACTUAL] + ['snapshotok=1']
    timer += ['if(seenrev==0&&dirty==0)', '{']+copy_snapshot_lines()+['}',
             'if(readpending==1&&cackseq==seq&&cackop==3&&cackrc==0)', '{']+copy_snapshot_lines()+[
             'dirty=0', 'readpending=0', '}',
             'seenrev=crev', 'stale=0', 'if(pending==1&&cackseq==seq&&cackop==waitop&&cackrc!=4)',
             '{', 'pending=0', 'if(cackrc==0)', '{', 'msg=1', '}else if(cackrc==8)', '{', 'msg=8',
             '}else', '{', 'msg=2', '}', '}', 'if(msg==4&&crfmode==0&&crfvalid==1)', '{', 'msg=1', '}',
             'click p1,1', '}', 'if(pending==1)', '{', 'age=age+1', 'if(age>=80)', '{',
             'pending=0', 'readpending=0', 'msg=5', 'click p1,1', '}', '}', 'if(stale==16)', '{', 'click p1,1', '}',
             'if(stale==4&&pending==0)', '{', 'op=3', 'click t3,1', '}']
    write('shouye.tm0.timer.txt', timer)
    write('shouye.postinit.txt', ['vis 255,0', 'paintpg=255', 'paintedit=255', 'paintpick=255',
                               'oldrf=255', 'tm0.tim=250', 'tm0.en=1', 'click p1,1', 'sendme'])
    manifest = dict(device='TJC4827T143_011R_P20', width=480, height=272, baud=115200,
                    native_architecture='shouye四逻辑页画布；p1绘图，t3发送，tm0=250ms',
                    pages=PAGES, draft_defaults=DEFAULTS, integer_limits=LIMITS,
                    readonly_globals=ACTUAL, field_ids=field_ids,
                    committed_snapshot_globals=['c'+name for name in ACTUAL],
                    snapshot_commit='PS writes rev last; timer copies all readonly globals to c* atomically; STOP/APPLY invalidate snapshot until next rev',
                    declared_int_value_bytes=(len(variables)+len(caches)+16)*4,
                    rendering=dict(revision='v10', cache_ints=len(caches),
                                   full_clear='physical page init only',
                                   body_clear='page/keypad/picker layout changes only',
                                   same_layout='changed regions only'),
                    native_compiled=False, hmi_or_tft_generated=False, hardware_tested=False,
                    apply=dict(version=6, opcode=14, bytes=63, crc_input_bytes=56, arguments=fields, rate_step_bps=1),
                    other_requests=dict(version=2, opcode=3, bytes=51),
                    fixed_profile=dict(if_hz=70000000, target_mdBm=-30000, external_reference_hz=10000000,
                                       ad9361_reference_hz=40000000, calibration_required_per_modulation=True),
                    final_output=dict(frequency_khz_ranges=[[5091000,5150000],[5925000,6125000]],
                                      frequency_step_khz=100, power_dbm_range=[-40,-10],
                                      screen_power_step_db=0.5, draft_power_unit_db=0.5,
                                      lo1_khz=1430000, mid_khz=1500000,
                                      lo2='final_khz-1500000', final_rf_hardware_qualified=False),
                    stop='A5 5A 01 02 FF FF FF',
                    sync='dsl|dsh<<16|dsx<<32; 16..33 bits; MSB first; reject overflow')
    (BASE/'control_manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding='utf-8')
    for page, title in enumerate(TABS):
        draw(page).save(BASE/f'page_{page}_{title}.png')
    draw(1, picker=True).save(BASE/'PCM码型选择.png')
    draw(2, keypad=('dsh', 0xFE6B)).save(BASE/'同步字HEX键盘.png')
    sheet = Image.new('RGB', (984, 848), '#DDE5ED')
    for page in range(4):
        sheet.paste(draw(page), (8+(page % 2)*488, 8+(page // 2)*280))
    sheet.paste(draw(1, picker=True), (8, 568))
    sheet.paste(draw(2, keypad=('dsh', 0xFE6B)), (496, 568))
    sheet.save(BASE/'四页最终射频总览.png')
    print('GENERATED', BASE)


if __name__ == '__main__':
    native_events()
