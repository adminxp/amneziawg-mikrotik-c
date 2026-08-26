#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>
#include "test.h"
#include "proxy.h"

static int check4(const char *host, const char *cur_ip) {
    struct sockaddr_in cur;
    memset(&cur, 0, sizeof(cur));
    cur.sin_family = AF_INET;
    inet_pton(AF_INET, cur_ip, &cur.sin_addr);
    return resolve_addr_check(host, (struct sockaddr *)&cur);
}

static int check6(const char *host, const char *cur_ip) {
    struct sockaddr_in6 cur;
    memset(&cur, 0, sizeof(cur));
    cur.sin6_family = AF_INET6;
    inet_pton(AF_INET6, cur_ip, &cur.sin6_addr);
    return resolve_addr_check(host, (struct sockaddr *)&cur);
}

static void test_literal_match(void) {
    /* Literal IP resolves to itself — still present */
    ASSERT_EQ(check4("127.0.0.1", "127.0.0.1"), 0);
}

static void test_literal_mismatch(void) {
    /* Current IP is not among the records — gone */
    ASSERT_EQ(check4("127.0.0.1", "10.9.9.9"), 1);
}

static void test_hosts_file(void) {
    /* localhost comes from /etc/hosts, no network needed */
    ASSERT_EQ(check4("localhost", "127.0.0.1"), 0);
}

static void test_resolve_error(void) {
    /* Empty name fails without touching the network */
    ASSERT_EQ(check4("", "127.0.0.1"), -1);
}

static void test_ipv6_literal_match(void) {
    /* AF_UNSPEC must return the AAAA record for a v6 literal */
    ASSERT_EQ(check6("::1", "::1"), 0);
}

static void test_ipv6_literal_mismatch(void) {
    ASSERT_EQ(check6("::1", "2001:db8::1"), 1);
}

static void test_family_must_match(void) {
    /* Same host, wrong family: an A record never satisfies a v6 endpoint and
     * vice versa, otherwise a dual-stack name would look unchanged forever. */
    ASSERT_EQ(check6("127.0.0.1", "::1"), 1);
    ASSERT_EQ(check4("::1", "127.0.0.1"), 1);
}

static void test_hostname_with_aaaa(void) {
    /* ip6-localhost is in the stock /etc/hosts of every Debian-derived image;
     * skip rather than fail where it is absent. */
    if (check6("ip6-localhost", "::1") == -1) return;
    ASSERT_EQ(check6("ip6-localhost", "::1"), 0);
    ASSERT_EQ(check6("ip6-localhost", "2001:db8::1"), 1);
}

/* ---- parse_host_port ---- */

static void test_parse_ipv4(void) {
    char host[256];
    uint16_t port = 0;
    ASSERT_EQ(parse_host_port("1.2.3.4:51820", host, sizeof(host), &port), 0);
    ASSERT_EQ(strcmp(host, "1.2.3.4"), 0);
    ASSERT_EQ(port, 51820);
}

static void test_parse_hostname(void) {
    char host[256];
    uint16_t port = 0;
    ASSERT_EQ(parse_host_port("vpn.example.com:443", host, sizeof(host), &port), 0);
    ASSERT_EQ(strcmp(host, "vpn.example.com"), 0);
    ASSERT_EQ(port, 443);
}

static void test_parse_wildcard_listen(void) {
    /* AWG_LISTEN=":51820" — empty host means INADDR_ANY */
    char host[256];
    uint16_t port = 0;
    ASSERT_EQ(parse_host_port(":51820", host, sizeof(host), &port), 0);
    ASSERT_EQ(host[0], '\0');
    ASSERT_EQ(port, 51820);
}

static void test_parse_ipv6_bracketed(void) {
    char host[256];
    uint16_t port = 0;
    ASSERT_EQ(parse_host_port("[::1]:51820", host, sizeof(host), &port), 0);
    ASSERT_EQ(strcmp(host, "::1"), 0);
    ASSERT_EQ(port, 51820);

    ASSERT_EQ(parse_host_port("[2001:db8::1]:443", host, sizeof(host), &port), 0);
    ASSERT_EQ(strcmp(host, "2001:db8::1"), 0);
    ASSERT_EQ(port, 443);
}

static void test_parse_ipv6_bare_rejected(void) {
    /* No port to be had, and splitting on the last colon would silently
     * produce the wrong host — reject instead. */
    char host[256];
    uint16_t port = 0;
    ASSERT_EQ(parse_host_port("2001:db8::1", host, sizeof(host), &port), -1);
    ASSERT_EQ(parse_host_port("::1", host, sizeof(host), &port), -1);
}

static void test_parse_garbage(void) {
    char host[256];
    uint16_t port = 0;
    ASSERT_EQ(parse_host_port("noport", host, sizeof(host), &port), -1);
    ASSERT_EQ(parse_host_port("host:", host, sizeof(host), &port), -1);
    ASSERT_EQ(parse_host_port("host:abc", host, sizeof(host), &port), -1);
    ASSERT_EQ(parse_host_port("[::1:51820", host, sizeof(host), &port), -1);
    ASSERT_EQ(parse_host_port("[::1]51820", host, sizeof(host), &port), -1);
    ASSERT_EQ(parse_host_port("[::1]:", host, sizeof(host), &port), -1);
    ASSERT_EQ(parse_host_port("host:99999", host, sizeof(host), &port), -1);
    ASSERT_EQ(parse_host_port("", host, sizeof(host), &port), -1);
}

static void test_parse_host_too_long(void) {
    char host[8];
    uint16_t port = 0;
    ASSERT_EQ(parse_host_port("verylonghostname:443", host, sizeof(host), &port), -1);
    ASSERT_EQ(parse_host_port("[2001:db8::dead:beef]:443", host, sizeof(host), &port), -1);
}

/* ---- parse_host_ports / portset_pick ---- */

static void test_ports_single(void) {
    /* The old one-port form must keep parsing exactly as it did. */
    char host[256];
    portset_t ps;
    ASSERT_EQ(parse_host_ports("1.2.3.4:51820", host, sizeof(host), &ps), 0);
    ASSERT_EQ(strcmp(host, "1.2.3.4"), 0);
    ASSERT_EQ(ps.n, 1);
    ASSERT_EQ(ps.total, 1);
    ASSERT_EQ(ps.r[0].lo, 51820);
    ASSERT_EQ(ps.r[0].hi, 51820);
}

static void test_ports_list(void) {
    char host[256];
    portset_t ps;
    ASSERT_EQ(parse_host_ports("vpn.example.com:443,8080", host, sizeof(host), &ps), 0);
    ASSERT_EQ(strcmp(host, "vpn.example.com"), 0);
    ASSERT_EQ(ps.n, 2);
    ASSERT_EQ(ps.total, 2);
    ASSERT_EQ(ps.r[0].lo, 443);
    ASSERT_EQ(ps.r[1].lo, 8080);
}

static void test_ports_ranges(void) {
    char host[256];
    portset_t ps;
    ASSERT_EQ(parse_host_ports("1.2.3.4:20150-20299,21500-21649,443",
                               host, sizeof(host), &ps), 0);
    ASSERT_EQ(ps.n, 3);
    ASSERT_EQ(ps.total, 150 + 150 + 1);
    ASSERT_EQ(ps.r[0].hi, 20299);
    ASSERT_EQ(ps.r[2].lo, 443);
    ASSERT_EQ(ps.r[2].hi, 443);
}

static void test_ports_ipv6(void) {
    char host[256];
    portset_t ps;
    ASSERT_EQ(parse_host_ports("[2001:db8::1]:443,6000-6100", host, sizeof(host), &ps), 0);
    ASSERT_EQ(strcmp(host, "2001:db8::1"), 0);
    ASSERT_EQ(ps.n, 2);
    ASSERT_EQ(ps.total, 1 + 101);
}

static void test_ports_garbage(void) {
    char host[256];
    portset_t ps;
    ASSERT_EQ(parse_host_ports("h:0", host, sizeof(host), &ps), -1);
    ASSERT_EQ(parse_host_ports("h:70000", host, sizeof(host), &ps), -1);
    /* Reversed range — a typo, not something to silently turn around. */
    ASSERT_EQ(parse_host_ports("h:6800-6085", host, sizeof(host), &ps), -1);
    ASSERT_EQ(parse_host_ports("h:443,", host, sizeof(host), &ps), -1);
    ASSERT_EQ(parse_host_ports("h:,443", host, sizeof(host), &ps), -1);
    ASSERT_EQ(parse_host_ports("h:443,,8080", host, sizeof(host), &ps), -1);
    ASSERT_EQ(parse_host_ports("h:a-b", host, sizeof(host), &ps), -1);
    ASSERT_EQ(parse_host_ports("h:443-", host, sizeof(host), &ps), -1);
    ASSERT_EQ(parse_host_ports("h:443 ,8080", host, sizeof(host), &ps), -1);
}

static void test_ports_too_many(void) {
    /* One past AWG_MAX_PORT_RANGES tokens is a refusal, not a truncation. */
    char host[256], spec[512];
    portset_t ps;
    int off = 2;
    memcpy(spec, "h:", 2);
    for (int i = 0; i < AWG_MAX_PORT_RANGES + 1; i++)
        off += sprintf(spec + off, "%s%d", i ? "," : "", 20000 + i);
    ASSERT_EQ(parse_host_ports(spec, host, sizeof(host), &ps), -1);
}

static void test_pick_within_set(void) {
    char host[256];
    portset_t ps;
    fastrand_t rng;
    fastrand_init(&rng, 12345);
    ASSERT_EQ(parse_host_ports("h:100-102,500,900-909", host, sizeof(host), &ps), 0);
    ASSERT_EQ(ps.total, 3 + 1 + 10);
    for (int i = 0; i < 2000; i++) {
        uint16_t v = portset_pick(&ps, &rng, 0);
        int ok = (v >= 100 && v <= 102) || v == 500 || (v >= 900 && v <= 909);
        ASSERT(ok);
    }
}

static void test_pick_covers_both(void) {
    /* Uniform over ports, so a two-port set must yield both. */
    char host[256];
    portset_t ps;
    fastrand_t rng;
    fastrand_init(&rng, 777);
    ASSERT_EQ(parse_host_ports("h:443,8080", host, sizeof(host), &ps), 0);
    int seen443 = 0, seen8080 = 0;
    for (int i = 0; i < 200; i++) {
        uint16_t v = portset_pick(&ps, &rng, 0);
        if (v == 443) seen443 = 1;
        if (v == 8080) seen8080 = 1;
    }
    ASSERT(seen443 && seen8080);
}

static void test_pick_avoids_dead_port(void) {
    /* Two ports: one redraw always lands on the other one. */
    char host[256];
    portset_t ps;
    fastrand_t rng;
    fastrand_init(&rng, 42);
    ASSERT_EQ(parse_host_ports("h:443,8080", host, sizeof(host), &ps), 0);
    uint16_t cur = 443;
    int repeats = 0;
    for (int i = 0; i < 100; i++) {
        uint16_t next = portset_pick(&ps, &rng, cur);
        ASSERT(next == 443 || next == 8080);
        if (next == cur) repeats++;
        cur = next;
    }
    /* One redraw, so a repeat needs both draws to hit the dead port: a quarter
     * of the time here, against half if the redraw were missing. */
    ASSERT(repeats < 40);
    /* A single-port set has nowhere to go — it must still answer that port. */
    ASSERT_EQ(parse_host_ports("h:51820", host, sizeof(host), &ps), 0);
    ASSERT_EQ(portset_pick(&ps, &rng, 51820), 51820);
}

int main(void) {
    fprintf(stderr, "=== dns tests ===\n");
    RUN_TEST(literal_match);
    RUN_TEST(literal_mismatch);
    RUN_TEST(hosts_file);
    RUN_TEST(resolve_error);
    RUN_TEST(ipv6_literal_match);
    RUN_TEST(ipv6_literal_mismatch);
    RUN_TEST(family_must_match);
    RUN_TEST(hostname_with_aaaa);
    RUN_TEST(parse_ipv4);
    RUN_TEST(parse_hostname);
    RUN_TEST(parse_wildcard_listen);
    RUN_TEST(parse_ipv6_bracketed);
    RUN_TEST(parse_ipv6_bare_rejected);
    RUN_TEST(parse_garbage);
    RUN_TEST(parse_host_too_long);
    RUN_TEST(ports_single);
    RUN_TEST(ports_list);
    RUN_TEST(ports_ranges);
    RUN_TEST(ports_ipv6);
    RUN_TEST(ports_garbage);
    RUN_TEST(ports_too_many);
    RUN_TEST(pick_within_set);
    RUN_TEST(pick_covers_both);
    RUN_TEST(pick_avoids_dead_port);
    TEST_MAIN_END();
}
