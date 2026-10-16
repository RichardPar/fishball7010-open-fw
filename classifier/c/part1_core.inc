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
