"""Emitter detector v4 (prototype): edge-pair detection for wide blocks, ripple-aware peak detection for narrow emitters."""
import numpy as np
from scipy.signal import find_peaks
from scipy.ndimage import uniform_filter1d, median_filter, minimum_filter1d, maximum_filter1d

CLOCKS = [100e6/3, 20e6, 25e6, 40e6, 50e6, 100e6, 61.44e6, 30.72e6, 125e6, 200e6]

def prep(frames_lin, fc, fs, rf_bw):
    a = np.asarray(frames_lin, dtype=np.float64); n = a.shape[1]
    return dict(db=20*np.log10(a.mean(0)+1e-12), maxdb=20*np.log10(a.max(0)+1e-12), n=n, hz=fs/4096.0, fc=fc, fs=fs, rf_bw=rf_bw, c=n//2)

def usable(p):
    half = int(0.47*p['fs']/p['hz']); return max(8,p['c']-half), min(p['n']-8,p['c']+half)

def clock_harmonic(f_hz, tol_hz=4e3):
    for f0 in CLOCKS:
        k = round(f_hz/f0)
        if k >= 1 and abs(f_hz-k*f0) < tol_hz: return (f0,k)
    return None

def _side(w, k, thr, lim, step, rise):
    """One side of a peak walk: go outwards while the spectrum stays >= thr. If it falls and then rises again by more than `rise` dB
    (the start of a neighbouring emitter) stop at the valley bottom and report the valley level, else stop where it drops below thr."""
    m=w[k]; edge=k; x=k
    while x!=lim and w[x+step]>=thr:
        x+=step
        if w[x]>m+rise: return edge,float(m)
        if w[x]<=m: m=w[x]; edge=x
    return x,None

def _walk(w, k, thr, lo, hi, rise=3.0):
    """Returns (a, b, valley_left, valley_right); valley_* is the valley level when that side ended on a neighbouring emitter."""
    a,ml=_side(w,k,thr,lo,-1,rise); b,mr=_side(w,k,thr,hi-1,1,rise)
    return a,b,ml,mr

def wide_blocks(p, w, lo, hi, edge_db=6.0, min_hz=900e3, step_db=6.0, win_hz=400e3):
    """Plateau segmentation. Cut at every sharp edge; narrow segments (spikes, carriers) are transparent; merge neighbours at similar
    levels; a segment is a block if it is wide, flat enough and clearly above at least one neighbour."""
    hz=p['hz']; L=max(6,int(80e3/hz))
    # a median filter with a 400 kHz window removes anything narrower than ~200 kHz (carriers, FM stations, spikes) but keeps a
    # ripply plateau at its middle level (an opening would collapse deep ripple to the floor)
    ow=max(9,int(win_hz/hz))|1; op=median_filter(w,size=ow,mode='nearest')
    s5=uniform_filter1d(op,5,mode='nearest')
    cs=np.concatenate([[0],np.cumsum(s5)])
    k=np.arange(lo+L,hi-L)
    D=(cs[k+L]-cs[k])/L-(cs[k]-cs[k-L])/L
    pk,_=find_peaks(np.abs(D),height=edge_db,distance=L)
    bnd=[[int(k[i]),float(D[i])] for i in pk]                      # [position, signed edge strength]
    minw=int(min_hz/hz)
    def med(a,b):
        a=max(lo,a); b=min(hi,b)
        return float(np.median(s5[a:b])) if b>a else None
    # 1. narrow segments are transparent, except a dip between two plateaus at clearly different levels (a real guard gap)
    changed=True; keep=set()
    while changed:
        changed=False
        edges=[[lo,0.0]]+bnd+[[hi,0.0]]
        for j in range(1,len(edges)-2):
            wdt=edges[j+1][0]-edges[j][0]
            if wdt>=minw or edges[j][0] in keep: continue
            s1,s2=edges[j][1],edges[j+1][1]; d1,d2=abs(s1),abs(s2)
            if s1*s2>0:                                  # same sign: a staircase edge, keep one
                del bnd[j if d1>=d2 else j-1]
            elif s1<0:                                   # fall then rise: a narrow dip
                ml=med(edges[j][0]-3*L,edges[j][0]-L); mr=med(edges[j+1][0]+L,edges[j+1][0]+3*L)
                if ml is not None and mr is not None and abs(ml-mr)>=4.0:
                    keep.add(edges[j][0]); continue      # different plateaus either side: a real gap
                del bnd[j-1:j+1]
            elif abs(d1-d2)>6.0: del bnd[j-1 if d1<d2 else j]   # rise then fall: a spike on an edge: keep the stronger edge
            else: del bnd[j-1:j+1]                        # isolated narrow peak: drop both
            changed=True; break
    edges=[[lo,0.0]]+bnd+[[hi,0.0]]
    segs=[[edges[j][0],edges[j+1][0]] for j in range(len(edges)-1)]
    # 2. merge neighbours at similar levels
    changed=True
    while changed and len(segs)>1:
        changed=False
        for j in range(len(segs)-1):
            m1=med(segs[j][0]+L//2,segs[j][1]-L//2); m2=med(segs[j+1][0]+L//2,segs[j+1][1]-L//2)
            if m1 is not None and m2 is not None and abs(m1-m2)<3.0:
                segs[j][1]=segs[j+1][1]; del segs[j+1]; changed=True; break
    lv=[med(a+L//2,b-L//2) for a,b in segs]
    out=[]
    for j,(a,b) in enumerate(segs):
        if lv[j] is None or (b-a)<minw: continue
        left=lv[j-1] if j>0 else None; right=lv[j+1] if j<len(segs)-1 else None
        nb=[x for x in (left,right) if x is not None]
        if not nb: continue                                         # no edges at all: the whole span (receiver hump)
        if lv[j] < min(nb)+step_db: continue
        top=float(np.median(s5[a+L//2:b-L//2])); seg=s5[a:b]
        # trim gentle skirts: shrink while below top-10 dB
        a2,b2=a,b
        while a2<b2 and s5[a2]<top-10.0: a2+=1
        while b2>a2 and s5[b2-1]<top-10.0: b2-=1
        if (b2-a2)<minw: continue
        fill=float(np.mean(s5[a2:b2]>top-10.0))
        if fill<0.75: continue
        rv=abs(edges[[e[0] for e in edges].index(a)][1]) if a!=lo else 0.0
        fv=abs(edges[[e[0] for e in edges].index(b)][1]) if b!=hi else 0.0
        out.append((a2,b2,rv,-fv,top,fill,(a<=lo+L) or (b>=hi-L)))
    return out

def detect(p, narrow_prom=6.0):
    db=p['db']; hz=p['hz']; c=p['c']; lo,hi=usable(p)
    s=uniform_filter1d(db,3,mode='nearest')
    nb=np.median(np.concatenate([s[c-40:c-8],s[c+9:c+41]]))
    dc_bump=float(s[c-3:c+4].max()-nb); dc_artefact=dc_bump<8.0
    w=s.copy()
    if dc_artefact: w[c-3:c+4]=np.linspace(w[c-4],w[c+4],7)
    floor=float(np.percentile(w[lo:hi],2))
    resid=w-median_filter(w,25,mode='nearest'); mad=median_filter(np.abs(resid),201,mode='nearest')*1.4826
    ems=[]; blocks=[]
    for r,f,rv,fv,top,fill,clipped in wide_blocks(p,w,lo,hi):
        seg=w[r:f+1]; inner=w[r+int(0.1*(f-r)):f-int(0.1*(f-r))+1]
        blocks.append((r,f))
        ems.append(dict(kind='wide',centre_bin=0.5*(r+f),f_hz=p['fc']+(0.5*(r+f)-c)*hz,bw_hz=(f-r+1)*hz,level_db=top,peak_over_floor=float(top-floor),
                        flat_db=float(np.std(inner)),edge_up_db=rv,edge_dn_db=-fv,fill=fill,clipped=bool(clipped),
                        burst_db=float(np.mean(p['maxdb'][r:f+1]-db[r:f+1])),lo=int(r),hi=int(f),peak_bin=int(r+np.argmax(seg))))
    npk,npr=find_peaks(w[lo:hi],prominence=narrow_prom); npk=npk+lo
    edge_guard=int(0.03*(hi-lo))
    for k,l,rb in zip(npk,npr['left_bases']+lo,npr['right_bases']+lo):
        if k<lo+edge_guard or k>hi-edge_guard: continue
        base=max(w[l],w[rb]); P=w[k]-base
        if P<max(narrow_prom,5.0*mad[k]): continue
        inside=[(a,b) for a,b in blocks if a<=k<=b]
        if inside:
            a,b=inside[0]; local=float(np.median(w[max(a,k-60):min(b,k+60)+1]))
            if w[k]-local<10.0: continue
            P=w[k]-local
        a26,b26,_,_=_walk(w,k,w[k]-min(26.0,0.8*P),lo,hi); ah,bh,vl,vr=_walk(w,k,w[k]-0.5*P,lo,hi); a3,b3,_,_=_walk(w,k,w[k]-3.0,lo,hi)
        if vl is not None or vr is not None:                  # a neighbour shares this peak's skirt: measure above the valley, not the global base
            P=float(w[k]-max(vl if vl is not None else w[l], vr if vr is not None else w[rb]))
        if (bh-ah+1)*hz>1.5e6: continue                       # a hump this broad is the receiver response, not a narrow emitter
        ems.append(dict(kind='narrow',centre_bin=0.5*(ah+bh),f_hz=p['fc']+(0.5*(ah+bh)-c)*hz,bw_hz=(bh-ah+1)*hz,bw3_hz=(b3-a3+1)*hz,bw26_hz=(b26-a26+1)*hz,
                        prom_db=float(P),level_db=float(w[k]),peak_over_floor=float(w[k]-floor),burst_db=float(np.mean(p['maxdb'][ah:bh+1]-db[ah:bh+1])),
                        lo=int(ah),hi=int(bh),peak_bin=int(k),clipped=False))
    for e in ems: e['fc_hz']=p['fc']
    ems.sort(key=lambda e:e['f_hz'])
    return dict(s=w,floor=floor,lo=lo,hi=hi,emitters=ems,dc_bump=dc_bump,dc_artefact=dc_artefact)
