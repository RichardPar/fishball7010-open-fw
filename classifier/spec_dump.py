"""Print the Python reference result for a capture in a stable text format (used for C-vs-Python parity)."""
import sys,json,numpy as np
sys.path.insert(0,'.')
import detect4 as D, classify as C
nm=sys.argv[1]
m=json.load(open('captures/%s.json'%nm)); a=np.fromfile('captures/%s.bin'%nm,dtype='<f4').reshape(-1,m['floats_per_frame'])[:,1:]
p=D.prep(a,m['fc_hz'],m['fs_hz'],m['rf_bw_hz']); r=D.detect(p)
print('# %s floor %.2f lo %d hi %d dc_bump %.2f dc_artefact %d n %d'%(nm,r['floor'],r['lo'],r['hi'],r['dc_bump'],int(r['dc_artefact']),p['n']))
for e in r['emitters']:
    lab,cls,cf=C.classify(e)
    print('%s lo %d hi %d f %.0f bw %.0f snr %.2f burst %.2f cls %s conf %.2f'%('W' if e['kind']=='wide' else 'N',e['lo'],e['hi'],e['f_hz'],e['bw_hz'],e['peak_over_floor'],e['burst_db'],cls,cf))
