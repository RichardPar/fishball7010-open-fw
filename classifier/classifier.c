/*
 * classifier.c (v2) -- onboard emitter detector and signal classifier for the Maia /waterfall spectrum.
 *
 * What it does, once per interval (default 1 s):
 *   1. averages the frames received in the interval (mean and max per bin) and reads fc / sample rate / RF bandwidth from the Maia API;
 *   2. finds WIDE blocks (LTE, DVB-T, DAB ...): the spectrum is median-filtered (removes carriers/ripple), cut at sharp edges, and
 *      plateaus clearly above a neighbour are kept;
 *   3. finds NARROW emitters (carriers, FM, GSM, bursts ...): prominent peaks that also stand out above the local ripple;
 *   4. measures each emitter in Hz (occupied bandwidth, SNR, flatness, burstiness) and names it from bandwidth, shape, burstiness,
 *      the frequency band, channel rasters and known board-clock harmonics;
 *   5. tracks emitters across intervals (persistence, drift) and publishes the best one on the topics the Dashboard already reads
 *      (label, confidence, freq_offset, bandwidth_bins, frame_bins, template_id, feature_*) plus the full list as JSON.
 *
 * Frame format (maia-httpd /waterfall): float32[]; f[0] is a step index, f[1..] are FFT bins as linear amplitude, the array being
 * centred on the LO. No templates are used (the -f option is accepted and ignored for compatibility with S61classifier).
 *
 * Build: arm-linux-gnueabihf-gcc -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -static -o classifier classifier.c -lm
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <math.h>
#include <time.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <errno.h>
#include <fcntl.h>

#define MAXN   4200            /* bins per frame (4095 on this radio) */
#define MAXEM  64
#define MAXBND 512

/* ---------------------------------------------------------------- classes */
enum { CLS_UNK=0, CLS_FM, CLS_GSM, CLS_LTE, CLS_DVBT, CLS_DAB, CLS_WIFI, CLS_BURST, CLS_CW, CLS_SPUR, CLS_NARROW, CLS_OFDM, CLS_WIDE, CLS_N };
static const char *CLS_NAME[CLS_N] = { "unk","fm","gsm","lte","dvbt","dab","wifi","burst","cw","spur","narrow","ofdm","wide" };

typedef struct {
    int    wide;                 /* 1 = block, 0 = narrow emitter */
    int    lo, hi, peak_bin;     /* bin range and peak (indices into the frame array) */
    double centre_bin, f_hz, bw_hz, bw3_hz, bw26_hz;
    double prom_db, level_db, snr_db, flat_db, burst_db;
    int    clipped;
    int    cls; double conf; char label[80];
    int    track; double duty_pct;
} emitter_t;

typedef struct {
    int n, c, lo, hi, dc_artefact;
    double hz, fc, fs, rf_bw, floor_db, dc_bump;
    double db[MAXN], maxdb[MAXN], w[MAXN];
    emitter_t em[MAXEM]; int nem;
} spec_t;

/* ---------------------------------------------------------------- numeric helpers (mirror numpy/scipy used in the Python spec) */
static int cmp_d(const void *a, const void *b) { double x=*(const double*)a, y=*(const double*)b; return (x>y)-(x<y); }

static double median_of(const double *x, int m)               /* numpy.median */
{
    if (m <= 0) return 0.0;
    double *t = malloc((size_t)m * sizeof(double)); memcpy(t, x, (size_t)m * sizeof(double));
    qsort(t, (size_t)m, sizeof(double), cmp_d);
    double r = (m & 1) ? t[m/2] : 0.5*(t[m/2-1] + t[m/2]);
    free(t); return r;
}

static double percentile_of(const double *x, int m, double pct)   /* numpy.percentile, linear */
{
    double *t = malloc((size_t)m * sizeof(double)); memcpy(t, x, (size_t)m * sizeof(double));
    qsort(t, (size_t)m, sizeof(double), cmp_d);
    double idx = (m-1) * pct / 100.0; int i0 = (int)floor(idx); double fr = idx - i0;
    double r = (i0+1 < m) ? t[i0] + fr*(t[i0+1]-t[i0]) : t[i0];
    free(t); return r;
}

static inline int clampi(int i, int lo, int hi) { return i < lo ? lo : (i > hi ? hi : i); }

static void box_filter(const double *x, int n, int size, double *out)   /* scipy uniform_filter1d, mode nearest */
{
    int r = size/2;
    for (int i = 0; i < n; i++) { double s = 0; for (int k = -r; k <= r; k++) s += x[clampi(i+k,0,n-1)]; out[i] = s/size; }
}

static void median_filter_nearest(const double *x, int n, int size, double *out)   /* scipy median_filter, mode nearest */
{
    if (size < 1) size = 1;
    if (size > n) size = n | 1;
    if (!(size & 1)) size++;
    int r = size/2, pn = n + 2*r;
    double *pad = malloc((size_t)pn * sizeof(double)), *win = malloc((size_t)size * sizeof(double));
    for (int i = 0; i < pn; i++) pad[i] = x[clampi(i-r,0,n-1)];
    memcpy(win, pad, (size_t)size * sizeof(double)); qsort(win, (size_t)size, sizeof(double), cmp_d);
    out[0] = win[size/2];
    for (int i = 1; i < n; i++) {
        double ov = pad[i-1], iv = pad[i+size-1];
        int lo = 0, hi = size-1;                                  /* find ov (lower bound) */
        while (lo < hi) { int mid = (lo+hi)/2; if (win[mid] < ov) lo = mid+1; else hi = mid; }
        memmove(win+lo, win+lo+1, (size_t)(size-1-lo) * sizeof(double));       /* remove it */
        int p = 0, q = size-1;                                    /* insert iv into the size-1 element sorted prefix */
        while (p < q) { int mid = (p+q)/2; if (win[mid] < iv) p = mid+1; else q = mid; }
        memmove(win+p+1, win+p, (size_t)(size-1-p) * sizeof(double)); win[p] = iv;
        out[i] = win[size/2];
    }
    free(pad); free(win);
}

/* scipy.signal.find_peaks building blocks */
static int local_maxima(const double *x, int n, int *pk)
{
    int cnt = 0, i = 1, imax = n-1;
    while (i < imax) {
        if (x[i-1] < x[i]) {
            int ia = i+1;
            while (ia < imax && x[ia] == x[i]) ia++;
            if (x[ia] < x[i]) { pk[cnt++] = (i + ia - 1) / 2; i = ia; }
        }
        i++;
    }
    return cnt;
}

static double prominence_of(const double *x, int n, int peak, int *lbase, int *rbase)
{
    double lmin = x[peak], rmin = x[peak]; int lb = peak, rb = peak;
    for (int i = peak; i >= 0 && x[i] <= x[peak]; i--) if (x[i] < lmin) { lmin = x[i]; lb = i; }
    for (int i = peak; i <= n-1 && x[i] <= x[peak]; i++) if (x[i] < rmin) { rmin = x[i]; rb = i; }
    *lbase = lb; *rbase = rb;
    return x[peak] - (lmin > rmin ? lmin : rmin);
}

typedef struct { int pos; double h; } pk_t;
static int cmp_pk_h(const void *a, const void *b) { const pk_t *x=a,*y=b; if (x->h < y->h) return -1; if (x->h > y->h) return 1; return x->pos - y->pos; }
static int find_peaks_height_distance(const double *x, int n, double height, int distance, int *out)   /* find_peaks(x, height=, distance=) */
{
    int *pk = malloc((size_t)n * sizeof(int)); int m = local_maxima(x, n, pk), cnt = 0;
    int *cand = malloc((size_t)(m+1) * sizeof(int));
    for (int i = 0; i < m; i++) if (x[pk[i]] >= height) cand[cnt++] = pk[i];
    free(pk);
    pk_t *pr = malloc((size_t)(cnt+1) * sizeof(pk_t)); char *keep = malloc((size_t)(cnt+1));
    for (int i = 0; i < cnt; i++) { pr[i].pos = i; pr[i].h = x[cand[i]]; keep[i] = 1; }
    qsort(pr, (size_t)cnt, sizeof(pk_t), cmp_pk_h);
    for (int i = cnt-1; i >= 0; i--) {
        int j = pr[i].pos; if (!keep[j]) continue;
        for (int k = j-1; k >= 0 && cand[j]-cand[k] < distance; k--) keep[k] = 0;
        for (int k = j+1; k < cnt && cand[k]-cand[j] < distance; k++) keep[k] = 0;
    }
    int o = 0; for (int i = 0; i < cnt; i++) if (keep[i]) out[o++] = cand[i];
    free(cand); free(pr); free(keep); return o;
}

/* one side of a peak walk (see detect4.py _side): returns the edge; *valley is set when the side ended on a neighbouring emitter */
static int walk_side(const double *w, int k, double thr, int lim, int step, double rise, int *has_valley, double *valley)
{
    double m = w[k]; int edge = k, x = k;
    *has_valley = 0;
    while (x != lim && w[x+step] >= thr) {
        x += step;
        if (w[x] > m + rise) { *has_valley = 1; *valley = m; return edge; }
        if (w[x] <= m) { m = w[x]; edge = x; }
    }
    return x;
}
static void walk(const double *w, int k, double thr, int lo, int hi, int *a, int *b, int *hvl, double *vl, int *hvr, double *vr)
{
    *a = walk_side(w, k, thr, lo, -1, 3.0, hvl, vl);
    *b = walk_side(w, k, thr, hi-1, 1, 3.0, hvr, vr);
}

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
    double *med25 = malloc((size_t)n*sizeof(double)), *res = calloc((size_t)n, sizeof(double)), *mad = malloc((size_t)n*sizeof(double));
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

/* ---------------------------------------------------------------- classification (mirrors classifier-v2/classify.py) */
enum { BF_FM=1, BF_DAB=2, BF_DVBT=4, BF_LTE=8, BF_GSM=16, BF_SRD=32, BF_ISM=64 };
static const struct { double lo, hi; int flags; } BANDS[] = {
    { 87.5e6,   108.0e6,  BF_FM },            /* FM broadcast */
    { 174e6,    240e6,    BF_DAB },
    { 470e6,    694e6,    BF_DVBT },
    { 791e6,    821e6,    BF_LTE },           /* LTE800 DL */
    { 832e6,    862e6,    BF_LTE },           /* LTE800 UL */
    { 863e6,    870e6,    BF_SRD },           /* SRD 868 */
    { 880e6,    915e6,    BF_LTE|BF_GSM },    /* GSM900 UL */
    { 925e6,    960e6,    BF_LTE|BF_GSM },    /* GSM900 DL */
    { 1710e6,   1785e6,   BF_LTE|BF_GSM },    /* GSM1800 UL */
    { 1805e6,   1880e6,   BF_LTE|BF_GSM },    /* GSM1800 DL */
    { 2110e6,   2170e6,   BF_LTE },           /* UMTS2100 DL */
    { 2400e6,   2483.5e6, BF_ISM|BF_SRD },
    { 2620e6,   2690e6,   BF_LTE },           /* LTE2600 DL */
    { 5150e6,   5875e6,   BF_ISM|BF_SRD },
    { 433.05e6, 434.79e6, BF_ISM|BF_SRD },
};
static const double LTE_OBW[][2] = { {1.4e6,1.08e6},{3e6,2.7e6},{5e6,4.5e6},{10e6,9.0e6},{15e6,13.5e6},{20e6,18.0e6} };

static int band_flags(double f)
{
    int fl = 0;
    for (size_t i = 0; i < sizeof(BANDS)/sizeof(BANDS[0]); i++) if (BANDS[i].lo <= f && f <= BANDS[i].hi) fl |= BANDS[i].flags;
    return fl;
}
static double conf_clamp(double v) { return v < 0.05 ? 0.05 : (v > 0.97 ? 0.97 : v); }
static double rnd(double x) { return floor(x + 0.5); }

static void classify_emitter(emitter_t *e, double fc_hz, int seen)
{
    double f = e->f_hz, bw = e->bw_hz, burst = e->burst_db;
    int bf = band_flags(f);
    e->label[0] = 0;
    #define SET(c, base, text...) do { e->cls = (c); e->conf = conf_clamp(base); snprintf(e->label, sizeof(e->label), text); return; } while (0)
    if (!e->wide) {
        double bw3 = e->bw3_hz, bw26 = e->bw26_hz, prom = e->prom_db;
        double h0 = 0; int hk = 0;
        int h = clock_harmonic(f, &h0, &hk);
        if (fabs(f - fc_hz) < 20e3 && bw3 < 20e3 && prom > 12 && burst < 4.0 && !h)
            SET(CLS_SPUR, 0.6 + (prom > 30 ? 0.1 : 0), "Carrier on the LO frequency (LO leakage or board spur)");
        int on_mhz = fabs(f/1e6 - rnd(f/1e6))*1e6 < 3e3;
        int fm_raster = fabs(f/1e5 - rnd(f/1e5))*1e5 < 10e3;
        int gsm = (bf & BF_GSM) != 0;
        double g1 = fabs((f-935.0e6)/0.2e6 - rnd((f-935.0e6)/0.2e6))*0.2e6, g2 = fabs((f-1805.0e6)/0.2e6 - rnd((f-1805.0e6)/0.2e6))*0.2e6;
        int gsm_raster = gsm && (g1 < 15e3 || g2 < 15e3);
        if (bw3 < 20e3 && burst < 4.0 && prom > 12 && (h || (on_mhz && prom > 20))) {
            double base = (h ? 0.75 : 0.6) + (prom > 30 ? 0.1 : 0) + (seen > 2 ? 0.05 : 0);
            if (h) SET(CLS_SPUR, base, "Board clock harmonic (%d x %.2f MHz)", hk, h0/1e6);
            SET(CLS_SPUR, base, "Board clock harmonic (exact %.0f MHz)", rnd(f/1e6));
        }
        if ((bf & BF_FM) && prom >= 12 && bw3 >= 8e3 && bw <= 320e3 && (fm_raster || bw26 >= 100e3))
            SET(CLS_FM, 0.55 + (prom > 25 ? 0.15 : 0) + (fm_raster ? 0.15 : 0) + (bw26 >= 120e3 ? 0.1 : 0) + (seen > 2 ? 0.05 : 0), "FM broadcast");
        if (gsm && (prom >= 15 || (gsm_raster && prom >= 8)) && burst < 12 && bw <= 400e3 && (gsm_raster || (150e3 <= bw && bw <= 300e3)))
            SET(CLS_GSM, 0.5 + (gsm_raster ? 0.2 : 0) + ((150e3 <= bw && bw <= 300e3) ? 0.1 : 0) + (prom > 30 ? 0.1 : 0) + (burst < 4 ? 0.1 : 0), "GSM carrier (200 kHz)");
        if (burst >= 10 && bw <= 400e3)
            SET(CLS_BURST, 0.45 + ((bf & BF_SRD) ? 0.15 : 0) + (burst > 20 ? 0.1 : 0), "Burst transmitter (%s, ~%.0f kHz)",
                (90e3 <= bw && bw <= 400e3) ? "LoRa-like" : "FSK/narrowband", bw/1e3);
        if (bw3 <= 8e3 && burst < 4 && prom >= 15)
            SET(CLS_CW, 0.5 + (prom > 30 ? 0.1 : 0), "Carrier (CW)");
        if (bw <= 40e3) SET(CLS_NARROW, 0.35, "Narrowband signal (%.0f kHz)", bw/1e3);
        SET(CLS_UNK, 0.3, "Signal (%.0f kHz)", bw/1e3);
    }
    int best = -1; double besterr = 1e9;
    for (int i = 0; i < 6; i++) { double err = fabs(bw - LTE_OBW[i][1])/LTE_OBW[i][1]; if (err < 0.12 && err < besterr) { besterr = err; best = i; } }
    if (best >= 0 && (bf & BF_LTE))
        SET(CLS_LTE, 0.6 + 0.2*(1 - besterr/0.12) + (e->flat_db < 6 ? 0.1 : 0) + (!e->clipped ? 0.05 : 0), "LTE %g MHz", LTE_OBW[best][0]/1e6);
    if ((bf & BF_DVBT) && 6.5e6 <= bw && bw <= 8.2e6) SET(CLS_DVBT, 0.8, "DVB-T/T2 (8 MHz channel)");
    if ((bf & BF_DAB) && 1.3e6 <= bw && bw <= 1.7e6) SET(CLS_DAB, 0.8, "DAB ensemble (1.5 MHz)");
    if ((bf & BF_ISM) && 14e6 <= bw && bw <= 24e6) SET(CLS_WIFI, 0.7, "Wi-Fi (20 MHz OFDM)");
    if (best >= 0) SET(CLS_OFDM, 0.4, "OFDM-like block, ~%g MHz", LTE_OBW[best][0]/1e6);
    SET(CLS_WIDE, 0.3, "Wideband signal (%.2f MHz)", bw/1e6);
    #undef SET
}

/* ---------------------------------------------------------------- tracking across intervals */
#define MAXTR 96
typedef struct { int used, wide, hits, missed; double f_hz, bw_hz, first_bin, first_t; long first_iv, last_iv; uint32_t seen; } track_t;

static int popcount32(uint32_t v) { int c = 0; while (v) { c += v & 1; v >>= 1; } return c; }

static void track_update(track_t *tr, spec_t *S, long iv, double t_now)
{
    int claimed[MAXTR]; memset(claimed, 0, sizeof(claimed));
    for (int i = 0; i < S->nem; i++) {
        emitter_t *e = &S->em[i];
        double tol = e->wide ? (0.25*e->bw_hz > 250e3 ? 0.25*e->bw_hz : 250e3) : (3*S->hz > 12e3 ? 3*S->hz : 12e3);
        int bj = -1; double bd = 1e300;
        for (int j = 0; j < MAXTR; j++) {
            if (!tr[j].used || claimed[j] || tr[j].wide != e->wide) continue;
            double d = fabs(tr[j].f_hz - e->f_hz);
            if (d < tol && d < bd) { bd = d; bj = j; }
        }
        if (bj < 0) {
            for (int j = 0; j < MAXTR; j++) if (!tr[j].used) { bj = j; break; }
            if (bj < 0) { e->track = -1; e->duty_pct = 100; classify_emitter(e, S->fc, 1); continue; }
            memset(&tr[bj], 0, sizeof(tr[bj]));
            tr[bj].used = 1; tr[bj].wide = e->wide; tr[bj].first_iv = iv; tr[bj].first_bin = e->centre_bin; tr[bj].first_t = t_now;
        }
        claimed[bj] = 1;
        track_t *t = &tr[bj];
        t->f_hz = e->f_hz; t->bw_hz = e->bw_hz; t->hits++; t->missed = 0; t->last_iv = iv;
        t->seen = (t->seen << 1) | 1u;
        e->track = bj;
        long age = iv - t->first_iv + 1; if (age > 16) age = 16;
        e->duty_pct = 100.0 * popcount32(t->seen & (age >= 32 ? 0xffffffffu : ((1u << age) - 1))) / (double)age;
        classify_emitter(e, S->fc, t->hits);
    }
    for (int j = 0; j < MAXTR; j++) if (tr[j].used && !claimed[j]) {
        tr[j].seen <<= 1; if (++tr[j].missed > 8) tr[j].used = 0;
    }
}

/* ---------------------------------------------------------------- post-processing: Wi-Fi/ISM burst clusters
 * An 802.11 burst shows up in the averaged spectrum as many narrow, strongly bursty lines. Several of them close together inside an ISM band
 * are one activity, not a dozen transmitters: replace them with a single emitter spanning the cluster. (Not in the Python spec.) */
static int g_aggregate = 1;
static void aggregate_wifi(spec_t *s)
{
    if (!g_aggregate) return;
    int member[MAXEM] = {0}, ng = 0; int gcount[MAXEM];
    int last = -1;
    for (int i = 0; i < s->nem; i++) {
        emitter_t *e = &s->em[i];
        if (e->wide || !(band_flags(e->f_hz) & BF_ISM) || e->burst_db < 6.0 || !(e->cls == CLS_BURST || e->cls == CLS_UNK || e->cls == CLS_NARROW)) continue;
        if (last >= 0 && e->f_hz - s->em[last].f_hz <= 6e6) gcount[ng-1]++;
        else { gcount[ng] = 1; ng++; }
        member[i] = ng; last = i;
    }
    int drop[MAXEM] = {0}, any = 0;
    for (int g = 0; g < ng; g++) {
        if (gcount[g] < 4) continue;
        double fmin = 1e300, fmax = -1e300, snr = -1e9, bsum = 0, dsum = 0; int n = 0, first = -1;
        for (int i = 0; i < s->nem; i++) if (member[i] == g + 1) {
            const emitter_t *e = &s->em[i];
            if (e->f_hz < fmin) fmin = e->f_hz;
            if (e->f_hz > fmax) fmax = e->f_hz;
            if (e->snr_db > snr) snr = e->snr_db;
            bsum += e->burst_db; dsum += e->duty_pct; n++;
            if (first < 0) first = i; else drop[i] = 1;
        }
        emitter_t *a = &s->em[first]; double hz = s->hz;
        double span = fmax - fmin + 1e6; if (span > 24e6) span = 24e6;
        a->wide = 1; a->f_hz = 0.5*(fmin + fmax); a->bw_hz = span; a->bw3_hz = a->bw26_hz = span;
        a->centre_bin = s->c + (a->f_hz - s->fc)/hz; a->lo = (int)(a->centre_bin - span/hz/2); a->hi = (int)(a->centre_bin + span/hz/2);
        a->snr_db = snr; a->burst_db = bsum/n; a->duty_pct = dsum/n; a->track = -1; a->flat_db = 0; a->clipped = 0;
        a->cls = CLS_WIFI; a->conf = conf_clamp(0.5 + 0.05*(n < 6 ? n : 6));
        snprintf(a->label, sizeof(a->label), "Wi-Fi/ISM burst activity (%d peaks, %.1f MHz)", n, (fmax - fmin)/1e6);
        any = 1;
    }
    if (!any) return;
    int o = 0;
    for (int i = 0; i < s->nem; i++) if (!drop[i]) s->em[o++] = s->em[i];
    s->nem = o;
}

/* ---------------------------------------------------------------- application */
struct app {
    volatile bool running;
    double  threshold, interval;
    bool    verbose;
    const char *host; int port;
    uint8_t *msg_buf; size_t msg_len, msg_cap;
    /* accumulation of the current interval */
    double  sum[MAXN], mx[MAXN]; int nbins, nframes; struct timespec acc_start;
    /* radio settings */
    double  fc, fs, rf_bw, prev_fc, prev_fs; bool prev_valid;
    track_t tr[MAXTR]; long iv; int locked_track;
    struct timespec t0;
};
static struct app A;
static spec_t S;

static double elapsed_since(const struct timespec *then)
{
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - then->tv_sec) + (now.tv_nsec - then->tv_nsec) / 1e9;
}
static void ensure_msg_cap(struct app *a, size_t need)
{
    if (a->msg_cap >= need) return;
    size_t newcap = a->msg_cap ? a->msg_cap * 2 : 65536;
    while (newcap < need) newcap *= 2;
    a->msg_buf = realloc(a->msg_buf, newcap); a->msg_cap = newcap;
}

/* ---- MQTT: one persistent mqtt_pubd pipe (not retained), fork-free per message; falls back to mosquitto_pub ---- */
static FILE *g_pub = NULL; static time_t g_pub_retry = 0;
static void mqtt_pub_system(const char *topic, const char *value)
{
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "/usr/bin/mosquitto_pub -i tezuka_classifier -t 'state/classifier/%s' -m '%s' 2>/dev/null", topic, value);
    if (system(cmd) < 0) { }
}
static void mqtt_pub(const char *topic, const char *value)
{
    if (!g_pub && time(NULL) >= g_pub_retry) { g_pub = popen("/usr/bin/mqtt_pubd -i tezuka_classifier -n 2>/dev/null", "w"); g_pub_retry = time(NULL) + 10; }
    if (g_pub) {
        if (fprintf(g_pub, "state/classifier/%s\t%s\n", topic, value) > 0 && fflush(g_pub) == 0) return;
        pclose(g_pub); g_pub = NULL;
    }
    mqtt_pub_system(topic, value);
}
/* republish only when the value changed, or every 10th interval so late subscribers catch up */
static struct { char topic[32]; char val[64]; } g_cache[24];
static void pub_changed(const char *topic, const char *value)
{
    int slot = -1;
    for (int i = 0; i < 24; i++) { if (!g_cache[i].topic[0]) { if (slot < 0) slot = i; continue; } if (!strcmp(g_cache[i].topic, topic)) { slot = i; break; } }
    if (slot >= 0 && !strcmp(g_cache[slot].topic, topic) && !strcmp(g_cache[slot].val, value) && (A.iv % 10)) return;
    if (slot >= 0) { snprintf(g_cache[slot].topic, sizeof(g_cache[slot].topic), "%s", topic); snprintf(g_cache[slot].val, sizeof(g_cache[slot].val), "%s", value); }
    mqtt_pub(topic, value);
}

/* ---- tiny HTTP GET to maia-httpd for the radio settings ---- */
static int tcp_connect(const char *host, int port, double timeout_s);
static int send_all(int fd, const void *buf, size_t n);
static int json_num(const char *s, const char *key, double *out)
{
    char pat[48]; snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(s, pat); if (!p) return -1;
    p += strlen(pat); while (*p == ' ') p++;
    char *end; double v = strtod(p, &end); if (end == p) return -1;
    *out = v; return 0;
}
static int fetch_radio(struct app *a)
{
    int fd = tcp_connect(a->host, a->port, 1.0); if (fd < 0) return -1;
    char req[160]; int n = snprintf(req, sizeof(req), "GET /api/ad9361 HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", a->host);
    char resp[3072]; size_t have = 0; struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    if (send_all(fd, req, (size_t)n) == 0) {
        while (have < sizeof(resp)-1 && elapsed_since(&t0) < 1.5) {
            struct pollfd p = { fd, POLLIN, 0 };
            if (poll(&p, 1, 200) <= 0) continue;
            ssize_t r = recv(fd, resp + have, sizeof(resp)-1-have, 0);
            if (r <= 0) break;
            have += (size_t)r;
        }
    }
    close(fd); resp[have] = 0;
    char *body = strstr(resp, "\r\n\r\n"); if (!body) return -1;
    double fc, fs, bw;
    if (json_num(body, "rx_lo_frequency", &fc) || json_num(body, "sampling_frequency", &fs) || json_num(body, "rx_rf_bandwidth", &bw)) return -1;
    if (fs < 1e5) return -1;
    a->fc = fc; a->fs = fs; a->rf_bw = bw; return 0;
}

/* ---- per-interval analysis + publishing ---- */
static void reset_acc(struct app *a) { a->nframes = 0; memset(a->sum, 0, sizeof(a->sum)); memset(a->mx, 0, sizeof(a->mx)); clock_gettime(CLOCK_MONOTONIC, &a->acc_start); }

static void build_spec(spec_t *s, const struct app *a, double fc, double fs, double rf_bw)
{
    s->n = a->nbins; s->fc = fc; s->fs = fs; s->rf_bw = rf_bw; s->hz = fs/4096.0;
    for (int i = 0; i < s->n; i++) {
        s->db[i]    = 20.0*log10(a->sum[i]/a->nframes + 1e-12);
        s->maxdb[i] = 20.0*log10(a->mx[i] + 1e-12);
    }
}

static int cmp_snr_desc(const void *a, const void *b) { const emitter_t *x=a, *y=b; return (y->snr_db > x->snr_db) - (y->snr_db < x->snr_db); }

static void json_emitters(const spec_t *s, char *out, size_t cap)
{
    emitter_t srt[MAXEM]; memcpy(srt, s->em, (size_t)s->nem * sizeof(emitter_t));
    qsort(srt, (size_t)s->nem, sizeof(emitter_t), cmp_snr_desc);
    size_t o = 0; o += (size_t)snprintf(out + o, cap - o, "[");
    for (int i = 0; i < s->nem && i < 16; i++) {
        const emitter_t *e = &srt[i];
        char item[260];
        int m = snprintf(item, sizeof(item), "%s{\"f\":%.0f,\"bw\":%.0f,\"snr\":%.1f,\"c\":\"%s\",\"p\":%.2f,\"w\":%d,\"b\":%.1f,\"d\":%.0f,\"l\":\"%s\"}",
                         i ? "," : "", e->f_hz, e->bw_hz, e->snr_db, CLS_NAME[e->cls], e->conf, e->wide, e->burst_db, e->duty_pct, e->label);
        if (m <= 0 || o + (size_t)m + 2 >= cap) break;
        memcpy(out + o, item, (size_t)m); o += (size_t)m;
    }
    out[o++] = ']'; out[o] = 0;
}

static void publish_all(struct app *a, const spec_t *s, double t_now)
{
    char buf[64];
    /* primary emitter for the single-label topics the Dashboard already reads */
    int pbest = -1; double bs = -1e9;
    for (int i = 0; i < s->nem; i++) {
        const emitter_t *e = &s->em[i];
        if (e->cls == CLS_SPUR || e->conf < a->threshold) continue;
        double sc = e->snr_db + (e->wide ? 6 : 0) + (e->track >= 0 && e->track == a->locked_track ? 6 : 0);
        if (sc > bs) { bs = sc; pbest = i; }
    }
    a->locked_track = pbest >= 0 ? s->em[pbest].track : -1;
    int cnt_nonspur = 0; for (int i = 0; i < s->nem; i++) if (s->em[i].cls != CLS_SPUR) cnt_nonspur++;
    if (pbest >= 0) {
        const emitter_t *e = &s->em[pbest];
        char lab[128]; snprintf(lab, sizeof(lab), "%s @ %.3f MHz", e->label, e->f_hz/1e6);
        pub_changed("label", lab);
        snprintf(buf, sizeof(buf), "%.3f", e->conf);                           pub_changed("confidence", buf);
        snprintf(buf, sizeof(buf), "%d", (int)floor(e->centre_bin - s->c + 0.5)); pub_changed("freq_offset", buf);
        snprintf(buf, sizeof(buf), "%d", (int)floor(e->bw_hz/s->hz + 0.5));    pub_changed("bandwidth_bins", buf);
        snprintf(buf, sizeof(buf), "%d", s->n);                                pub_changed("frame_bins", buf);
        snprintf(buf, sizeof(buf), "%d", e->cls);                              pub_changed("template_id", buf);
        snprintf(buf, sizeof(buf), "%d", (int)floor(e->bw_hz/s->hz + 0.5));    pub_changed("feature_bandwidth_bins", buf);
        snprintf(buf, sizeof(buf), "%.1f", e->duty_pct);                       pub_changed("feature_duty_cycle_pct", buf);
    } else {
        pub_changed("label", "unknown"); pub_changed("confidence", "0.0");
        pub_changed("feature_bandwidth_bins", "0"); pub_changed("feature_duty_cycle_pct", "n/a");
    }
    /* spectrum shape features over the usable band */
    double mxp = 0, sp = 0, slog = 0; int m = 0;
    for (int i = s->lo; i < s->hi; i++) { double p = pow(10.0, s->db[i]/10.0); if (p > mxp) mxp = p; sp += p; slog += log(p + 1e-30); m++; }
    if (m > 0 && sp > 0) {
        snprintf(buf, sizeof(buf), "%.2f", 10*log10(mxp/(sp/m)));                  pub_changed("feature_papr_db", buf);
        snprintf(buf, sizeof(buf), "%.4f", exp(slog/m)/(sp/m));                    pub_changed("feature_flatness", buf);
    }
    snprintf(buf, sizeof(buf), "%d", cnt_nonspur);                                 pub_changed("feature_peak_count", buf);
    pub_changed("feature_drift_bins_s", "n/a");
    snprintf(buf, sizeof(buf), "%d", s->nem);                                      pub_changed("emitter_count", buf);
    snprintf(buf, sizeof(buf), "%.1f", s->floor_db);                               pub_changed("noise_floor_db", buf);
    snprintf(buf, sizeof(buf), "%.0f", s->fc);                                     pub_changed("center_hz", buf);
    snprintf(buf, sizeof(buf), "%.0f", s->fs);                                     pub_changed("sample_rate_hz", buf);
    static char js[3800]; json_emitters(s, js, sizeof(js));
    pub_changed("emitters", js);
    (void)t_now;
}

static void run_interval(struct app *a)
{
    if (a->nframes <= 0 || a->nbins < 64) { reset_acc(a); return; }
    if (fetch_radio(a) != 0) { reset_acc(a); return; }
    if (a->prev_valid && (a->fc != a->prev_fc || a->fs != a->prev_fs)) {      /* retuned during the interval: data is mixed, start over */
        memset(a->tr, 0, sizeof(a->tr)); a->locked_track = -1;
        a->prev_fc = a->fc; a->prev_fs = a->fs; reset_acc(a); return;
    }
    a->prev_fc = a->fc; a->prev_fs = a->fs; a->prev_valid = true;
    build_spec(&S, a, a->fc, a->fs, a->rf_bw);
    int frames = a->nframes;
    reset_acc(a);
    analyze(&S);
    a->iv++;
    track_update(a->tr, &S, a->iv, elapsed_since(&a->t0));
    aggregate_wifi(&S);
    publish_all(a, &S, elapsed_since(&a->t0));
    if (a->verbose) {
        fprintf(stderr, "classifier: %d frames, %d emitters, floor %.1f dB\n", frames, S.nem, S.floor_db);
        for (int i = 0; i < S.nem; i++) fprintf(stderr, "  %9.3f MHz bw %8.0f snr %5.1f %-6s %.2f %s\n", S.em[i].f_hz/1e6, S.em[i].bw_hz, S.em[i].snr_db, CLS_NAME[S.em[i].cls], S.em[i].conf, S.em[i].label);
    }
}

static void tick(struct app *a)
{
    if (a->nframes > 0 && elapsed_since(&a->acc_start) >= a->interval) run_interval(a);
}

static void handle_frame(struct app *a, const float *f, size_t n_floats)
{
    if (n_floats < 65 || n_floats - 1 > MAXN) return;
    int n = (int)(n_floats - 1);
    if (n != a->nbins) { a->nbins = n; reset_acc(a); }
    if (a->nframes == 0) clock_gettime(CLOCK_MONOTONIC, &a->acc_start);
    for (int i = 0; i < n; i++) { double v = f[i+1]; a->sum[i] += v; if (v > a->mx[i]) a->mx[i] = v; }
    a->nframes++;
    tick(a);
}
/* Minimal RFC 6455 client: plain TCP (no TLS — the feed is localhost), binary/text messages, fragmentation,
 * ping/pong and close. Replaces libwebsockets so the daemon is one self-contained static binary.        */
/* ------------------------------------------------------------------ */
#define WS_MAX_MSG (32u * 1024u * 1024u)

static void b64_encode16(const uint8_t *in, char *out)   /* 16 bytes -> 24 chars (with '=' padding) */
{
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0, i = 0;
    for (; i + 3 <= 16; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];
        out[o++] = tab[v >> 18 & 63]; out[o++] = tab[v >> 12 & 63]; out[o++] = tab[v >> 6 & 63]; out[o++] = tab[v & 63];
    }
    uint32_t v = (uint32_t)in[15] << 16;                  /* last byte */
    out[o++] = tab[v >> 18 & 63]; out[o++] = tab[v >> 12 & 63]; out[o++] = '='; out[o++] = '=';
    out[o] = 0;
}

static int tcp_connect(const char *host, int port, double timeout_s)
{
    /* Numeric IPv4 or "localhost" only: a static glibc cannot reliably use NSS name resolution at run time. */
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)port);
    if (strcmp(host, "localhost") == 0) host = "127.0.0.1";
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) return -1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    if (rc != 0) {
        if (errno != EINPROGRESS) { close(fd); return -1; }
        struct pollfd p = { fd, POLLOUT, 0 };
        int err = 0; socklen_t l = sizeof(err);
        if (poll(&p, 1, (int)(timeout_s * 1000)) <= 0 ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l) != 0 || err) { close(fd); return -1; }
    }
    fcntl(fd, F_SETFL, fl);                               /* back to blocking; reads are gated by poll() */
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

static int send_all(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= (size_t)w;
    }
    return 0;
}

/* Client-to-server frames must be masked (RFC 6455 §5.3). Control payloads are <= 125 bytes. */
static int ws_send_ctrl(int fd, uint8_t opcode, const uint8_t *payload, size_t n)
{
    uint8_t f[2 + 4 + 125];
    if (n > 125) n = 125;
    static const uint8_t key[4] = { 0x5a, 0xc3, 0x19, 0xe7 };
    f[0] = 0x80 | opcode;
    f[1] = 0x80 | (uint8_t)n;
    memcpy(f + 2, key, 4);
    for (size_t i = 0; i < n; i++) f[6 + i] = payload[i] ^ key[i & 3];
    return send_all(fd, f, 6 + n);
}

/* HTTP upgrade. Returns 0 on success; any bytes that arrived after the header are returned in pre[]. */
static int ws_handshake(int fd, const char *host, int port, const char *path, uint8_t *pre, size_t *pre_len)
{
    uint8_t rnd[16];
    FILE *u = fopen("/dev/urandom", "rb");
    if (!u || fread(rnd, 1, sizeof(rnd), u) != sizeof(rnd)) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        for (int i = 0; i < 16; i++) rnd[i] = (uint8_t)(ts.tv_nsec >> (i % 8) ^ i * 37);
    }
    if (u) fclose(u);
    char key[32]; b64_encode16(rnd, key);

    char req[512];
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nOrigin: http://%s\r\n\r\n",
        path, host, port, key, host);
    if (n <= 0 || (size_t)n >= sizeof(req) || send_all(fd, req, (size_t)n) != 0) return -1;

    char resp[4096]; size_t have = 0; char *eoh = NULL;
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    while (!eoh && have < sizeof(resp) - 1) {
        if (elapsed_since(&t0) > 5.0) return -1;
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 200) <= 0) continue;
        ssize_t r = recv(fd, resp + have, sizeof(resp) - 1 - have, 0);
        if (r <= 0) return -1;
        have += (size_t)r; resp[have] = 0;
        eoh = strstr(resp, "\r\n\r\n");
    }
    if (!eoh || strncmp(resp, "HTTP/1.1 101", 12) != 0) return -1;
    size_t hdr_len = (size_t)(eoh - resp) + 4;
    *pre_len = have - hdr_len;
    if (*pre_len > 8192) return -1;
    memcpy(pre, resp + hdr_len, *pre_len);
    return 0;
}

/* Frame loop: returns when the peer closes, errors, or A.running goes false. */
static void ws_run(int fd, const uint8_t *pre, size_t pre_len)
{
    size_t cap = 65536, len = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) return;
    if (pre_len > cap) pre_len = cap;
    memcpy(buf, pre, pre_len); len = pre_len;

    while (A.running) {
        size_t off = 0;
        for (;;) {                                         /* consume every complete frame in buf */
            size_t avail = len - off;
            if (avail < 2) break;
            const uint8_t *f = buf + off;
            int fin = f[0] & 0x80, op = f[0] & 0x0f, masked = f[1] & 0x80;
            uint64_t plen = f[1] & 0x7f;
            size_t hdr = 2;
            if (plen == 126)      { if (avail < 4)  break; plen = (uint64_t)f[2] << 8 | f[3]; hdr = 4; }
            else if (plen == 127) { if (avail < 10) break; plen = 0; for (int i = 0; i < 8; i++) plen = plen << 8 | f[2 + i]; hdr = 10; }
            uint8_t mk[4] = { 0, 0, 0, 0 };
            if (masked) { if (avail < hdr + 4) break; memcpy(mk, f + hdr, 4); hdr += 4; }
            if (plen > WS_MAX_MSG) goto done;              /* refuse absurd frames */
            if (avail < hdr + plen) {                      /* need more bytes: grow the buffer if this frame cannot fit */
                size_t need = hdr + (size_t)plen;
                if (off == 0 && need > cap) {
                    size_t nc = cap; while (nc < need) nc *= 2;
                    uint8_t *nb = realloc(buf, nc);
                    if (!nb) goto done;
                    buf = nb; cap = nc;
                }
                break;
            }
            uint8_t *pl = buf + off + hdr;
            if (masked) for (size_t i = 0; i < plen; i++) pl[i] ^= mk[i & 3];

            if (op == 0x0 || op == 0x1 || op == 0x2) {     /* continuation / text / binary */
                if (op != 0x0) A.msg_len = 0;
                ensure_msg_cap(&A, A.msg_len + (size_t)plen);
                memcpy(A.msg_buf + A.msg_len, pl, (size_t)plen);
                A.msg_len += (size_t)plen;
                if (fin) {
                    if (A.msg_len >= sizeof(float) * 2 && (A.msg_len % sizeof(float)) == 0)
                        handle_frame(&A, (const float *)A.msg_buf, A.msg_len / sizeof(float));
                    A.msg_len = 0;
                }
            } else if (op == 0x8) {                        /* close */
                ws_send_ctrl(fd, 0x8, pl, plen >= 2 ? 2 : 0);
                goto done;
            } else if (op == 0x9) {                        /* ping -> pong with the same payload */
                if (ws_send_ctrl(fd, 0xA, pl, (size_t)plen) != 0) goto done;
            }                                              /* 0xA pong and anything else: ignore */
            off += hdr + (size_t)plen;
        }
        if (off) { memmove(buf, buf + off, len - off); len -= off; }

        struct pollfd p = { fd, POLLIN, 0 };
        int pr = poll(&p, 1, 200);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) { tick(&A); continue; }
        if (len == cap) { /* a single frame larger than the buffer is handled above; this is only a safety net */
            uint8_t *nb = realloc(buf, cap * 2); if (!nb) break; buf = nb; cap *= 2;
        }
        ssize_t r = recv(fd, buf + len, cap - len, 0);
        if (r < 0) { if (errno == EINTR || errno == EAGAIN) continue; break; }
        if (r == 0) break;                                 /* peer closed */
        len += (size_t)r;
    }
done:
    free(buf);
}

/* ---------------------------------------------------------------- main */
static void on_signal(int s) { (void)s; A.running = false; }

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "  -H HOST   maia-httpd host        (default: localhost)\n"
        "  -p PORT   maia-httpd port        (default: 80)\n"
        "  -P PATH   waterfall WS path      (default: /waterfall)\n"
        "  -c THRESH confidence floor for the primary label (default: 0.40)\n"
        "  -i SEC    analysis interval      (default: 1.0)\n"
        "  -f/-m     accepted and ignored (old template options)\n"
        "  -A 0|1    cluster bursty ISM peaks into one Wi-Fi emitter (default 1)\n"
        "  -t FILE   offline test: analyse float32 frames from FILE (needs -F fc_Hz -S fs_Hz -B rfbw_Hz -N floats_per_frame)\n"
        "  -v        verbose\n"
        "  -h        this help\n", prog);
}

static int offline(const char *file, double fc, double fs, double bw, int nfl)
{
    FILE *f = fopen(file, "rb"); if (!f) { perror(file); return 1; }
    float *fr = malloc((size_t)nfl * sizeof(float));
    A.nbins = nfl - 1; reset_acc(&A);
    while (fread(fr, sizeof(float), (size_t)nfl, f) == (size_t)nfl) {
        for (int i = 0; i < A.nbins; i++) { double v = fr[i+1]; A.sum[i] += v; if (v > A.mx[i]) A.mx[i] = v; }
        A.nframes++;
    }
    fclose(f); free(fr);
    if (!A.nframes) return 1;
    build_spec(&S, &A, fc, fs, bw);
    analyze(&S);
    const char *nm = strrchr(file, '/'); nm = nm ? nm + 1 : file;
    for (int i = 0; i < S.nem; i++) classify_emitter(&S.em[i], fc, 1);
    aggregate_wifi(&S);
    printf("# %.*s floor %.2f lo %d hi %d dc_bump %.2f dc_artefact %d n %d\n", (int)strcspn(nm, "."), nm, S.floor_db, S.lo, S.hi, S.dc_bump, S.dc_artefact, S.n);
    for (int i = 0; i < S.nem; i++) {
        emitter_t *e = &S.em[i];
        printf("%s lo %d hi %d f %.0f bw %.0f snr %.2f burst %.2f cls %s conf %.2f\n", e->wide ? "W" : "N", e->lo, e->hi, e->f_hz, e->bw_hz, e->snr_db, e->burst_db, CLS_NAME[e->cls], e->conf);
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *path = "/waterfall", *tfile = NULL;
    double tfc = 0, tfs = 0, tbw = 0; int tn = 0;
    A.host = "localhost"; A.port = 80; A.threshold = 0.40; A.interval = 1.0; A.locked_track = -1;
    int opt;
    while ((opt = getopt(argc, argv, "H:p:P:f:c:m:i:t:F:S:B:N:A:vh")) != -1) {
        switch (opt) {
        case 'H': A.host = optarg; break;
        case 'p': A.port = atoi(optarg); break;
        case 'P': path = optarg; break;
        case 'c': A.threshold = atof(optarg); break;
        case 'i': A.interval = atof(optarg); break;
        case 'f': case 'm': break;
        case 'A': g_aggregate = atoi(optarg); break;
        case 't': tfile = optarg; break;
        case 'F': tfc = atof(optarg); break;
        case 'S': tfs = atof(optarg); break;
        case 'B': tbw = atof(optarg); break;
        case 'N': tn = atoi(optarg); break;
        case 'v': A.verbose = true; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }
    if (tfile) return offline(tfile, tfc, tfs, tbw, tn);
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal); signal(SIGPIPE, SIG_IGN);
    clock_gettime(CLOCK_MONOTONIC, &A.t0);
    A.running = true;
    while (A.running) {
        int fd = tcp_connect(A.host, A.port, 5.0);
        if (fd < 0) { fprintf(stderr, "classifier: connect failed, retrying in 5s\n"); sleep(5); continue; }
        uint8_t pre[8192]; size_t pre_len = 0;
        if (ws_handshake(fd, A.host, A.port, path, pre, &pre_len) != 0) {
            fprintf(stderr, "classifier: WebSocket handshake failed, retrying in 5s\n"); close(fd); sleep(5); continue;
        }
        A.msg_len = 0; A.nbins = 0; reset_acc(&A);
        fprintf(stderr, "classifier: connected to %s\n", path);
        ws_run(fd, pre, pre_len);
        close(fd);
        if (A.running) { fprintf(stderr, "classifier: connection closed, will retry\n"); sleep(1); }
    }
    free(A.msg_buf);
    if (g_pub) pclose(g_pub);
    return 0;
}
