#ifndef AWG_PROXY_H
#define AWG_PROXY_H

#include "transform.h"
#include "fastrand.h"
#include <stdint.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define BUF_SIZE       AWG_PACKET_BUF_SIZE
#define BATCH_SIZE     32
#define H4_RING_SIZE   4096
#define GRO_BUF_SIZE   65536

/* Session table for server mode */
#define SESSION_TABLE_SIZE 4096
#define SESSION_TABLE_MASK (SESSION_TABLE_SIZE - 1)

typedef struct {
    uint32_t sender_index;
    struct sockaddr_in addr;
    _Atomic int peer_slot;
    _Atomic int prof;      /* obfuscation profile this client speaks */
    _Atomic int valid;
} session_entry_t;

/* Per-source profile cache (server mode, fallback chain enabled). Direct
 * mapped and advisory only: a miss just costs one extra decode attempt, so it
 * is written by the c2s thread alone and needs no synchronisation. */
#define PROF_CACHE_SIZE 64
#define PROF_CACHE_MASK (PROF_CACHE_SIZE - 1)

typedef struct {
    uint32_t key;   /* 0 = empty */
    uint8_t prof;
} prof_cache_entry_t;

static inline uint32_t prof_cache_key(const struct sockaddr_in *a) {
    uint32_t k = (uint32_t)a->sin_addr.s_addr ^ ((uint32_t)a->sin_port << 16)
               ^ (uint32_t)a->sin_port;
    return k ? k : 1u;
}


#ifndef UDP_GRO
#define UDP_GRO        104
#endif
#ifndef UDP_SEGMENT
#define UDP_SEGMENT    103
#endif
#ifndef IPPROTO_UDP
#define IPPROTO_UDP    17
#endif
#ifndef IP_MTU_DISCOVER
#define IP_MTU_DISCOVER   10
#endif
#ifndef IP_PMTUDISC_DONT
#define IP_PMTUDISC_DONT  0
#endif
#ifndef IPPROTO_IPV6
#define IPPROTO_IPV6      41
#endif
#ifndef IPV6_MTU_DISCOVER
#define IPV6_MTU_DISCOVER  23
#endif
#ifndef IPV6_PMTUDISC_DONT
#define IPV6_PMTUDISC_DONT 0
#endif
#ifndef IPV6_V6ONLY
#define IPV6_V6ONLY       26
#endif

/* One resolved remote endpoint. len == 0 means "this family has no record". */
typedef struct {
    struct sockaddr_storage sa;
    socklen_t len;
} awg_addr_t;

typedef struct {
    /* === Hot fields === */
    awg_config_t *cfg;              /* 8B */
    int listen_fd;                  /* 4B */
    _Atomic int remote_fd;          /* 4B */
    _Atomic int remote_fd2;         /* 4B — Happy Eyeballs probe socket, -1 = none */
    _Atomic int stopped;            /* 4B */
    _Atomic int has_client;         /* 4B */
    _Atomic int last_active;        /* 4B */
    _Atomic int last_remote_rx;     /* 4B — set when data received from remote */
    _Atomic int reconnect_needed;   /* 4B */
    /* First-event flags (one per connection — touched once after start/reconnect) */
    _Atomic uint8_t fe_init_seen;       /* WG handshake init seen from client */
    _Atomic uint8_t fe_init_sent;       /* AWG handshake init sent to remote */
    _Atomic uint8_t fe_remote_pkt;      /* any packet received from remote */
    _Atomic uint8_t fe_resp_received;   /* AWG handshake response received */
    _Atomic uint8_t fe_resp_sent;       /* WG handshake response delivered to client */
    _Atomic uint8_t fe_transport_c2s;   /* first transport packet to remote */
    _Atomic uint8_t fe_transport_s2c;   /* first transport packet to client */
    uint8_t _pad_fe;
    struct sockaddr_in client_addr; /* 16B */
    int gso_ok;                     /* 4B */
    int gro_enabled;                /* 4B */
    int c2s_headroom;               /* 4B — bytes reserved before recv_c2s data */
    int s2c_headroom;               /* 4B — bytes reserved before recv_s2c data */
    uint16_t h4_idx[AWG_MAX_PROFILES];
    fastrand_t rng;                 /* 8B — cold paths (init, H4 ring) */
    fastrand_t rng_c2s;             /* 8B — c2s thread only */
    fastrand_t rng_s2c;             /* 8B — s2c thread only */

    /* === Warm: batch I/O === */

    /* Batch I/O buffers — c2s direction */
    struct {
        uint8_t bufs[BATCH_SIZE][BUF_SIZE + AWG_PACKET_HEADROOM];
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
        uint8_t bufs[BATCH_SIZE][BUF_SIZE + AWG_PACKET_HEADROOM];
        struct mmsghdr msgs[BATCH_SIZE];
        struct iovec iovecs[BATCH_SIZE];
    } recv_s2c;

    struct {
        struct mmsghdr msgs[BATCH_SIZE];
        struct iovec iovecs[BATCH_SIZE];
        struct sockaddr_in addrs[BATCH_SIZE];
    } send_s2c;

    /* === Cold: init/reconnect === */
    struct sockaddr_in listen_addr;
    /* Remote is the only leg that speaks both families. The socket is
     * connect()ed and every send goes through send(), so nothing on the hot
     * path ever looks at these. */
    awg_addr_t remote;              /* endpoint behind remote_fd */
    awg_addr_t remote_alt;          /* endpoint behind remote_fd2 (probe) */
    char remote_host[256];
    uint16_t remote_port;
    int auto_src_port;
    int local_port;

    /* Happy Eyeballs: c2s keeps a copy of the first packet it sends so the
     * probe can replay it on the other family. Published by the release store
     * to he_sent; he_pkt/he_pkt_len are plain fields behind it. he_evfd wakes
     * the probe the moment that happens, so the head start is measured from
     * the send and not from whenever poll() next returns. */
    _Atomic int he_sent;
    uint64_t he_sent_ms;
    int he_pkt_len;
    int he_evfd;
    uint8_t he_pkt[BUF_SIZE + AWG_PACKET_HEADROOM];

    uint32_t cps_counter;
    uint8_t *junk_buf;
    int *junk_sizes;

    int signal_fd;
    int timer_fd;

    uint8_t cps_bufs[5][1500];
    int cps_lens[5];

    /* GRO state — s2c */
    uint8_t gro_buf[GRO_BUF_SIZE];
    struct iovec gro_iov;
    struct msghdr gro_hdr;
    uint8_t gro_cmsg[32];

    /* GRO state — c2s */
    uint8_t gro_buf_c2s[GRO_BUF_SIZE];
    struct iovec gro_iov_c2s;
    struct msghdr gro_hdr_c2s;
    uint8_t gro_cmsg_c2s[32];
    struct sockaddr_in gro_addr_c2s;
    int gro_enabled_c2s;

    /* === Large cold arrays === */
    prof_cache_entry_t prof_cache[PROF_CACHE_SIZE];
    uint32_t h4_ring[AWG_MAX_PROFILES][H4_RING_SIZE];
    session_entry_t sessions[SESSION_TABLE_SIZE];

} proxy_t;

/* Session table operations */
static inline session_entry_t *session_get_entry(proxy_t *p, uint32_t index) {
    uint32_t slot = index & SESSION_TABLE_MASK;
    for (int i = 0; i < 4; i++) {
        uint32_t s = (slot + i) & SESSION_TABLE_MASK;
        if (atomic_load_explicit(&p->sessions[s].valid, memory_order_acquire) &&
            p->sessions[s].sender_index == index)
            return &p->sessions[s];
    }
    return NULL;
}

static inline session_entry_t *session_put_prof(proxy_t *p, uint32_t index,
                                                struct sockaddr_in *addr, int prof) {
    uint32_t slot = index & SESSION_TABLE_MASK;
    for (int i = 0; i < 4; i++) {
        uint32_t s = (slot + i) & SESSION_TABLE_MASK;
        if (!atomic_load_explicit(&p->sessions[s].valid, memory_order_acquire) ||
            p->sessions[s].sender_index == index) {
            int preserve_peer = atomic_load_explicit(&p->sessions[s].valid, memory_order_relaxed) &&
                                p->sessions[s].sender_index == index;
            p->sessions[s].sender_index = index;
            p->sessions[s].addr = *addr;
            if (!preserve_peer)
                atomic_store_explicit(&p->sessions[s].peer_slot, -1, memory_order_relaxed);
            atomic_store_explicit(&p->sessions[s].prof, prof, memory_order_relaxed);
            atomic_store_explicit(&p->sessions[s].valid, 1, memory_order_release);
            return &p->sessions[s];
        }
    }
    p->sessions[slot].sender_index = index;
    p->sessions[slot].addr = *addr;
    atomic_store_explicit(&p->sessions[slot].peer_slot, -1, memory_order_relaxed);
    atomic_store_explicit(&p->sessions[slot].prof, prof, memory_order_relaxed);
    atomic_store_explicit(&p->sessions[slot].valid, 1, memory_order_release);
    return &p->sessions[slot];
}

static inline void session_put(proxy_t *p, uint32_t index, struct sockaddr_in *addr) {
    session_put_prof(p, index, addr, 0);
}

static inline struct sockaddr_in *session_get(proxy_t *p, uint32_t index) {
    session_entry_t *entry = session_get_entry(p, index);
    return entry ? &entry->addr : NULL;
}

static inline void session_set_peer_slot(session_entry_t *entry, int peer_slot) {
    if (entry)
        atomic_store_explicit(&entry->peer_slot, peer_slot, memory_order_relaxed);
}

static inline int session_get_peer_slot(session_entry_t *entry) {
    return entry ? atomic_load_explicit(&entry->peer_slot, memory_order_relaxed) : -1;
}

/* Find client entry when only one unique client exists in session table */
static inline session_entry_t *session_find_sole_entry(proxy_t *p) {
    session_entry_t *found = NULL;
    for (int i = 0; i < SESSION_TABLE_SIZE; i++) {
        if (!atomic_load_explicit(&p->sessions[i].valid, memory_order_acquire))
            continue;
        if (!found) {
            found = &p->sessions[i];
        } else if (found->addr.sin_addr.s_addr != p->sessions[i].addr.sin_addr.s_addr ||
                   found->addr.sin_port != p->sessions[i].addr.sin_port) {
            return NULL; /* multiple clients */
        }
    }
    return found;
}

/* Find client address when only one unique client exists in session table */
static inline struct sockaddr_in *session_find_sole_client(proxy_t *p) {
    session_entry_t *entry = session_find_sole_entry(p);
    return entry ? &entry->addr : NULL;
}

/* Re-check DNS records (A and AAAA) for host: 0 = cur still present,
 * 1 = cur gone, -1 = resolve error. Walks every record to tolerate
 * round-robin DNS; a record matches only when family and address both do. */
int resolve_addr_check(const char *host, const struct sockaddr *cur);

/* Split "host:port" / "[v6addr]:port". A bare IPv6 literal is rejected: the
 * port is mandatory, so brackets are the only unambiguous form. */
int parse_host_port(const char *s, char *host, int hostmax, uint16_t *port);

/* Largest WireGuard MTU whose full-size transport packet still fits a
 * 1500-byte path:
 *   IP(20|40) + UDP(8) + S4 + WG hdr(16) + round_up(mtu,16) + tag(16) <= 1500
 * The IPv6 header is 20 bytes longer, so the same config needs a lower MTU —
 * the reason wg-quick picks 1420 for IPv4 and 1400 for IPv6. */
static inline int awg_max_wg_mtu(int s4, int ipv6) {
    int room = (ipv6 ? 1420 : 1440) - s4;
    if (room < 0) room = 0;
    return (room / 16) * 16;
}

/* Segment size of a coalesced UDP read, 0 when the kernel did not coalesce. */
int gro_seg_size(const struct msghdr *hdr);

/* Initialize proxy. Returns 0 on success. */
int proxy_init(proxy_t *p, awg_config_t *cfg,
               const char *listen_str, const char *remote_str, int src_port);

/* Run proxy event loop. Blocks until signal or error. Returns 0 on clean shutdown. */
int proxy_run(proxy_t *p);

#endif
