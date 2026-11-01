"""Independent IRIG-106-24 Eq. 2-19..21 checks against exported RTL scans.

Numerical tolerances below are engineering regression thresholds, not IRIG
RF acceptance limits. No peak-normalized FFT is used as an RF mask verdict.
"""
from pathlib import Path
import json
import sys
import numpy as np
from scipy.integrate import quad


def unscaled_pulse(t):
    t = np.asarray(t, dtype=float)
    rho, b = .70, 1.25
    u = rho*b*t/2
    den = 1-4*u*u
    ratio = np.divide(np.cos(np.pi*u), den, out=np.full_like(t, np.pi/4),
                      where=np.abs(den)>1e-10)
    window = np.where(np.abs(t)<=3, 1.,
                      np.where(np.abs(t)<=4, .5+.5*np.cos(np.pi*(np.abs(t)-3)), 0.))
    return ratio*np.sinc(b*t/2)*window


def spectrum_metrics(z, rate):
    z = np.asarray(z, dtype=complex)
    env = np.abs(z)
    spectrum = np.abs(np.fft.fftshift(np.fft.fft(z*np.hanning(len(z)))))**2
    cdf = np.cumsum(spectrum)/spectrum.sum()
    f = np.fft.fftshift(np.fft.fftfreq(len(z), 1/61_440_000))
    low, high = f[np.searchsorted(cdf, [.005,.995])]
    return dict(b99_hz=float(high-low), b99_over_rate=float((high-low)/rate),
                envelope_rms_percent=float(100*env.std()/env.mean()))


def check_phase_trace(directory, area):
    trace=np.loadtxt(directory/'phase_trace.txt')
    valid=trace[:,1].astype(bool)
    incoming=trace[valid,2].astype(int)
    # Equations 2-22, 2-16, 2-17 evaluated without reading DUT internal state.
    alph=[]
    previous=0
    a1=a2=-1
    for i,b in enumerate(incoming):
        delta=int(b)^previous^(i%2==0)
        a=2*delta-1
        alph.append((-1 if i%2==0 else 1)*a1*(a-a2)//2)
        previous=delta;a2=a1;a1=a
    accepted_before=np.cumsum(valid)-valid.astype(int)
    history=np.zeros((len(trace),8))
    alph=np.asarray(alph)
    for k in range(8):
        indices=accepted_before-1-k
        use=indices>=0
        history[use,k]=alph[indices[use]]
    ages=trace[:,0,None]/2**32+np.arange(8)[None,:]-4
    g=unscaled_pulse(ages)*.5/area
    expected_sum=np.sum(g*history,axis=1)
    # Analytic phase-response convolution: completed tails plus active pulses.
    # Current RTL evaluates integrated phase directly (three-cycle latency),
    # rather than a rectangular sum of sampled instantaneous frequencies.
    grid=np.arange(8*32768+1)/32768
    pulse=unscaled_pulse(grid-4)*.5/area
    q=np.r_[0,np.cumsum((pulse[:-1]+pulse[1:])/(2*32768))]
    completed=np.maximum(accepted_before-8,0).astype(int)
    prefix=np.r_[0,np.cumsum(alph)]
    direct=.5*(prefix[completed]*.5+
               np.sum(np.interp(ages+4,grid,q)*history,axis=1))
    phase=np.zeros(len(trace))
    phase[3:]=direct[:-3]
    phase_error=((trace[:,3]/2**32-phase+.5)%1-.5)*360
    shaped_error=trace[2:,4]-expected_sum[:-2]*32768
    assert np.max(np.abs(phase_error))<1.0, np.max(np.abs(phase_error))
    assert np.max(np.abs(shaped_error))<40, np.max(np.abs(shaped_error))
    return dict(max_phase_error_deg=float(np.max(np.abs(phase_error))),
                rms_phase_error_deg=float(np.sqrt(np.mean(phase_error**2))),
                max_shaped_sum_error_lsb=float(np.max(np.abs(shaped_error))))


def main(directory, report_path=None):
    directory=Path(directory)
    area=quad(lambda t: float(unscaled_pulse(t)), -4,4,
              points=[-3,3,-1/.875,1/.875], epsabs=1e-12)[0]
    pulse=np.loadtxt(directory/'pulse_scan.txt')
    ideal=unscaled_pulse(pulse[:,0]/4096-4)*.5/area*32768
    fixed_error=pulse[:,1]-ideal
    old_error=pulse[:,2]-ideal
    sine=np.loadtxt(directory/'sine_scan.txt')
    sine_ref=32767*np.sin(2*np.pi*sine[:,0]/65536)
    metrics=dict(
        pulse_area_fixed=float(pulse[:,1].sum()/4096/32768),
        pulse_area_baseline=float(pulse[:,2].sum()/4096/32768),
        pulse_max_error_lsb_fixed=float(np.max(np.abs(fixed_error))),
        pulse_max_error_lsb_baseline=float(np.max(np.abs(old_error))),
        sine_max_error_lsb_fixed=float(np.max(np.abs(sine[:,1]-sine_ref))),
        sine_max_error_lsb_baseline=float(np.max(np.abs(sine[:,2]-sine_ref))),
        phase_integrator=check_phase_trace(directory,area), captures={})
    assert metrics['pulse_max_error_lsb_fixed'] < 10, metrics
    assert abs(metrics['pulse_area_fixed']-.5)<.0001, metrics
    assert metrics['sine_max_error_lsb_fixed'] < 4, metrics
    for rate in (10000,5000000,12000000,12500000,15360000):
        iq=np.loadtxt(directory/f'soqpsk_compare_{rate}.txt')
        new=spectrum_metrics(iq[:,0]+1j*iq[:,1], rate)
        old=spectrum_metrics(iq[:,2]+1j*iq[:,3], rate)
        metrics['captures'][rate]=dict(fixed=new,baseline=old)
        assert new['envelope_rms_percent']<.1, (rate,new)
        if rate>=5000000:
            assert abs(new['b99_over_rate']-.78)<.078, (rate,new)
    # Never overwrite historical audit evidence when re-running a new build.
    report=Path(report_path) if report_path else directory/'numeric_results.json'
    report.write_text(json.dumps(metrics,indent=2),encoding='utf-8')
    print(json.dumps(metrics,indent=2))
    print('PASS independent SOQPSK pulse, phase-to-IQ and spectrum prechecks')
    return 0

if __name__=='__main__':
    raise SystemExit(main(sys.argv[1], sys.argv[2] if len(sys.argv)>2 else None))
