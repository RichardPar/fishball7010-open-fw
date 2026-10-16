/* Test harness: included after the classifier source (main renamed) so it can call the static functions. */
#include <sys/stat.h>
int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: %s templates.bin frames.bin reps\n", argv[0]); return 2; }
    struct templates T; memset(&T, 0, sizeof(T));
    if (templates_load(argv[1], &T) != 0) return 1;
    FILE *f = fopen(argv[2], "rb"); if (!f) return 1;
    int reps = atoi(argv[3]);
    uint32_t nfl; /* floats per frame: first 4 bytes of the file */
    if (fread(&nfl, 4, 1, f) != 1) return 1;
    float *fr = malloc(nfl * sizeof(float)); int idx = 0;
    double t_total = 0; long runs = 0;
    while (fread(fr, sizeof(float), nfl, f) == nfl) {
        uint32_t live_n = nfl - 1;
        float *live = malloc(live_n * sizeof(float)); memcpy(live, fr + 1, live_n * sizeof(float));
        struct frame_features ff = extract_frame_features(live, live_n);
        struct match_result r; memset(&r, 0, sizeof(r));
        struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
        for (int k = 0; k < reps; k++) {
            float *w = malloc(live_n * sizeof(float)); memcpy(w, live, live_n * sizeof(float));
            if (normalize(w, live_n)) r = classify(w, live_n, &T, &ff); else { r.found = false; r.score = 0; }
            free(w);
        }
        clock_gettime(CLOCK_MONOTONIC, &b);
        t_total += (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9; runs += reps;
        printf("%3d found=%d tpl=%u score=%.6f off=%d bw=%u\n", idx++, r.found, r.template_idx, r.score, r.bin_offset, r.bandwidth_bins);
        free(live);
    }
    fprintf(stderr, "%s: %ld classify() runs, %.3f ms each\n", argv[0], runs, 1000.0 * t_total / (runs ? runs : 1));
    return 0;
}
