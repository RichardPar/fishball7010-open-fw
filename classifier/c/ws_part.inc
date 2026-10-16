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
