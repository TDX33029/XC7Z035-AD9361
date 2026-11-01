"""执行真正TJC事件，核验差分输出、残影和控制回归；不访问硬件。"""
import hashlib
import json
import random
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFont
import test_final_rf_v10 as base
import screen_design as sd

ROOT = Path(__file__).resolve().parent
OLD = ROOT.parent / 'final_rf_v7_20261018'
DRAW = base.parse((ROOT/'shouye.p1.press.txt').read_text(encoding='utf-8'))
OLD_DRAW = base.parse((OLD/'shouye.p1.press.txt').read_text(encoding='utf-8'))
checks = 0


def check(value, message='check failed'):
    global checks
    assert value, message
    checks += 1


def execute(nodes, state, trace):
    for node in nodes:
        if node[0] == 'if':
            execute(node[2] if base.expr(node[1], state) else node[3], state, trace)
            continue
        command = node[1]
        if command.startswith(('cls ', 'fill ')):
            trace.append(command)
        elif command.startswith('xstr '):
            args = command[5:].split(',', 10)
            text = json.loads(args[-1]) if args[-1].startswith('"') else state[args[-1]]
            trace.append(('xstr', tuple(map(int, args[:10])), str(text)))
        elif command.startswith('covx '):
            source, dest, length, fmt = command[5:].split(',')
            state[dest] = format(state[source], 'X' if fmt == '2' else 'd').zfill(int(length))
        else:
            name, expression = command.split('=', 1)
            if name.endswith('.txt'):
                state[name] = ''.join(json.loads(part) if part.startswith('"') else state[part]
                                      for part in expression.split('+'))
            else:
                state[name] = base.expr(expression, state)


def render(state):
    trace = []
    execute(DRAW, state, trace)
    return trace


def cls_count(trace):
    return sum(isinstance(item, str) and item.startswith('cls ') for item in trace)


def body_clears(trace):
    return sum(isinstance(item, str) and item.startswith('fill 0,31,480,213,') for item in trace)


def texts(trace):
    return [item[-1] for item in trace if isinstance(item, tuple)]


# v9有意升级功率单位；字节协议由test_final_rf_v9逐项校验。
check(sd.request('applytx', sd.DEFAULTS)[1][-1] == 30000)
check(sd.TABS == ['发射','数据帧','同步字','状态'])
check(sd.OPTIONS['dm'] == ['PCM/FM','SOQPSK-TG'])
check(not any(name in ('applytfs','applyref','applyrx','stoprx') for page in sd.PAGES for _,name,*_ in page))

state = base.state()
trace = render(state)
check(cls_count(trace) == 1, 'first physical page needs background')
for _ in range(20):
    check(render(state) == [], 'unchanged state must generate zero graphics')
for page in range(4):
    base.tap(state, page*120+40, 260)
    trace = render(state)
    check(cls_count(trace) == 0)
    if page:
        check(body_clears(trace) == 1, 'layout transition needs one content clear')
    for _ in range(4):
        state['rev'] += 1
        trace = render(state)
        if state['rev'] > 1:
            check(not trace, 'identical new revision must not repaint')

# 同页选项点击、数字键盘连续输入不允许清页，空白/同一选项不重画。
state = base.state(); render(state)
base.tap(state, 220, 122)
trace = render(state)
check(not cls_count(trace) and not body_clears(trace))
check(not any(isinstance(i, tuple) and i[1][1] < 31 for i in trace), 'static header repainted')
base.tap(state, 220, 122)
check(render(state) == [])
base.tap(state, 50, 55)
trace = render(state)
check(body_clears(trace) == 1 and cls_count(trace) == 0)
for title in ('清空', '5', '1', '3', '7', '0', '0', '0', '退格'):
    x,y,w,h,_ = next(k for k in sd.keys_for('df') if k[-1] == title)
    base.tap(state, x+w//2, y+h//2)
    trace = render(state)
    check(not cls_count(trace) and not body_clears(trace), title)
    check(not any(isinstance(i, str) and i.startswith('fill ') for i in trace), 'keypad buttons repainted')
check(base.tap(state, 430, 15) == bytes.fromhex('A5 5A 01 02 FF FF FF'))
check(not state['edit'] and not state['pick'] and not state['pending'])
trace = render(state)
check(not cls_count(trace) and body_clears(trace) == 1)

# 页眉变化一次只画一次，不先擦成RF:?再覆盖ON；不改变v4状态判定。
state = base.state(crev=1, snapshotok=1, csysvalid=1, croute=2, coutkhz=5137000,
                   crfpmag=30000, crfvalid=1, crfmode=4, crfphase=4)
check(texts(render(state)).count('RF: ON') == 1)
check(render(state) == [])
state['stale'] = 16
trace = render(state)
check('RF: ?' in texts(trace) and 'RF: ON' not in texts(trace))

# 像素模型核对差分结果与同版本强制完整重绘的最终结果。
# 仅验证图形命令的等价性，不把PC像素模型当作TJC物理刷新时序测量。
font = ImageFont.truetype('C:/Windows/Fonts/msyh.ttc', 16)


def color(number):
    return ((number >> 11) * 255 // 31, ((number >> 5) & 63) * 255 // 63,
            (number & 31) * 255 // 31)


def paint(canvas, trace):
    draw = ImageDraw.Draw(canvas)
    for item in trace:
        if isinstance(item, str):
            op, data = item.split(' ', 1)
            if op == 'cls':
                draw.rectangle((0,0,479,271), fill=color(int(data)))
            else:
                x,y,w,h,c = map(int,data.split(','))
                draw.rectangle((x,y,x+w-1,y+h-1),fill=color(c))
        else:
            _, (x,y,w,h,f,fc,bc,xc,yc,sta), text = item
            tile = Image.new('RGB',(w,h),color(bc))
            ImageDraw.Draw(tile).text((0,(h-16)//2),text,font=font,fill=color(fc))
            canvas.paste(tile,(x,y))


state = base.state()
canvas = Image.new('RGB',(480,272))
rng = random.Random(20261019)
for index in range(480):
    if index % 8 == 0:
        state.update(pg=rng.randrange(4), edit=0, pick=0)
    elif index % 8 == 1:
        state.update(edit=rng.randrange(1,len(sd.LIMITS)+1), entry=rng.randrange(65536))
    elif index % 8 == 4:
        state.update(edit=0,pick=1)
    elif index % 8 == 6:
        state.update(edit=0,pick=0)
    state.update(dm=rng.randrange(2), dc=rng.randrange(7), dfr=rng.randrange(2), di=rng.randrange(2),
                 ds=rng.randrange(2), dsx=rng.randrange(2), dsb=rng.randrange(16,34),
                 dsh=rng.randrange(65536), dsl=rng.randrange(65536), entry=rng.randrange(65536),
                 msg=rng.choice([0,1,2,3,4,5,6,8]), badkey=rng.randrange(2),
                 pending=rng.randrange(2), rev=rng.randrange(3), stale=rng.choice([0,4,15,16]),
                 crev=rng.randrange(3), snapshotok=rng.randrange(2),
                 csysvalid=rng.randrange(2), croute=rng.randrange(3), crfmode=rng.randrange(7),
                 coutkhz=rng.choice([0,5137000,6000000]), crfvalid=rng.randrange(2),
                 ctempmc=rng.randrange(40000), ctempok=rng.randrange(2),
                 crfphase=rng.randrange(6), crffault=rng.randrange(3),
                 crfpmag=rng.choice([0,10000,20000,30000,40000]),
                 crfexpect=rng.choice([0,30000,100000,100001]), crfmodel=rng.randrange(3),
                 crefsrc=rng.randrange(2), crefok=rng.randrange(2))
    expected = Image.new('RGB',(480,272))
    baseline = []
    full = dict(state, paintpg=255, paintedit=255, paintpick=255, oldrf=255)
    execute(DRAW,full,baseline)
    paint(expected,baseline)
    trace = render(state)
    paint(canvas,trace)
    delta = ImageChops.difference(canvas,expected).getbbox()
    if delta:
        raise AssertionError(f'incremental drawing differs: case={index} pg={state["pg"]} edit={state["edit"]} pick={state["pick"]} bbox={delta}')
    check(not index or cls_count(trace) == 0, 'later full-screen clear')
    check(True)

result = dict(passed=True, checks=checks, random_framebuffer_cases=480,
              user_hmi_sha256=hashlib.sha256((OLD/'P351_FinalRF_480x272_v7.HMI').read_bytes()).hexdigest(),
              protocol_events_byte_identical=False, power_protocol_upgrade='dpo*500 mdBm', hardware_tested=False,
              native_compilation_checked_by_this_test=False,
              covers=['zero redraw on unchanged clicks/revisions', 'no cls after physical init',
                      'keypad digits redraw value only', 'page/overlay cleanup', 'STOP priority',
                      'single write RF header', 'pixel model equals same-version full redraw',
                      'no TFS/clock/RX controls', 'v5/op13 fixed IF screen protocol'])
(ROOT/'verification/render_flicker.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
print(f'PASS {checks} rendering checks, 480 framebuffer transitions; not physical-screen measurement')
