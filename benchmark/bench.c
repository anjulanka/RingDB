/*
 * Copyright 2026 The RingDB Authors
 * Licensed under the AGPLv3 / SSPLv1 / RSALv2 Tri-License Framework
 *
 * benchmark/bench.c
 * Multi-threaded pipelined benchmark client for RingDB.
 *
 * Usage: ringdb-bench [options]
 *   -h HOST       Server hostname    (default: 127.0.0.1)
 *   -p PORT       Server port        (default: 6379)
 *   -t THREADS    Client threads     (default: 4)
 *   -c CONNS      Conns/thread       (default: 4)
 *   -P PIPELINE   Pipeline depth     (default: 16)
 *   -d DURATION   Duration (seconds) (default: 10)
 *   -s VALUE_SIZE Value size (bytes) (default: 32)
 *   -n KEYS       Key space size     (default: 100000)
 *   -r SET_PCT    % of SET ops       (default: 50)
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ── compile-time defaults ──────────────────────────────────────────────── */
#define DEF_HOST      "127.0.0.1"
#define DEF_PORT      6379
#define DEF_THREADS   4
#define DEF_CONNS     4
#define DEF_PIPELINE  16
#define DEF_DURATION  10
#define DEF_VSIZE     32
#define DEF_KEYS      100000
#define DEF_SET_PCT   50

#define MAX_CONNS_PER_THREAD  64
#define MAX_VSIZE             512
#define MAX_KEY_LEN           32
#define NET_BUF_SIZE          (256 * 1024)   /* socket SO_SNDBUF/SO_RCVBUF   */
#define RESP_BUF_SIZE         (1 << 20)      /* 1 MB staging buffer          */

/* ── latency histogram ──────────────────────────────────────────────────── */
/* 128 slots × 50 µs/slot = 0..6.35 ms linear, slot 128 = overflow (>6.4ms) */
#define HIST_SLOTS    128
#define HIST_US_STEP  50

typedef struct {
    uint64_t slots[HIST_SLOTS + 1];
} Hist;

static void hist_record(Hist *h, uint64_t us) {
    uint64_t idx = us / HIST_US_STEP;
    if (idx > HIST_SLOTS) idx = HIST_SLOTS;
    h->slots[idx]++;
}

static uint64_t hist_percentile(const Hist *h, double pct, uint64_t total) {
    uint64_t target = (uint64_t)((double)total * pct / 100.0);
    uint64_t cumul  = 0;
    for (int i = 0; i <= HIST_SLOTS; i++) {
        cumul += h->slots[i];
        if (cumul >= target)
            return (i == HIST_SLOTS) ? ((uint64_t)HIST_SLOTS * HIST_US_STEP)
                                     : (uint64_t)i * HIST_US_STEP;
    }
    return (uint64_t)HIST_SLOTS * HIST_US_STEP;
}

/* ── timing ─────────────────────────────────────────────────────────────── */
static inline uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* ── RESP builders ──────────────────────────────────────────────────────── */
static int build_set(char *buf, size_t max, int key_id, const char *value, int vsize) {
    char key[MAX_KEY_LEN];
    int klen = snprintf(key, sizeof(key), "key:%07d", key_id);
    return snprintf(buf, max,
        "*3\r\n$3\r\nSET\r\n$%d\r\n%s\r\n$%d\r\n%.*s\r\n",
        klen, key, vsize, vsize, value);
}

static int build_get(char *buf, size_t max, int key_id) {
    char key[MAX_KEY_LEN];
    int klen = snprintf(key, sizeof(key), "key:%07d", key_id);
    return snprintf(buf, max,
        "*2\r\n$3\r\nGET\r\n$%d\r\n%s\r\n",
        klen, key);
}

/* ── RESP response counter ──────────────────────────────────────────────── */
/* Returns the number of complete RESP messages found in buf[0..len).
 * Stops at the first incomplete frame so the caller can receive more data. */
static int count_responses(const char *buf, int len) {
    int count = 0, i = 0;
    while (i < len) {
        char t = buf[i++];
        if (t == '+' || t == '-' || t == ':') {
            /* simple string / error / integer — scan to \r\n */
            while (i < len - 1 && !(buf[i] == '\r' && buf[i + 1] == '\n'))
                i++;
            if (i >= len - 1) break; /* incomplete */
            i += 2;
            count++;
        } else if (t == '$') {
            /* bulk string: $<len>\r\n<data>\r\n  or  $-1\r\n (nil) */
            int n = 0, neg = 0;
            if (i < len && buf[i] == '-') { neg = 1; i++; }
            while (i < len && buf[i] >= '0' && buf[i] <= '9')
                n = n * 10 + (buf[i++] - '0');
            if (i + 1 >= len) break; /* incomplete: no \r\n after length */
            i += 2; /* skip \r\n after length */
            if (!neg) {
                if (i + n + 2 > len) break; /* incomplete: data not fully received */
                i += n + 2; /* skip data + \r\n */
            }
            count++;
        } else {
            break; /* unknown or incomplete prefix */
        }
    }
    return count;
}

/* ── config ─────────────────────────────────────────────────────────────── */
typedef struct {
    char host[64];
    int  port;
    int  threads;
    int  conns;
    int  pipeline;
    int  duration;
    int  vsize;
    int  num_keys;
    int  set_pct;
} Config;

/* ── per-thread state ───────────────────────────────────────────────────── */
typedef struct {
    Config   *cfg;
    int       tid;
    uint64_t  total_ops;
    uint64_t  errors;
    double    elapsed_s;
    Hist      hist;
} Worker;

static atomic_int g_running = 1;

/* ── open one TCP connection ────────────────────────────────────────────── */
static int open_conn(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    /* disable Nagle — essential for accurate pipelining latency */
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    int bufsz = NET_BUF_SIZE;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));

    /* wake up every 100 ms so the thread can check g_running */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) { close(fd); return -1; }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    return fd;
}

/* ── warmup: pre-fill key space via batched SETs ────────────────────────── */
static void warmup(const Config *cfg) {
    int fd = open_conn(cfg->host, cfg->port);
    if (fd < 0) {
        fprintf(stderr, "[warmup] connect failed: %s\n", strerror(errno));
        return;
    }

    char *value = malloc((size_t)cfg->vsize + 1);
    if (!value) { close(fd); return; }
    memset(value, 'x', (size_t)cfg->vsize);
    value[cfg->vsize] = '\0';

    char *sbuf = malloc(RESP_BUF_SIZE);
    char *rbuf = malloc(RESP_BUF_SIZE);
    if (!sbuf || !rbuf) { free(value); free(sbuf); free(rbuf); close(fd); return; }

    printf("[warmup] Writing %d keys (%d-byte values)...\n", cfg->num_keys, cfg->vsize);

    const int batch = 256;
    int pending = 0;
    int soff    = 0;

    for (int k = 0; k < cfg->num_keys; k++) {
        soff += build_set(sbuf + soff, (size_t)(RESP_BUF_SIZE - soff), k, value, cfg->vsize);
        pending++;

        if (pending == batch || k == cfg->num_keys - 1) {
            send(fd, sbuf, soff, 0);
            int got = 0, roff = 0;
            while (got < pending) {
                int n = (int)recv(fd, rbuf + roff, RESP_BUF_SIZE - roff - 1, 0);
                if (n <= 0) break;
                roff += n;
                got = count_responses(rbuf, roff);
            }
            soff = 0; pending = 0;
        }
    }

    free(value); free(sbuf); free(rbuf);
    close(fd);
    printf("[warmup] Done. Key space ready.\n");
}

/* ── worker thread ──────────────────────────────────────────────────────── */
static void *worker_main(void *arg) {
    Worker *w   = arg;
    Config *cfg = w->cfg;

    /* open connections */
    int nc = cfg->conns < MAX_CONNS_PER_THREAD ? cfg->conns : MAX_CONNS_PER_THREAD;
    int fds[MAX_CONNS_PER_THREAD];
    for (int i = 0; i < nc; i++) {
        fds[i] = open_conn(cfg->host, cfg->port);
        if (fds[i] < 0) {
            fprintf(stderr, "[thread %d] conn %d failed: %s\n",
                    w->tid, i, strerror(errno));
            nc = i;
            break;
        }
    }
    if (nc == 0) return NULL;

    char *sbuf  = malloc(RESP_BUF_SIZE);
    char *rbuf  = malloc(RESP_BUF_SIZE);
    char *value = malloc((size_t)cfg->vsize + 1);
    if (!sbuf || !rbuf || !value) {
        free(sbuf); free(rbuf); free(value);
        for (int i = 0; i < nc; i++) close(fds[i]);
        return NULL;
    }
    memset(value, 'v', (size_t)cfg->vsize);
    value[cfg->vsize] = '\0';

    unsigned int seed   = (unsigned int)(w->tid + 1);
    uint64_t     ops    = 0;
    uint64_t     errs   = 0;
    uint64_t     t0     = now_us();

    while (atomic_load_explicit(&g_running, memory_order_relaxed)) {
        /* round-robin across connections */
        int fd = fds[ops % (uint64_t)nc];

        /* build pipeline burst */
        int soff = 0;
        for (int p = 0; p < cfg->pipeline; p++) {
            int key    = rand_r(&seed) % cfg->num_keys;
            int is_set = ((int)(rand_r(&seed) % 100)) < cfg->set_pct;
            if (is_set)
                soff += build_set(sbuf + soff, (size_t)(RESP_BUF_SIZE - soff),
                                  key, value, cfg->vsize);
            else
                soff += build_get(sbuf + soff, (size_t)(RESP_BUF_SIZE - soff), key);
        }

        uint64_t ts = now_us();

        /* send entire pipeline in one shot */
        int sent = 0;
        while (sent < soff) {
            int n = (int)send(fd, sbuf + sent, (size_t)(soff - sent), 0);
            if (n <= 0) { errs++; goto next_iter; }
            sent += n;
        }

        /* drain until all pipeline responses are received */
        int got = 0, roff = 0;
        while (got < cfg->pipeline) {
            int n = (int)recv(fd, rbuf + roff, RESP_BUF_SIZE - roff - 1, 0);
            if (n <= 0) {
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    /* recv timed out — check stop flag */
                    if (!atomic_load_explicit(&g_running, memory_order_relaxed))
                        goto done;
                    continue;
                }
                errs++;
                break;
            }
            roff += n;
            got = count_responses(rbuf, roff);
        }

        /* record per-op latency: divide batch elapsed by pipeline depth,
         * then record once per op so percentiles are always correct. */
        uint64_t elapsed_us = now_us() - ts;
        uint64_t per_op_us  = elapsed_us / (uint64_t)cfg->pipeline;
        for (int pi = 0; pi < cfg->pipeline; pi++)
            hist_record(&w->hist, per_op_us);
        ops += (uint64_t)cfg->pipeline;

    next_iter:;
    }
    done:

    w->total_ops = ops;
    w->errors    = errs;
    w->elapsed_s = (double)(now_us() - t0) / 1.0e6;

    free(sbuf); free(rbuf); free(value);
    for (int i = 0; i < nc; i++) close(fds[i]);
    return NULL;
}

/* ── usage ──────────────────────────────────────────────────────────────── */
static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -h HOST       Server hostname    (default: " DEF_HOST ")\n"
        "  -p PORT       Server port        (default: %d)\n"
        "  -t THREADS    Client threads     (default: %d)\n"
        "  -c CONNS      Conns/thread       (default: %d)\n"
        "  -P PIPELINE   Pipeline depth     (default: %d)\n"
        "  -d DURATION   Duration (seconds) (default: %d)\n"
        "  -s VALUE_SIZE Value size (bytes) (default: %d)\n"
        "  -n KEYS       Key space size     (default: %d)\n"
        "  -r SET_PCT    %% of SET ops      (default: %d)\n",
        prog, DEF_PORT, DEF_THREADS, DEF_CONNS,
        DEF_PIPELINE, DEF_DURATION, DEF_VSIZE, DEF_KEYS, DEF_SET_PCT);
}

/* ── main ───────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    Config cfg = {
        .port     = DEF_PORT,
        .threads  = DEF_THREADS,
        .conns    = DEF_CONNS,
        .pipeline = DEF_PIPELINE,
        .duration = DEF_DURATION,
        .vsize    = DEF_VSIZE,
        .num_keys = DEF_KEYS,
        .set_pct  = DEF_SET_PCT,
    };
    strncpy(cfg.host, DEF_HOST, sizeof(cfg.host) - 1);

    int opt;
    while ((opt = getopt(argc, argv, "h:p:t:c:P:d:s:n:r:")) != -1) {
        switch (opt) {
            case 'h': strncpy(cfg.host, optarg, sizeof(cfg.host) - 1); break;
            case 'p': cfg.port     = atoi(optarg); break;
            case 't': cfg.threads  = atoi(optarg); break;
            case 'c': cfg.conns    = atoi(optarg); break;
            case 'P': cfg.pipeline = atoi(optarg); break;
            case 'd': cfg.duration = atoi(optarg); break;
            case 's': cfg.vsize    = atoi(optarg); break;
            case 'n': cfg.num_keys = atoi(optarg); break;
            case 'r': cfg.set_pct  = atoi(optarg); break;
            default:  usage(argv[0]); return 1;
        }
    }

    /* clamp values to safe ranges */
    if (cfg.threads  < 1)   cfg.threads  = 1;
    if (cfg.conns    < 1)   cfg.conns    = 1;
    if (cfg.pipeline < 1)   cfg.pipeline = 1;
    if (cfg.duration < 1)   cfg.duration = 1;
    if (cfg.vsize    < 1)   cfg.vsize    = 1;
    if (cfg.vsize    > MAX_VSIZE) cfg.vsize = MAX_VSIZE;
    if (cfg.num_keys < 1)   cfg.num_keys = 1;
    if (cfg.set_pct  < 0)   cfg.set_pct  = 0;
    if (cfg.set_pct  > 100) cfg.set_pct  = 100;

    int total_conns = cfg.threads * cfg.conns;

    printf("=============================================================\n");
    printf(" RingDB Benchmark  —  %s:%d\n", cfg.host, cfg.port);
    printf("=============================================================\n");
    printf("  Threads      : %d\n", cfg.threads);
    printf("  Conns/thread : %d  (total: %d)\n", cfg.conns, total_conns);
    printf("  Pipeline     : %d\n", cfg.pipeline);
    printf("  Duration     : %d s\n", cfg.duration);
    printf("  Value size   : %d B\n", cfg.vsize);
    printf("  Key space    : %d\n", cfg.num_keys);
    printf("  SET %%        : %d%%  GET %%: %d%%\n",
           cfg.set_pct, 100 - cfg.set_pct);
    printf("=============================================================\n");

    warmup(&cfg);

    Worker   *workers = calloc((size_t)cfg.threads, sizeof(Worker));
    pthread_t *tids   = malloc((size_t)cfg.threads * sizeof(pthread_t));
    if (!workers || !tids) {
        fprintf(stderr, "out of memory\n");
        free(workers); free(tids);
        return 1;
    }

    for (int i = 0; i < cfg.threads; i++) {
        workers[i].cfg = &cfg;
        workers[i].tid = i;
        pthread_create(&tids[i], NULL, worker_main, &workers[i]);
    }

    printf("[bench] Running for %d second(s)...\n", cfg.duration);
    sleep((unsigned)cfg.duration);
    atomic_store(&g_running, 0);

    for (int i = 0; i < cfg.threads; i++)
        pthread_join(tids[i], NULL);

    /* aggregate across threads */
    uint64_t total_ops = 0;
    uint64_t total_err = 0;
    double   max_elapsed = 0.0;
    Hist     agg = {0};

    for (int i = 0; i < cfg.threads; i++) {
        total_ops  += workers[i].total_ops;
        total_err  += workers[i].errors;
        if (workers[i].elapsed_s > max_elapsed)
            max_elapsed = workers[i].elapsed_s;
        for (int s = 0; s <= HIST_SLOTS; s++)
            agg.slots[s] += workers[i].hist.slots[s];
    }

    double ops_per_sec  = (max_elapsed > 0.0) ? (double)total_ops / max_elapsed : 0.0;
    double throughput_mb = ops_per_sec * (double)cfg.vsize / (1024.0 * 1024.0);

    uint64_t p50  = hist_percentile(&agg, 50.0,  total_ops);
    uint64_t p99  = hist_percentile(&agg, 99.0,  total_ops);
    uint64_t p999 = hist_percentile(&agg, 99.9,  total_ops);

    printf("\n=============================================================\n");
    printf(" Results\n");
    printf("=============================================================\n");
    printf("  Total ops    : %lu\n",    total_ops);
    printf("  Errors       : %lu\n",    total_err);
    printf("  Duration     : %.2f s\n", max_elapsed);
    printf("  Throughput   : %.0f ops/sec\n", ops_per_sec);
    printf("  Data rate    : %.2f MB/s\n",    throughput_mb);
    printf("  Latency p50  : %lu µs\n", p50);
    printf("  Latency p99  : %lu µs\n", p99);
    printf("  Latency p99.9: %lu µs\n", p999);
    printf("=============================================================\n");

    if (total_err == 0)
        printf("  Status: PASS\n");
    else
        printf("  Status: WARN  (%lu errors)\n", total_err);

    printf("=============================================================\n");

    free(workers);
    free(tids);
    return (int)(total_err > 0);
}
