
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
    int member[MAXEM] = {0}, ng = 0; int gstart[MAXEM], gcount[MAXEM];
    int last = -1;
    for (int i = 0; i < s->nem; i++) {
        emitter_t *e = &s->em[i];
        if (e->wide || !(band_flags(e->f_hz) & BF_ISM) || e->burst_db < 6.0 || !(e->cls == CLS_BURST || e->cls == CLS_UNK || e->cls == CLS_NARROW)) continue;
        if (last >= 0 && e->f_hz - s->em[last].f_hz <= 6e6) gcount[ng-1]++;
        else { gstart[ng] = i; gcount[ng] = 1; ng++; }
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
