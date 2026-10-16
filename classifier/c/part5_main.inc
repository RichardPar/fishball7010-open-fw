
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
