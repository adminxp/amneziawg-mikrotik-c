/*
 * Stress test for awg-proxy: fork/exec proxy, mock UDP endpoints,
 * craft WireGuard packets, measure packet loss under load.
 *
 * All traffic is 127.0.0.1 only. No real keys, no external connections.
 * Run: make test-stress (requires make build first)
 */

#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#include "test.h"
#include "transform.h"
#include "chacha20.h"

/* AWG parameters matching env vars passed to proxy */
#define TEST_JC   2
#define TEST_JMIN 50
#define TEST_JMAX 100
#define TEST_S1   20
#define TEST_S2   15
#define TEST_H1   1234567891u
#define TEST_H2   1234567892u
#define TEST_H3   1234567893u
#define TEST_H4   1234567894u

/* Non-zero keys to exercise MAC1 recompute path. NOT real keys. */
#define DUMMY_SERVER_PUB "AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE="  /* 32 x 0x01 */
#define DUMMY_CLIENT_PUB "AgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgI="  /* 32 x 0x02 */

/* AWG 3.0 scenario: every padding must be >= 12 (the ChaCha20 nonce). */
#define TEST_V3_S1 20
#define TEST_V3_S2 15
#define TEST_V3_S3 18
#define TEST_V3_S4 14
#define TEST_HP_KEY_HEX \
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
static const uint8_t TEST_HP_KEY[32] = {
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
    0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,
};

#define BURST_COUNT     10000
#define MULTI_PER_CLIENT 2500
#define BIDIR_COUNT     5000
#define RECV_TIMEOUT_MS 4000
#define PROXY_BINARY    "../builds/awg-proxy"

/* ---- Helpers ---- */

static int find_free_port(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
    socklen_t len = sizeof(a);
    getsockname(fd, (struct sockaddr *)&a, &len);
    int port = ntohs(a.sin_port);
    close(fd);
    return port;
}

static int make_udp_socket(int port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    /* Increase recv/send buffers for burst traffic */
    int bufsize = 8 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port = htons(port)
    };
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
    return fd;
}

/* IPv6 loopback mock endpoint. Returns -1 when the host has no ::1 at all,
 * which the IPv6 scenarios treat as "skip", not "fail". */
static int make_udp_socket6(int port) {
    int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt));
    int bufsize = 8 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    struct sockaddr_in6 a;
    memset(&a, 0, sizeof(a));
    a.sin6_family = AF_INET6;
    a.sin6_addr = in6addr_loopback;
    a.sin6_port = htons(port);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
    return fd;
}

static int make_client_socket(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int bufsize = 8 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    return fd;
}

/* Unbound v6 client. -1 means the host has no IPv6 at all — skip, not fail. */
static int make_client_socket6(void) {
    int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int bufsize = 8 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    return fd;
}

static struct sockaddr_in make_addr(int port) {
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port = htons(port)
    };
    return a;
}

static struct sockaddr_in6 make_addr6(int port) {
    struct sockaddr_in6 a;
    memset(&a, 0, sizeof(a));
    a.sin6_family = AF_INET6;
    a.sin6_addr = in6addr_loopback;
    a.sin6_port = htons(port);
    return a;
}

static char *itoa_buf(int v, char *buf) {
    snprintf(buf, 32, "%d", v);
    return buf;
}

static char *utoa_buf(uint32_t v, char *buf) {
    snprintf(buf, 32, "%u", v);
    return buf;
}

static int64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static long get_rss_kb(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    long rss = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line + 6, "%ld", &rss);
            break;
        }
    }
    fclose(f);
    return rss;
}

/* listen_str and remote_str are passed to AWG_LISTEN / AWG_REMOTE verbatim, so
 * IPv6 scenarios can hand in "[::]:port", "[::1]:port" or a dual-stack name.
 * he_delay may be NULL to keep the default. */
static pid_t start_proxy_listen_remote(const char *mode, const char *listen_str,
                                       const char *remote_str, const char *he_delay) {
    /* Find a free port for proxy's source to avoid auto_src_port reconnect race */
    int src_port = find_free_port();

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        char sp[8];
        char jc[8], jmin[8], jmax[8], s1[8], s2[8];
        char h1[16], h2[16], h3[16], h4[16];

        setenv("AWG_LISTEN", listen_str, 1);
        setenv("AWG_REMOTE", remote_str, 1);
        if (he_delay) setenv("AWG_HE_DELAY", he_delay, 1);
        else unsetenv("AWG_HE_DELAY");
        setenv("AWG_MODE", mode, 1);
        setenv("AWG_JC", itoa_buf(TEST_JC, jc), 1);
        setenv("AWG_JMIN", itoa_buf(TEST_JMIN, jmin), 1);
        setenv("AWG_JMAX", itoa_buf(TEST_JMAX, jmax), 1);
        setenv("AWG_S1", itoa_buf(TEST_S1, s1), 1);
        setenv("AWG_S2", itoa_buf(TEST_S2, s2), 1);
        setenv("AWG_H1", utoa_buf(TEST_H1, h1), 1);
        setenv("AWG_H2", utoa_buf(TEST_H2, h2), 1);
        setenv("AWG_H3", utoa_buf(TEST_H3, h3), 1);
        setenv("AWG_H4", utoa_buf(TEST_H4, h4), 1);
        setenv("AWG_SERVER_PUB", DUMMY_SERVER_PUB, 1);
        setenv("AWG_CLIENT_PUB", DUMMY_CLIENT_PUB, 1);
        setenv("AWG_LOG_LEVEL", "error", 1);
        setenv("AWG_TIMEOUT", "30", 1);
        setenv("AWG_NO_GRO", "1", 1);
        setenv("AWG_SRC_PORT", itoa_buf(src_port, sp), 1);

        execl(PROXY_BINARY, "awg-proxy", NULL);
        _exit(127);
    }
    usleep(200000); /* 200ms for proxy startup */
    return pid;
}

static pid_t start_proxy_remote(const char *mode, int listen_port,
                                const char *remote_str, const char *he_delay) {
    char lbuf[32];
    snprintf(lbuf, sizeof(lbuf), ":%d", listen_port);
    return start_proxy_listen_remote(mode, lbuf, remote_str, he_delay);
}

static pid_t start_proxy(const char *mode, int listen_port, int remote_port) {
    char rbuf[64];
    snprintf(rbuf, sizeof(rbuf), "127.0.0.1:%d", remote_port);
    return start_proxy_remote(mode, listen_port, rbuf, NULL);
}

static pid_t start_proxy_with_gro(const char *mode, int listen_port, int remote_port) {
    int src_port = find_free_port();
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        char lbuf[32], rbuf[64], sp[8];
        char jc[8], jmin[8], jmax[8], s1[8], s2[8];
        char h1[16], h2[16], h3[16], h4[16];

        snprintf(lbuf, sizeof(lbuf), ":%d", listen_port);
        snprintf(rbuf, sizeof(rbuf), "127.0.0.1:%d", remote_port);

        setenv("AWG_LISTEN", lbuf, 1);
        setenv("AWG_REMOTE", rbuf, 1);
        setenv("AWG_MODE", mode, 1);
        setenv("AWG_JC", itoa_buf(TEST_JC, jc), 1);
        setenv("AWG_JMIN", itoa_buf(TEST_JMIN, jmin), 1);
        setenv("AWG_JMAX", itoa_buf(TEST_JMAX, jmax), 1);
        setenv("AWG_S1", itoa_buf(TEST_S1, s1), 1);
        setenv("AWG_S2", itoa_buf(TEST_S2, s2), 1);
        setenv("AWG_H1", utoa_buf(TEST_H1, h1), 1);
        setenv("AWG_H2", utoa_buf(TEST_H2, h2), 1);
        setenv("AWG_H3", utoa_buf(TEST_H3, h3), 1);
        setenv("AWG_H4", utoa_buf(TEST_H4, h4), 1);
        setenv("AWG_SERVER_PUB", DUMMY_SERVER_PUB, 1);
        setenv("AWG_CLIENT_PUB", DUMMY_CLIENT_PUB, 1);
        setenv("AWG_LOG_LEVEL", "error", 1);
        setenv("AWG_TIMEOUT", "30", 1);
        /* GRO enabled — no AWG_NO_GRO */
        unsetenv("AWG_NO_GRO");
        setenv("AWG_SRC_PORT", itoa_buf(src_port, sp), 1);

        execl(PROXY_BINARY, "awg-proxy", NULL);
        _exit(127);
    }
    usleep(200000);
    return pid;
}

/* AWG 3.0 proxy: same profile plus S3/S4 and a header protection key. */
static pid_t start_proxy_v3(const char *mode, int listen_port, int remote_port) {
    int src_port = find_free_port();
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        char lbuf[32], rbuf[64], sp[8];
        char jc[8], jmin[8], jmax[8], s1[8], s2[8], s3[8], s4[8];
        char h1[16], h2[16], h3[16], h4[16];

        snprintf(lbuf, sizeof(lbuf), ":%d", listen_port);
        snprintf(rbuf, sizeof(rbuf), "127.0.0.1:%d", remote_port);

        setenv("AWG_LISTEN", lbuf, 1);
        setenv("AWG_REMOTE", rbuf, 1);
        setenv("AWG_MODE", mode, 1);
        setenv("AWG_JC", itoa_buf(TEST_JC, jc), 1);
        setenv("AWG_JMIN", itoa_buf(TEST_JMIN, jmin), 1);
        setenv("AWG_JMAX", itoa_buf(TEST_JMAX, jmax), 1);
        setenv("AWG_S1", itoa_buf(TEST_V3_S1, s1), 1);
        setenv("AWG_S2", itoa_buf(TEST_V3_S2, s2), 1);
        setenv("AWG_S3", itoa_buf(TEST_V3_S3, s3), 1);
        setenv("AWG_S4", itoa_buf(TEST_V3_S4, s4), 1);
        setenv("AWG_H1", utoa_buf(TEST_H1, h1), 1);
        setenv("AWG_H2", utoa_buf(TEST_H2, h2), 1);
        setenv("AWG_H3", utoa_buf(TEST_H3, h3), 1);
        setenv("AWG_H4", utoa_buf(TEST_H4, h4), 1);
        setenv("AWG_HEADER_PROTECTION_KEY", TEST_HP_KEY_HEX, 1);
        setenv("AWG_SERVER_PUB", DUMMY_SERVER_PUB, 1);
        setenv("AWG_CLIENT_PUB", DUMMY_CLIENT_PUB, 1);
        setenv("AWG_LOG_LEVEL", "error", 1);
        setenv("AWG_TIMEOUT", "30", 1);
        setenv("AWG_NO_GRO", "1", 1);
        setenv("AWG_SRC_PORT", itoa_buf(src_port, sp), 1);

        execl(PROXY_BINARY, "awg-proxy", NULL);
        _exit(127);
    }
    usleep(200000);
    return pid;
}

static void stop_proxy(pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    int st;
    waitpid(pid, &st, 0);
}

/* ---- Packet crafting ---- */

static void make_wg_init(uint8_t *buf, uint32_t sender_index) {
    memset(buf, 0, WG_INIT_SIZE);
    uint32_t t = WG_HANDSHAKE_INIT;
    memcpy(buf, &t, 4);
    memcpy(buf + 4, &sender_index, 4);
    for (int i = 8; i < WG_INIT_SIZE; i++)
        buf[i] = (uint8_t)(i ^ (sender_index & 0xFF));
}

/* make_wg_response not needed — proxy doesn't validate response crypto */

static void make_wg_transport(uint8_t *buf, uint32_t receiver_index,
                               uint64_t counter, int total_size) {
    memset(buf, 0, total_size);
    uint32_t t = WG_TRANSPORT_DATA;
    memcpy(buf, &t, 4);
    memcpy(buf + 4, &receiver_index, 4);
    memcpy(buf + 8, &counter, 8);
    for (int i = 16; i < total_size; i++)
        buf[i] = (uint8_t)(i ^ (counter & 0xFF));
}

/* AWG-format init: S1 padding + H1 type + init payload */
static void make_awg_init(uint8_t *buf, uint32_t sender_index) {
    for (int i = 0; i < TEST_S1; i++) buf[i] = (uint8_t)(i * 7);
    uint32_t h = TEST_H1;
    memcpy(buf + TEST_S1, &h, 4);
    memcpy(buf + TEST_S1 + 4, &sender_index, 4);
    for (int i = 8; i < WG_INIT_SIZE; i++)
        buf[TEST_S1 + i] = (uint8_t)(i ^ (sender_index & 0xFF));
}

/* make_awg_response not needed — burst test only needs transport */

/* AWG-format transport: H4 type + transport payload */
static void make_awg_transport(uint8_t *buf, uint32_t receiver_index,
                                uint64_t counter, int payload_size) {
    int total = payload_size;
    memset(buf, 0, total);
    uint32_t h = TEST_H4;
    memcpy(buf, &h, 4);
    memcpy(buf + 4, &receiver_index, 4);
    memcpy(buf + 8, &counter, 8);
    for (int i = 16; i < total; i++)
        buf[i] = (uint8_t)(i ^ (counter & 0xFF));
}

/* Receive one packet with timeout, return size or -1 */
static int recv_one(int fd, uint8_t *buf, int bufsize, int timeout_ms) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int r = poll(&pfd, 1, timeout_ms);
    if (r <= 0) return -1;
    ssize_t n = recvfrom(fd, buf, bufsize, MSG_DONTWAIT, NULL, NULL);
    return (int)n;
}

/* Drain socket of all pending packets */
static void drain_socket(int fd) {
    uint8_t buf[2048];
    while (recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT, NULL, NULL) > 0);
}

/* Async receiver: runs in a thread, counts packets until stopped */
typedef struct {
    int fd;
    _Atomic int count;
    _Atomic int stop;
    /* For index tracking (server_multiclient routing check) */
    uint32_t *indices;     /* NULL if not tracking */
    int max_indices;
} async_recv_t;

static void *async_recv_thread(void *arg) {
    async_recv_t *r = (async_recv_t *)arg;
    uint8_t buf[2048];
    struct pollfd pfd = { .fd = r->fd, .events = POLLIN };

    while (!atomic_load(&r->stop)) {
        int ret = poll(&pfd, 1, 100);
        if (ret <= 0) continue;
        for (;;) {
            ssize_t n = recvfrom(r->fd, buf, sizeof(buf), MSG_DONTWAIT, NULL, NULL);
            if (n <= 0) break;
            int idx = atomic_fetch_add(&r->count, 1);
            if (r->indices && idx < r->max_indices && n >= 8) {
                uint32_t ri;
                memcpy(&ri, buf + 4, 4);
                r->indices[idx] = ri;
            }
        }
    }
    /* Final drain */
    for (;;) {
        ssize_t n = recvfrom(r->fd, buf, sizeof(buf), MSG_DONTWAIT, NULL, NULL);
        if (n <= 0) break;
        int idx = atomic_fetch_add(&r->count, 1);
        if (r->indices && idx < r->max_indices && n >= 8) {
            uint32_t ri;
            memcpy(&ri, buf + 4, 4);
            r->indices[idx] = ri;
        }
    }
    return NULL;
}

static void async_recv_start(async_recv_t *r, pthread_t *t, int fd) {
    memset(r, 0, sizeof(*r));
    r->fd = fd;
    pthread_create(t, NULL, async_recv_thread, r);
}

static int async_recv_stop(async_recv_t *r, pthread_t t) {
    atomic_store(&r->stop, 1);
    pthread_join(t, NULL);
    return atomic_load(&r->count);
}

/* ---- Scenario 1: Normal mode burst throughput ---- */

static void test_normal_burst(void) {
    int listen_port = find_free_port();
    int remote_port = find_free_port();
    ASSERT(listen_port > 0);
    ASSERT(remote_port > 0);

    int server_fd = make_udp_socket(remote_port);
    ASSERT(server_fd >= 0);

    pid_t proxy = start_proxy("normal", listen_port, remote_port);
    ASSERT(proxy > 0);

    int client_fd = make_client_socket();
    ASSERT(client_fd >= 0);
    struct sockaddr_in proxy_addr = make_addr(listen_port);

    /* Handshake: send init */
    uint8_t init_buf[WG_INIT_SIZE];
    make_wg_init(init_buf, 0x1000);
    sendto(client_fd, init_buf, WG_INIT_SIZE, 0,
           (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));

    /* Server: receive junk + transformed init; skip junk, find init */
    usleep(200000);
    uint8_t rbuf[2048];
    int init_received = 0;
    for (int i = 0; i < TEST_JC + 5; i++) {
        int n = recv_one(server_fd, rbuf, sizeof(rbuf), 500);
        if (n == TEST_S1 + WG_INIT_SIZE) {
            uint32_t h;
            memcpy(&h, rbuf + TEST_S1, 4);
            if (h == TEST_H1) { init_received = 1; break; }
        }
    }
    ASSERT(init_received);
    drain_socket(server_fd);

    /* Start async receiver BEFORE sending burst */
    async_recv_t srv_recv;
    pthread_t srv_thread;
    async_recv_start(&srv_recv, &srv_thread, server_fd);

    /* Burst: send transport packets.
     * Pace sends in batches of 16 (half proxy BATCH_SIZE=32) with
     * micro-pause to avoid kernel UDP buffer overflow on loopback. */
    uint8_t transport[200];
    for (int i = 0; i < BURST_COUNT; i++) {
        make_wg_transport(transport, 0x2000, (uint64_t)i, 200);
        sendto(client_fd, transport, 200, 0,
               (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
        if ((i + 1) % 64 == 0) usleep(100);
    }

    /* Wait for proxy to flush everything */
    usleep(500000);
    int received = async_recv_stop(&srv_recv, srv_thread);
    double loss = 100.0 * (BURST_COUNT - received) / BURST_COUNT;

    fprintf(stderr, "          (sent=%d, recv=%d, loss=%.2f%%)\n", BURST_COUNT, received, loss);
    ASSERT(received >= BURST_COUNT * 99 / 100);

    stop_proxy(proxy);
    close(client_fd);
    close(server_fd);
}

/* ---- Scenario 2: Reverse mode bidirectional ---- */

typedef struct {
    int fd;
    struct sockaddr_in dest;
    int count;
    int direction; /* 0=c2s, 1=s2c */
} bidir_args_t;

static void *bidir_sender(void *arg) {
    bidir_args_t *a = (bidir_args_t *)arg;
    uint8_t buf[200];

    for (int i = 0; i < a->count; i++) {
        if (a->direction == 0) {
            /* Client sends AWG transport to proxy */
            make_awg_transport(buf, 0x2000, (uint64_t)i, 200);
        } else {
            /* Server sends WG transport to proxy */
            make_wg_transport(buf, 0x1000, (uint64_t)i, 200);
        }
        sendto(a->fd, buf, 200, 0,
               (struct sockaddr *)&a->dest, sizeof(a->dest));
        if ((i + 1) % 64 == 0) usleep(100);
    }
    return NULL;
}

static void test_reverse_bidirectional(void) {
    int listen_port = find_free_port();
    int remote_port = find_free_port();
    ASSERT(listen_port > 0);
    ASSERT(remote_port > 0);

    int server_fd = make_udp_socket(remote_port);
    ASSERT(server_fd >= 0);

    pid_t proxy = start_proxy("reverse", listen_port, remote_port);
    ASSERT(proxy > 0);

    int client_fd = make_client_socket();
    ASSERT(client_fd >= 0);
    struct sockaddr_in proxy_addr = make_addr(listen_port);

    /* Handshake: client sends AWG init to register with proxy */
    uint8_t awg_init[TEST_S1 + WG_INIT_SIZE];
    make_awg_init(awg_init, 0x1000);
    sendto(client_fd, awg_init, sizeof(awg_init), 0,
           (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
    usleep(200000);

    /* Drain the init at server */
    {
        uint8_t tmp[2048];
        for (int i = 0; i < 5; i++)
            recv_one(server_fd, tmp, sizeof(tmp), 200);
    }

    /* Now send bidirectional transport */
    bidir_args_t c2s_args = { .fd = client_fd, .dest = proxy_addr,
                               .count = BIDIR_COUNT, .direction = 0 };
    bidir_args_t s2c_args = { .fd = server_fd, .dest = { 0 },
                               .count = BIDIR_COUNT, .direction = 1 };

    /* Capture proxy's source address from server side */
    {
        uint8_t probe[TEST_S1 + WG_INIT_SIZE];
        make_awg_init(probe, 0x1001);
        sendto(client_fd, probe, sizeof(probe), 0,
               (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
        usleep(100000);
        uint8_t tmp[2048];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int got_addr = 0;
        for (int i = 0; i < 10; i++) {
            ssize_t n = recvfrom(server_fd, tmp, sizeof(tmp), MSG_DONTWAIT,
                                 (struct sockaddr *)&from, &fromlen);
            if (n > 0) { s2c_args.dest = from; got_addr = 1; }
        }
        ASSERT(got_addr);
        while (recvfrom(server_fd, tmp, sizeof(tmp), MSG_DONTWAIT, NULL, NULL) > 0);
    }

    /* Start async receivers BEFORE sending */
    async_recv_t srv_recv, cli_recv;
    pthread_t srv_thread, cli_thread;
    async_recv_start(&srv_recv, &srv_thread, server_fd);
    async_recv_start(&cli_recv, &cli_thread, client_fd);

    /* Launch sender threads */
    pthread_t t_c2s, t_s2c;
    pthread_create(&t_c2s, NULL, bidir_sender, &c2s_args);
    pthread_create(&t_s2c, NULL, bidir_sender, &s2c_args);
    pthread_join(t_c2s, NULL);
    pthread_join(t_s2c, NULL);

    /* Wait for proxy to flush */
    usleep(500000);
    int c2s_recv = async_recv_stop(&srv_recv, srv_thread);
    int s2c_recv = async_recv_stop(&cli_recv, cli_thread);

    double c2s_loss = 100.0 * (BIDIR_COUNT - c2s_recv) / BIDIR_COUNT;
    double s2c_loss = 100.0 * (BIDIR_COUNT - s2c_recv) / BIDIR_COUNT;

    fprintf(stderr, "          (c2s: %d/%d, s2c: %d/%d, loss=%.2f%%/%.2f%%)\n",
            c2s_recv, BIDIR_COUNT, s2c_recv, BIDIR_COUNT, c2s_loss, s2c_loss);
    ASSERT(c2s_recv >= BIDIR_COUNT * 99 / 100);
    ASSERT(s2c_recv >= BIDIR_COUNT * 99 / 100);

    stop_proxy(proxy);
    close(client_fd);
    close(server_fd);
}

/* ---- Scenario 3: Server mode multi-client ---- */

static void test_server_multiclient(void) {
    int listen_port = find_free_port();
    int remote_port = find_free_port();
    ASSERT(listen_port > 0);
    ASSERT(remote_port > 0);

    int server_fd = make_udp_socket(remote_port);
    ASSERT(server_fd >= 0);

    pid_t proxy = start_proxy("server", listen_port, remote_port);
    ASSERT(proxy > 0);

    struct sockaddr_in proxy_addr = make_addr(listen_port);
    int num_clients = 4;
    int client_fds[4];
    uint32_t sender_indices[4] = { 0x1000, 0x2000, 0x3000, 0x4000 };

    /* Create clients and do handshakes */
    for (int c = 0; c < num_clients; c++) {
        client_fds[c] = make_client_socket();
        ASSERT(client_fds[c] >= 0);

        /* Send AWG-format init (reverse/server mode: client sends AWG) */
        uint8_t awg_init[TEST_S1 + WG_INIT_SIZE];
        make_awg_init(awg_init, sender_indices[c]);
        sendto(client_fds[c], awg_init, sizeof(awg_init), 0,
               (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
        usleep(50000);
    }

    /* Server drains all handshakes/junk */
    usleep(300000);
    {
        uint8_t tmp[2048];
        while (recvfrom(server_fd, tmp, sizeof(tmp), MSG_DONTWAIT, NULL, NULL) > 0);
    }

    /* Get proxy's source addr from server perspective */
    struct sockaddr_in proxy_remote_addr;
    memset(&proxy_remote_addr, 0, sizeof(proxy_remote_addr));
    {
        /* Re-send one init to capture addr */
        uint8_t awg_init[TEST_S1 + WG_INIT_SIZE];
        make_awg_init(awg_init, sender_indices[0]);
        sendto(client_fds[0], awg_init, sizeof(awg_init), 0,
               (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
        usleep(100000);
        uint8_t tmp[2048];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(server_fd, tmp, sizeof(tmp), MSG_DONTWAIT,
                             (struct sockaddr *)&from, &fromlen);
        if (n > 0) {
            proxy_remote_addr = from;
        }
        while (recvfrom(server_fd, tmp, sizeof(tmp), MSG_DONTWAIT, NULL, NULL) > 0);
    }

    /* Start async receiver on server BEFORE sending */
    async_recv_t srv_recv;
    pthread_t srv_thread;
    async_recv_start(&srv_recv, &srv_thread, server_fd);

    /* Each client sends transport packets */
    int total_sent = 0;
    for (int c = 0; c < num_clients; c++) {
        uint8_t buf[200];
        for (int i = 0; i < MULTI_PER_CLIENT; i++) {
            make_awg_transport(buf, sender_indices[c], (uint64_t)i, 200);
            sendto(client_fds[c], buf, 200, 0,
                   (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
            total_sent++;
            if ((total_sent) % 64 == 0) usleep(100);
        }
    }

    /* Wait for proxy to flush */
    usleep(500000);
    int server_recv = async_recv_stop(&srv_recv, srv_thread);

    /* Start async receivers on each client for routing check */
    async_recv_t cli_recvs[4];
    pthread_t cli_threads[4];
    uint32_t cli_indices[4][200];
    for (int c = 0; c < num_clients; c++) {
        memset(&cli_recvs[c], 0, sizeof(async_recv_t));
        cli_recvs[c].fd = client_fds[c];
        cli_recvs[c].indices = cli_indices[c];
        cli_recvs[c].max_indices = 200;
        pthread_create(&cli_threads[c], NULL, async_recv_thread, &cli_recvs[c]);
    }

    /* Server sends responses addressed to each client via receiver_index */
    for (int c = 0; c < num_clients; c++) {
        uint8_t buf[200];
        for (int i = 0; i < 100; i++) {
            make_wg_transport(buf, sender_indices[c], (uint64_t)i, 200);
            sendto(server_fd, buf, 200, 0,
                   (struct sockaddr *)&proxy_remote_addr, sizeof(proxy_remote_addr));
        }
        usleep(10000);
    }

    /* Wait and collect */
    usleep(500000);
    int routing_ok = 1;
    int total_client_recv = 0;
    for (int c = 0; c < num_clients; c++) {
        int cnt = async_recv_stop(&cli_recvs[c], cli_threads[c]);
        total_client_recv += cnt;
        for (int i = 0; i < cnt && i < 200; i++) {
            if (cli_indices[c][i] != sender_indices[c]) {
                routing_ok = 0;
                break;
            }
        }
    }

    double loss = 100.0 * (total_sent - server_recv) / total_sent;
    fprintf(stderr, "          (total: %d/%d, loss=%.2f%%, client_recv=%d, routing: %s)\n",
            server_recv, total_sent, loss, total_client_recv,
            routing_ok ? "OK" : "FAIL");

    ASSERT(server_recv >= total_sent * 99 / 100);
    ASSERT(routing_ok);

    stop_proxy(proxy);
    for (int c = 0; c < num_clients; c++) close(client_fds[c]);
    close(server_fd);
}

/* ---- Scenario 4: Server-initiated rekey ---- */

static void test_server_rekey(void) {
    int listen_port = find_free_port();
    int remote_port = find_free_port();
    ASSERT(listen_port > 0);
    ASSERT(remote_port > 0);

    int server_fd = make_udp_socket(remote_port);
    ASSERT(server_fd >= 0);

    pid_t proxy = start_proxy("server", listen_port, remote_port);
    ASSERT(proxy > 0);

    int client_fd = make_client_socket();
    ASSERT(client_fd >= 0);
    struct sockaddr_in proxy_addr = make_addr(listen_port);

    /* Client sends AWG init to establish session */
    uint8_t awg_init[TEST_S1 + WG_INIT_SIZE];
    make_awg_init(awg_init, 0x5000);
    sendto(client_fd, awg_init, sizeof(awg_init), 0,
           (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
    usleep(300000);

    /* Get proxy's address from server side */
    struct sockaddr_in proxy_remote_addr;
    {
        uint8_t tmp[2048];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int got = 0;
        for (int i = 0; i < TEST_JC + 5; i++) {
            ssize_t n = recvfrom(server_fd, tmp, sizeof(tmp), MSG_DONTWAIT,
                                 (struct sockaddr *)&from, &fromlen);
            if (n > 0) {
                proxy_remote_addr = from;
                got = 1;
            }
        }
        ASSERT(got);
        while (recvfrom(server_fd, tmp, sizeof(tmp), MSG_DONTWAIT, NULL, NULL) > 0);
    }

    /* Server sends WG handshake init (server-initiated rekey) */
    uint8_t init[WG_INIT_SIZE];
    make_wg_init(init, 0x6000);
    sendto(server_fd, init, WG_INIT_SIZE, 0,
           (struct sockaddr *)&proxy_remote_addr, sizeof(proxy_remote_addr));

    /* Client should receive it (proxy transforms WG->AWG and routes to sole client) */
    usleep(300000);
    uint8_t rbuf[2048];
    int delivered = 0;
    for (int i = 0; i < 10; i++) {
        int n = recv_one(client_fd, rbuf, sizeof(rbuf), 500);
        if (n == TEST_S1 + WG_INIT_SIZE) {
            uint32_t h;
            memcpy(&h, rbuf + TEST_S1, 4);
            if (h == TEST_H1) { delivered = 1; break; }
        }
    }

    fprintf(stderr, "          (delivered: %d/1)\n", delivered);
    ASSERT(delivered);

    stop_proxy(proxy);
    close(client_fd);
    close(server_fd);
}

/* ---- Scenario 5: Concurrent handshakes ---- */

typedef struct {
    int fd;
    struct sockaddr_in dest;
    uint32_t sender_index;
} hs_thread_args_t;

static void *hs_sender(void *arg) {
    hs_thread_args_t *a = (hs_thread_args_t *)arg;
    uint8_t init[WG_INIT_SIZE];
    make_wg_init(init, a->sender_index);
    sendto(a->fd, init, WG_INIT_SIZE, 0,
           (struct sockaddr *)&a->dest, sizeof(a->dest));
    return NULL;
}

static void test_concurrent_handshakes(void) {
    int listen_port = find_free_port();
    int remote_port = find_free_port();
    ASSERT(listen_port > 0);
    ASSERT(remote_port > 0);

    int server_fd = make_udp_socket(remote_port);
    ASSERT(server_fd >= 0);

    pid_t proxy = start_proxy("normal", listen_port, remote_port);
    ASSERT(proxy > 0);

    struct sockaddr_in proxy_addr = make_addr(listen_port);
    int nthreads = 8;
    pthread_t threads[8];
    hs_thread_args_t args[8];

    /* Each thread gets its own socket to avoid contention */
    for (int i = 0; i < nthreads; i++) {
        args[i].fd = make_client_socket();
        ASSERT(args[i].fd >= 0);
        args[i].dest = proxy_addr;
        args[i].sender_index = 0xA000 + (uint32_t)i;
    }

    /* Launch all threads simultaneously */
    for (int i = 0; i < nthreads; i++)
        pthread_create(&threads[i], NULL, hs_sender, &args[i]);
    for (int i = 0; i < nthreads; i++)
        pthread_join(threads[i], NULL);

    /* Server receives: each init produces junk + transformed init.
     * Total expected: nthreads inits (+ junk).
     * Verify each init is valid (correct size, H1 type, no corruption). */
    usleep(500000);
    int valid = 0, corrupted = 0;
    uint8_t rbuf[2048];
    for (int i = 0; i < nthreads * (TEST_JC + 2); i++) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(server_fd, rbuf, sizeof(rbuf), MSG_DONTWAIT,
                             (struct sockaddr *)&from, &fromlen);
        if (n <= 0) break;
        if (n == TEST_S1 + WG_INIT_SIZE) {
            uint32_t h;
            memcpy(&h, rbuf + TEST_S1, 4);
            if (h == TEST_H1) {
                /* Verify sender_index is one of ours */
                uint32_t si;
                memcpy(&si, rbuf + TEST_S1 + 4, 4);
                int found = 0;
                for (int j = 0; j < nthreads; j++) {
                    if (si == args[j].sender_index) { found = 1; break; }
                }
                if (found) valid++;
                else corrupted++;
            }
        }
    }

    fprintf(stderr, "          (valid: %d/%d, corrupted: %d)\n", valid, nthreads, corrupted);
    ASSERT(valid == nthreads);
    ASSERT(corrupted == 0);

    for (int i = 0; i < nthreads; i++) close(args[i].fd);
    stop_proxy(proxy);
    close(server_fd);
}

/* ---- Scenario 6: Scale test — 100K / 1M / 10M through one proxy ---- */

static void test_scale(void) {
    int listen_port = find_free_port();
    int remote_port = find_free_port();
    ASSERT(listen_port > 0 && remote_port > 0);

    int server_fd = make_udp_socket(remote_port);
    ASSERT(server_fd >= 0);

    pid_t proxy = start_proxy("normal", listen_port, remote_port);
    ASSERT(proxy > 0);

    int client_fd = make_client_socket();
    ASSERT(client_fd >= 0);
    struct sockaddr_in proxy_addr = make_addr(listen_port);

    /* Warm up: handshake to establish client address in proxy */
    uint8_t init_buf[WG_INIT_SIZE];
    make_wg_init(init_buf, 0xF000);
    sendto(client_fd, init_buf, WG_INIT_SIZE, 0,
           (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
    usleep(300000);
    drain_socket(server_fd);

    long base_rss = get_rss_kb(proxy);
    int scales[] = { 100000, 1000000, 5000000 };
    const char *labels[] = { "100K", "1M", "5M" };
    double send_rates[3] = {0};

    /* Pre-fill transport packet (proxy doesn't inspect payload beyond type) */
    uint8_t transport[200];
    make_wg_transport(transport, 0x2000, 0, 200);

    for (int s = 0; s < 3; s++) {
        int count = scales[s];
        long rss_before = get_rss_kb(proxy);

        async_recv_t srv_recv;
        pthread_t srv_thread;
        async_recv_start(&srv_recv, &srv_thread, server_fd);

        int64_t t0 = now_us();

        for (int i = 0; i < count; i++) {
            sendto(client_fd, transport, 200, 0,
                   (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
            if ((i + 1) % 64 == 0) usleep(100);
        }

        int64_t t_sent = now_us();

        /* Wait for proxy to flush — scale with packet count */
        int wait_us = count >= 2500000 ? 2000000
                    : count >= 500000  ? 1500000
                    :                     500000;
        usleep(wait_us);

        int received = async_recv_stop(&srv_recv, srv_thread);
        drain_socket(server_fd); /* clean slate for next round */
        long rss_after = get_rss_kb(proxy);

        double send_s = (t_sent - t0) / 1e6;
        double pps = count / send_s;
        double mbps = (count * 200.0) / send_s / (1024 * 1024);
        send_rates[s] = pps;
        double loss = 100.0 * (count - received) / count;
        long rss_delta = rss_after - rss_before;

        fprintf(stderr, "          %3s: %d/%d loss=%.2f%%  "
                "%.1fK pkt/s (%.0f MB/s)  RSS: %ld→%ldKB (Δ%+ldKB)\n",
                labels[s], received, count, loss,
                pps / 1000, mbps, rss_before, rss_after, rss_delta);

        ASSERT(received >= count * 99 / 100); /* < 1% loss */
        ASSERT(rss_delta < 1024);             /* no leak: < 1MB growth */
    }

    /* Throughput consistency: 5M rate should be >= 80% of 100K rate */
    if (send_rates[0] > 0) {
        double ratio = send_rates[2] / send_rates[0];
        fprintf(stderr, "          throughput 5M/100K: %.0f%%\n", ratio * 100);
        ASSERT(ratio >= 0.80);
    }

    long final_rss = get_rss_kb(proxy);
    fprintf(stderr, "          memory: base=%ldKB final=%ldKB (Δ%+ldKB)\n",
            base_rss, final_rss, final_rss - base_rss);
    ASSERT(final_rss - base_rss < 1024);

    stop_proxy(proxy);
    close(client_fd);
    close(server_fd);
}

/* ---- Scenario 7: GSO on connected socket ---- */

#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif

static void test_gso_connected(void) {
    /* Verify kernel UDP GSO works with msg_name=NULL (connected socket).
     * Before the fix, send_gso() returned early when addr==NULL,
     * preventing GSO on the upload (c2s) path. */
    int recv_port = find_free_port();
    ASSERT(recv_port > 0);

    int recv_fd = make_udp_socket(recv_port);
    ASSERT(recv_fd >= 0);

    int send_fd = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT(send_fd >= 0);
    struct sockaddr_in dest = make_addr(recv_port);
    int ret = connect(send_fd, (struct sockaddr *)&dest, sizeof(dest));
    ASSERT(ret == 0);

    /* Build 4 same-size packets in one buffer */
    int seg_size = 200;
    int count = 4;
    uint8_t data[800];
    for (int i = 0; i < count; i++)
        memset(data + i * seg_size, (uint8_t)(i + 1), seg_size);

    /* sendmsg with UDP_SEGMENT cmsg, msg_name=NULL */
    struct iovec iov = { .iov_base = data, .iov_len = (size_t)(seg_size * count) };

    union {
        char buf[CMSG_SPACE(sizeof(uint16_t))];
        struct cmsghdr align;
    } cmsg_u;
    memset(&cmsg_u, 0, sizeof(cmsg_u));

    struct msghdr hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;
    hdr.msg_control = cmsg_u.buf;
    hdr.msg_controllen = sizeof(cmsg_u.buf);
    /* msg_name = NULL — connected socket, this is the scenario the fix enables */

    struct cmsghdr *cm = CMSG_FIRSTHDR(&hdr);
    cm->cmsg_level = IPPROTO_UDP;
    cm->cmsg_type = UDP_SEGMENT;
    cm->cmsg_len = CMSG_LEN(sizeof(uint16_t));
    uint16_t ss = (uint16_t)seg_size;
    memcpy(CMSG_DATA(cm), &ss, sizeof(ss));

    ssize_t sent = sendmsg(send_fd, &hdr, 0);
    if (sent < 0 && errno == ENOPROTOOPT) {
        fprintf(stderr, "          (kernel lacks UDP_SEGMENT, skipping)\n");
        close(send_fd);
        close(recv_fd);
        return;
    }
    ASSERT_EQ((int)sent, seg_size * count);

    /* Verify all 4 packets arrived individually */
    usleep(50000);
    int received = 0;
    uint8_t rbuf[256];
    for (int i = 0; i < count + 2; i++) {
        ssize_t n = recvfrom(recv_fd, rbuf, sizeof(rbuf), MSG_DONTWAIT, NULL, NULL);
        if (n <= 0) break;
        ASSERT_EQ((int)n, seg_size);
        ASSERT_EQ(rbuf[0], (uint8_t)(received + 1));
        received++;
    }
    ASSERT_EQ(received, count);

    fprintf(stderr, "          (GSO connected: sent %d segments, received %d)\n", count, received);
    close(send_fd);
    close(recv_fd);
}

/* ---- Scenario 8: GRO-enabled bidirectional throughput ---- */

static void test_gro_bidirectional(void) {
    /* Run proxy WITH GRO enabled and verify both directions work.
     * Before the fix: GRO was only on s2c (download).
     * After: GRO on both directions, GSO on both directions. */
    int listen_port = find_free_port();
    int remote_port = find_free_port();
    ASSERT(listen_port > 0 && remote_port > 0);

    int server_fd = make_udp_socket(remote_port);
    ASSERT(server_fd >= 0);

    pid_t proxy = start_proxy_with_gro("normal", listen_port, remote_port);
    ASSERT(proxy > 0);

    int client_fd = make_client_socket();
    ASSERT(client_fd >= 0);
    struct sockaddr_in proxy_addr = make_addr(listen_port);

    /* Handshake */
    uint8_t init_buf[WG_INIT_SIZE];
    make_wg_init(init_buf, 0xD000);
    sendto(client_fd, init_buf, WG_INIT_SIZE, 0,
           (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
    usleep(300000);

    /* Capture proxy's remote address from server side */
    struct sockaddr_in proxy_remote_addr;
    memset(&proxy_remote_addr, 0, sizeof(proxy_remote_addr));
    {
        uint8_t tmp[2048];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int got = 0;
        for (int i = 0; i < TEST_JC + 5; i++) {
            ssize_t n = recvfrom(server_fd, tmp, sizeof(tmp), MSG_DONTWAIT,
                                 (struct sockaddr *)&from, &fromlen);
            if (n > 0) { proxy_remote_addr = from; got = 1; }
        }
        ASSERT(got);
        drain_socket(server_fd);
    }

    int count = BIDIR_COUNT;

    /* c2s (upload): client → proxy → server */
    async_recv_t srv_recv;
    pthread_t srv_thread;
    async_recv_start(&srv_recv, &srv_thread, server_fd);

    int64_t t0_c2s = now_us();
    {
        uint8_t transport[200];
        for (int i = 0; i < count; i++) {
            make_wg_transport(transport, 0x2000, (uint64_t)i, 200);
            sendto(client_fd, transport, 200, 0,
                   (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
            if ((i + 1) % 64 == 0) usleep(100);
        }
    }
    int64_t t1_c2s = now_us();

    usleep(500000);
    int c2s_recv = async_recv_stop(&srv_recv, srv_thread);
    drain_socket(server_fd);

    /* s2c (download): server → proxy → client */
    drain_socket(client_fd);
    async_recv_t cli_recv;
    pthread_t cli_thread;
    async_recv_start(&cli_recv, &cli_thread, client_fd);

    int64_t t0_s2c = now_us();
    {
        uint8_t transport[200];
        for (int i = 0; i < count; i++) {
            make_awg_transport(transport, 0x2000, (uint64_t)i, 200);
            sendto(server_fd, transport, 200, 0,
                   (struct sockaddr *)&proxy_remote_addr, sizeof(proxy_remote_addr));
            if ((i + 1) % 64 == 0) usleep(100);
        }
    }
    int64_t t1_s2c = now_us();

    usleep(500000);
    int s2c_recv = async_recv_stop(&cli_recv, cli_thread);

    double c2s_time = (t1_c2s - t0_c2s) / 1e6;
    double s2c_time = (t1_s2c - t0_s2c) / 1e6;
    double c2s_loss = 100.0 * (count - c2s_recv) / count;
    double s2c_loss = 100.0 * (count - s2c_recv) / count;

    fprintf(stderr, "          c2s: %d/%d (%.2f%% loss, %.3fs)  "
            "s2c: %d/%d (%.2f%% loss, %.3fs)\n",
            c2s_recv, count, c2s_loss, c2s_time,
            s2c_recv, count, s2c_loss, s2c_time);

    ASSERT(c2s_recv >= count * 99 / 100);
    ASSERT(s2c_recv >= count * 99 / 100);

    stop_proxy(proxy);
    close(client_fd);
    close(server_fd);
}

/* ---- Scenario 9: Throughput benchmark (realistic MTU) ---- */

static void test_throughput_benchmark(void) {
    /* Measure real throughput in both directions with realistic WG packet size.
     * WG transport overhead: 32 bytes header + 16 bytes AEAD tag = 1432 bytes for MTU 1420.
     * Plus AWG overhead: S1 padding on init, H4 type swap on transport. */
    int listen_port = find_free_port();
    int remote_port = find_free_port();
    ASSERT(listen_port > 0 && remote_port > 0);

    int server_fd = make_udp_socket(remote_port);
    ASSERT(server_fd >= 0);

    pid_t proxy = start_proxy_with_gro("normal", listen_port, remote_port);
    ASSERT(proxy > 0);

    int client_fd = make_client_socket();
    ASSERT(client_fd >= 0);
    struct sockaddr_in proxy_addr = make_addr(listen_port);

    /* Handshake */
    uint8_t init_buf[WG_INIT_SIZE];
    make_wg_init(init_buf, 0xE000);
    sendto(client_fd, init_buf, WG_INIT_SIZE, 0,
           (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
    usleep(300000);

    struct sockaddr_in proxy_remote_addr;
    memset(&proxy_remote_addr, 0, sizeof(proxy_remote_addr));
    {
        uint8_t tmp[2048];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int got = 0;
        for (int i = 0; i < TEST_JC + 5; i++) {
            ssize_t n = recvfrom(server_fd, tmp, sizeof(tmp), MSG_DONTWAIT,
                                 (struct sockaddr *)&from, &fromlen);
            if (n > 0) { proxy_remote_addr = from; got = 1; }
        }
        ASSERT(got);
        drain_socket(server_fd);
    }

    /* Realistic WG transport packet: 1432 bytes (MTU 1420 - IP/UDP overhead absorbed) */
    int pkt_size = 1400;
    int duration_pkts = 500000;

    /* Pre-fill packet */
    uint8_t wg_pkt[1500];
    make_wg_transport(wg_pkt, 0x2000, 0, pkt_size);
    uint8_t awg_pkt[1500];
    make_awg_transport(awg_pkt, 0x2000, 0, pkt_size);

    /* c2s benchmark (upload) */
    async_recv_t srv_recv;
    pthread_t srv_thread;
    async_recv_start(&srv_recv, &srv_thread, server_fd);

    int64_t t0 = now_us();
    for (int i = 0; i < duration_pkts; i++) {
        sendto(client_fd, wg_pkt, pkt_size, 0,
               (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
        if ((i + 1) % 64 == 0) usleep(50);
    }
    int64_t t1 = now_us();

    usleep(1000000);
    int c2s_recv = async_recv_stop(&srv_recv, srv_thread);
    double c2s_sec = (t1 - t0) / 1e6;
    double c2s_mbps = (double)c2s_recv * pkt_size * 8.0 / c2s_sec / 1e6;

    /* s2c benchmark (download) */
    drain_socket(client_fd);
    drain_socket(server_fd);

    async_recv_t cli_recv;
    pthread_t cli_thread;
    async_recv_start(&cli_recv, &cli_thread, client_fd);

    int64_t t2 = now_us();
    for (int i = 0; i < duration_pkts; i++) {
        sendto(server_fd, awg_pkt, pkt_size, 0,
               (struct sockaddr *)&proxy_remote_addr, sizeof(proxy_remote_addr));
        if ((i + 1) % 64 == 0) usleep(50);
    }
    int64_t t3 = now_us();

    usleep(1000000);
    int s2c_recv = async_recv_stop(&cli_recv, cli_thread);
    double s2c_sec = (t3 - t2) / 1e6;
    double s2c_mbps = (double)s2c_recv * pkt_size * 8.0 / s2c_sec / 1e6;

    double c2s_loss = 100.0 * (duration_pkts - c2s_recv) / duration_pkts;
    double s2c_loss = 100.0 * (duration_pkts - s2c_recv) / duration_pkts;

    fprintf(stderr, "          packet size: %d bytes,  count: %d\n", pkt_size, duration_pkts);
    fprintf(stderr, "          c2s(upload):   %7.1f Mbit/s  (%d/%d, loss=%.2f%%)\n",
            c2s_mbps, c2s_recv, duration_pkts, c2s_loss);
    fprintf(stderr, "          s2c(download): %7.1f Mbit/s  (%d/%d, loss=%.2f%%)\n",
            s2c_mbps, s2c_recv, duration_pkts, s2c_loss);

    double ratio = (c2s_mbps > s2c_mbps) ? s2c_mbps / c2s_mbps : c2s_mbps / s2c_mbps;
    fprintf(stderr, "          parity: %.0f%%\n", ratio * 100);

    ASSERT(c2s_recv >= duration_pkts * 98 / 100);
    ASSERT(s2c_recv >= duration_pkts * 98 / 100);

    stop_proxy(proxy);
    close(client_fd);
    close(server_fd);
}

/* ---- Scenario 10: Site-to-site profile fallback (initiator) ---- */

/* Primary (v2) profile — deliberately distinct sizes/types from the v1
 * fallback so the mock server can tell which profile obfuscated each init. */
#define FB_PRI_S1 25
#define FB_PRI_S2 15
#define FB_PRI_H1 2000000001u
#define FB_PRI_H2 2000000002u
#define FB_PRI_H3 2000000003u
#define FB_PRI_H4 2000000004u

static pid_t start_proxy_fallback(int listen_port, int remote_port) {
    int src_port = find_free_port();
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        char lbuf[32], rbuf[64], sp[8];
        char jc[8], jmin[8], jmax[8];
        char s1[8], s2[8], h1[16], h2[16], h3[16], h4[16];
        char fs1[8], fs2[8], fh1[16], fh2[16], fh3[16], fh4[16];

        snprintf(lbuf, sizeof(lbuf), ":%d", listen_port);
        snprintf(rbuf, sizeof(rbuf), "127.0.0.1:%d", remote_port);

        setenv("AWG_LISTEN", lbuf, 1);
        setenv("AWG_REMOTE", rbuf, 1);
        setenv("AWG_MODE", "normal", 1);
        setenv("AWG_JC", itoa_buf(TEST_JC, jc), 1);
        setenv("AWG_JMIN", itoa_buf(TEST_JMIN, jmin), 1);
        setenv("AWG_JMAX", itoa_buf(TEST_JMAX, jmax), 1);
        /* Primary profile */
        setenv("AWG_S1", itoa_buf(FB_PRI_S1, s1), 1);
        setenv("AWG_S2", itoa_buf(FB_PRI_S2, s2), 1);
        setenv("AWG_H1", utoa_buf(FB_PRI_H1, h1), 1);
        setenv("AWG_H2", utoa_buf(FB_PRI_H2, h2), 1);
        setenv("AWG_H3", utoa_buf(FB_PRI_H3, h3), 1);
        setenv("AWG_H4", utoa_buf(FB_PRI_H4, h4), 1);
        /* v1 fallback profile (reuses the TEST_* constants) */
        setenv("AWG_FB_S1", itoa_buf(TEST_S1, fs1), 1);
        setenv("AWG_FB_S2", itoa_buf(TEST_S2, fs2), 1);
        setenv("AWG_FB_H1", utoa_buf(TEST_H1, fh1), 1);
        setenv("AWG_FB_H2", utoa_buf(TEST_H2, fh2), 1);
        setenv("AWG_FB_H3", utoa_buf(TEST_H3, fh3), 1);
        setenv("AWG_FB_H4", utoa_buf(TEST_H4, fh4), 1);
        setenv("AWG_FB_AFTER", "5", 1);
        setenv("AWG_SERVER_PUB", DUMMY_SERVER_PUB, 1);
        setenv("AWG_CLIENT_PUB", DUMMY_CLIENT_PUB, 1);
        setenv("AWG_LOG_LEVEL", "error", 1);
        setenv("AWG_TIMEOUT", "60", 1);
        setenv("AWG_NO_GRO", "1", 1);
        setenv("AWG_SRC_PORT", itoa_buf(src_port, sp), 1);

        execl(PROXY_BINARY, "awg-proxy", NULL);
        _exit(127);
    }
    usleep(200000);
    return pid;
}

static void test_s2s_fallback(void) {
    int listen_port = find_free_port();
    int remote_port = find_free_port();
    ASSERT(listen_port > 0 && remote_port > 0);

    int server_fd = make_udp_socket(remote_port);
    ASSERT(server_fd >= 0);

    pid_t proxy = start_proxy_fallback(listen_port, remote_port);
    ASSERT(proxy > 0);

    int client_fd = make_client_socket();
    ASSERT(client_fd >= 0);
    struct sockaddr_in proxy_addr = make_addr(listen_port);

    /* Simulate WireGuard retransmitting handshake inits while the remote stays
     * silent (as an old v1 peer would when it can't decode the v2 obfuscation).
     * Primary (v2) inits must appear first; after fb_after seconds of silence
     * the proxy must switch to the v1 fallback profile. */
    int primary_seen = 0, fallback_seen = 0;
    int primary_first = 0, fallback_after_primary = 0;
    uint8_t rbuf[2048];
    int64_t start = now_us();
    int64_t last_send = 0;
    uint32_t si = 0x7000;

    while ((now_us() - start) < 12000000) { /* run for 12s */
        int64_t nowt = now_us();
        if (nowt - last_send >= 500000) { /* resend init every 500ms */
            uint8_t init_buf[WG_INIT_SIZE];
            make_wg_init(init_buf, si++);
            sendto(client_fd, init_buf, WG_INIT_SIZE, 0,
                   (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
            last_send = nowt;
        }
        int n = recv_one(server_fd, rbuf, sizeof(rbuf), 100);
        if (n == FB_PRI_S1 + WG_INIT_SIZE) {
            uint32_t h;
            memcpy(&h, rbuf + FB_PRI_S1, 4);
            if (h == FB_PRI_H1) {
                primary_seen++;
                if (!fallback_seen) primary_first = 1;
            }
        } else if (n == TEST_S1 + WG_INIT_SIZE) {
            uint32_t h;
            memcpy(&h, rbuf + TEST_S1, 4);
            if (h == TEST_H1) {
                fallback_seen++;
                if (primary_seen) fallback_after_primary = 1;
            }
        }
    }

    fprintf(stderr, "          (primary inits=%d, fallback inits=%d, order primary->fallback=%s)\n",
            primary_seen, fallback_seen,
            (primary_first && fallback_after_primary) ? "yes" : "no");
    ASSERT(primary_seen > 0);       /* started on the primary profile */
    ASSERT(fallback_seen > 0);      /* switched to fallback after silence */
    ASSERT(fallback_after_primary); /* primary was tried before the fallback */

    stop_proxy(proxy);
    close(client_fd);
    close(server_fd);
}

/* ---- Main ---- */

/* ---- Scenario 11: AWG 3.0 header protection end to end ---- */

/* Recover the message type of a received v3 datagram: nonce is the first 12
 * bytes, the type is the first 4 keystream bytes XOR'd into the message. */
static uint32_t v3_unmask_type(const uint8_t *pkt, int pad) {
    uint8_t ks[CHACHA20_BLOCK_SIZE];
    uint32_t onwire, mask;
    chacha20_block(TEST_HP_KEY, pkt, 0, ks);
    memcpy(&onwire, pkt + pad, 4);
    memcpy(&mask, ks, 4);
    return onwire ^ mask;
}

/* Build what a real AWG 3.0 peer would put on the wire for a transport packet. */
static int make_awg_v3_transport(uint8_t *buf, uint32_t receiver_index,
                                 uint64_t counter, int msg_size) {
    for (int i = 0; i < TEST_V3_S4; i++)
        buf[i] = (uint8_t)(counter * 31 + i * 7 + 3);
    uint8_t *msg = buf + TEST_V3_S4;
    memset(msg, 0, (size_t)msg_size);
    uint32_t t = TEST_H4;
    memcpy(msg, &t, 4);
    memcpy(msg + 4, &receiver_index, 4);
    memcpy(msg + 8, &counter, 8);
    for (int i = 16; i < msg_size; i++)
        msg[i] = (uint8_t)(i ^ (counter & 0xFF));
    chacha20_xor(TEST_HP_KEY, buf, msg, AWG_HP_TRANSPORT_HDR);
    return TEST_V3_S4 + msg_size;
}

static void test_v3_header_protection(void) {
    int listen_port = find_free_port();
    int remote_port = find_free_port();
    ASSERT(listen_port > 0);
    ASSERT(remote_port > 0);

    int server_fd = make_udp_socket(remote_port);
    ASSERT(server_fd >= 0);

    pid_t proxy = start_proxy_v3("normal", listen_port, remote_port);
    ASSERT(proxy > 0);

    int client_fd = make_client_socket();
    ASSERT(client_fd >= 0);
    struct sockaddr_in proxy_addr = make_addr(listen_port);

    /* c2s handshake: the init must arrive padded, and its type must only be
     * readable after ChaCha20 — not in the clear. */
    uint8_t init_buf[WG_INIT_SIZE];
    make_wg_init(init_buf, 0x3000);
    sendto(client_fd, init_buf, WG_INIT_SIZE, 0,
           (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));

    usleep(200000);
    uint8_t rbuf[2048];
    int init_received = 0;
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    for (int i = 0; i < TEST_JC + 5; i++) {
        struct pollfd pfd = { .fd = server_fd, .events = POLLIN };
        if (poll(&pfd, 1, 500) <= 0) break;
        fromlen = sizeof(from);
        int n = (int)recvfrom(server_fd, rbuf, sizeof(rbuf), 0,
                              (struct sockaddr *)&from, &fromlen);
        if (n != TEST_V3_S1 + WG_INIT_SIZE) continue;
        uint32_t clear;
        memcpy(&clear, rbuf + TEST_V3_S1, 4);
        ASSERT(clear != TEST_H1); /* header must be encrypted on the wire */
        if (v3_unmask_type(rbuf, TEST_V3_S1) == TEST_H1) { init_received = 1; break; }
    }
    ASSERT(init_received);
    drain_socket(server_fd);

    /* c2s transport: burst, check loss and that every packet is protected */
    async_recv_t srv_recv;
    pthread_t srv_thread;
    async_recv_start(&srv_recv, &srv_thread, server_fd);

    uint8_t transport[200];
    const int v3_burst = BURST_COUNT / 2;
    for (int i = 0; i < v3_burst; i++) {
        make_wg_transport(transport, 0x4000, (uint64_t)i, 200);
        sendto(client_fd, transport, 200, 0,
               (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
        if ((i + 1) % 64 == 0) usleep(100);
    }
    usleep(500000);
    int received = async_recv_stop(&srv_recv, srv_thread);
    double loss = 100.0 * (v3_burst - received) / v3_burst;
    fprintf(stderr, "          (c2s sent=%d, recv=%d, loss=%.2f%%)\n",
            v3_burst, received, loss);
    ASSERT(received >= v3_burst * 99 / 100);
    drain_socket(server_fd);

    /* s2c: a genuine AWG 3.0 transport packet must come back out as plain WG */
    uint8_t awg[TEST_V3_S4 + 200];
    int awg_len = make_awg_v3_transport(awg, 0x3000, 77, 200);
    sendto(server_fd, awg, (size_t)awg_len, 0, (struct sockaddr *)&from, fromlen);

    int got_wg = 0;
    for (int i = 0; i < 5; i++) {
        int n = recv_one(client_fd, rbuf, sizeof(rbuf), 1000);
        if (n != 200) continue;
        uint32_t t;
        memcpy(&t, rbuf, 4);
        if (t != WG_TRANSPORT_DATA) continue;
        uint32_t ridx;
        uint64_t ctr;
        memcpy(&ridx, rbuf + 4, 4);
        memcpy(&ctr, rbuf + 8, 8);
        ASSERT_EQ(ridx, 0x3000u);
        ASSERT_EQ(ctr, 77u);
        for (int j = 16; j < 200; j++)
            ASSERT_EQ(rbuf[j], (uint8_t)(j ^ 77));
        got_wg = 1;
        break;
    }
    ASSERT(got_wg);

    stop_proxy(proxy);
    close(client_fd);
    close(server_fd);
}

/* ---- Scenario 12: IPv6 remote leg ---- */

/* Wait for the transformed handshake init on an already-bound mock server.
 * Junk and CPS packets precede it, so the size + H1 pair is the marker. */
static int await_awg_init(int fd, struct sockaddr_storage *from, socklen_t *fromlen,
                          int tries, int timeout_ms) {
    uint8_t rbuf[2048];
    for (int i = 0; i < tries; i++) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        if (poll(&pfd, 1, timeout_ms) <= 0) break;
        *fromlen = sizeof(*from);
        int n = (int)recvfrom(fd, rbuf, sizeof(rbuf), 0,
                              (struct sockaddr *)from, fromlen);
        if (n != TEST_S1 + WG_INIT_SIZE) continue;
        uint32_t h;
        memcpy(&h, rbuf + TEST_S1, 4);
        if (h == TEST_H1) return 1;
    }
    return 0;
}

static void test_ipv6_remote(void) {
    int listen_port = find_free_port();
    int remote_port = find_free_port();
    ASSERT(listen_port > 0);
    ASSERT(remote_port > 0);

    int server_fd = make_udp_socket6(remote_port);
    if (server_fd < 0) {
        fprintf(stderr, "          (no IPv6 loopback on this host, skipped)\n");
        return;
    }

    char rbuf_env[64];
    snprintf(rbuf_env, sizeof(rbuf_env), "[::1]:%d", remote_port);
    pid_t proxy = start_proxy_remote("normal", listen_port, rbuf_env, NULL);
    ASSERT(proxy > 0);

    int client_fd = make_client_socket();
    ASSERT(client_fd >= 0);
    struct sockaddr_in proxy_addr = make_addr(listen_port);

    /* Handshake over the v6 leg */
    uint8_t init_buf[WG_INIT_SIZE];
    make_wg_init(init_buf, 0x6000);
    sendto(client_fd, init_buf, WG_INIT_SIZE, 0,
           (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));

    struct sockaddr_storage from;
    socklen_t fromlen = sizeof(from);
    int init_received = await_awg_init(server_fd, &from, &fromlen, TEST_JC + 5, 1000);
    ASSERT(init_received);
    ASSERT_EQ(from.ss_family, AF_INET6);
    drain_socket(server_fd);

    /* c2s transport over IPv6 */
    async_recv_t srv_recv;
    pthread_t srv_thread;
    async_recv_start(&srv_recv, &srv_thread, server_fd);

    const int burst = BURST_COUNT / 4;
    uint8_t transport[200];
    for (int i = 0; i < burst; i++) {
        make_wg_transport(transport, 0x6001, (uint64_t)i, 200);
        sendto(client_fd, transport, 200, 0,
               (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
        if ((i + 1) % 64 == 0) usleep(100);
    }
    usleep(500000);
    int received = async_recv_stop(&srv_recv, srv_thread);
    fprintf(stderr, "          (c2s over IPv6: sent=%d, recv=%d)\n", burst, received);
    ASSERT(received >= burst * 99 / 100);

    /* s2c: an AWG transport packet from the v6 server must reach the client */
    uint8_t awg[200];
    make_awg_transport(awg, 0x6000, 4242, 200);
    sendto(server_fd, awg, sizeof(awg), 0, (struct sockaddr *)&from, fromlen);

    uint8_t rbuf[2048];
    int got_wg = 0;
    for (int i = 0; i < 5; i++) {
        int n = recv_one(client_fd, rbuf, sizeof(rbuf), 1000);
        if (n != 200) continue;
        uint32_t t;
        memcpy(&t, rbuf, 4);
        if (t != WG_TRANSPORT_DATA) continue;
        uint64_t ctr;
        memcpy(&ctr, rbuf + 8, 8);
        ASSERT_EQ(ctr, 4242u);
        got_wg = 1;
        break;
    }
    ASSERT(got_wg);

    stop_proxy(proxy);
    close(client_fd);
    close(server_fd);
}

/* ---- Scenario 13: Happy Eyeballs — IPv4 black hole, IPv6 answers ---- */

static void test_happy_eyeballs(void) {
    int listen_port = find_free_port();
    int remote_port = find_free_port();
    ASSERT(listen_port > 0);
    ASSERT(remote_port > 0);

    /* "localhost" carries both an A and an AAAA record in every stock
     * /etc/hosts, which is exactly the dual-stack name the probe is for. */
    int v6_fd = make_udp_socket6(remote_port);
    if (v6_fd < 0) {
        fprintf(stderr, "          (no IPv6 loopback on this host, skipped)\n");
        return;
    }
    /* Bound but never answering: a real black hole rather than an ICMP reject,
     * so the probe has to reach its timeout instead of an error. */
    int v4_blackhole = make_udp_socket(remote_port);
    ASSERT(v4_blackhole >= 0);

    char rbuf_env[64];
    snprintf(rbuf_env, sizeof(rbuf_env), "localhost:%d", remote_port);
    pid_t proxy = start_proxy_remote("normal", listen_port, rbuf_env, "200");
    ASSERT(proxy > 0);

    int client_fd = make_client_socket();
    ASSERT(client_fd >= 0);
    struct sockaddr_in proxy_addr = make_addr(listen_port);

    uint8_t init_buf[WG_INIT_SIZE];
    make_wg_init(init_buf, 0x7100);
    int64_t t0 = now_us();
    sendto(client_fd, init_buf, WG_INIT_SIZE, 0,
           (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));

    /* The replayed init must show up on the IPv6 socket well inside the
     * handshake retry interval — the point of the probe is not to wait for a
     * timeout. Allow generous slack for a loaded CI box. */
    struct sockaddr_storage from;
    socklen_t fromlen = sizeof(from);
    int init_received = await_awg_init(v6_fd, &from, &fromlen, TEST_JC + 5, 2000);
    int64_t elapsed_ms = (now_us() - t0) / 1000;
    fprintf(stderr, "          (IPv6 init after %lldms)\n", (long long)elapsed_ms);
    ASSERT(init_received);
    ASSERT_EQ(from.ss_family, AF_INET6);
    /* Must track AWG_HE_DELAY, not some poll tick that happens to be close.
     * WireGuard's own retry is 5s away, so a regression to "wait for the next
     * retransmit" would sail past this bound. */
    ASSERT(elapsed_ms < 1000);

    /* Answer over IPv6 so the probe locks onto that family... */
    uint8_t awg[200];
    make_awg_transport(awg, 0x7100, 1, 200);
    sendto(v6_fd, awg, sizeof(awg), 0, (struct sockaddr *)&from, fromlen);
    usleep(300000);
    drain_socket(v6_fd);
    drain_socket(v4_blackhole);

    /* ...and everything after the switch must go there, not to the v4 hole. */
    uint8_t transport[200];
    for (int i = 0; i < 200; i++) {
        make_wg_transport(transport, 0x7101, (uint64_t)i, 200);
        sendto(client_fd, transport, 200, 0,
               (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
    }
    usleep(400000);

    int on_v6 = 0, on_v4 = 0;
    uint8_t buf[2048];
    while (recvfrom(v6_fd, buf, sizeof(buf), MSG_DONTWAIT, NULL, NULL) > 0) on_v6++;
    while (recvfrom(v4_blackhole, buf, sizeof(buf), MSG_DONTWAIT, NULL, NULL) > 0) on_v4++;
    fprintf(stderr, "          (after switch: v6=%d, v4=%d)\n", on_v6, on_v4);
    ASSERT(on_v6 >= 200 * 99 / 100);
    ASSERT_EQ(on_v4, 0);

    stop_proxy(proxy);
    close(client_fd);
    close(v6_fd);
    close(v4_blackhole);
}

/* ---- Scenario 14: dual-stack hub — AWG_LISTEN on [::] serves both families ----
 *
 * This is the server-mode leg the MikroTik hub needs: the AWG side faces the
 * internet over IPv6 while the WireGuard side stays on the router's veth over
 * IPv4. A v4 client on the same socket arrives v4-mapped, so both must be
 * routed back by receiver_index to the family they came from. */
static void test_server_listen6(void) {
    int listen_port = find_free_port();
    int remote_port = find_free_port();
    ASSERT(listen_port > 0);
    ASSERT(remote_port > 0);

    int probe = make_client_socket6();
    if (probe < 0) {
        fprintf(stderr, "          (no IPv6 on this host, skipped)\n");
        return;
    }
    close(probe);

    /* WG side stays IPv4 — that is the veth inside the router. */
    int server_fd = make_udp_socket(remote_port);
    ASSERT(server_fd >= 0);

    char lbuf[32], rbuf[64];
    snprintf(lbuf, sizeof(lbuf), "[::]:%d", listen_port);
    snprintf(rbuf, sizeof(rbuf), "127.0.0.1:%d", remote_port);
    pid_t proxy = start_proxy_listen_remote("server", lbuf, rbuf, NULL);
    ASSERT(proxy > 0);

    int fd6 = make_client_socket6();
    int fd4 = make_client_socket();
    ASSERT(fd6 >= 0 && fd4 >= 0);
    struct sockaddr_in6 hub6 = make_addr6(listen_port);
    struct sockaddr_in  hub4 = make_addr(listen_port);

    const uint32_t idx6 = 0x6a00, idx4 = 0x4a00;
    uint8_t awg_init[TEST_S1 + WG_INIT_SIZE];

    make_awg_init(awg_init, idx6);
    ASSERT(sendto(fd6, awg_init, sizeof(awg_init), 0,
                  (struct sockaddr *)&hub6, sizeof(hub6)) > 0);
    make_awg_init(awg_init, idx4);
    ASSERT(sendto(fd4, awg_init, sizeof(awg_init), 0,
                  (struct sockaddr *)&hub4, sizeof(hub4)) > 0);
    usleep(300000);

    /* Both handshakes must have arrived at the WG server as plain WG. */
    struct sockaddr_in proxy_src;
    memset(&proxy_src, 0, sizeof(proxy_src));
    int seen6 = 0, seen4 = 0;
    for (;;) {
        uint8_t buf[2048];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(server_fd, buf, sizeof(buf), MSG_DONTWAIT,
                             (struct sockaddr *)&from, &fromlen);
        if (n <= 0) break;
        proxy_src = from;
        if (n != WG_INIT_SIZE) continue;   /* junk/CPS ahead of the init */
        uint32_t t, si;
        memcpy(&t, buf, 4);
        memcpy(&si, buf + 4, 4);
        if (t != WG_HANDSHAKE_INIT) continue;
        if (si == idx6) seen6 = 1;
        if (si == idx4) seen4 = 1;
    }
    fprintf(stderr, "          (WG init seen: v6-client=%d, v4-client=%d)\n", seen6, seen4);
    ASSERT(seen6);
    ASSERT(seen4);
    ASSERT(proxy_src.sin_port != 0);

    /* Reply to each by receiver_index; each must come back on its own family. */
    uint8_t wg[200];
    for (int i = 0; i < 50; i++) {
        make_wg_transport(wg, idx6, (uint64_t)i, 200);
        sendto(server_fd, wg, 200, 0, (struct sockaddr *)&proxy_src, sizeof(proxy_src));
        make_wg_transport(wg, idx4, (uint64_t)i, 200);
        sendto(server_fd, wg, 200, 0, (struct sockaddr *)&proxy_src, sizeof(proxy_src));
    }
    usleep(400000);

    /* Count only packets carrying the receiver_index this client owns: a
     * session-table mix-up would show up as a delivery to the wrong family. */
    int got6 = 0, got4 = 0, misrouted = 0;
    struct { int fd; uint32_t mine; uint32_t other; int *hit; } side[2] = {
        { fd6, idx6, idx4, &got6 }, { fd4, idx4, idx6, &got4 }
    };
    for (int s = 0; s < 2; s++) {
        uint8_t buf[2048];
        ssize_t n;
        while ((n = recvfrom(side[s].fd, buf, sizeof(buf), MSG_DONTWAIT, NULL, NULL)) > 0) {
            if (n != 200) continue;         /* junk ahead of the stream */
            uint32_t ri;
            memcpy(&ri, buf + 4, 4);
            if (ri == side[s].mine) (*side[s].hit)++;
            else if (ri == side[s].other) misrouted++;
        }
    }
    fprintf(stderr, "          (routed back: v6=%d, v4=%d of 50 each, misrouted=%d)\n",
            got6, got4, misrouted);
    ASSERT(got6 >= 50 * 9 / 10);
    ASSERT(got4 >= 50 * 9 / 10);
    ASSERT_EQ(misrouted, 0);

    stop_proxy(proxy);
    close(fd6);
    close(fd4);
    close(server_fd);
}

int main(void) {
    fprintf(stderr, "=== stress tests ===\n");
    RUN_TEST(normal_burst);
    RUN_TEST(reverse_bidirectional);
    RUN_TEST(server_multiclient);
    RUN_TEST(server_rekey);
    RUN_TEST(concurrent_handshakes);
    RUN_TEST(scale);
    RUN_TEST(gso_connected);
    RUN_TEST(gro_bidirectional);
    RUN_TEST(throughput_benchmark);
    RUN_TEST(s2s_fallback);
    RUN_TEST(v3_header_protection);
    RUN_TEST(ipv6_remote);
    RUN_TEST(happy_eyeballs);
    RUN_TEST(server_listen6);
    TEST_MAIN_END();
}
