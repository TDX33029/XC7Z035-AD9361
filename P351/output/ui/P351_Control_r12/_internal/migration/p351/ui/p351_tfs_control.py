"""P351 TFS adapter: confirm carrier identity before external writes."""
from shared_modules import load_shared
from p351_board_config import read_config

shared=load_shared('tfs_control')
original_exchange=shared.exchange

def exchange(board,local,op,value=0,**kwargs):
    if op not in (0,5):read_config(board,local)
    return original_exchange(board,local,op,value,**kwargs)

shared.exchange=exchange
control=shared.control
