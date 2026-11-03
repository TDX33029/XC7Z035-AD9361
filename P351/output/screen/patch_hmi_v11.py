# -*- coding: utf-8 -*-
"""离线改写 HMI：把 v11 优化事件码写入 0.pa / Program.s 条目，产出 v11.HMI。
不改图片/字库/其余页面；目录(块头)同步更新 0.pa 与 Program.s 的指针。
所有解析步骤自带边界校验，任一断言失败即中止不落盘。
"""
import struct
import sys
from pathlib import Path

BASE = Path(__file__).resolve().parent
SRC = BASE / 'P351_FinalRF_480x272_v11_wip.HMI'
DST = BASE / 'P351_FinalRF_480x272_v11.HMI'
BS = 524288

data = SRC.read_bytes()
print('src size', len(data))

# ---------- 定位 0.pa / Program.s 条目（块0目录） ----------
cnt, = struct.unpack_from('<I', data, 0)
recs = []
p = 4
for i in range(cnt):
    name = data[p:p+16]
    o, s, f = struct.unpack_from('<III', data, p+16)
    recs.append([name, o, s, f])
    p += 28
pa_rec = next(r for r in recs if r[0].startswith(b'0.pa'))
ps_rec = next(r for r in recs if r[0].startswith(b'Program.s'))
PA_OFF, PA_SIZE = pa_rec[1], pa_rec[2]
PS_OFF, PS_SIZE = ps_rec[1], ps_rec[2]
print('0.pa @%d+%d  Program.s @%d+%d' % (PA_OFF, PA_SIZE, PS_OFF, PS_SIZE))
assert PA_OFF + PA_SIZE == PS_OFF, '0.pa 与 Program.s 应连续'

pa = data[PA_OFF:PA_OFF+PA_SIZE]

# ---------- 解析 0.pa：头(56B) + TOC(15×12, 偏移相对+56) + 节 ----------
toc_base, n_sec = 56, 15
secs = []
for i in range(n_sec):
    o, s, z = struct.unpack_from('<III', pa, toc_base + i*12)
    assert z == 0
    secs.append([o, s])
assert secs[0][0] == n_sec * 12, '首节应紧跟 TOC'
abs_off = [toc_base + o for o, s in secs]
for i in range(n_sec - 1):
    assert abs_off[i] + secs[i][1] == abs_off[i+1], '节应连续'
assert abs_off[-1] + secs[-1][1] == PA_SIZE, '末节应到条目尾'


def parse_records(blob, pos, count):
    """严格解析 count 个 [u32 len][gbk 行]，返回 (行列表, 结束位置)。"""
    lines = []
    for _ in range(count):
        n, = struct.unpack_from('<I', blob, pos)
        pos += 4
        if n > 65536 or pos + n > len(blob):
            raise ValueError('record length out of range')
        lines.append(blob[pos:pos+n].decode('gbk'))
        pos += n
    return lines, pos


def find_event(sec_bytes, count):
    """在节内定位 codes<kind>-<count> 令牌，返回 (令牌起, 记录起, 记录止)。"""
    import re
    for m in re.finditer(rb'codes([a-z]+)-([0-9]+)', sec_bytes):
        if int(m.group(2)) == count:
            tok_start = m.start()
            rec_start = m.end()
            return tok_start, rec_start
    raise ValueError('token with count %d not found' % count)


def event_attr_boundary(sec_bytes, pos):
    """校验 pos 处是 [u32 len][ascii 名] 属性、节尾，或节尾 4 字节 0 终止符。"""
    if pos == len(sec_bytes):
        return True
    if pos + 4 == len(sec_bytes):
        return struct.unpack_from('<I', sec_bytes, pos)[0] == 0
    n, = struct.unpack_from('<I', sec_bytes, pos)
    if not 1 <= n <= 64 or pos + 4 + n > len(sec_bytes):
        return False
    nm = sec_bytes[pos+4:pos+4+n]
    return all(0x20 <= c < 0x7f for c in nm)


def replace_event(sec_bytes, old_count, new_lines):
    """替换节内事件记录；返回新节字节。"""
    tok, rec0 = find_event(sec_bytes, old_count)
    old_lines, rec1 = parse_records(sec_bytes, rec0, old_count)
    assert event_attr_boundary(sec_bytes, rec1), '旧记录块边界校验失败'
    newrec = b''.join(struct.pack('<I', len(l.encode('gbk'))) + l.encode('gbk')
                      for l in new_lines)
    return sec_bytes[:rec0] + newrec + sec_bytes[rec1:], old_lines


def canon(lines):
    import re as _re
    return ''.join(_re.sub(r'\s+', '', l) for l in lines
                   if l.strip() and not l.strip().startswith('//'))


def load_lines(name):
    try:
        return (BASE / name).read_text(encoding='utf-8').splitlines()
    except UnicodeDecodeError:
        return (BASE / name).read_text(encoding='gbk').splitlines()


# ---------- p1 节(2)：1123 -> 新源 ----------
# 已知漂移：编辑器内首行 if(paintpg==255) 被注释(//if...)，其余与 v10 源一致；
# v11 恢复守卫（否则 cls 全屏清屏会击穿早退门闸）。
sec2 = pa[abs_off[2]:abs_off[2]+secs[2][1]]
p1v = load_lines('shouye.p1.press.v11.txt')
new_sec2, old_p1 = replace_event(sec2, 1123, p1v)
_p1src = load_lines('shouye.p1.press.txt')
assert _p1src[0] == 'if(paintpg==255)'
assert canon(old_p1) == canon(_p1src[1:]), '旧 p1 与源不一致(超出已知漂移)'

# ---------- t3 节(11)：253 -> 新源 ----------
sec11 = pa[abs_off[11]:abs_off[11]+secs[11][1]]
t3v = load_lines('shouye.t3.press.v11.txt')
new_sec11, old_t3 = replace_event(sec11, 253, t3v)
assert canon(old_t3) == canon(load_lines('shouye.t3.press.txt')), '旧 t3 与源不一致'

# ---------- tm0 节(14)：210 -> 新源 ----------
sec14 = pa[abs_off[14]:abs_off[14]+secs[14][1]]
tmv = load_lines('shouye.tm0.timer.v11.txt')
new_sec14, old_tm = replace_event(sec14, 210, tmv)
assert canon(old_tm) == canon(load_lines('shouye.tm0.timer.txt')), '旧 tm0 与源不一致'

# ---------- 重组 0.pa ----------
new_secs = []
for i in range(n_sec):
    if i == 2:
        new_secs.append(new_sec2)
    elif i == 11:
        new_secs.append(new_sec11)
    elif i == 14:
        new_secs.append(new_sec14)
    else:
        new_secs.append(pa[abs_off[i]:abs_off[i]+secs[i][1]])
rel = n_sec * 12
new_toc = b''
for s in new_secs:
    new_toc += struct.pack('<III', rel, len(s), 0)
    rel += len(s)
new_pa_head = bytearray(pa[:toc_base])
struct.pack_into('<I', new_pa_head, 4, toc_base + rel)  # 总长 = 56+180+节和
new_pa = bytes(new_pa_head) + new_toc + b''.join(new_secs)
print('0.pa: %d -> %d bytes' % (PA_SIZE, len(new_pa)))

# ---------- Program.s 条目：整段文本替换 ----------
old_ps = data[PS_OFF:PS_OFF+PS_SIZE].decode('gbk')
assert canon(old_ps.splitlines()) == canon(load_lines('Program.s.txt')), '旧 Program.s 与源不一致'
new_ps = ('\r\n'.join(load_lines('Program.s.v11.txt')) + '\r\n').encode('gbk')
print('Program.s: %d -> %d bytes' % (PS_SIZE, len(new_ps)))

# ---------- 组装新文件 + 全部块头目录指针更新 ----------
base = data[:PA_OFF]
new_file = bytearray(base + new_pa + new_ps)
pa2_off = PA_OFF
ps2_off = PA_OFF + len(new_pa)
nb = (len(new_file) + BS - 1) // BS
patched = 0
for b in range(nb):
    ho = b * BS
    if ho + 4 > len(new_file):
        break
    c, = struct.unpack_from('<I', new_file, ho)
    if c > 2000:
        continue
    q = ho + 4
    for i in range(c):
        name = bytes(new_file[q:q+16])
        if name[:5] == b'0.pa\x00' or name.startswith(b'0.pa'):
            struct.pack_into('<III', new_file, q+16, pa2_off, len(new_pa), 0)
            patched += 1
        elif name.startswith(b'Program.s'):
            struct.pack_into('<III', new_file, q+16, ps2_off, len(new_ps), 0)
            patched += 1
        q += 28
print('directory records patched:', patched, '(expect %d x2)' % nb)

DST.write_bytes(bytes(new_file))
print('written', DST, len(new_file), 'bytes; last block slack:',
      nb*BS - len(new_file))
