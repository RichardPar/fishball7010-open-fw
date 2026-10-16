"""Synthetic spectra for classifier validation. Each frame is |X[k]| with X = sqrt(PSD[k]) * CN(0,1) (Rayleigh per bin), 4095 bins centred on the LO,
averaged by the classifier over ~25 frames per interval just like the live /waterfall feed. Signals are placed at absolute RF frequencies."""
import numpy as np, subprocess, sys, json
N=4095; C=2047
def axis(fs): return (np.arange(N)-C)*fs/4096.0
def shape_flat(f,f0,bw,roll=60e3):            # OFDM-like flat block; skirt falls ~35 dB over ~2*roll like a real filtered carrier
    d=np.abs(f-f0)-bw/2; return np.where(d<=0,1.0,np.exp(-(d/roll)**2*3.0))
def shape_gauss(f,f0,bw): return np.exp(-0.5*((f-f0)/(bw/2.355))**2)
def line(f,f0,hz):                            # CW line (Hann-ish leakage over 3 bins)
    d=np.abs(f-f0)/hz; return np.where(d<1.5,np.cos(np.pi*d/3)**2,0.0)
def make(fc,fs,sigs,rng,frames=25,noise_slope_db=0.0):
    f=axis(fs)+fc; hz=fs/4096.0
    base=np.ones(N)*10**(-noise_slope_db*(np.abs(f-fc)/(fs/2))/10)     # gentle receiver roll-off
    out=np.zeros((frames,N),dtype=np.float32)
    for i in range(frames):
        psd=base.copy()
        for s in sigs:
            if s.get('duty',1.0)<1.0 and rng.random()>s['duty']: continue
            if s['type']=='block': psd+=10**(s['snr']/10)*shape_flat(f,s['f'],s['bw'])
            elif s['type']=='gauss': psd+=10**(s['snr']/10)*shape_gauss(f,s['f'],s['bw'])
            elif s['type']=='cw': psd+=10**(s['snr']/10)*line(f,s['f'],hz)*3
        M=64                                   # Maia integrates many FFTs per output frame
        out[i]=np.sqrt(psd*rng.gamma(M,1.0/M,N))*1e3
    return out
def run(name,fc,fs,sigs,seed=1,frames=25,rf_bw=None):
    rng=np.random.default_rng(seed); fr=make(fc,fs,sigs,rng,frames)
    arr=np.concatenate([np.arange(frames,dtype=np.float32)[:,None],fr],1)
    arr.astype('<f4').tofile('synth/%s.bin'%name)
    r=subprocess.run(['./classifier-native','-t','synth/%s.bin'%name,'-F',str(fc),'-S',str(fs),'-B',str(rf_bw or fs*0.8),'-N',str(N+1)],capture_output=True,text=True)
    ems=[]
    for ln in r.stdout.splitlines():
        if ln.startswith('#'): continue
        t=ln.split(); ems.append(dict(wide=t[0]=='W',f=float(t[t.index('f')+1]),bw=float(t[t.index('bw')+1]),snr=float(t[t.index('snr')+1]),cls=t[t.index('cls')+1],conf=float(t[t.index('conf')+1])))
    return ems
def has(ems,cls,f,tol,bwr=None):
    for e in ems:
        if e['cls']==cls and abs(e['f']-f)<=tol and (bwr is None or bwr[0]<=e['bw']<=bwr[1]): return True
    return False
