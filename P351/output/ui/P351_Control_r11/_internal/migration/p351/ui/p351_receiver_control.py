"""P351 V9独立RX2、七码型及ID型复帧；ACK不是RF验收结论。"""
from dataclasses import dataclass
import secrets
import socket
import struct
import time

CONTROL=struct.Struct('<4sIBBBBQIIiHHQBBH')
REPLY=struct.Struct('<4sI31I')
BIT_HEADER=struct.Struct('<4sBBHIHHHHIII')
LINE_CODES=('NRZ-L','NRZ-M','NRZ-S','Biphase-L','Biphase-M','Biphase-S','RNRZ-L')
ERRORS={1:'参数非法（码型/帧长/同步字/SFID）',2:'PL能力/ABI不匹配',3:'忙或参考时钟/LVDS未就绪（先停止I/Q监视；改RX LO/BW时需关闭TX）',
        4:'RX2调谐/PLL/采样率回读失败',5:'PL寄存器回读失败',6:'PL停止超时',7:'ADC缺样或帧失同步，需重新应用',8:'位流FIFO溢出，需重新应用',9:'码钟估计快照超时，数据无效'}

@dataclass(frozen=True)
class ReceiverConfig:
    center_hz:int=5137000000
    bit_rate:int=4000000
    bandwidth_hz:int=20000000
    gain_db:int=10
    mode:int=0
    line_code:int=0
    frame_bytes:int=512
    sync_word:int=0xFE6B2840
    enabled:bool=True
    sync_bits:int=32
    frame_count:int=1
    sfid_start:int=0

    def validate(self):
        fields=(self.center_hz,self.bit_rate,self.bandwidth_hz,self.gain_db,self.mode,self.line_code,self.frame_bytes,self.sync_word,
            self.sync_bits,self.frame_count,self.sfid_start)
        if any(type(v) is not int for v in fields):raise ValueError('接收参数须为整数Hz/bit/s/dB/字节')
        if not 70000000<=self.center_hz<=6000000000:raise ValueError('RX中心频率70–6000 MHz')
        if not 100000<=self.bit_rate<=20000000:raise ValueError('接收码率0.1–20 Mbit/s')
        if not max(200000,self.bit_rate)<=self.bandwidth_hz<=56000000:raise ValueError('RX滤波带宽0.2–56 MHz，且不小于码率；这不是B99')
        if not 0<=self.gain_db<=60:raise ValueError('RX手动增益0–60 dB；不能保护过大射频输入')
        if self.mode not in (0,1) or self.line_code not in (range(7) if self.mode==0 else (0,)):
            raise ValueError('PCM/FM支持七码型；SOQPSK固定标准差分/随机化')
        if not 8<=self.frame_bytes<=2048 or self.frame_bytes%4:raise ValueError('小帧长度8–2048字节，4字节对齐')
        if not 16<=self.sync_bits<=33 or not 0<=self.sync_word<(1<<self.sync_bits):raise ValueError('同步字须匹配16–33位长度')
        if not 1<=self.frame_count<=256 or self.sfid_start not in (0,1) or (self.frame_count>1 and self.frame_count+self.sfid_start>256):
            raise ValueError('ID型复帧1–256小帧；SFID首编号0/1，最后编号不能超过255')
        if self.frame_count*self.frame_bytes>16384:raise ValueError('复帧总长度不能超过本工程TX的16384字节重放缓存')
        if type(self.enabled) is not bool:raise ValueError('enabled须为布尔值')
        return self

    def packet(self,nonce):
        self.validate()
        return CONTROL.pack(b'RXC2',nonce,2,int(self.enabled),self.mode,self.line_code,
            self.center_hz,self.bit_rate,self.bandwidth_hz,self.gain_db,self.frame_bytes,self.frame_count,
            self.sync_word,self.sync_bits,self.sfid_start,0)

@dataclass(frozen=True)
class ReceiverSnapshot:
    words:tuple
    @property
    def configured(self):return bool(self.words[1])
    @property
    def status(self):return self.words[13]
    @property
    def locked(self):return self.configured and self.status&3==3 and not self.status&12 and not self.words[3]
    @property
    def raw_bits(self):return self.words[14]|self.words[20]<<32
    @property
    def frames(self):return self.words[15]|self.words[21]<<32
    @property
    def errors(self):return self.words[16]|self.words[22]<<32
    @property
    def payload_bits(self):return self.words[17]|self.words[23]<<32
    @property
    def pn_aligned(self):return self.locked and bool(self.status&64) and bool(self.words[30]&256)
    @property
    def ber(self):return self.errors/self.payload_bits if self.pn_aligned and self.payload_bits else None
    @property
    def clock_trim_ppm(self):
        if self.words[3] or not self.configured or not self.status&32:return None
        value=self.words[24];value=value-0x100000000 if value&0x80000000 else value
        return value*1000000/(1<<24)
    def config(self):
        w=self.words
        return ReceiverConfig(w[4]|w[5]<<32,w[6],w[7],w[8],w[9],w[10],w[11],w[12]|w[26]<<32,bool(w[1]),w[27],w[28],w[29]).validate()

def parse_snapshot(data,nonce):
    if len(data)!=REPLY.size:raise ValueError('RXA1长度不匹配V9')
    magic,token,*w=REPLY.unpack(data)
    if magic!=b'RXA1' or token!=nonce or w[0]!=3 or w[1]>1:raise ValueError('RXA1标识/请求号/版本错误')
    snapshot=ReceiverSnapshot(tuple(w));snapshot.config()
    if w[3] not in (0,*ERRORS):raise ValueError('未知接收错误码')
    if snapshot.errors>snapshot.payload_bits:raise ValueError('接收统计不合理')
    if snapshot.clock_trim_ppm is not None and abs(snapshot.clock_trim_ppm)>1001:raise ValueError('码钟估计超出PL微调限值')
    return snapshot

def _exchange(sock,endpoint,packet,magic,nonce,timeout=.5,attempts=3):
    for _ in range(attempts):
        sock.sendto(packet,endpoint) # 使用同一nonce重试；PS不会重启已成功的同一请求。
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            sock.settimeout(max(.001,deadline-time.monotonic()))
            try:data,sender=sock.recvfrom(2048)
            except socket.timeout:break
            if sender==endpoint and len(data)>=8 and struct.unpack_from('<4sI',data)==(magic,nonce):return data
    raise TimeoutError('RX2控制未收到匹配回执；不把sendto成功当作执行成功')

def query(board,local):
    nonce=secrets.randbits(32)
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as sock:
        sock.bind((local,0))
        return parse_snapshot(_exchange(sock,(board,40000),struct.pack('<4sI',b'RXQ1',nonce),b'RXA1',nonce),nonce)

def apply(board,local,config):
    import p351_board_config
    config.validate()
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as sock:
        sock.bind((local,0))
        # 先核实板型及PL DSP能力，不误写7020旧板寄存器。
        p351_board_config.query_on_socket(sock,(board,40000))
        nonce=secrets.randbits(32)
        reply=_exchange(sock,(board,40000),config.packet(nonce),b'RXAK',nonce)
        if len(reply)!=12:raise ValueError('RXAK长度错误')
        result=struct.unpack_from('<I',reply,8)[0]
        if result:raise ValueError(ERRORS.get(result,f'未知错误{result}'))
    state=query(board,local)
    if state.words[3] or (state.config()!=config if config.enabled else state.configured):
        raise ValueError('配置ACK后快照未确认，或随后发生采样/FIFO故障')
    return state

@dataclass(frozen=True)
class BitPacket:
    epoch:int
    sequence:int
    offset:int
    rate:int
    status:int
    payload:bytes
    def bits(self):
        return [(word>>shift)&1 for (word,) in struct.iter_unpack('<I',self.payload) for shift in range(31,-1,-1)]

def parse_bits(data):
    if len(data)<BIT_HEADER.size:raise ValueError('RXB1头截断')
    magic,ver,flags,size,epoch,seqlo,seqhi,count,reserved,offset,rate,status=BIT_HEADER.unpack_from(data)
    if magic!=b'RXB1' or ver!=1 or size!=len(data) or not 1<=count<=128 or size!=32+count*4 or reserved:
        raise ValueError('RXB1版本/长度错误')
    if flags!=status&15 or not 100000<=rate<=20000000:raise ValueError('RXB1状态/码率错误')
    return BitPacket(epoch,seqlo|seqhi<<16,offset,rate,status,data[32:])

class BitContinuity:
    """丢包/重复/重配/码率变化不能静默拼接为连续数据。"""
    def __init__(self):self.previous=None
    def validate(self,packet):
        old=self.previous
        if old and (packet.epoch!=old.epoch or packet.rate!=old.rate or
            packet.sequence!=(old.sequence+1)&0xffffffff or packet.offset!=(old.offset+len(old.payload)*8)&0xffffffff):
            raise ValueError('RXB1不连续；必须结束旧数据段，不能补零或静默拼接')
        if packet.status&12:raise ValueError('RXB1携带采样/FIFO故障')
        if packet.status&3!=3:raise ValueError('RXB1未确认启用及锁帧，不能当作有效连续位流')
        self.previous=packet
        return packet

    def accept(self,packet):
        self.validate(packet)
        return packet.bits()
