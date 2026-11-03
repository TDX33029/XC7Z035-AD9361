# -*- coding: utf-8 -*-
"""v11 事件源生成：基于 v10 导出源做三处优化，输出 *.v11.txt 供 HMI 改写用。
优化点（与 2026-11-01 串口无数据/易卡死诊断对应）：
  1. p1 绘图器：入口加“无变化早退”门闸——界面无变化时触摸只做约 20 次比较即返回，
     不再走完 1123 行解释体；这是脱机/联机下“点一下卡一秒”的主因。
  2. t3 发送器：去掉帧前的 sendme（0x66 页码帧污染自定义帧流，板端自有 2s 探测）。
  3. tm0 轮询：pending 超时 20s→2s；板端静默时每 2s 主动查询（独立standalone可见心跳，
     板端在线时由 rev 推送抑制，不产生多余流量）。
"""
from pathlib import Path

BASE = Path(__file__).resolve().parent


def read(name):
    try:
        return (BASE / name).read_text(encoding='utf-8').splitlines()
    except UnicodeDecodeError:
        return (BASE / name).read_text(encoding='gbk').splitlines()


def write(name, lines):
    with open(BASE / name, 'w', encoding='gbk', newline='') as f:
        f.write('\r\n'.join(lines) + '\r\n')


# ---------- 1) p1.painter ----------
p1 = read('shouye.p1.press.txt')
# 锚点：第 15 行(0 基 14)为 viewok=0；末尾 paintpg=pg 前
i_viewok = p1.index('viewok=0')
i_paintpg = p1.index('paintpg=pg')
assert p1[i_viewok - 1] == '}', p1[i_viewok - 2:i_viewok]

gate = ['foot=0',
        'if(edit==0&&pick==0)',
        '{',
        'if(rev==0||stale>=16)',
        '{',
        'foot=1',
        '}',
        'if(pending==1)',
        '{',
        'foot=2',
        '}',
        'if(msg==1)',
        '{',
        'foot=3',
        '}',
        'if(msg==2)',
        '{',
        'foot=4',
        '}',
        'if(msg==3)',
        '{',
        'foot=5',
        '}',
        'if(msg==4)',
        '{',
        'foot=6',
        '}',
        'if(msg==5)',
        '{',
        'foot=7',
        '}',
        'if(msg==6)',
        '{',
        'foot=8',
        '}',
        'if(msg==8)',
        '{',
        'foot=9',
        '}',
        '}',
        'if(redraw==0)',
        '{',
        'if(df!=pv0||dpo!=pv1||dm!=pv2||dr!=pv3||dc!=pv4||dfr!=pv6||di!=pv7||db!=pv8||dn!=pv9||dsl!=pv10||dsh!=pv11)',
        '{',
        'redraw=1',
        '}',
        '}',
        'if(redraw==0)',
        '{',
        'if(dsb!=pv12||dsx!=pv13||entry!=pv35||phalf!=pv43||keyerr!=olderr)',
        '{',
        'redraw=1',
        '}',
        '}',
        'if(redraw==0)',
        '{',
        'if(crev!=pcrev||foot!=pv45)',
        '{',
        'redraw=1',
        '}',
        '}',
        'if(redraw==1)',
        '{']
# 门闸后的绘图体整体包进 if(redraw==1)；pcrev 在绘图体末尾提交
p1v = p1[:i_viewok] + gate + p1[i_viewok:i_paintpg]
# i_paintpg 前一行应是 footer 的收尾 '}'，先补上绘图体的关闭括号
assert p1[i_paintpg - 1] == '}', p1[i_paintpg - 3:i_paintpg + 2]
p1v += ['}', 'pcrev=crev'] + p1[i_paintpg:]
write('shouye.p1.press.v11.txt', p1v)

# ---------- 2) t3.sender ----------
t3 = read('shouye.t3.press.txt')
assert t3.count('sendme') == 1
t3v = [l for l in t3 if l.strip() != 'sendme']
write('shouye.t3.press.v11.txt', t3v)

# ---------- 3) tm0.poller ----------
tm = read('shouye.tm0.timer.txt')
s = '\r\n'.join(tm)
assert 'if(age>=80)' in s and 'if(stale<1000)' in s and 'if(stale==4&&pending==0)' in s
s = s.replace('if(age>=80)', 'if(age>=8)')
s = s.replace('if(stale<1000)', 'if(stale<32000)')
old_q = 'if(stale==4&&pending==0)\r\n{\r\nop=3\r\nclick t3,1\r\n}'
new_q = ('if(pending==0&&stale>=4&&stale<16&&snapshotok==0)\r\n'
         '{\r\ntmp=stale%8\r\nif(tmp==0)\r\n{\r\nop=3\r\nclick t3,1\r\n}\r\n}\r\n'
         'if(pending==0&&stale>=16)\r\n'
         '{\r\ntmp=stale%8\r\nif(tmp==0)\r\n{\r\nop=3\r\nclick t3,1\r\n}\r\n}')
assert old_q in s
s = s.replace(old_q, new_q)
write('shouye.tm0.timer.v11.txt', s.split('\r\n'))

# ---------- 4) Program.s ----------
pg_ = read('Program.s.txt')
assert 'int txsum=0,txok=0,txsel=0' in pg_
i = pg_.index('int txsum=0,txok=0,txsel=0')
pgv = pg_[:i + 1] + ['int pcrev=0'] + pg_[i + 1:]
write('Program.s.v11.txt', pgv)

for n in ('shouye.p1.press.v11.txt', 'shouye.t3.press.v11.txt',
          'shouye.tm0.timer.v11.txt', 'Program.s.v11.txt'):
    print(n, len(read(n)), 'lines')
