"""P351 V9接收协议保护；保留38-word原发射字段。"""
from dataclasses import dataclass
import secrets
import socket
import struct
import time
from shared_modules import load_shared

shared=load_shared('board_config')
P351_FLAG=2048
QUALIFIED_FLAG=4096
RX_IQ_FIXED_FLAG=8192
SINGLE_SLOT_FLAG=16384
RX_DSP_FLAG=32768
RX_TIMING_FLAG=65536
legacy_parse=shared.parse_config

@dataclass(frozen=True)
class BoardConfig(shared.BoardConfig):
    @property
    def hardware_qualified(self):return bool(self.get('flags') & QUALIFIED_FLAG)

    def ui_fields(self):
        fields=super().ui_fields()
        # Old-board absolute-power calibration is not a P351 PA calibration.
        for name in ('tx1_power_dbm','tx2_power_dbm','tx1_zero_attenuation_dbm','tx2_zero_attenuation_dbm'):
            fields.pop(name,None)
        fields['tx1_attenuation_db']=f'{self.get("att1")/1000:g}'
        return fields

def parse_config(data,nonce):
    if len(data)!=shared.REPLY.size:raise ValueError('P351配置报文长度不符')
    magic,token,*words=shared.REPLY.unpack(data)
    if magic!=b'CFGA' or token!=nonce:raise ValueError('P351配置标识/请求号不符')
    if words[0]!=9 or not words[1]&P351_FLAG or words[1]&~131071:
        raise ValueError('不是匹配的P351 V9固件：拒绝使用旧板/旧版本配置')
    # TX身份确认不以RX解调/连续位同步功能为前提；RX启动入口单独检查。
    if not words[1]&SINGLE_SLOT_FLAG:
        raise ValueError('PL缺少单槽采集能力：请更新匹配的V9 XSA/位流')
    if not words[1]&RX_IQ_FIXED_FLAG:
        raise ValueError('PL缺少RX I/Q修复能力：请更新匹配的V5 XSA/位流')
    if words[1]&768!=768:raise ValueError('P351必须使用物理RX2/TX2的1R1T配置')
    # Validate all common fields independently using the unchanged shared V3 validator.
    compatible=list(words);compatible[0]=3;compatible[1]&=2047
    legacy_parse(shared.REPLY.pack(magic,token,*compatible),nonce)
    return BoardConfig(tuple(words))


def require_rx_capabilities(config,*,demod=True):
    flags=config.get('flags')
    needed=RX_IQ_FIXED_FLAG|SINGLE_SLOT_FLAG
    if demod:needed|=RX_DSP_FLAG|RX_TIMING_FLAG
    if flags&needed!=needed:
        raise ValueError('当前固件缺少所选RX功能能力；TX直发不受此项限制')
    return config

# The separately loaded module has isolated globals. The original UI file/module
# is not modified, and its normal Z7 startup still uses its own V1-V3 parser.
shared.parse_config=parse_config
read_config=shared.read_config
confirm_rf_config=shared.confirm_rf_config

def query_on_socket(sock,endpoint,*,timeout=.4,attempts=3):
    nonce=secrets.randbits(32)
    packet=struct.pack('<4sI',b'CFGQ',nonce)
    for _ in range(attempts):
        sock.sendto(packet,endpoint)
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            sock.settimeout(max(.001,deadline-time.monotonic()))
            try:data,sender=sock.recvfrom(2048)
            except socket.timeout:break
            if sender!=endpoint or len(data)<8:continue
            if struct.unpack_from('<4sI',data)!=(b'CFGA',nonce):continue
            return parse_config(data,nonce)
    raise TimeoutError('未取得P351 V9只读身份确认；未下发参考控制')
