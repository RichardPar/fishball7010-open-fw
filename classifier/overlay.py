import sys,json,glob,importlib,numpy as np, matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
sys.path.insert(0,'.')
mod=importlib.import_module(sys.argv[1]); out=sys.argv[2]
caps=[]
for jf in sorted(glob.glob('captures/*.json')):
    m=json.load(open(jf)); caps.append((m,np.fromfile(jf[:-5]+'.bin',dtype='<f4').reshape(-1,m['floats_per_frame'])[:,1:]))
fig,ax=plt.subplots(len(caps),1,figsize=(13,3.0*len(caps)))
for (m,a),x in zip(caps,ax):
    p=mod.prep(a,m['fc_hz'],m['fs_hz'],m['rf_bw_hz']); r=mod.detect(p); hz=p['hz']; n=p['n']; f=(m['fc_hz']+(np.arange(n)-p['c'])*hz)/1e6
    nw=sum(1 for e in r['emitters'] if e['kind']=='wide'); nn=len(r['emitters'])-nw
    print('%-8s %2d emitters: %d wide, %d narrow | DC bump %.1f dB (%s)'%(m['name'],len(r['emitters']),nw,nn,r['dc_bump'],'artefact' if r['dc_artefact'] else 'real carrier on LO'))
    for e in r['emitters']:
        if e['kind']=='wide': print('     WIDE   %9.3f MHz  bw %7.3f MHz  level %+5.1f dB over floor  flat %.1f  edges +%.0f/-%.0f dB%s'%(e['f_hz']/1e6,e['bw_hz']/1e6,e['peak_over_floor'],e['flat_db'],e['edge_up_db'],e['edge_dn_db'],'  [clipped]' if e['clipped'] else ''))
    x.plot(f,p['db']-r['floor'],lw=.4,color='0.5'); x.axvline(f[r['lo']],color='0.8'); x.axvline(f[r['hi']],color='0.8')
    for e in r['emitters']:
        if e['kind']=='wide': x.axvspan(f[e['lo']],f[e['hi']],color='tab:green',alpha=.25)
        else: x.plot(f[e['peak_bin']],p['db'][e['peak_bin']]-r['floor'],'v',color='tab:red',ms=5)
    x.set_title('%s  fc %.2f MHz  (green = wide block, red = narrow emitter, grey lines = analysed span)'%(m['name'],m['fc_hz']/1e6),fontsize=9); x.grid(alpha=.3)
plt.tight_layout(); plt.savefig(out,dpi=80)
