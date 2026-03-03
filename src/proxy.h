#ifndef AWG_PROXY_H
#define AWG_PROXY_H

#include "transform.h"
#include "fastrand.h"
#include <stdint.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define BUF_SIZE       1500
#define BATCH_SIZE     128
#define H4_RING_SIZE   256
#define GRO_BUF_SIZE   65536

#ifndef UDP_GRO
#define UDP_GRO        104
#endif
#ifndef UDP_SEGMENT
#define UDP_SEGMENT    103
#endif
#ifndef IPPROTO_UDP
#define IPPROTO_UDP    17
#endif

typedef struct {
    awg_config_t *cfg;

    /* Addresses */
    struct sockaddr_in listen_addr;
    struct sockaddr_in remote_addr;
    char remote_host[256];   /* original hostname for re-resolve */
    uint16_t remote_port;

    /* Client address (last seen) — written by c2s thread, read by s2c thread */
    struct sockaddr_in client_addr;
    _Atomic int has_client;

    /* Sockets */
    int listen_fd;
    _Atomic int remote_fd;

    /* State — shared between threads */
    _Atomic int stopped;
    _Atomic int handshake_done;
    _Atomic int last_active;          /* activity flag */
    int auto_src_port;
    int local_port;           /* desired src port, 0 = kernel assigns */

    /* CPS counter */
    uint32_t cps_counter;

    /* Pre-allocated junk buffers */
    uint8_t *junk_buf;        /* jc * jmax bytes */
    int *junk_sizes;          /* jc entries */

    /* PRNG */
    fastrand_t rng;

    /* H4 ring buffer */
    uint32_t h4_ring[H4_RING_SIZE];
    uint8_t h4_idx;

    /* Batch I/O buffers — c2s direction */
    struct {
        uint8_t bufs[BATCH_SIZE][BUF_SIZE + 256];
        struct mmsghdr msgs[BATCH_SIZE];
        struct iovec iovecs[BATCH_SIZE];
        struct sockaddr_in addrs[BATCH_SIZE];
    } recv_c2s;

    struct {
        struct mmsghdr msgs[BATCH_SIZE];
        struct iovec iovecs[BATCH_SIZE];
    } send_c2s;

    /* Batch I/O buffers — s2c direction (non-GRO path) */
    struct {
        uint8_t bufs[BATCH_SIZE][BUF_SIZE + 256];
        struct mmsghdr msgs[BATCH_SIZE];
        struct iovec iovecs[BATCH_SIZE];
    } recv_s2c;

    struct {
        uint8_t bufs[BATCH_SIZE][BUF_SIZE + 256];
        struct mmsghdr msgs[BATCH_SIZE];
        struct iovec iovecs[BATCH_SIZE];
        struct sockaddr_in addrs[BATCH_SIZE];
    } send_s2c;

    /* GRO state — s2c direction */
    int gro_enabled;
    uint8_t gro_buf[GRO_BUF_SIZE];
    struct iovec gro_iov;
    struct msghdr gro_hdr;
    uint8_t gro_cmsg[32];

    /* GSO state */
    int gso_ok;

    /* Signal/timer fds */
    int signal_fd;
    int timer_fd;

    /* CPS packet buffers */
    uint8_t cps_bufs[5][1500];
    int cps_lens[5];

    /* Reconnect coordination */
    _Atomic int reconnect_needed;

} proxy_t;

/* Initialize proxy. Returns 0 on success. */
int proxy_init(proxy_t *p, awg_config_t *cfg,
               const char *listen_str, const char *remote_str, int src_port);

/* Run proxy event loop. Blocks until signal or error. Returns 0 on clean shutdown. */
int proxy_run(proxy_t *p);

#endif
