#include "proxy.h"
#include "cps.h"
#include "log.h"
#include "base64.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef VERSION
#define VERSION "dev"
#endif

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static int parse_int_str(const char *s) {
    int v = 0, neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}

static uint32_t parse_uint32(const char *s) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (uint32_t)(*s - '0'); s++; }
    return v;
}

/* Parse "MIN-MAX" or "VALUE" into hrange_t */
static int parse_hrange(const char *s, hrange_t *r) {
    const char *dash = NULL;
    for (const char *p = s; *p; p++) {
        if (*p == '-') { dash = p; break; }
    }
    if (!dash) {
        r->min = r->max = parse_uint32(s);
    } else {
        char tmp[32];
        int hlen = (int)(dash - s);
        if (hlen >= 32) return -1;
        memcpy(tmp, s, hlen);
        tmp[hlen] = '\0';
        r->min = parse_uint32(tmp);
        r->max = parse_uint32(dash + 1);
        if (r->min > r->max) return -1;
    }
    return 0;
}

static const char *getenv_required(const char *name, int *err_count) {
    const char *v = getenv(name);
    if (!v || !v[0]) {
        const char *parts[] = { name, " is not set" };
        log_msgn("FATAL: ", parts, 2);
        (*err_count)++;
        return "";
    }
    return v;
}

static void fatal(const char *msg) {
    log_msg("FATAL: ", msg);
    _exit(1);
}

static proxy_t g_proxy;
static awg_config_t g_config;
/* CPS templates storage */
static cps_template_t g_cps_storage[5];

int main(void) {
    int errs = 0;
    awg_config_t *cfg = &g_config;
    memset(cfg, 0, sizeof(*cfg));

    /* Required env vars */
    const char *listen_str = getenv_required("AWG_LISTEN", &errs);
    const char *remote_str = getenv_required("AWG_REMOTE", &errs);
    const char *jc_str  = getenv_required("AWG_JC", &errs);
    const char *jmin_str = getenv_required("AWG_JMIN", &errs);
    const char *jmax_str = getenv_required("AWG_JMAX", &errs);
    const char *s1_str  = getenv_required("AWG_S1", &errs);
    const char *s2_str  = getenv_required("AWG_S2", &errs);
    const char *h1_str  = getenv_required("AWG_H1", &errs);
    const char *h2_str  = getenv_required("AWG_H2", &errs);
    const char *h3_str  = getenv_required("AWG_H3", &errs);
    const char *h4_str  = getenv_required("AWG_H4", &errs);
    const char *spub_str = getenv_required("AWG_SERVER_PUB", &errs);
    const char *cpub_str = getenv_required("AWG_CLIENT_PUB", &errs);

    if (errs > 0) {
        fatal("missing required environment variables (see above)");
    }

    /* Parse integers */
    cfg->jc = parse_int_str(jc_str);
    cfg->jmin = parse_int_str(jmin_str);
    cfg->jmax = parse_int_str(jmax_str);
    cfg->s1 = parse_int_str(s1_str);
    cfg->s2 = parse_int_str(s2_str);

    /* Parse H ranges */
    if (parse_hrange(h1_str, &cfg->h1) < 0) fatal("AWG_H1: invalid range");
    if (parse_hrange(h2_str, &cfg->h2) < 0) fatal("AWG_H2: invalid range");
    if (parse_hrange(h3_str, &cfg->h3) < 0) fatal("AWG_H3: invalid range");
    if (parse_hrange(h4_str, &cfg->h4) < 0) fatal("AWG_H4: invalid range");

    /* Decode server public key */
    {
        int len = base64_decode(spub_str, slen(spub_str), cfg->server_pub, 32);
        if (len != 32) fatal("AWG_SERVER_PUB: must decode to 32 bytes");
    }

    /* Decode client public key */
    {
        int len = base64_decode(cpub_str, slen(cpub_str), cfg->client_pub, 32);
        if (len != 32) fatal("AWG_CLIENT_PUB: must decode to 32 bytes");
    }

    /* Optional v2 params */
    const char *v;
    if ((v = getenv("AWG_S3")) && v[0]) cfg->s3 = parse_int_str(v);
    if ((v = getenv("AWG_S4")) && v[0]) cfg->s4 = parse_int_str(v);

    /* CPS templates I1-I5 */
    const char *inames[] = { "AWG_I1", "AWG_I2", "AWG_I3", "AWG_I4", "AWG_I5" };
    for (int i = 0; i < 5; i++) {
        v = getenv(inames[i]);
        if (v && v[0]) {
            if (cps_parse(v, &g_cps_storage[i]) < 0) {
                const char *eparts[] = { inames[i], ": invalid CPS template" };
                log_msgn("FATAL: ", eparts, 2);
                _exit(1);
            }
            cfg->cps[i] = &g_cps_storage[i];
        }
    }

    /* AWG_MODE: normal (default), reverse, server */
    cfg->mode = AWG_MODE_NORMAL;
    v = getenv("AWG_MODE");
    if (v && v[0]) {
        if (v[0] == 'r') cfg->mode = AWG_MODE_REVERSE;
        else if (v[0] == 's') cfg->mode = AWG_MODE_SERVER;
    }

    /* Compute derived fields */
    config_compute(cfg);

    /* Timeout */
    cfg->timeout = 180;
    if ((v = getenv("AWG_TIMEOUT")) && v[0])
        cfg->timeout = parse_int_str(v);

    /* Log level */
    cfg->log_level = LOG_INFO;
    v = getenv("AWG_LOG_LEVEL");
    if (v) {
        if (v[0] == 'n') cfg->log_level = LOG_NONE;
        else if (v[0] == 'e') cfg->log_level = LOG_ERROR;
        else if (v[0] == 'i') cfg->log_level = LOG_INFO;
        else if (v[0] == 'd') cfg->log_level = LOG_DEBUG;
    }
    g_log_level = cfg->log_level;

    /* Socket buffer */
    cfg->socket_buf = 16 * 1024 * 1024;
    if ((v = getenv("AWG_SOCKET_BUF")) && v[0])
        cfg->socket_buf = parse_int_str(v);

    /* Source port */
    int src_port = 0;
    if ((v = getenv("AWG_SRC_PORT")) && v[0])
        src_port = parse_int_str(v);

    /* CPU affinity */
    cfg->cpu_c2s = -1;
    cfg->cpu_s2c = -1;
    if ((v = getenv("AWG_CPU_C2S")) && v[0])
        cfg->cpu_c2s = parse_int_str(v);
    if ((v = getenv("AWG_CPU_S2C")) && v[0])
        cfg->cpu_s2c = parse_int_str(v);

    /* Busy poll */
    cfg->busy_poll = 0;
    if ((v = getenv("AWG_BUSY_POLL")) && v[0])
        cfg->busy_poll = parse_int_str(v);

    /* No GRO */
    cfg->no_gro = 0;
    if ((v = getenv("AWG_NO_GRO")) && v[0])
        cfg->no_gro = parse_int_str(v);

    /* Determine protocol mode */
    const char *mode = "v1";
    if (cfg->s3 > 0 || cfg->s4 > 0 ||
        cfg->h1.min != cfg->h1.max || cfg->h2.min != cfg->h2.max ||
        cfg->h3.min != cfg->h3.max || cfg->h4.min != cfg->h4.max) {
        mode = "v2";
    } else if (cfg->cps[0] || cfg->cps[1] || cfg->cps[2] || cfg->cps[3] || cfg->cps[4]) {
        mode = "v1.5";
    }

    /* Startup log */
    {
        const char *awg_mode_str = cfg->mode == AWG_MODE_REVERSE ? "reverse" :
                                   cfg->mode == AWG_MODE_SERVER  ? "server" : "normal";
        const char *parts[] = { "awg-proxy ", VERSION, " linux/c proto=", mode,
                                " awg_mode=", awg_mode_str };
        log_infon(parts, 6);
    }
    {
        char spb[12];
        const char *parts[] = { "listen=", listen_str, " remote=", remote_str,
            " src_port=", src_port > 0 ? u32_to_str(spb, src_port) : "auto" };
        log_infon(parts, 6);
    }
    {
        char jcb[12], jminb[12], jmaxb[12];
        const char *parts[] = { "config: JC=", u32_to_str(jcb, cfg->jc),
            " JMIN=", u32_to_str(jminb, cfg->jmin),
            " JMAX=", u32_to_str(jmaxb, cfg->jmax) };
        log_infon(parts, 6);
    }
    {
        char s1b[12], s2b[12], s3b[12], s4b[12];
        const char *parts[] = { "config: S1=", u32_to_str(s1b, cfg->s1),
            " S2=", u32_to_str(s2b, cfg->s2),
            " S3=", u32_to_str(s3b, cfg->s3),
            " S4=", u32_to_str(s4b, cfg->s4) };
        log_infon(parts, 8);
    }
    {
        const char *parts[] = { "config: H1=", h1_str, " H2=", h2_str,
            " H3=", h3_str, " H4=", h4_str };
        log_infon(parts, 8);
    }
    if (cfg->no_gro)
        log_info("config: UDP GRO disabled (AWG_NO_GRO=1)");
    if (cfg->cpu_c2s >= 0 || cfg->cpu_s2c >= 0 || cfg->busy_poll > 0) {
        char c2sb[12], s2cb[12], bpb[12];
        const char *parts[] = {
            "perf: cpu_c2s=", cfg->cpu_c2s >= 0 ? u32_to_str(c2sb, cfg->cpu_c2s) : "auto",
            " cpu_s2c=", cfg->cpu_s2c >= 0 ? u32_to_str(s2cb, cfg->cpu_s2c) : "auto",
            " busy_poll=", cfg->busy_poll > 0 ? u32_to_str(bpb, cfg->busy_poll) : "off"
        };
        log_infon(parts, 6);
    }

    /* Init and run proxy */
    if (proxy_init(&g_proxy, cfg, listen_str, remote_str, src_port) < 0) {
        fatal("proxy init failed");
    }

    int ret = proxy_run(&g_proxy);
    return ret;
}
