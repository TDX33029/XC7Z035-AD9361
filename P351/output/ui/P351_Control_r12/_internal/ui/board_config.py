"""板端当前配置只读快照：不启动发射、不改变参考时钟、不改变I/Q采集收件人。"""
from __future__ import annotations
import secrets
import socket
import struct
import time
from dataclasses import dataclass

NAMES = ('version flags tx1 tx2 lo_low lo_high bw fs att1 att2 rate rate_word dev_word '
         'offset_word amplitude line rolloff randomizer differential ref_source ref_rate '
         'task bits repeat source framed seed invert sync_low sync_high sync_bits '
         'frame_bytes frame_count sfid_start raw_bits zero_mdBm target_mdBm boot_result').split()
REPLY = struct.Struct('<4sI38I')
TXCF = struct.Struct('<4sBBHQIII')
RF_CONFIG_VALID = 1024


def signed32(value):
    return value if value < 0x80000000 else value - 0x100000000


@dataclass(frozen=True)
class BoardConfig:
    words: tuple[int, ...]

    def get(self, name):
        return self.words[NAMES.index(name)]

    @property
    def lo_hz(self):
        return self.get('lo_low') | self.get('lo_high') << 32

    @property
    def rf_config_valid(self):
        """V3明确报告事务已校验；V1/V2不能据缓存推断校准成功。"""
        return self.get('version') >= 3 and bool(self.get('flags') & RF_CONFIG_VALID)

    @property
    def rf_channels(self):
        """物理RX/TX；V1是旧RX1/TX1固件，V2用板端标志明确报告。"""
        if self.get('version') == 1:
            return 1, 1
        flags = self.get('flags')
        return (2 if flags & 256 else 1), (2 if flags & 512 else 1)

    def ui_fields(self):
        """只映射已知字段；网络任务不伪造PN11/帧内容。功率为校准估算。"""
        modes = {0: '不发射', 3: 'SOQPSK-TG（PL）', 4: 'PCM/FM（PL）', 6: '正弦波/CW（PL）'}
        lines = ('NRZ-L', 'NRZ-M', 'NRZ-S', 'Biφ-L（Manchester）', 'Biφ-M', 'Biφ-S', 'RNRZ-L（IRIG随机化）')
        zero = signed32(self.get('zero_mdBm')) / 1000
        result = dict(tx1_mode=modes[self.get('tx1')], tx2_mode=modes[self.get('tx2')],
                      tx_lo_mhz=f'{self.lo_hz / 1e6:g}', tx_bw_mhz=f'{self.get("bw") / 1e6:g}',
                      tx1_zero_attenuation_dbm=f'{zero:g}', tx2_zero_attenuation_dbm=f'{zero:g}',
                      tx1_power_dbm=f'{zero - self.get("att1") / 1000:g}',
                      tx2_power_dbm=f'{zero - self.get("att2") / 1000:g}',
                      reference_source=('外部10 MHz参考' if self.get('ref_source') else '内部40 MHz晶振'),
                      line_code=lines[self.get('line')], soqpsk_differential=bool(self.get('differential')),
                      rolloff=('标准/无（PCM/FM Bessel）', 'RRC α=0.20', 'RRC α=0.35',
                               'RRC α=0.50', 'RRC α=0.75', 'RRC α=1.00')[self.get('rolloff')])
        if self.get('rate') or self.get('rate_word'):
            rate = self.get('rate') or round(self.get('rate_word') * self.get('fs') / 2**32)
            result['bit_rate'] = str(rate)
        if self.get('tx1') in (3, 4):
            result.update(pcm_deviation_hz=f'{self.get("dev_word") * self.get("fs") / 2**32:.9g}',
                          pcm_frequency_offset_hz=f'{signed32(self.get("offset_word")) * self.get("fs") / 2**32:.9g}',
                          pcm_amplitude=f'{self.get("amplitude") / 32767:.9g}',
                          profile=('SOQPSK-TG' if self.get('tx1') == 3 else 'PCM/FM 标准4阶Bessel（连续码率）'))
        if self.get('source'):
            repeat = self.get('repeat')
            result.update(task_id=str(self.get('task')), valid_bits=str(self.get('bits')),
                          repeat_mode=('无限循环' if repeat == 0xffffffff else '指定附加次数' if repeat else '单次'),
                          repeat_count=str(0 if repeat == 0xffffffff else repeat))
        if self.get('source') == 1:
            result.update(source_type='PN11', pn_seed=f'{self.get("seed"):X}',
                          pn_invert=bool(self.get('invert')), frame_sync_enabled=bool(self.get('framed')),
                          frame_sync_length=str(self.get('sync_bits')),
                          frame_sync_word=f'{self.get("sync_low") | self.get("sync_high") << 32:X}',
                          minor_frame_bytes=str(self.get('frame_bytes')), id_frame_count=str(self.get('frame_count')),
                          sfid_start=str(self.get('sfid_start')))
        return result


def parse_config(data: bytes, nonce: int) -> BoardConfig:
    if len(data) != REPLY.size:
        raise ValueError('配置报文长度不符')
    magic, received, *values = REPLY.unpack(data)
    if magic != b'CFGA' or received != nonce:
        raise ValueError('配置报文标识/请求号不符')
    c = BoardConfig(tuple(values))
    if c.get('version') not in (1, 2, 3):
        raise ValueError('配置协议版本不兼容')
    allowed_flags = {1: 255, 2: 1023, 3: 2047}[c.get('version')]
    if c.get('flags') & ~allowed_flags or c.get('tx1') not in (0, 3, 4, 6) or c.get('tx2') != 0:
        raise ValueError('配置模式不兼容')
    if c.rf_channels[0] != c.rf_channels[1]:
        raise ValueError('本1R1T配置不支持不同物理RX/TX通道')
    if not 70000000 <= c.lo_hz <= 6000000000 or not 200000 <= c.get('bw') <= 56000000 or c.get('fs') != 61440000:
        raise ValueError('频率/带宽/采样率无效')
    if any(c.get(key) > 89750 or c.get(key) % 250 for key in ('att1', 'att2')):
        raise ValueError('衰减读回无效')
    if c.get('line') > 6 or c.get('rolloff') > 5 or c.get('amplitude') > 32767 or c.get('source') > 2 or c.get('boot_result') > 3:
        raise ValueError('调制/任务字段无效')
    if c.get('randomizer') > 1 or c.get('differential') > 1 or (c.get('ref_source'), c.get('ref_rate')) not in ((0, 40000000), (1, 10000000)):
        raise ValueError('参考/编码字段无效')
    if c.get('source') == 1:
        if not c.get('flags') & 16 or not 1 <= c.get('seed') <= 2047 or c.get('invert') > 1 or c.get('framed') > 1:
            raise ValueError('本地PN11参数无效')
        if c.get('framed'):
            sync = c.get('sync_low') | c.get('sync_high') << 32
            count = c.get('frame_count')
            if (not 16 <= c.get('sync_bits') <= 33 or sync >= 1 << c.get('sync_bits') or
                not 1 <= count <= 256 or not 1 <= c.get('frame_bytes') <= 2048 or
                c.get('frame_bytes') * count > 16384 or c.get('sfid_start') > 1 or
                count > 1 and c.get('sfid_start') + count > 256 or
                c.get('frame_bytes') * 8 <= c.get('sync_bits') + (8 if count > 1 else 0) or
                c.get('bits') != c.get('frame_bytes') * count * 8):
                raise ValueError('组帧快照无效')
        elif not 1 <= c.get('raw_bits') <= 131072 or c.get('bits') != c.get('raw_bits'):
            raise ValueError('无帧PN长度无效')
    return c


def read_config(board: str, local: str, *, port=40000, timeout=0.4, attempts=3):
    nonce = secrets.randbits(32)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind((local, 0))
        for _ in range(attempts):
            sock.sendto(struct.pack('<4sI', b'CFGQ', nonce), (board, port))
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                sock.settimeout(max(0.001, deadline - time.monotonic()))
                try:
                    data, sender = sock.recvfrom(2048)
                except socket.timeout:
                    break
                if sender != (board, port) or len(data) < 8:
                    continue
                if struct.unpack_from('<4sI', data) != (b'CFGA', nonce):
                    continue
                return parse_config(data, nonce)
    raise TimeoutError('未读到当前配置：板卡未连接或ELF不支持CFGQ；没有修改板卡参数')


def confirm_rf_config(board: str, local: str, config: bytes, *, port=40000,
                      timeout=12.0, cancelled=None) -> BoardConfig:
    """只读确认TXCF实际已提交；大跨度校准结束前不得报告成功或发送BIT START。

    校准时长不是固定100ms。超时交给调用方发送门控STOP，不自动重发TXCF。
    此处确认软件配置与板端校验结果，不替代频谱仪测量。
    """
    if len(config) != TXCF.size:
        raise ValueError('TXCF报文长度不符')
    magic, mode1, mode2, shape, lo, bw, att1, att2 = TXCF.unpack(config)
    if magic != b'TXCF' or timeout <= 0:
        raise ValueError('TXCF确认参数无效')
    expected = dict(tx1=mode1, tx2=mode2, bw=bw, att1=att1, att2=att2,
                    line=shape & 15, rolloff=(shape >> 4) & 15)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if cancelled is not None and cancelled():
            raise InterruptedError('已取消换频，未继续START')
        try:
            current = read_config(board, local, port=port,
                                  timeout=min(0.4, max(0.001, deadline - time.monotonic())),
                                  attempts=1)
        except TimeoutError:
            continue
        if cancelled is not None and cancelled():
            raise InterruptedError('已取消换频，未继续START')
        if current.get('version') < 3:
            raise ValueError('ELF不支持换频完成校验，请更新PS程序；未继续START')
        if (current.rf_config_valid and current.get('flags') & 1 and
                not current.get('flags') & 128 and current.lo_hz == lo and
                all(current.get(key) == value for key, value in expected.items())):
            return current
        time.sleep(min(0.05, max(0, deadline - time.monotonic())))
    raise TimeoutError('换频未通过板端确认（校准未完成、配置拒绝或网络异常）；未继续START')
