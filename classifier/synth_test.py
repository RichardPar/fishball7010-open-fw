import numpy as np, sys
from synth import *
res=[]
def check(name,ok,detail=''): res.append(ok); print(('PASS ' if ok else 'FAIL ')+name+('  '+detail if detail else ''))
def show(ems): return '; '.join('%s%.3fM/%.0fk/%s/%.2f'%('W' if e['wide'] else 'N',e['f']/1e6,e['bw']/1e3,e['cls'],e['conf']) for e in ems if e['cls']!='spur')[:300]
# 1 FM stations on 100 kHz raster (several), 25 dB
fm=[dict(type='gauss',f=f,bw=120e3,snr=s) for f,s in [(97.1e6,25),(97.9e6,30),(98.3e6,22),(99.5e6,28)]]
e=run('fm',98e6,10e6,fm); check('FM: 4 stations',all(has(e,'fm',s['f'],15e3) for s in fm) and sum(x['cls']=='fm' for x in e)==4,show(e))
# 2 GSM carriers on 200 kHz raster at 940 MHz
gs=[dict(type='gauss',f=935.0e6+k*0.2e6,bw=100e3,snr=28) for k in (5,11,12,40)]
e=run('gsm',940e6,20e6,gs); check('GSM: 4 carriers',all(has(e,'gsm',s['f'],15e3) for s in gs),show(e))
# 3 LTE 10 MHz and 5 MHz in band 800
lt=[dict(type='block',f=806e6,bw=9.0e6,snr=18),dict(type='block',f=847e6,bw=4.5e6,snr=20)]
e=run('lte',826e6,61.44e6,lt,rf_bw=56e6); check('LTE 10+5 MHz',has(e,'lte',806e6,300e3,(8.3e6,9.6e6)) and has(e,'lte',847e6,300e3,(4.0e6,5.0e6)),show(e))
# 4 DVB-T 8 MHz at 570 MHz
e=run('dvbt',570e6,30.72e6,[dict(type='block',f=570e6,bw=7.6e6,snr=18)]); check('DVB-T 8MHz',has(e,'dvbt',570e6,300e3),show(e))
# 5 DAB 1.5 MHz at 225
e=run('dab',225e6,10e6,[dict(type='block',f=225.648e6,bw=1.5e6,snr=18)]); check('DAB ensemble',has(e,'dab',225.648e6,200e3),show(e))
# 6 LoRa-like bursts 125 kHz, 10 % duty at 868.1
e=run('lora',868e6,10e6,[dict(type='block',f=868.1e6,bw=125e3,snr=25,duty=0.12)],frames=50); check('LoRa burst 868.1',has(e,'burst',868.1e6,40e3),show(e))
# 7 Wi-Fi 20 MHz bursty at 2437
e=run('wifi',2437e6,30.72e6,[dict(type='block',f=2437e6,bw=16.6e6,snr=16,duty=0.5)],frames=50); check('Wi-Fi 20 MHz',any(x['cls']=='wifi' and abs(x['f']-2437e6)<2e6 for x in e),show(e))
# 8 CW 433.92
e=run('cw',433.9e6,2.5e6,[dict(type='cw',f=433.92e6,snr=35)]); check('CW carrier',any(x['cls'] in ('cw','narrow') and abs(x['f']-433.92e6)<20e3 for x in e),show(e))
# 9 noise only: false alarms over many seeds/bands
fa=0; tot=0; bad=[]
for seed in range(40):
    fc=[100e6,433e6,868e6,940e6,1800e6,2437e6,3500e6][seed%7]; fs=[2.5e6,10e6,30.72e6,61.44e6][seed%4]
    e=run('noise',fc,fs,[],seed=seed+100); tot+=1
    n=[x for x in e if x['cls']!='spur']
    if n: fa+=1; bad.append(show(n))
print('noise-only runs with any emitter: %d / %d'%(fa,tot)); [print('   ',b) for b in bad[:5]]
check('noise false alarms < 5%',fa/tot<0.05)
# 10 noise with receiver roll-off 
fa=0
for seed in range(20):
    rng=np.random.default_rng(seed+300); fr=make(868e6,30.72e6,[],rng,noise_slope_db=12.0)
    arr=np.concatenate([np.arange(25,dtype=np.float32)[:,None],fr],1); arr.astype('<f4').tofile('synth/n2.bin')
    import subprocess
    r=subprocess.run(['./classifier-native','-t','synth/n2.bin','-F','868e6','-S','30.72e6','-B','24e6','-N','4096'],capture_output=True,text=True)
    L=[l for l in r.stdout.splitlines() if not l.startswith('#')]
    if L:
        fa+=1
        if fa<=3: print('   ',L[:3])
print('noise + 12 dB roll-off runs with emitters: %d / 20'%fa)
print('\n%d / %d checks passed'%(sum(res),len(res)))
