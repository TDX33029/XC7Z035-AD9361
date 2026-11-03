"""PC整机模式：与屏幕v6/op14共用PS事务；独立直发UI不使用本模块的功率模型。"""
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation
import ipaddress
import secrets
import socket
import struct
import time

import p351_board_config as board_config
import p351_final_rf as final_rf

CAPABILITY=0x00060E01
ERRORS={1:'参数错误',2:'另一个任务占用',3:'执行失败',5:'事务超时',6:'已安全停发',
        7:'序号/重发冲突',8:'硬件资格、标定或外设未就绪'}

def scaled(text,scale):
    try:value=Decimal(str(text))*scale
    except InvalidOperation as exc:raise ValueError('请输入有效数字') from exc
    if not value.is_finite() or value!=value.to_integral_value():
        raise ValueError('设置值精度超出允许步进')
    return int(value)

def crc16(data):
    crc=65535
    for b in data:
        crc^=b
        for _ in range(8):crc=(crc>>1)^(0xA001 if crc&1 else 0)
    return crc

@dataclass(frozen=True)
class SystemPlan:
    mode:int=0
    rate:int=4000000
    code:int=0
    framed:int=1
    frame_bytes:int=512
    frame_count:int=1
    infinite:int=1
    sync:int=0xFE6B2840
    sync_bits:int=32
    final_khz:int=5137000
    target_mdbm:int=-30000

    def arguments(self):
        if any(type(v) is not int for v in vars(self).values()):raise ValueError('参数必须为整数单位')
        if self.mode not in (0,1) or not 100000<=self.rate<=20000000:raise ValueError('调制/码率超出范围')
        if not 0<=self.code<=6 or self.framed not in (0,1) or self.infinite not in (0,1):raise ValueError('码型或选项无效')
        if not final_rf.frequency_valid(self.final_khz):raise ValueError('最终频率须在两工作段内，100kHz步进')
        if not -40000<=self.target_mdbm<=-10000 or self.target_mdbm%500:raise ValueError('最终功率-40..-10dBm，0.5dB步进')
        if not 1<=self.frame_bytes<=2048 or not 1<=self.frame_count<=256 or self.frame_bytes*self.frame_count>16384:
            raise ValueError('小帧/复帧超出16KiB容量')
        if not 16<=self.sync_bits<=33 or not 0<=self.sync<1<<self.sync_bits:raise ValueError('同步字长度/数值无效')
        if self.framed and self.frame_bytes*8<=self.sync_bits+(8 if self.frame_count>1 else 0):raise ValueError('帧中没有载荷')
        return [self.mode,self.rate//1000,self.code if self.mode==0 else 0,self.framed,
                self.frame_bytes,self.frame_count,self.infinite,self.sync&65535,(self.sync>>16)&65535,
                self.sync_bits+((self.sync>>32)&1)*256+(self.rate%1000)*1024,self.final_khz,-self.target_mdbm]

    def wire(self,sequence):
        if not 1<=sequence<=65535:raise ValueError('序号无效')
        prefix=b'\xa5\x5a\x06\x0e'+struct.pack('<13I',sequence,*self.arguments())
        return prefix+struct.pack('<I',crc16(prefix))+b'\xff'*3

    def matches(self,state):
        w=state.get
        sync=w('sync_low')+(w('sync_high')<<32)
        return (w('mode')==(4 if self.mode==0 else 3) and w('final_khz')==self.final_khz and final_rf.signed(w('target_mdbm'))==self.target_mdbm
                and w('rate')==self.rate and w('code')==(self.code if self.mode==0 else 0)
                and w('framed')==self.framed and w('fbytes')==self.frame_bytes
                and w('fcount')==self.frame_count and sync==self.sync and w('sync_bits')==self.sync_bits
                and w('repeat')==(0xffffffff if self.infinite else 0))

def exchange(sock,endpoint,payload,magic,nonce,timeout=.4):
    sock.sendto(payload,endpoint)
    until=time.monotonic()+timeout
    while time.monotonic()<until:
        sock.settimeout(max(.001,until-time.monotonic()))
        data,sender=sock.recvfrom(2048)
        if sender!=endpoint or len(data)!=12:continue
        got,token,result=struct.unpack('<4sII',data)
        if got==magic and token==nonce:return result
    raise TimeoutError('整机控制未取得匹配回执')

def apply_system(board,local,plan,*,timeout=17,progress=None):
    """不调用独立TXCF/TFS拼接整机动作；无新固件能力时明确拒绝。"""
    if not .5<=timeout<=30:raise ValueError('等待时间无效')
    wire=plan.wire(secrets.randbelow(65535)+1)
    sequence=struct.unpack_from('<I',wire,4)[0]
    for address in (board,local):
        a=ipaddress.IPv4Address(address)
        if a.is_unspecified or a.is_loopback or a.is_multicast or int(a)>>24>=224:raise ValueError('需要有效单播IPv4')
    if board==local:raise ValueError('板卡和本机地址不能相同')
    # 只核验V9身份/能力；发射许可由PS逐次判定。即使保护会拒绝，也把请求
    # 送达板端，让RDFA把用户请求值同步到另一侧，同时绝不发射。
    board_config.read_config(board,local)
    endpoint=(board,40000)
    nonce=secrets.randbits(32)
    request=b'FRCM'+struct.pack('<I',nonce)+wire
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as sock:
        sock.bind((local,0))
        capability=exchange(sock,endpoint,struct.pack('<4sI',b'FRCQ',nonce),b'FRCI',nonce)
        if capability!=CAPABILITY:raise ValueError('PS不支持配套精确码率整机事务；不降级直发')
        until=time.monotonic()+timeout
        while time.monotonic()<until:
            try:result=exchange(sock,endpoint,request,b'FRCA',nonce)
            except socket.timeout:
                continue # 仅同socket/nonce/原报文重发；不会启动第二个任务。
            if result==4:
                if progress:progress('PS已接收，正在检查参考/LO2/中频及码流；尚未确认RF输出')
                time.sleep(.15);continue
            if result:raise ValueError('整机未开启：'+ERRORS.get(result,f'未知错误{result}'))
            state=final_rf.read_status(board,local)
            if state.get('sequence')!=sequence or not plan.matches(state):
                raise ValueError('整机回读与本次请求不一致；RF状态未知，请查询或STOP')
            if state.valid or (not plan.infinite and state.get('phase')==5 and not state.get('fault')):
                return state
            raise ValueError('整机回读未确认运行/完成：'+state.text())
    raise TimeoutError('整机确认超时；不沿用旧ON，请查询或紧急STOP。PS有独立事务超时保护')
