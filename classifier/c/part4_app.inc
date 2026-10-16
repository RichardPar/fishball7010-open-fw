
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
