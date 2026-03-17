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

static int make_client_socket(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
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

static pid_t start_proxy(const char *mode, int listen_port, int remote_port) {
    /* Find a free port for proxy's source to avoid auto_src_port reconnect race */
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
        setenv("AWG_NO_GRO", "1", 1);
        setenv("AWG_SRC_PORT", itoa_buf(src_port, sp), 1);

        execl(PROXY_BINARY, "awg-proxy", NULL);
        _exit(127);
    }
    usleep(200000); /* 200ms for proxy startup */
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

/* ---- Main ---- */

int main(void) {
    fprintf(stderr, "=== stress tests ===\n");
    RUN_TEST(normal_burst);
    RUN_TEST(reverse_bidirectional);
    RUN_TEST(server_multiclient);
    RUN_TEST(server_rekey);
    RUN_TEST(concurrent_handshakes);
    RUN_TEST(scale);
    TEST_MAIN_END();
}
