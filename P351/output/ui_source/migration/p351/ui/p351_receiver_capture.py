"""只读采集恢复位流；不启动/重配RX、不触碰RF。坏段保留.part及明确错误元数据。"""
from array import array
from dataclasses import asdict
import json
import math
import os
from pathlib import Path
import socket
import sys
import time
from p351_receiver_control import BitContinuity,parse_bits,query


def capture(board,local,path,seconds=2.0):
    if not math.isfinite(seconds) or not 0.1<=seconds<=60:
        raise ValueError('采集时长0.1–60秒')
    output=Path(path);partial=Path(str(output)+'.part');metadata=Path(str(output)+'.json')
    if any(p.exists() for p in (output,partial,metadata)):
        raise FileExistsError('不覆盖已有位流或元数据，请选择新文件名')
    report={'format':'MSB-first packed bits, first bit is NOT necessarily a frame boundary',
        'complete':False,'rf_qualified':False,'board_ip':board,'local_ip':local,'packets':0,'bits':0,'requested_seconds':seconds}
    tracking=BitContinuity();started=None
    try:
        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as sock:
            sock.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,4*1024*1024)
            sock.bind((local,40002));sock.settimeout(.2)
            before=query(board,local)
            if not before.locked:raise ValueError('先在RX2页应用配置并确认数字锁帧，再采集位流')
            report.update(config=asdict(before.config()),epoch=before.words[2])
            deadline=time.monotonic()+3
            with partial.open('xb') as stream:
                while True:
                    now=time.monotonic()
                    if started is not None and now-started>=seconds:break
                    if started is None and now>=deadline:raise TimeoutError('未收到UDP40002位流；请检查接收任务、目标本机IP及防火墙')
                    try:data,sender=sock.recvfrom(2048)
                    except socket.timeout:continue
                    if sender!=(board,40002):continue
                    packet=parse_bits(data)
                    if packet.epoch!=before.words[2] or packet.rate!=before.config().bit_rate:
                        raise ValueError('位流epoch/码率与已回读任务不一致')
                    # validate不展开成Python bit列表，避免20M时无谓内存/CPU负担。
                    tracking.validate(packet)
                    if started is None:
                        started=now;report.update(first_sequence=packet.sequence,first_bit_offset=packet.offset)
                    words=array('I');words.frombytes(packet.payload)
                    if sys.byteorder=='little':words.byteswap()
                    stream.write(words.tobytes())
                    report['packets']+=1;report['bits']+=len(packet.payload)*8
                    report.update(last_sequence=packet.sequence,last_bit_offset=packet.offset)
            after=query(board,local)
            if not after.locked or after.words[2]!=before.words[2] or after.config()!=before.config():
                raise ValueError('采集结束时任务/锁帧状态改变，保留为不完整段')
            if not report['packets']:raise TimeoutError('没有有效位流包')
            report['elapsed_seconds']=time.monotonic()-started
            os.rename(partial,output) # Windows同目录不覆盖；成功段才获得正式.bin名称。
            report['complete']=True
    except Exception as exc:
        report['error']=str(exc)
        raise
    finally:
        # 即使出错，也不能把已写入的部分数据标成连续有效数据。
        with metadata.open('x',encoding='utf-8') as stream:
            json.dump(report,stream,ensure_ascii=False,indent=2)
    return report


def main():
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--board',default='192.168.10.2');parser.add_argument('--local',required=True)
    parser.add_argument('--seconds',type=float,default=2);parser.add_argument('--output',required=True)
    args=parser.parse_args()
    print(json.dumps(capture(args.board,args.local,args.output,args.seconds),ensure_ascii=False,indent=2))


if __name__=='__main__':main()
