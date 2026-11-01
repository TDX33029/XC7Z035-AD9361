"""屏幕草稿镜像客户端：PC 上位机屏幕为主同步的 UDP 通道。

PS 固件（2026-11-02 起）通过 TJC get 指令轮询屏幕草稿变量并维护镜像；
PC 用 SDRQ 读取整表、SDRW 写单变量。屏幕 tm0 每 250ms 重绘，草稿写回
后屏幕显示在 0.25s 内更新（无需触屏）。
"""
import secrets
import socket

PORT = 40000
VARS = ['df', 'dr', 'dpo', 'dm', 'dc', 'dfr', 'db', 'dn', 'di',
        'dsl', 'dsh', 'dsb', 'dsx', 'edit']


def read_mirror(board_ip, local_ip, timeout=1.0):
    """SDRQ → {'vars':[14], 'edit':0/1}；无应答/格式错返回 None。"""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    try:
        sock.bind((local_ip, 0)) if local_ip else sock.bind(('', 0))
        nonce = secrets.randbits(32)
        sock.sendto(b"SDRQ" + nonce.to_bytes(4, 'little'), (board_ip, PORT))
        data, _ = sock.recvfrom(160)
        if len(data) < 58 or data[:4] != b"SDRS":
            return None
        if int.from_bytes(data[4:8], 'little') != nonce:
            return None
        count = int.from_bytes(data[50:54], 'little')
        if count != len(VARS):
            return None
        vals = [int.from_bytes(data[54 + 4 * i:58 + 4 * i], 'little', signed=True)
                for i in range(count)]
        edit = int.from_bytes(data[54 + 4 * count:58 + 4 * count], 'little')
        return {'vars': vals, 'edit': edit}
    except (OSError, socket.timeout):
        return None
    finally:
        sock.close()


def write_draft(board_ip, local_ip, idx, value, timeout=1.0):
    """SDRW 写单变量：0=成功，1=序号无效，2=屏幕编辑中，3=未入队，None=无应答。"""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    try:
        sock.bind((local_ip, 0)) if local_ip else sock.bind(('', 0))
        nonce = secrets.randbits(32)
        payload = (b"SDRW" + nonce.to_bytes(4, 'little') +
                   idx.to_bytes(4, 'little') +
                   (value & 0xFFFFFFFF).to_bytes(4, 'little', signed=True))
        sock.sendto(payload, (board_ip, PORT))
        deadline = timeout
        while deadline > 0:
            try:
                data, _ = sock.recvfrom(64)
            except socket.timeout:
                return None
            if len(data) >= 9 and data[:4] == b"SDWK" and \
                    int.from_bytes(data[4:8], 'little') == nonce:
                return data[8]
            deadline -= timeout
        return None
    except (OSError, socket.timeout):
        return None
    finally:
        sock.close()
