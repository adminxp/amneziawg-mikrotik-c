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
    TEST_MAIN_END();
}
