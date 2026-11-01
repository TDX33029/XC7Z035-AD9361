"""Bounded UDP control of the PL TFS SPI peripheral; no RF-output inference."""
from decimal import Decimal, InvalidOperation
import secrets
import socket
import struct
import time

REQUEST = struct.Struct('<4sIBBHQI')
REPLY = struct.Struct('<4sII')
SNAPSHOT = struct.Struct('<4sIIIIII')

def frequency_units(mhz: str, fine: bool = False) -> int:
    try:
        value = Decimal(mhz)
        if not value.is_finite() or not Decimal(50) <= value <= Decimal(22600):
            raise ValueError('TFS频综频率范围为50–22600 MHz')
        units = value * (10_000_000_000 if fine else 10_000_000)
        if units != units.to_integral_value():
            raise ValueError('频率精度超过所选步进：0.1 mHz' if fine else '频率精度超过所选步进：0.1 Hz')
        return int(units)
    except InvalidOperation as exc:
        raise ValueError('请输入有效频率（MHz）') from exc

def make_packet(op: int, value: int = 0, nonce: int = 0, divider: int = 5) -> bytes:
    if op not in (0, 1, 2, 3, 4, 5, 0x81, 0x82, 0x86) or not 5 <= divider <= 65535:
        raise ValueError('无效TFS命令或SPI时钟分频')
    if op == 1 and not 500_000_000_000 <= value <= 226_000_000_000_000:
        raise ValueError('0.1 mHz频率字越界')
    if op == 2 and not 500_000_000 <= value <= 226_000_000_000:
        raise ValueError('0.1 Hz频率字越界')
    if op == 4 and not 0 <= value <= 126:
        raise ValueError('外部衰减范围0–63 dB，步进0.5 dB')
    return REQUEST.pack(b'TFS1', nonce, 1, op, divider, value, 0)

def status_text(status: int) -> str:
    error = (status >> 16) & 255
    if error:
        raise RuntimeError({1: '当前XSA/ELF未包含TFS控制器，请更新硬件与软件',
                            2: '板端拒绝参数', 3: 'SPI正忙，请稍后重试',
                            4: '当前PL没有七线衰减控制，请升级TFS IP并重新生成BIT',
                            5: '衰减输出寄存器核验失败'}.get(error, f'TFS错误{error}'))
    if not status & 256:
        raise RuntimeError('未确认TFS控制器存在')
    phase = 'SPI传输中' if status & 1 else ('SPI传输完成' if status & 2 else 'SPI空闲')
    if status & 4:
        phase += '；存在错误或事务已中止'
    return phase + ('；LD=1（锁定指示）' if status & 8 else '；LD=0（未锁定或未接线）')

def exchange(board: str, local: str, op: int, value: int = 0, *, timeout: float = .6,
             attempts: int = 3, snapshot: bool = False):
    nonce = secrets.randbits(32)
    packet = make_packet(op, value, nonce)
    if snapshot:
        packet = b'TFS2' + packet[4:]
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind((local, 0))
        for _ in range(attempts):
            sock.sendto(packet, (board, 40000))
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                sock.settimeout(max(.001, deadline - time.monotonic()))
                try:
                    data, endpoint = sock.recvfrom(2048)
                except socket.timeout:
                    break
                layout = SNAPSHOT if snapshot else REPLY
                if endpoint != (board, 40000) or len(data) != layout.size:
                    continue
                magic, token, status, *words = layout.unpack(data)
                if magic == (b'TFA2' if snapshot else b'TFAK') and token == nonce:
                    status_text(status)  # validate hardware/protocol errors
                    return (status, *words) if snapshot else status
    raise TimeoutError('未收到TFS确认；不能据此认定频率已设置。请检查新ELF与网络。')

def apply_and_observe(board: str, local: str, op: int, value: int = 0) -> str:
    status = exchange(board, local, op, value)
    # Bound polling to the SPI transaction, never wait indefinitely for LD.
    for _ in range(10):
        if not status & 1:
            break
        time.sleep(.01)
        status = exchange(board, local, 0, attempts=1)
    if status & 1:
        raise TimeoutError('PL SPI仍忙；请使用中止SPI，检查PL时钟/复位')
    return status_text(status) + '；LD不等于输出频率实测确认'


def transaction(board, local, op, value=0):
    """Read a coherent PS snapshot after a bounded SPI transaction."""
    result = exchange(board, local, op, value, snapshot=True)
    for _ in range(10):
        if not result[0] & 1:
            break
        time.sleep(.01)
        result = exchange(board, local, 0, snapshot=True, attempts=1)
    status, low, mid, high, opcode = result
    if status & 1:
        raise TimeoutError('SPI事务超时，请中止后重试')
    if status & 4:
        raise RuntimeError('PL报告SPI错误或事务被中止')
    if op not in (0, 3) and (opcode != op or not status & 2):
        raise RuntimeError('SPI结果与请求不一致，可能存在其他控制端操作')
    return status, low | (mid << 32) | (high << 64)


def read_frequency(board, local, fine=False):
    _, raw = transaction(board, local, 0x81 if fine else 0x82)
    bits = 64 if fine else 48
    value = (raw >> 16) & ((1 << bits) - 1)
    minimum, maximum = ((500_000_000_000, 226_000_000_000_000) if fine
                        else (500_000_000, 226_000_000_000))
    if not minimum <= value <= maximum:
        raise RuntimeError('频率读回无效：检查MISO接线、读功能支持及采样时序')
    return value


def read_lock(board, local):
    status, raw = transaction(board, local, 0x86)
    byte = raw & 255
    if byte & 0xfe:
        raise RuntimeError('锁定返回的保留位非零，尚不能确认读回有效')
    return bool(byte & 1), bool(status & 8)


def control(board, local, action, fine=False, value=0, verify=False, lock_timeout=6.0):
    if action in ('atten_set', 'atten_get', 'atten_max'):
        target = 126 if action == 'atten_max' else value
        status = exchange(board, local, 5 if action == 'atten_get' else 4, target)
        if not status & 0x200:
            raise RuntimeError('当前PL没有七线衰减控制，请更新BIT')
        levels = (status >> 24) & 127
        actual = attenuation_from_levels(levels)
        if action != 'atten_get' and actual != target:
            raise RuntimeError('衰减输出寄存器与请求不一致')
        pins = ' '.join(str((levels >> i) & 1) for i in range(7))
        return f'外部衰减设置：{actual/2:g} dB；V1→V7：{pins}；已核对PL寄存器，实际衰减需仪表测量'
    if action == 'abort':
        return apply_and_observe(board, local, 3)
    if action == 'status':
        return status_text(exchange(board, local, 0))
    if action == 'read':
        actual = read_frequency(board, local, fine)
        mhz = Decimal(actual) / (10_000_000_000 if fine else 10_000_000)
        return f'寄存器读回：{mhz:f} MHz；实际射频频率需仪表核验'
    if action == 'lock':
        locked, ld = read_lock(board, local)
        return f'SPI锁定位={int(locked)}；LD={int(ld)}（保留位按零校验）'
    if action != 'set':
        raise ValueError('未知频综操作')
    if not .1 <= lock_timeout <= 30:
        raise ValueError('锁定等待时间须在0.1–30秒之间')
    transaction(board, local, 1 if fine else 2, value)
    verified = False
    if verify:
        actual = read_frequency(board, local, fine)
        if actual != value:
            raise RuntimeError(f'频率已发送，但读回不一致：设置字={value}，读回字={actual}')
        verified = True
    deadline = time.monotonic() + lock_timeout
    while time.monotonic() < deadline:
        if verify:
            locked, ld = read_lock(board, local)
            ready = locked and ld
        else:
            ready = bool(exchange(board, local, 0, timeout=.2, attempts=1) & 8)
        if ready:
            return ('频率已发送；读回一致；SPI与LD均锁定' if verified else
                    '频率已发送；LD已锁定；未执行寄存器读回') + '；输出频率仍需仪表核验'
        time.sleep(.05)
    raise TimeoutError(f'频率已发送，但在{lock_timeout:g}秒内未确认锁定；未自动关闭射频')


def attenuation_units(db: str) -> int:
    try:
        value = Decimal(db) * 2
        if not value.is_finite() or not 0 <= value <= 126 or value != value.to_integral_value():
            raise ValueError('外部衰减须为0–63 dB，步进0.5 dB')
        return int(value)
    except InvalidOperation as exc:
        raise ValueError('请输入有效衰减值（dB）') from exc


def attenuation_from_levels(levels: int) -> int:
    """Return half-dB units from active-low V1..V7, bit0=V1."""
    if not 0 <= levels <= 127:
        raise ValueError('七线电平越界')
    return ((~levels) & 63) + (63 if not levels & 64 else 0)
