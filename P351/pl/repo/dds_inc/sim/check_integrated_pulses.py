"""Verify checked-in Q24 phase-response tables against analytic pulses.

Requires NumPy. Tables are phase integrals, not sampled frequency impulses.
Thresholds are engineering regression limits, NOT IRIG RF acceptance limits.
"""
from pathlib import Path
import re
import numpy as np

HERE = Path(__file__).resolve().parent.parent
N = 32768
t = np.arange(8*N+1)/N
p = 2*np.pi*.7*np.array([-.995209+1.257106j, -.995209-1.257106j,
                        -1.370068+.410250j, -1.370068-.410250j])
r = np.array([np.prod(-p)/(x*np.prod(x-np.delete(p,i))) for i,x in enumerate(p)])

def step(x):
    return np.where(x>=0, 1+sum((a*np.exp(b*np.maximum(x,0))).real for a,b in zip(r,p)),0)

pcm = step(t)-step(t-.5)
x=t-4; u=.7*1.25*x/2; den=1-4*u*u
ratio=np.divide(np.cos(np.pi*u),den,out=np.full_like(t,np.pi/4),where=np.abs(den)>1e-10)
win=np.where(abs(x)<=3,1.,np.where(abs(x)<=4,.5+.5*np.cos(np.pi*(abs(x)-3)),0.))
soq=ratio*np.sinc(1.25*x/2)*win
soq*=.5/np.trapezoid(soq,t)
for name,g,span in [('pcm_fm_modulator',pcm,4),('soqpsk_tg_modulator',soq,8)]:
    q=np.r_[0,np.cumsum((g[:-1]+g[1:])/(2*N))]
    nodes=np.interp(np.arange(span*32+1)/32,t,q)
    nodes*=.5/nodes[-1]
    source=(HERE/f'{name}.v').read_text()
    section=source.split('function signed [25:0] integrated_pulse;')[1].split('endfunction')[0]
    entries=re.findall(r"9'd(\d+): integrated_pulse = (-?)26'sd(\d+);",section)
    assert len(entries)==len(nodes)
    values=np.array([(-1 if sign else 1)*int(v) for _,sign,v in entries])
    assert [int(i) for i,_,_ in entries]==list(range(len(nodes)))
    error=max(abs(values-nodes*2**24))
    assert error<.51,(name,error)
    assert values[0]==0 and values[-1]==2**23
    print(f'PASS {name}: {len(nodes)} nodes, max error {error:.4f} Q24 LSB, exact half-area endpoint')
