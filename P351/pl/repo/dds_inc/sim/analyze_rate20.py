"""Independent continuous-time pulse integration versus current RTL.
Engineering waveform errors, not receiver EVM/BER or RF certification.
Uses published analytic Bessel/TG formulae, NOT DUT ROM values.
"""
from pathlib import Path
import json, hashlib, os
os.environ.setdefault('RATE20_INTEGRATED','1')
import numpy as np

ROOT=Path(os.environ.get('RATE20_OUTPUT','output/rate20_engineering_20260921'))
DATA=Path(os.environ.get('RATE20_DATA', str(ROOT/'sim/rate20.sim/sim_1/behav/xsim')))
FS=61_440_000
GRID=16384

def bessel_B(x):
    x=np.asarray(x)
    total=np.zeros_like(x,dtype=float)
    for c,a,w,theta in [(1.321405,-.995209,1.257106,126.547182),(4.049108,-1.370068,.410250,-78.794227)]:
        th=np.deg2rad(theta); b=2*np.pi*.7
        total+=2*c*np.exp(a*b*x)/(a*a+w*w)*((w*np.sin(th)-a*np.cos(th))*np.cos(w*b*x)+(a*np.sin(th)+w*np.cos(th))*np.sin(w*b*x))
    return total

# Reconstruct unity-DC fourth-order transfer function from Table 2-3 poles.
# This avoids relying on the table's angle/residue convention.
POLES=2*np.pi*.7*np.array([-.995209+1.257106j,-.995209-1.257106j,-1.370068+.410250j,-1.370068-.410250j])
NUM=np.prod(-POLES)
RES=np.array([NUM/(p*np.prod(p-np.delete(POLES,i))) for i,p in enumerate(POLES)])
def step(x):
    x=np.asarray(x)
    value=1+sum((r*np.exp(p*np.maximum(x,0))).real for r,p in zip(RES,POLES))
    return np.where(x>=0,value,0.)

def tg(x):
    t=np.asarray(x)-4;u=.7*1.25*t/2;den=1-4*u*u
    ratio=np.divide(np.cos(np.pi*u),den,out=np.full_like(t,np.pi/4),where=np.abs(den)>1e-10)
    win=np.where(np.abs(t)<=3,1.,np.where(np.abs(t)<=4,.5+.5*np.cos(np.pi*(np.abs(t)-3)),0.))
    return ratio*np.sinc(1.25*t/2)*win

grid=np.arange(8*GRID+1)/GRID
gpcm=step(grid)-step(grid-.5) # unity-DC Bessel, half-bit chip, infinite tail sampled to 8 Tb
gsoq=tg(grid);gsoq*=.5/np.trapezoid(gsoq,grid)
def integral(g):return np.r_[0,np.cumsum((g[:-1]+g[1:])/(2*GRID))]
QPCM=integral(gpcm);QSOQ=integral(gsoq)

def encode(bits,code):
    state=0;out=[]
    for b in bits:
        b=int(b)
        if code in (0,6):a=b;c=b
        elif code==1:a=state^b;c=a
        elif code==2:a=state^(1-b);c=a
        elif code==3:a=b;c=1-b
        elif code==4:a=state^(1-b);c=1-a
        else:a=state^b;c=1-a
        state=c;out.extend([2*a-1,2*c-1])
    return np.array(out)

def alpha(bits):
    prev=0;a1=a2=-1;out=[]
    for i,b in enumerate(bits):
        delta=int(b)^prev^(i%2==0);a=2*delta-1
        out.append((-1 if i%2==0 else 1)*a1*(a-a2)//2)
        prev=delta;a2=a1;a1=a
    return np.array(out)

def waveform_phase(u,weights,spacing,q,gain):
    # integrate all past frequency pulses; completed pulses use prefix sums.
    idx=np.floor(u/spacing).astype(int)
    ntaps=int(8/spacing)+1
    prefix=np.r_[0,np.cumsum(weights)]
    ans=prefix[np.clip(idx-ntaps+1,0,len(weights))]*q[-1]
    for k in range(ntaps):
        j=idx-k;good=(j>=0)&(j<len(weights))
        age=u-j*spacing
        ans+=np.where(good,weights[np.clip(j,0,len(weights)-1)]*np.interp(age,grid,q,left=0,right=q[-1]),0)
    return ans*gain

def shaped(u,weights,spacing,g):
    idx=np.floor(u/spacing).astype(int);ans=np.zeros_like(u)
    for k in range(int(8/spacing)+1):
        j=idx-k;good=(j>=0)&(j<len(weights));age=u-j*spacing
        ans+=np.where(good,weights[np.clip(j,0,len(weights)-1)]*np.interp(age,grid,g,left=0,right=0),0)
    return ans

def phase_errors(actual,expected):
    e=np.exp(2j*np.pi*(actual-expected));rotation=np.angle(np.mean(e))
    err=np.angle(e*np.exp(-1j*rotation))
    return dict(phase_rms_deg=float(np.rad2deg(np.sqrt(np.mean(err**2)))),
                phase_peak_deg=float(np.rad2deg(np.max(np.abs(err)))),
                waveform_rms_percent=float(100*np.sqrt(np.mean(np.abs(np.exp(1j*err)-1)**2))))

def psd(z):
    # Welch, same parameters for RTL and reference; no dependency on scipy.
    n=min(8192,len(z));starts=range(0,len(z)-n+1,n//2);w=np.hanning(n)
    power=np.mean([np.abs(np.fft.fftshift(np.fft.fft(z[i:i+n]*w)))**2 for i in starts],axis=0)
    return np.fft.fftshift(np.fft.fftfreq(n,1/FS)),power/power.sum()

def spectral(z,rate):
    f,p=psd(z);c=np.cumsum(p);a,b=np.searchsorted(c,[.005,.995])
    return dict(b99_hz=float(f[b]-f[a]),outside_Rb_power_dBc=float(10*np.log10(max(p[np.abs(f)>rate].sum(),1e-30))))

def analyze(rate,pattern):
    inp=np.loadtxt(DATA/f'input_{rate}_{pattern}.txt',dtype=np.int64)
    valid=inp[:,1].astype(bool);before=np.cumsum(valid)-valid
    raw=inp[valid,2];rand=inp[valid,3];fraction=inp[:,0]/2**32
    u=before-1+fraction
    mask=(u>24)&(u<len(raw)-16)
    indices=np.where(mask)[0]
    r=round(rate/FS*2**32)/2**32
    intervals=np.diff(np.where(valid)[0])
    # After acquisition the shared fractional phase must advance smoothly,
    # including 32-bit word and replay-frame boundaries.
    smooth=np.diff(u)[np.where(before[1:]>2)[0]]
    assert np.max(np.abs(smooth-r))<1e-9
    result=dict(rate_hz=rate,pattern=pattern,samples=len(inp),bits=int(valid.sum()),
        interval_min=int(intervals.min()),interval_max=int(intervals.max()),
        rate_word_error_ppm=(r*FS/rate-1)*1e6,modes={})
    for mode in ['pcm0','pcm1','pcm2','pcm3','pcm4','pcm5','pcm6','soq']:
        print('analyzing',rate,pattern,mode,flush=True)
        a=np.loadtxt(DATA/f'{mode}_{rate}_{pattern}.txt')
        ispcm=mode!='soq';code=int(mode[-1]) if ispcm else -1
        weights=encode(rand if code==6 else raw,code) if ispcm else alpha(rand)
        spacing=.5 if ispcm else 1.;q=QPCM if ispcm else QSOQ;g=gpcm if ispcm else gsoq
        gain=.35 if ispcm else .5
        actual=a[:,0]/2**32
        # Adjust only fixed propagation delay and constant carrier phase.
        # No frequency-offset or polynomial phase correction is allowed.
        center = (-(int(os.environ.get('RATE20_PCM_LATENCY','5')) if ispcm else 3)*r if os.environ.get('RATE20_INTEGRATED') else
                  (.03125 if ispcm else 0)-(4.5 if ispcm else 3.5)*r)
        sel=indices[::max(1,len(indices)//8192)]
        def objective(offset):
            ideal=waveform_phase(u[sel]+offset,weights,spacing,q,gain)
            return 1-abs(np.mean(np.exp(2j*np.pi*(actual[sel]-ideal))))
        low,high=center-r,center+r
        for _ in range(24):
            x=low+(high-low)*.382;y=low+(high-low)*.618
            if objective(x)<objective(y):high=y
            else:low=x
        offset=(low+high)/2
        ideal=waveform_phase(u+offset,weights,spacing,q,gain)
        m=phase_errors(actual[indices],ideal[indices]);m['reference_offset_Tb']=float(offset)
        phase_unwrap=np.unwrap(2*np.pi*(actual[indices]-ideal[indices]))/(2*np.pi)
        time=indices/FS
        slope,intercept=np.polyfit(time-time[0],phase_unwrap,1)
        m['fitted_frequency_bias_hz']=float(slope)
        m['after_frequency_bias_removal']=phase_errors(actual[indices],ideal[indices]+slope*(time-time[0])+intercept)
        chunksize=max(16,round(256/r));local=[]
        for st in range(0,len(indices)-chunksize+1,chunksize):
            ix=indices[st:st+chunksize]
            local.append(phase_errors(actual[ix],ideal[ix])['waveform_rms_percent'])
        m['local_256bit_waveform_rms_percent_median']=float(np.median(local)) if local else None
        m['local_256bit_waveform_rms_percent_worst']=float(max(local)) if local else None
        # Independent analytic pulse + same discrete integration isolates ROM/interpolation errors.
        freq=shaped(u+(.03125 if ispcm else 0),weights,spacing,g)
        discrete=np.zeros(len(inp));delay=5 if ispcm else 4
        discrete[delay:]=np.cumsum(freq[:len(inp)-delay])*r*gain
        m['same_sample_integrator']=phase_errors(actual[indices],discrete[indices])
        # Frequency-step mismatch against continuously integrated reference (no drift correction).
        dd=np.diff(actual-ideal);dd=(dd+.5)%1-.5
        m['frequency_step_error_rms_hz']=float(FS*np.sqrt(np.mean(dd[indices[1:]-1]**2)))
        increments=(np.diff(actual)+.5)%1-.5
        m['mean_output_baseband_frequency_hz']=float(FS*np.mean(increments[indices[1:]-1]))
        z=a[:,1]+1j*a[:,2];env=np.abs(z[indices]);m['envelope_rms_percent']=float(100*env.std()/env.mean())
        # LUT + amplitude + 12-bit quantization only, with three-cycle output pipeline.
        zideal=26214*32767/2**19*np.exp(2j*np.pi*np.roll(actual,3))
        m['iq_conversion_rms_percent']=float(100*np.sqrt(np.mean(np.abs(z[indices]-zideal[indices])**2))/np.mean(env))
        m['rtl_spectrum']=spectral(z[indices],rate)
        refz=np.exp(2j*np.pi*np.roll(ideal,3));m['reference_spectrum']=spectral(refz[indices],rate)
        m['b99_relative_error_percent']=100*(m['rtl_spectrum']['b99_hz']/m['reference_spectrum']['b99_hz']-1)
        result['modes'][mode]=m
        if rate==20000000 and pattern==0 and mode in ('pcm0','pcm3','soq'):
            f,p=psd(z[indices]);_,pr=psd(refz[indices])
            np.savez_compressed(ROOT/f'spectrum_{mode}.npz',f=f,rtl=p,reference=pr)
    return result

if __name__=='__main__':
    cases=[(100000,0),(5000000,0),(12500000,0),(15360000,0),(18000000,0),(20000000,0),(20000000,1),(20000000,2),(20000000,3)]
    # Verify analytic Bessel pulse against independently documented mid-bin examples.
    probe=(step((np.arange(8)+.5)/16)-step((np.arange(8)+.5)/16-.5))*32768
    print('Bessel first nodes',probe)
    assert np.max(np.abs(probe-np.array([2,140,822,2398,4956,8337,12229,16272])))<2
    results=[]
    for rate,pattern in cases:
        results.append(analyze(rate,pattern))
        # Engineering regression limits, not IRIG acceptance criteria.
        for mode, metrics in results[-1]['modes'].items():
            assert metrics['waveform_rms_percent'] < .1, (rate,pattern,mode,metrics)
            assert metrics['iq_conversion_rms_percent'] < .1, (rate,pattern,mode,metrics)
        (ROOT/'numeric_results.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
    manifest={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in [Path('repo/dds_inc/pcm_fm_modulator.v'),Path('repo/dds_inc/soqpsk_tg_modulator.v'),Path('repo/tx_bit_engine/src/tx_bit_engine_axis.v'),Path('repo/dds_inc/irig_randomizer.v'),Path('repo/dds_inc/sim/tb_rate20.sv'),Path(__file__)]}
    (ROOT/'source_sha256.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
