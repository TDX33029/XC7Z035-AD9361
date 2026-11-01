"""Strict read-only final-RF status; never issue a control or fallback write."""
from dataclasses import dataclass
import ipaddress
import secrets
import socket
import struct
import time

NAMES = ('version flags phase fault final_khz target_mdbm expect_mdbm model '
         'if_khz if_target_mdbm if_att_mdb ext_halfdb lo2_khz tfs_status mode rate '
         'code framed fbytes fcount sync_low sync_high sync_bits ref_source '
         'ref_rate task sequence board_version board_flags source repeat reserved').split()
REPLY = struct.Struct('<4sI32I')
VALID, OWNED, QUALIFIED, COMMITTED, REFERENCE_OK, TFS_LOCK, PLAN_KNOWN = (1,2,4,8,16,32,64)
MANUAL_TEST, DIRECT_ACTIVE = 128,256
PHASES = ('空闲','等待外部参考','等待二级本振','配置中频','运行','已完成/安全停发')
FAULTS = ('无','用户STOP','参考失效','TFS/SPI异常','二级本振失锁','后级衰减异常',
          'IF/BIT启动失败','任务/配置所有权异常','事务超时','AD9361状态异常','PL硬件联锁停发')
LINES = ('NRZ-L','NRZ-M','NRZ-S','Biphase-L','Biphase-M','Biphase-S','RNRZ-L')


def signed(value):
    return value if value < 0x80000000 else value - 0x100000000


def frequency_valid(khz):
    return khz % 100 == 0 and (5091000 <= khz <= 5150000 or 5925000 <= khz <= 6125000)


@dataclass(frozen=True)
class FinalRFStatus:
    words: tuple

    def get(self, name):
        return self.words[NAMES.index(name)]

    @property
    def valid(self):
        return bool(self.get('flags') & VALID)

    def pc_text(self):
        """PC只显示AD9361真实配置，不使用屏幕整机功率/增益模型。"""
        w=self.get;flags=w('flags')
        state='ON（PS配置有效，非射频实测）' if flags&DIRECT_ACTIVE else '未确认开启'
        if flags&OWNED:state='整机任务占用；不能同时改RF，紧急STOP仍可用'
        mode={0:'不发射',3:'SOQPSK-TG',4:'PCM/FM',6:'正弦/CW'}[w('mode')]
        reference='外10MHz驯服40MHz' if w('ref_source') else '内部40MHz'
        return (f'AD9361 TX2 / TRX2：{state}\nLO {w("if_khz")/1000:.3f} MHz · '
                f'TX衰减 {w("if_att_mdb")/1000:g} dB · {mode} · {w("rate")/1e6:g} Mbit/s\n'
                f'{reference} · 配置回读非功率实测；功放增益仅供屏幕整机控制。')

    def text(self):
        w = self.get
        flags = w('flags')
        state = 'RF: ON（PS数字执行状态）' if self.valid else 'RF: 未确认开启 / 不宣称有射频输出'
        if flags & DIRECT_ACTIVE:
            state='AD9361直发: ON（PS配置有效，非整机最终RF/非实测）'
        if flags & MANUAL_TEST:
            state+=' · 人工RF联调版，未完成硬件验收'
        status = f'{state} · {PHASES[w("phase")]} · {FAULTS[w("fault")]}'
        if flags & PLAN_KNOWN:
            title = '已应用' if self.valid else '最后计划（未生效/已停发）'
            status += f'\n{title}：{w("final_khz")/1000:.3f} MHz · 目标 {signed(w("target_mdbm"))/1000:g} dBm'
            if w('model'):
                kind = '增益模型估算' if w('model') == 1 else '整链标定估算'
                status += f' · {kind} {signed(w("expect_mdbm"))/1000:g} dBm（非实测）'
        else:
            status += '\n尚无已接受的最终射频计划；最终频率/功率不取自PC草稿。'
        status += f'\nAD9361：{w("if_khz")/1000:.3f} MHz · 衰减 {w("if_att_mdb")/1000:g} dB'
        status += (f'；后级衰减 {w("ext_halfdb")/2:g} dB' if w('tfs_status') & 0x200 else '；后级衰减读回不可用')
        status += (f'；LO2请求寄存器 {w("lo2_khz")/1000:.3f} MHz（非测频）'
                   if w('tfs_status') & 0x100 else '；TFS外设未就绪')
        mode = {0:'不发射',3:'SOQPSK-TG',4:'PCM/FM',6:'正弦诊断'}[w('mode')]
        source = ('无已提交码源','PS本地PN11','PC下载')[w('source')]
        status += f'\n{mode} · {w("rate")/1e6:g} Mbit/s · {source}'
        if w('mode') == 4:
            status += ' · ' + LINES[w('code')]
        if w('framed'):
            sync = (w('sync_high') << 32) | w('sync_low')
            status += f' · 同步 {sync:X}/{w("sync_bits")} bit · {w("fbytes")} byte × {w("fcount")}'
        else:
            status += ' · 不加帧同步'
        status += (' · 外10M ' + ('已取得数字资格' if flags & REFERENCE_OK else '未就绪')
                   if w('ref_source') else ' · 内部40M基准（数字状态）')
        return status


def parse_status(data, nonce, *, pc_only=False):
    if len(data) != REPLY.size:
        raise ValueError('最终RF状态长度不符，需配套新版PS')
    magic, token, *words = REPLY.unpack(data)
    if magic != b'FRFA' or token != nonce:
        raise ValueError('最终RF状态标识/请求号不符')
    w = dict(zip(NAMES, words))
    if w['version'] != 1 or w['reserved'] or w['flags'] & ~511:
        raise ValueError('最终RF状态版本/保留字段不符')
    if w['board_version'] != 9 or w['board_flags'] & 0xB00 != 0xB00 or w['board_flags'] & ~131071:
        raise ValueError('不是配套的P351 V9 / TX2-RX2状态')
    if pc_only:
        # PC直发与屏幕模型解耦：不验证/使用IF功率目标、功放增益模型、
        # 最终RF频段、LO2或外部衰减；只核对实际显示的AD9361字段。
        if (w['mode'] not in (0,3,4,6) or not 70000<=w['if_khz']<=6000000 or
                w['if_att_mdb']>89750 or w['if_att_mdb']%250 or w['rate']>20000000 or
                (w['ref_source'],w['ref_rate']) not in ((0,40000000),(1,10000000))):
            raise ValueError('AD9361直发状态参数越界')
        if w['flags']&DIRECT_ACTIVE and (w['flags']&(VALID|OWNED) or
                not w['flags']&QUALIFIED or w['mode'] not in (3,4,6) or
                not w['board_flags']&1024 or not w['board_flags']&1):
            raise ValueError('AD9361直发状态字段矛盾')
        return FinalRFStatus(tuple(words))
    if w['phase'] > 5 or w['fault'] > 10 or w['model'] > 2:
        raise ValueError('最终RF状态阶段/故障/标定类型无效')
    if (w['mode'] not in (0,3,4,6) or w['rate'] > 20000000 or w['code'] > 6 or
            w['framed'] > 1 or w['if_khz'] > 6000000 or w['if_att_mdb'] > 89750 or
            w['if_att_mdb'] % 250 or w['ext_halfdb'] > 126 or w['source'] > 2 or
            w['ref_source'] > 1 or w['fbytes'] > 2048 or w['fcount'] > 256 or
            w['sync_high'] > 1 or w['sync_bits'] not in (0,*range(16,34))):
        raise ValueError('最终RF状态参数越界')
    known = bool(w['flags'] & PLAN_KNOWN)
    if known and (not frequency_valid(w['final_khz']) or
                  not -40000 <= signed(w['target_mdbm']) <= -10000 or
                  signed(w['target_mdbm']) % 500 or not w['sequence']):
        raise ValueError('最终RF计划参数无效')
    if not known and any(w[n] for n in ('final_khz','target_mdbm','expect_mdbm','model','sequence')):
        raise ValueError('无最终计划时不能携带有效功率目标')
    if signed(w['if_target_mdbm']) != -30000:
        raise ValueError('TRX2固定目标应为-30dBm，而非最终射频功率')
    if w['model'] and not signed(w['target_mdbm'])-500 < signed(w['expect_mdbm']) <= signed(w['target_mdbm']):
        raise ValueError('功率估算与目标/0.5dB量化不一致')
    if w['framed'] and (not w['sync_bits'] or not w['fcount'] or
                        w['fbytes']*8 <= w['sync_bits']):
        raise ValueError('有帧同步时的帧格式无效')
    if w['flags'] & VALID:
        required = VALID | OWNED | QUALIFIED | COMMITTED | REFERENCE_OK | TFS_LOCK | PLAN_KNOWN
        if ((w['flags'] & ~MANUAL_TEST) != required or w['phase'] != 4 or w['fault'] or not w['model'] or
                w['if_khz'] != 70000 or w['lo2_khz'] != w['final_khz'] - 1500000 or
                not w['board_flags'] & 1024 or w['board_flags'] & (64|128) or
                w['ref_source'] != 1 or w['ref_rate'] != 10000000 or w['mode'] not in (3,4) or
                not 100000 <= w['rate'] <= 20000000):
            raise ValueError('RF ON字段相互矛盾，拒绝沿用旧ON')
    if w['flags'] & DIRECT_ACTIVE:
        if (w['flags'] & (VALID|OWNED) or not w['flags'] & QUALIFIED or w['mode'] not in (3,4,6) or
                not w['board_flags'] & 1024 or not w['board_flags'] & 1):
            raise ValueError('AD9361直发状态字段矛盾')
    return FinalRFStatus(tuple(words))


def read_status(board, local, *, timeout=.4, attempts=2, pc_only=False):
    if not 0 < timeout <= 2 or not 1 <= attempts <= 3:
        raise ValueError('最终RF查询重试参数无效')
    for text in (board, local):
        address = ipaddress.IPv4Address(text)
        if address.is_unspecified or address.is_multicast or address.is_loopback or int(address) >> 24 >= 224:
            raise ValueError('最终RF查询需要有效单播地址')
    if board == local:
        raise ValueError('板卡和本机不能同地址')
    endpoint = (board, 40000)
    nonce = secrets.randbits(32)
    payload = struct.pack('<4sI', b'FRFQ', nonce)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind((local, 0))
        for _ in range(attempts):
            sock.sendto(payload, endpoint)
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                sock.settimeout(max(.001, deadline-time.monotonic()))
                try:
                    data, sender = sock.recvfrom(2048)
                except socket.timeout:
                    break
                if sender != endpoint or len(data) < 8:
                    continue
                if struct.unpack_from('<4sI', data) != (b'FRFA', nonce):
                    continue
                return parse_status(data, nonce, pc_only=pc_only)
    raise TimeoutError('最终RF状态未响应；旧PS可能不支持FRFQ。未发送任何控制命令')


def read_pc_status(board,local,**kwargs):
    return read_status(board,local,pc_only=True,**kwargs)
