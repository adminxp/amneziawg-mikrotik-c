#include "proxy.h"
#include "cps.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>

/* ---- Helpers ---- */

static void fill_h4_ring(proxy_t *p) {
    if (p->cfg->h4.min == p->cfg->h4.max) {
        uint32_t v = p->cfg->h4.min;
        for (int i = 0; i < H4_RING_SIZE; i++)
            p->h4_ring[i] = v;
        return;
    }
    int span = (int)(p->cfg->h4.max - p->cfg->h4.min + 1);
    for (int i = 0; i < H4_RING_SIZE; i++)
        p->h4_ring[i] = p->cfg->h4.min + (uint32_t)fastrand_intn(&p->rng, span);
}

static inline uint32_t pick_h4(proxy_t *p) {
    uint32_t v = p->h4_ring[p->h4_idx];
    p->h4_idx++;
    if (p->h4_idx == 0)
        fill_h4_ring(p);
    return v;
}

static int resolve_addr(const char *host, uint16_t port, struct sockaddr_in *addr) {
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);

    if (inet_pton(AF_INET, host, &addr->sin_addr) == 1)
        return 0;

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
    struct addrinfo *res;
    if (getaddrinfo(host, NULL, &hints, &res) != 0)
        return -1;
    memcpy(addr, res->ai_addr, sizeof(*addr));
    addr->sin_port = htons(port);
    freeaddrinfo(res);
    return 0;
}

static int parse_host_port(const char *s, char *host, int hostmax, uint16_t *port) {
    const char *colon = NULL;
    int len = 0;
    while (s[len]) len++;
    for (int i = len - 1; i >= 0; i--) {
        if (s[i] == ':') { colon = s + i; break; }
    }
    if (!colon) return -1;

    int hlen = (int)(colon - s);
    if (hlen >= hostmax) return -1;
    memcpy(host, s, hlen);
    host[hlen] = '\0';

    *port = 0;
    for (const char *p = colon + 1; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        *port = *port * 10 + (*p - '0');
    }
    return 0;
}

static int create_udp_socket(int blocking) {
    int flags = SOCK_DGRAM | SOCK_CLOEXEC;
    if (!blocking) flags |= SOCK_NONBLOCK;
    return socket(AF_INET, flags, 0);
}

static void set_socket_buffers(int fd, int size) {
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
}

static void set_busy_poll(int fd, int usec) {
    if (usec <= 0) return;
    setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &usec, sizeof(usec));
#ifdef SO_BUSY_POLL_BUDGET
    int budget = BATCH_SIZE;
    setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL_BUDGET, &budget, sizeof(budget));
#endif
}

static void set_thread_affinity(int cpu, const char *name) {
    if (cpu < 0) return;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) == 0) {
        char buf[12];
        const char *parts[] = { name, " pinned to cpu", u32_to_str(buf, cpu) };
        log_infon(parts, 3);
    }
}

static void log_socket_buffers(int fd, const awg_config_t *cfg, const char *label) {
    int r = 0, w = 0;
    socklen_t len = sizeof(r);
    getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &r, &len);
    len = sizeof(w);
    getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &w, &len);
    char rb[12], wb[12], reqb[12];
    const char *parts[] = { label, " socket buf: requested=",
        u32_to_str(reqb, cfg->socket_buf / 1024), "KB, actual read=",
        u32_to_str(rb, r / 1024), "KB write=",
        u32_to_str(wb, w / 1024), "KB" };
    log_infon(parts, 8);
}

static int dial_remote(proxy_t *p, int blocking) {
    if (resolve_addr(p->remote_host, p->remote_port, &p->remote_addr) < 0) {
        log_error("resolve failed");
        return -1;
    }

    int fd = create_udp_socket(blocking);
    if (fd < 0) return -1;

    if (p->local_port > 0) {
        struct sockaddr_in local = { .sin_family = AF_INET, .sin_port = htons(p->local_port) };
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
            close(fd);
            return -1;
        }
    }

    if (connect(fd, (struct sockaddr *)&p->remote_addr, sizeof(p->remote_addr)) < 0) {
        close(fd);
        return -1;
    }

    set_socket_buffers(fd, p->cfg->socket_buf);
    set_busy_poll(fd, p->cfg->busy_poll);
    return fd;
}

/* ---- GRO/GSO ---- */

static int enable_gro(int fd) {
    int val = 1;
    return setsockopt(fd, IPPROTO_UDP, UDP_GRO, &val, sizeof(val)) == 0;
}

static void init_gro_state(proxy_t *p) {
    p->gro_iov.iov_base = p->gro_buf;
    p->gro_iov.iov_len = GRO_BUF_SIZE;
    memset(&p->gro_hdr, 0, sizeof(p->gro_hdr));
    p->gro_hdr.msg_iov = &p->gro_iov;
    p->gro_hdr.msg_iovlen = 1;
    p->gro_hdr.msg_control = p->gro_cmsg;
    p->gro_hdr.msg_controllen = sizeof(p->gro_cmsg);
}

/* recv_gro: blocking recvmsg with GRO. Returns total bytes, sets *seg_size.
 * seg_size=0 means no coalescing (single packet). */
static int recv_gro(proxy_t *p, int fd, int *seg_size) {
    p->gro_hdr.msg_controllen = sizeof(p->gro_cmsg);
    p->gro_hdr.msg_flags = 0;

    ssize_t n = recvmsg(fd, &p->gro_hdr, 0);
    if (n <= 0) {
        *seg_size = 0;
        return (int)n;
    }

    *seg_size = 0;
    /* Parse cmsg for UDP_GRO segment size */
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(&p->gro_hdr); cm; cm = CMSG_NXTHDR(&p->gro_hdr, cm)) {
        if (cm->cmsg_level == IPPROTO_UDP && cm->cmsg_type == UDP_SEGMENT) {
            uint16_t ss;
            memcpy(&ss, CMSG_DATA(cm), sizeof(ss));
            *seg_size = ss;
            break;
        }
    }

    return (int)n;
}

/* send_gso: send a prefix of same-size packets via one sendmsg with UDP_SEGMENT.
 * Returns number of packets sent, or negative errno on error. */
static int send_gso(int fd, struct iovec *iovecs, int count,
                    struct sockaddr_in *addr) {
    if (count <= 1) return 0;

    /* Find longest prefix of same-size packets */
    int seg_size = (int)iovecs[0].iov_len;
    int gso_count = 1;
    while (gso_count < count && (int)iovecs[gso_count].iov_len == seg_size)
        gso_count++;
    /* Last segment may be shorter per GSO spec */
    if (gso_count < count && (int)iovecs[gso_count].iov_len < seg_size)
        gso_count++;
    if (gso_count <= 1) return 0;

    /* Build cmsg with UDP_SEGMENT */
    union {
        char buf[CMSG_SPACE(sizeof(uint16_t))];
        struct cmsghdr align;
    } cmsg_u;
    memset(&cmsg_u, 0, sizeof(cmsg_u));

    struct msghdr hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.msg_iov = iovecs;
    hdr.msg_iovlen = gso_count;
    hdr.msg_control = cmsg_u.buf;
    hdr.msg_controllen = sizeof(cmsg_u.buf);

    struct cmsghdr *cm = CMSG_FIRSTHDR(&hdr);
    cm->cmsg_level = IPPROTO_UDP;
    cm->cmsg_type = UDP_SEGMENT;
    cm->cmsg_len = CMSG_LEN(sizeof(uint16_t));
    uint16_t ss = (uint16_t)seg_size;
    memcpy(CMSG_DATA(cm), &ss, sizeof(ss));

    if (addr) {
        hdr.msg_name = addr;
        hdr.msg_namelen = sizeof(struct sockaddr_in);
    }

    ssize_t ret = sendmsg(fd, &hdr, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (ret < 0) return -errno;
    return gso_count;
}

/* ---- Init ---- */

int proxy_init(proxy_t *p, awg_config_t *cfg,
               const char *listen_str, const char *remote_str, int src_port) {
    memset(p, 0, sizeof(*p));
    p->cfg = cfg;
    p->listen_fd = -1;
    atomic_store(&p->remote_fd, -1);
    p->signal_fd = -1;
    p->timer_fd = -1;
    p->gso_ok = 1;

    /* Parse listen address */
    char host[256];
    uint16_t port;
    if (parse_host_port(listen_str, host, sizeof(host), &port) < 0)
        return -1;
    memset(&p->listen_addr, 0, sizeof(p->listen_addr));
    p->listen_addr.sin_family = AF_INET;
    p->listen_addr.sin_port = htons(port);
    if (host[0] && inet_pton(AF_INET, host, &p->listen_addr.sin_addr) != 1)
        p->listen_addr.sin_addr.s_addr = INADDR_ANY;

    /* Parse remote address */
    if (parse_host_port(remote_str, p->remote_host, sizeof(p->remote_host), &p->remote_port) < 0)
        return -1;

    if (src_port > 0) {
        p->local_port = src_port;
    } else {
        p->auto_src_port = 1;
    }

    /* Init PRNG */
    uint64_t seed;
    int ufd = open("/dev/urandom", O_RDONLY);
    if (ufd >= 0) {
        read(ufd, &seed, 8);
        close(ufd);
    } else {
        seed = (uint64_t)(uintptr_t)p ^ 0xDEADBEEFCAFEULL;
    }
    fastrand_init(&p->rng, seed);

    /* Pre-allocate junk buffers */
    if (cfg->jc > 0 && cfg->jmax > 0) {
        p->junk_buf = (uint8_t *)malloc(cfg->jc * cfg->jmax);
        p->junk_sizes = (int *)malloc(cfg->jc * sizeof(int));
        if (!p->junk_buf || !p->junk_sizes) return -1;
    }

    /* Init H4 ring */
    fill_h4_ring(p);

    /* Init batch I/O structures — invariant fields set once */
    int prefix = cfg->s4;
    for (int i = 0; i < BATCH_SIZE; i++) {
        /* recv_c2s: listen socket, capture addr only on first msg */
        p->recv_c2s.iovecs[i].iov_base = p->recv_c2s.bufs[i] + prefix;
        p->recv_c2s.iovecs[i].iov_len = BUF_SIZE;
        p->recv_c2s.msgs[i].msg_hdr.msg_iov = &p->recv_c2s.iovecs[i];
        p->recv_c2s.msgs[i].msg_hdr.msg_iovlen = 1;
    }
    /* Capture client addr only from first packet in batch */
    p->recv_c2s.msgs[0].msg_hdr.msg_name = &p->recv_c2s.addrs[0];
    p->recv_c2s.msgs[0].msg_hdr.msg_namelen = sizeof(struct sockaddr_in);

    for (int i = 0; i < BATCH_SIZE; i++) {
        /* send_s2c: to listen socket with client addr */
        p->send_s2c.msgs[i].msg_hdr.msg_iov = &p->send_s2c.iovecs[i];
        p->send_s2c.msgs[i].msg_hdr.msg_iovlen = 1;
        p->send_s2c.msgs[i].msg_hdr.msg_name = &p->send_s2c.addrs[i];
        p->send_s2c.msgs[i].msg_hdr.msg_namelen = sizeof(struct sockaddr_in);

        /* send_c2s: to remote, connected — no addr needed */
        p->send_c2s.msgs[i].msg_hdr.msg_iov = &p->send_c2s.iovecs[i];
        p->send_c2s.msgs[i].msg_hdr.msg_iovlen = 1;
    }

    /* recv_s2c: remote socket, connected — no addr needed */
    for (int i = 0; i < BATCH_SIZE; i++) {
        p->recv_s2c.iovecs[i].iov_base = p->recv_s2c.bufs[i];
        p->recv_s2c.iovecs[i].iov_len = BUF_SIZE + 256;
        p->recv_s2c.msgs[i].msg_hdr.msg_iov = &p->recv_s2c.iovecs[i];
        p->recv_s2c.msgs[i].msg_hdr.msg_iovlen = 1;
    }

    /* Pre-fill S4 headroom with random data */
    if (prefix > 0) {
        for (int i = 0; i < BATCH_SIZE; i++)
            fastrand_fill(&p->rng, p->recv_c2s.bufs[i], prefix);
    }

    /* Init GRO state */
    init_gro_state(p);

    return 0;
}

/* ---- Send helpers ---- */

static int send_packet(int fd, const void *data, int len) {
    return (int)send(fd, data, len, MSG_DONTWAIT | MSG_NOSIGNAL);
}

static void send_junk_and_cps(proxy_t *p, int fd) {
    awg_config_t *cfg = p->cfg;

    /* CPS packets */
    int ncps = cps_generate_all(cfg->cps, &p->cps_counter,
                                 p->cps_bufs, p->cps_lens);
    for (int i = 0; i < ncps; i++)
        send_packet(fd, p->cps_bufs[i], p->cps_lens[i]);

    /* Junk packets */
    if (cfg->jc > 0 && cfg->jmax > 0) {
        fastrand_fill(&p->rng, p->junk_buf, cfg->jc * cfg->jmax);
        int njunk = generate_junk(cfg, p->junk_buf, p->junk_sizes);
        int off = 0;
        for (int i = 0; i < njunk; i++) {
            send_packet(fd, p->junk_buf + off, p->junk_sizes[i]);
            off += p->junk_sizes[i];
        }
    }
}

/* ---- Send batch with GSO ---- */

static void send_batch_gso(proxy_t *p, int fd, struct mmsghdr *msgs,
                           struct iovec *iovecs, int nsend,
                           struct sockaddr_in *addr) {
    int sent = 0;
    if (p->gso_ok && nsend > 1) {
        int n = send_gso(fd, iovecs, nsend, addr);
        if (n < 0) {
            int err = -n;
            if (err == ENOPROTOOPT || err == EIO)
                p->gso_ok = 0;
        } else {
            sent = n;
        }
    }
    if (sent < nsend) {
        sendmmsg(fd, msgs + sent, nsend - sent, MSG_DONTWAIT | MSG_NOSIGNAL);
    }
}

/* ---- c2s thread ---- */

__attribute__((hot))
static void *c2s_thread(void *arg) {
    proxy_t *p = (proxy_t *)arg;
    awg_config_t *cfg = p->cfg;
    set_thread_affinity(cfg->cpu_c2s, "c2s");
    int prefix = cfg->s4;
    int prev_nrecv = BATCH_SIZE;

    while (!atomic_load(&p->stopped)) {
        int remote_fd = atomic_load(&p->remote_fd);

        /* Reset iov_len only for previously used elements */
        for (int i = 0; i < prev_nrecv; i++) {
            p->recv_c2s.iovecs[i].iov_len = BUF_SIZE;
        }
        p->recv_c2s.msgs[0].msg_hdr.msg_namelen = sizeof(struct sockaddr_in);

        int nrecv = recvmmsg(p->listen_fd, p->recv_c2s.msgs, BATCH_SIZE,
                             MSG_WAITFORONE, NULL);
        if (nrecv <= 0) {
            if (atomic_load(&p->stopped)) break;
            if (errno == EINTR) continue;
            continue;
        }
        prev_nrecv = nrecv;

        atomic_store(&p->last_active, 1);

        /* Check client address from first packet */
        if (p->recv_c2s.addrs[0].sin_family == AF_INET) {
            if (!atomic_load(&p->has_client) ||
                p->client_addr.sin_addr.s_addr != p->recv_c2s.addrs[0].sin_addr.s_addr ||
                p->client_addr.sin_port != p->recv_c2s.addrs[0].sin_port) {
                p->client_addr = p->recv_c2s.addrs[0];
                atomic_store(&p->has_client, 1);
                char abuf[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &p->client_addr.sin_addr, abuf, sizeof(abuf));
                char pbuf[12];
                const char *parts[] = { "client: ", abuf, ":", u32_to_str(pbuf, ntohs(p->client_addr.sin_port)) };
                log_infon(parts, 4);

                if (p->auto_src_port) {
                    int cp = ntohs(p->recv_c2s.addrs[0].sin_port);
                    if (p->local_port != cp) {
                        p->local_port = cp;
                        int old_fd = atomic_load(&p->remote_fd);
                        if (old_fd >= 0) {
                            char pb2[12];
                            log_info2("src port: auto, reconnecting port=",
                                      u32_to_str(pb2, cp));
                            /* Signal reconnect needed; s2c thread will handle */
                            atomic_store(&p->reconnect_needed, 1);
                            shutdown(old_fd, SHUT_RDWR);
                        }
                    }
                }
            }
        }

        remote_fd = atomic_load(&p->remote_fd);
        if (remote_fd < 0) continue;

        /* Build sendmmsg batch */
        int nsend = 0;

        for (int i = 0; i < nrecv; i++) {
            int n = (int)p->recv_c2s.msgs[i].msg_len;
            if (n <= 0) continue;

            uint8_t *data = p->recv_c2s.bufs[i] + prefix;

            /* Transport data fast-path */
            if (n >= WG_TRANSPORT_MIN) {
                uint32_t h;
                memcpy(&h, data, 4);
                if (h == WG_TRANSPORT_DATA) {
                    if (!cfg->h4_noop) {
                        uint32_t h4 = pick_h4(p);
                        memcpy(data, &h4, 4);
                    }
                    int total = prefix > 0 ? prefix + n : n;
                    uint8_t *base = prefix > 0 ? p->recv_c2s.bufs[i] : data;
                    p->send_c2s.iovecs[nsend].iov_base = base;
                    p->send_c2s.iovecs[nsend].iov_len = total;
                    nsend++;
                    continue;
                }
            }

            /* Handshake slow path: flush batch first */
            if (nsend > 0) {
                send_batch_gso(p, remote_fd, p->send_c2s.msgs,
                               p->send_c2s.iovecs, nsend, NULL);
                nsend = 0;
            }

            int out_len, sendJunk;
            uint8_t *out = transform_outbound(p->recv_c2s.bufs[i], prefix, n,
                                               cfg, fastrand_u64(&p->rng),
                                               &out_len, &sendJunk);

            if (sendJunk && !atomic_load(&p->handshake_done)) {
                send_junk_and_cps(p, remote_fd);
                if (send_packet(remote_fd, out, out_len) >= 0)
                    atomic_store(&p->handshake_done, 1);
                continue;
            }

            /* Non-junk handshake */
            p->send_c2s.iovecs[nsend].iov_base = out;
            p->send_c2s.iovecs[nsend].iov_len = out_len;
            nsend++;
        }

        if (nsend > 0) {
            send_batch_gso(p, remote_fd, p->send_c2s.msgs,
                           p->send_c2s.iovecs, nsend, NULL);
        }
    }

    return NULL;
}

/* ---- s2c thread ---- */

__attribute__((hot))
static inline int process_s2c_pkt(proxy_t *p, uint8_t *pkt, int n,
                                   struct iovec *send_iovecs,
                                   struct sockaddr_in *send_addrs,
                                   int *nsend) {
    awg_config_t *cfg = p->cfg;
    int s4 = cfg->s4;

    /* Transport fast-path with precomputed ambiguity check */
    if (n >= s4 + WG_TRANSPORT_MIN) {
        if (!cfg->transport_size_ambiguous ||
            (n != cfg->init_total && n != cfg->resp_total && n != cfg->cookie_total)) {
            uint32_t h;
            memcpy(&h, pkt + s4, 4);
            if (hrange_contains(&cfg->h4, h)) {
                uint32_t wt = WG_TRANSPORT_DATA;
                memcpy(pkt + s4, &wt, 4);
                int idx = *nsend;
                send_iovecs[idx].iov_base = pkt + s4;
                send_iovecs[idx].iov_len = n - s4;
                send_addrs[idx] = p->client_addr;
                (*nsend)++;
                return 1;
            }
        }
    }

    /* Slow path: handshake or unknown */
    int out_len;
    uint8_t *out = transform_inbound(pkt, n, cfg, &out_len);
    if (!out) return 0;

    int idx = *nsend;
    send_iovecs[idx].iov_base = out;
    send_iovecs[idx].iov_len = out_len;
    send_addrs[idx] = p->client_addr;
    (*nsend)++;
    return 1;
}

static int do_reconnect(proxy_t *p) {
    int old_fd = atomic_load(&p->remote_fd);
    if (old_fd >= 0) {
        close(old_fd);
        atomic_store(&p->remote_fd, -1);
    }

    char abuf[64];
    inet_ntop(AF_INET, &p->remote_addr.sin_addr, abuf, sizeof(abuf));
    log_info2("reconnecting to ", abuf);

    int fd = dial_remote(p, 1);
    if (fd < 0) return -1;

    atomic_store(&p->remote_fd, fd);
    atomic_store(&p->last_active, 1);
    atomic_store(&p->handshake_done, 0);
    atomic_store(&p->has_client, 0);
    atomic_store(&p->reconnect_needed, 0);

    log_info2("reconnected to ", abuf);
    return fd;
}

__attribute__((hot))
static void *s2c_thread(void *arg) {
    proxy_t *p = (proxy_t *)arg;
    set_thread_affinity(p->cfg->cpu_s2c, "s2c");
    int reconnect_backoff = 1;
    int prev_nrecv = BATCH_SIZE;

    /* Try to enable GRO on initial remote fd */
    int remote_fd = atomic_load(&p->remote_fd);
    if (remote_fd >= 0) {
        p->gro_enabled = enable_gro(remote_fd);
        if (p->gro_enabled)
            log_info("s2c: UDP GRO enabled");
    }

    while (!atomic_load(&p->stopped)) {
        remote_fd = atomic_load(&p->remote_fd);

        /* Reconnect if needed */
        if (remote_fd < 0 || atomic_load(&p->reconnect_needed)) {
            struct timespec slp = { .tv_sec = reconnect_backoff };
            nanosleep(&slp, NULL);
            if (atomic_load(&p->stopped)) break;

            int new_fd = do_reconnect(p);
            if (new_fd < 0) {
                reconnect_backoff *= 2;
                if (reconnect_backoff > 30) reconnect_backoff = 30;
                log_error("reconnect failed, backing off");
                continue;
            }
            reconnect_backoff = 1;
            remote_fd = new_fd;
            /* Re-enable GRO on new fd */
            p->gro_enabled = enable_gro(remote_fd);
            if (p->gro_enabled)
                log_info("s2c: UDP GRO re-enabled");
            prev_nrecv = BATCH_SIZE;
            continue;
        }

        /* === Receive === */
        int nsend = 0;

        if (p->gro_enabled) {
            /* GRO path: one recvmsg returning coalesced buffer */
            int seg_size;
            int n = recv_gro(p, remote_fd, &seg_size);
            if (n <= 0) {
                if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                    if (!atomic_load(&p->stopped)) {
                        log_info("remote read error, will reconnect");
                        atomic_store(&p->reconnect_needed, 1);
                    }
                }
                continue;
            }

            atomic_store(&p->last_active, 1);
            reconnect_backoff = 1;

            if (!atomic_load(&p->has_client)) continue;

            if (seg_size > 0 && n > seg_size) {
                /* Coalesced: split buffer by seg_size */
                for (int off = 0; off < n && nsend < BATCH_SIZE; off += seg_size) {
                    int end = off + seg_size;
                    if (end > n) end = n;
                    int pkt_len = end - off;
                    process_s2c_pkt(p, p->gro_buf + off, pkt_len,
                                    p->send_s2c.iovecs, p->send_s2c.addrs, &nsend);
                }
            } else {
                /* Single packet */
                process_s2c_pkt(p, p->gro_buf, n,
                                p->send_s2c.iovecs, p->send_s2c.addrs, &nsend);
            }
        } else {
            /* Non-GRO path: recvmmsg with MSG_WAITFORONE */
            for (int i = 0; i < prev_nrecv; i++)
                p->recv_s2c.iovecs[i].iov_len = BUF_SIZE + 256;

            int nrecv = recvmmsg(remote_fd, p->recv_s2c.msgs, BATCH_SIZE,
                                 MSG_WAITFORONE, NULL);
            if (nrecv <= 0) {
                if (nrecv == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                    if (!atomic_load(&p->stopped)) {
                        log_info("remote read error, will reconnect");
                        atomic_store(&p->reconnect_needed, 1);
                    }
                }
                continue;
            }
            prev_nrecv = nrecv;

            atomic_store(&p->last_active, 1);
            reconnect_backoff = 1;

            if (!atomic_load(&p->has_client)) continue;

            for (int i = 0; i < nrecv; i++) {
                int n = (int)p->recv_s2c.msgs[i].msg_len;
                if (n <= 0) continue;
                process_s2c_pkt(p, p->recv_s2c.bufs[i], n,
                                p->send_s2c.iovecs, p->send_s2c.addrs, &nsend);
            }
        }

        /* === Send === */
        if (nsend > 0) {
            send_batch_gso(p, p->listen_fd, p->send_s2c.msgs,
                           p->send_s2c.iovecs, nsend, &p->send_s2c.addrs[0]);
        }
    }

    return NULL;
}

/* ---- Main ---- */

int proxy_run(proxy_t *p) {
    awg_config_t *cfg = p->cfg;

    /* Create listen socket (blocking for c2s thread) */
    p->listen_fd = create_udp_socket(1);
    if (p->listen_fd < 0) {
        log_error("socket create failed");
        return -1;
    }
    int opt = 1;
    setsockopt(p->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (bind(p->listen_fd, (struct sockaddr *)&p->listen_addr,
             sizeof(p->listen_addr)) < 0) {
        log_error("bind failed");
        return -1;
    }
    set_socket_buffers(p->listen_fd, cfg->socket_buf);
    set_busy_poll(p->listen_fd, cfg->busy_poll);
    log_socket_buffers(p->listen_fd, cfg, "listen");

    /* Connect to remote (blocking for s2c thread) */
    int rfd = dial_remote(p, 1);
    if (rfd < 0) {
        log_error("initial connect failed");
        return -1;
    }
    atomic_store(&p->remote_fd, rfd);
    log_socket_buffers(rfd, cfg, "remote");
    atomic_store(&p->last_active, 1);

    /* Signal handling */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    p->signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (p->signal_fd < 0) {
        log_error("signalfd failed");
        return -1;
    }

    /* Timer fd for timeout checks (every 5 seconds) */
    p->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (p->timer_fd < 0) {
        log_error("timerfd failed");
        return -1;
    }
    struct itimerspec ts = {
        .it_interval = { .tv_sec = 5 },
        .it_value = { .tv_sec = 5 },
    };
    timerfd_settime(p->timer_fd, 0, &ts, NULL);

    /* Launch c2s and s2c threads */
    pthread_t t_c2s, t_s2c;
    pthread_create(&t_c2s, NULL, c2s_thread, p);
    pthread_create(&t_s2c, NULL, s2c_thread, p);

    /* Main thread: signal handling + timeout */
    int timeout_secs = cfg->timeout > 0 ? cfg->timeout : 180;
    int checks_needed = timeout_secs / 5;
    if (checks_needed < 1) checks_needed = 1;
    int inactive_count = 0;

    /* Epoll for signal + timer only */
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        log_error("epoll_create failed");
        atomic_store(&p->stopped, 1);
        goto join;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = p->signal_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, p->signal_fd, &ev);
    ev.data.fd = p->timer_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, p->timer_fd, &ev);

    struct epoll_event events[2];

    while (!atomic_load(&p->stopped)) {
        int nev = epoll_wait(epfd, events, 2, 1000);
        if (nev < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < nev; i++) {
            int fd = events[i].data.fd;

            if (fd == p->signal_fd) {
                struct signalfd_siginfo si;
                read(p->signal_fd, &si, sizeof(si));
                log_info("shutting down");
                atomic_store(&p->stopped, 1);
                break;
            }

            if (fd == p->timer_fd) {
                uint64_t expirations;
                read(p->timer_fd, &expirations, sizeof(expirations));
                if (atomic_exchange(&p->last_active, 0)) {
                    inactive_count = 0;
                } else {
                    inactive_count++;
                    if (inactive_count >= checks_needed) {
                        log_info("remote timeout, triggering reconnect");
                        int rfd2 = atomic_load(&p->remote_fd);
                        if (rfd2 >= 0) {
                            atomic_store(&p->reconnect_needed, 1);
                            shutdown(rfd2, SHUT_RDWR);
                        }
                        inactive_count = 0;
                    }
                }
            }
        }
    }

    close(epfd);

join:
    /* Stop threads by shutting down sockets */
    atomic_store(&p->stopped, 1);
    if (p->listen_fd >= 0)
        shutdown(p->listen_fd, SHUT_RDWR);
    rfd = atomic_load(&p->remote_fd);
    if (rfd >= 0)
        shutdown(rfd, SHUT_RDWR);

    pthread_join(t_c2s, NULL);
    pthread_join(t_s2c, NULL);

    /* Cleanup */
    rfd = atomic_load(&p->remote_fd);
    if (rfd >= 0) close(rfd);
    if (p->listen_fd >= 0) close(p->listen_fd);
    if (p->signal_fd >= 0) close(p->signal_fd);
    if (p->timer_fd >= 0) close(p->timer_fd);
    free(p->junk_buf);
    free(p->junk_sizes);

    return 0;
}
