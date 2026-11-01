"""Explicit P351 reference semantics, with fresh identity before any REFC."""
import secrets
import struct
from tx_bit_protocol import exchange_ack
from p351_board_config import query_on_socket

def confirm_reference(sock,endpoint,config):
    magic,version,source,reserved,rate=struct.unpack('<4sBBHI',config)
    if magic!=b'REFC' or version!=1 or reserved or (source,rate) not in ((0,40000000),(1,10000000)):
        raise ValueError('P351仅支持内部40MHz或外部10MHz驯服40MHz')
    query_on_socket(sock,endpoint)  # Read-only identity; refuse a Z7/old P351 before writing REFC.
    nonce=secrets.randbits(32)
    packet=struct.pack('<4sBBHII',b'REFC',2,source,0,rate,nonce)
    result=exchange_ack(sock,endpoint,packet,b'RFAK',nonce,timeout=8.0,retries=2)
    if result:
        raise ValueError({1:'参考参数无效',2:'P351时钟接口未就绪',3:'参考事务忙/停止失败/用户STOP',
                          4:'参考不存在、频率或PLL锁定失败；保持停发'}.get(result,f'参考错误{result}'))
    return ('P351：外部10MHz存在并锁定，ADF4002驯服40MHz；AD9361 REFCLK仍为40MHz'
            if source else 'P351：内部40MHz自由运行，外部驯服关闭；AD9361 REFCLK=40MHz')
