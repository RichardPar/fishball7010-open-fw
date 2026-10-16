
/* ---------------------------------------------------------------- detector (mirrors classifier-v2/detect4.py) */
static const double CLOCKS[] = { 100e6/3, 20e6, 25e6, 40e6, 50e6, 100e6, 61.44e6, 30.72e6, 125e6, 200e6 };
#define NCLOCKS ((int)(sizeof(CLOCKS)/sizeof(CLOCKS[0])))

static int clock_harmonic(double f, double *f0_out, int *k_out)
{
    for (int i = 0; i < NCLOCKS; i++) {
        int k = (int)floor(f/CLOCKS[i] + 0.5);
        if (k >= 1 && fabs(f - k*CLOCKS[i]) < 4e3) { *f0_out = CLOCKS[i]; *k_out = k; return 1; }
    }
    return 0;
}

typedef struct { int a, b, clipped; double top; } blk_t;

/* median of s5[a:b] with the same clamping as the Python med() */
static int med_range(const double *s5, int lo, int hi, int a, int b, double *out)
{
    if (a < lo) a = lo;
    if (b > hi) b = hi;
    if (b <= a) return 0;
    *out = median_of(s5 + a, b - a);
    return 1;
}

static int wide_blocks(const spec_t *S, const double *w, int lo, int hi, blk_t *out, int maxout)
{
    const double edge_db = 6.0, min_hz = 900e3, step_db = 6.0, win_hz = 400e3;
    double hz = S->hz; int n = S->n;
    int L = (int)(80e3/hz); if (L < 6) L = 6;
    int ow = (int)(win_hz/hz); if (ow < 9) ow = 9; ow |= 1;
    double *op = malloc((size_t)n*sizeof(double)), *s5 = malloc((size_t)n*sizeof(double));
    median_filter_nearest(w, n, ow, op);
    box_filter(op, n, 5, s5);
    free(op);
    int nd = hi - lo - 2*L;                       /* D is defined for k = lo+L .. hi-L-1 */
    int nb = 0;
    static int bpos[MAXBND]; static double bsgn[MAXBND];
    if (nd > 3) {
        double *cs = malloc((size_t)(n+1)*sizeof(double)); cs[0] = 0; for (int i = 0; i < n; i++) cs[i+1] = cs[i] + s5[i];
        double *Dabs = malloc((size_t)nd*sizeof(double)), *Dv = malloc((size_t)nd*sizeof(double));
        for (int i = 0; i < nd; i++) { int k = lo + L + i; Dv[i] = (cs[k+L]-cs[k])/L - (cs[k]-cs[k-L])/L; Dabs[i] = fabs(Dv[i]); }
        int *pk = malloc((size_t)nd*sizeof(int));
        int np = find_peaks_height_distance(Dabs, nd, edge_db, L, pk);
        for (int i = 0; i < np && nb < MAXBND; i++) { bpos[nb] = lo + L + pk[i]; bsgn[nb] = Dv[pk[i]]; nb++; }
        free(cs); free(Dabs); free(Dv); free(pk);
    }
    int minw = (int)(min_hz/hz);
    /* 1. narrow segments are transparent, except a dip between two plateaus at clearly different levels (a real guard gap) */
    static int keep[MAXBND]; int nkeep = 0;
    int changed = 1;
    while (changed) {
        changed = 0;
        int ne = nb + 2;
        for (int j = 1; j < ne - 2; j++) {
            int pj  = (j == 0) ? lo : bpos[j-1];
            int pj1 = (j+1 == ne-1) ? hi : bpos[j];
            double s1 = bsgn[j-1], s2 = (j < nb) ? bsgn[j] : 0.0;
            int wdt = pj1 - pj;
            if (wdt >= minw) continue;
            int isk = 0; for (int q = 0; q < nkeep; q++) if (keep[q] == pj) isk = 1;
            if (isk) continue;
            double d1 = fabs(s1), d2 = fabs(s2);
            int del_first, del_count;
            if (s1*s2 > 0) { del_first = (d1 >= d2) ? j : j-1; del_count = 1; }
            else if (s1 < 0) {
                double ml, mr;
                int okl = med_range(s5, lo, hi, pj-3*L, pj-L, &ml), okr = med_range(s5, lo, hi, pj1+L, pj1+3*L, &mr);
                if (okl && okr && fabs(ml-mr) >= 4.0) { if (nkeep < MAXBND) keep[nkeep++] = pj; continue; }
                del_first = j-1; del_count = 2;
            }
            else if (fabs(d1-d2) > 6.0) { del_first = (d1 < d2) ? j-1 : j; del_count = 1; }
            else { del_first = j-1; del_count = 2; }
            for (int q = del_first; q + del_count < nb; q++) { bpos[q] = bpos[q+del_count]; bsgn[q] = bsgn[q+del_count]; }
            nb -= del_count; changed = 1; break;
        }
    }
    /* segments between consecutive boundaries */
    static int sa[MAXBND], sb[MAXBND]; int ns = 0;
    { int ne = nb + 2; for (int j = 0; j < ne-1; j++) { sa[ns] = (j == 0) ? lo : bpos[j-1]; sb[ns] = (j+1 == ne-1) ? hi : bpos[j]; ns++; } }
    /* 2. merge neighbours at similar levels */
    changed = 1;
    while (changed && ns > 1) {
        changed = 0;
        for (int j = 0; j < ns-1; j++) {
            double m1, m2;
            int o1 = med_range(s5, lo, hi, sa[j]+L/2, sb[j]-L/2, &m1), o2 = med_range(s5, lo, hi, sa[j+1]+L/2, sb[j+1]-L/2, &m2);
            if (o1 && o2 && fabs(m1-m2) < 3.0) {
                sb[j] = sb[j+1];
                for (int q = j+1; q < ns-1; q++) { sa[q] = sa[q+1]; sb[q] = sb[q+1]; }
                ns--; changed = 1; break;
            }
        }
    }
    static double lv[MAXBND]; static int lvok[MAXBND];
    for (int j = 0; j < ns; j++) lvok[j] = med_range(s5, lo, hi, sa[j]+L/2, sb[j]-L/2, &lv[j]);
    int no = 0;
    for (int j = 0; j < ns && no < maxout; j++) {
        int a = sa[j], b = sb[j];
        if (!lvok[j] || (b-a) < minw) continue;
        int have_l = (j > 0), have_r = (j < ns-1);
        if (!have_l && !have_r) continue;                           /* no edges at all: the receiver hump */
        double mn = 1e300;
        if (have_l && lvok[j-1] && lv[j-1] < mn) mn = lv[j-1];
        if (have_r && lvok[j+1] && lv[j+1] < mn) mn = lv[j+1];
        if (mn > 1e299) continue;
        if (lv[j] < mn + step_db) continue;
        int ta = a + L/2, tb = b - L/2;
        if (tb <= ta) continue;
        double top = median_of(s5 + ta, tb - ta);
        int a2 = a, b2 = b;
        while (a2 < b2 && s5[a2] < top - 10.0) a2++;
        while (b2 > a2 && s5[b2-1] < top - 10.0) b2--;
        if ((b2-a2) < minw) continue;
        int cnt = 0; for (int i = a2; i < b2; i++) if (s5[i] > top - 10.0) cnt++;
        if ((double)cnt / (b2-a2) < 0.75) continue;
        out[no].a = a2; out[no].b = b2; out[no].top = top; out[no].clipped = (a <= lo+L) || (b >= hi-L); no++;
    }
    free(s5);
    return no;
}

static void analyze(spec_t *S)
{
    int n = S->n, c = n/2; S->c = c;
    double hz = S->hz;
    int half = (int)(0.47*S->fs/hz);
    S->lo = (c-half > 8) ? c-half : 8;
    S->hi = (c+half < n-8) ? c+half : n-8;
    int lo = S->lo, hi = S->hi;
    double *s = S->w; box_filter(S->db, n, 3, s);
    double nbv[64]; int m = 0;
    for (int i = c-40; i < c-8; i++) nbv[m++] = s[i];
    for (int i = c+9; i < c+41; i++) nbv[m++] = s[i];
    double nbm = median_of(nbv, m);
    double mx = -1e300; for (int i = c-3; i < c+4; i++) if (s[i] > mx) mx = s[i];
    S->dc_bump = mx - nbm; S->dc_artefact = S->dc_bump < 8.0;
    double *w = S->w;
    if (S->dc_artefact) { double a0 = w[c-4], a1 = w[c+4]; for (int i = 0; i < 7; i++) w[c-3+i] = a0 + (a1-a0)*i/6.0; }
    S->floor_db = percentile_of(w + lo, hi - lo, 2.0);
    double *med25 = malloc((size_t)n*sizeof(double)), *res = malloc((size_t)n*sizeof(double)), *mad = malloc((size_t)n*sizeof(double));
    median_filter_nearest(w, n, 25, med25);
    for (int i = 0; i < n; i++) res[i] = fabs(w[i] - med25[i]);
    median_filter_nearest(res, n, 201, mad);
    for (int i = 0; i < n; i++) mad[i] *= 1.4826;
    S->nem = 0;
    blk_t blk[64]; int nbk = wide_blocks(S, w, lo, hi, blk, 64);
    for (int j = 0; j < nbk && S->nem < MAXEM; j++) {
        emitter_t *e = &S->em[S->nem++]; memset(e, 0, sizeof(*e));
        int r = blk[j].a, f = blk[j].b;
        e->wide = 1; e->lo = r; e->hi = f; e->centre_bin = 0.5*(r+f);
        e->f_hz = S->fc + (e->centre_bin - c)*hz; e->bw_hz = (f-r+1)*hz;
        e->level_db = blk[j].top; e->snr_db = blk[j].top - S->floor_db; e->clipped = blk[j].clipped;
        int ia = r + (int)(0.1*(f-r)), ib = f - (int)(0.1*(f-r)) + 1;
        double sm = 0, sq = 0; int cn = 0;
        for (int i = ia; i < ib; i++) { sm += w[i]; cn++; }
        if (cn > 3) { double mu = sm/cn; for (int i = ia; i < ib; i++) sq += (w[i]-mu)*(w[i]-mu); e->flat_db = sqrt(sq/cn); }
        double bsum = 0; for (int i = r; i <= f; i++) bsum += S->maxdb[i] - S->db[i];
        e->burst_db = bsum/(f-r+1);
        int am = r; for (int i = r; i <= f; i++) if (w[i] > w[am]) am = i;
        e->peak_bin = am;
    }
    /* narrow pass */
    int len = hi - lo; int *pk = malloc((size_t)len*sizeof(int));
    int npk = local_maxima(w + lo, len, pk);
    int guard = (int)(0.03*(hi-lo));
    for (int q = 0; q < npk && S->nem < MAXEM; q++) {
        int kk = pk[q] + lo, lb, rb;
        double P = prominence_of(w + lo, len, pk[q], &lb, &rb);
        if (P < 6.0) continue;
        if (kk < lo+guard || kk > hi-guard) continue;
        double thr = 6.0 > 5.0*mad[kk] ? 6.0 : 5.0*mad[kk];
        if (P < thr) continue;
        int inside = -1;
        for (int j = 0; j < nbk; j++) if (blk[j].a <= kk && kk <= blk[j].b) { inside = j; break; }
        if (inside >= 0) {
            int a = blk[inside].a, b = blk[inside].b;
            int x0 = kk-60 > a ? kk-60 : a, x1 = kk+60 < b ? kk+60 : b;
            double local = median_of(w + x0, x1 - x0 + 1);
            if (w[kk] - local < 10.0) continue;
            P = w[kk] - local;
        }
        int a26, b26, ah, bh, a3, b3, hl, hr, d1, d2, d3, d4; double vl = 0, vr = 0, dd;
        walk(w, kk, w[kk] - (26.0 < 0.8*P ? 26.0 : 0.8*P), lo, hi, &a26, &b26, &d1, &dd, &d2, &dd);
        walk(w, kk, w[kk] - 0.5*P, lo, hi, &ah, &bh, &hl, &vl, &hr, &vr);
        walk(w, kk, w[kk] - 3.0, lo, hi, &a3, &b3, &d3, &dd, &d4, &dd);
        if (hl || hr) {                                         /* a neighbour shares this skirt: prominence above the valley */
            double bl = hl ? vl : w[lb + lo], br = hr ? vr : w[rb + lo];
            P = w[kk] - (bl > br ? bl : br);
        }
        if ((bh-ah+1)*hz > 1.5e6) continue;                     /* too broad: the receiver response, not a narrow emitter */
        emitter_t *e = &S->em[S->nem++]; memset(e, 0, sizeof(*e));
        e->wide = 0; e->lo = ah; e->hi = bh; e->peak_bin = kk; e->centre_bin = 0.5*(ah+bh);
        e->f_hz = S->fc + (e->centre_bin - c)*hz; e->bw_hz = (bh-ah+1)*hz; e->bw3_hz = (b3-a3+1)*hz; e->bw26_hz = (b26-a26+1)*hz;
        e->prom_db = P; e->level_db = w[kk]; e->snr_db = w[kk] - S->floor_db;
        double bsum = 0; for (int i = ah; i <= bh; i++) bsum += S->maxdb[i] - S->db[i];
        e->burst_db = bsum/(bh-ah+1);
    }
    free(pk); free(med25); free(res); free(mad);
    /* sort by frequency (stable insertion sort; the list is short) */
    for (int i = 1; i < S->nem; i++) { emitter_t t = S->em[i]; int j = i-1; while (j >= 0 && S->em[j].f_hz > t.f_hz) { S->em[j+1] = S->em[j]; j--; } S->em[j+1] = t; }
}
