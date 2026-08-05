/* UDP GRO receive-side cmsg parsing.
 *
 * Worth pinning: the parser used to look for UDP_SEGMENT (103), which is the
 * send-side GSO type and never appears on receive, so every coalesced read was
 * mistaken for a single datagram and forwarded with 2-5 packets glued
 * together. The kernel reports the segment size as UDP_GRO (104) holding an
 * int -- see udp_cmsg_recv() in net/ipv4/udp.c. */
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "test.h"
#include "proxy.h"

/* Build a msghdr carrying one cmsg of the given level/type/int value. */
static int parse_with(char *cbuf, size_t cbuf_len, int level, int type, int val) {
    struct msghdr hdr;
    memset(&hdr, 0, sizeof(hdr));
    memset(cbuf, 0, cbuf_len);
    hdr.msg_control = cbuf;
    hdr.msg_controllen = cbuf_len;

    struct cmsghdr *cm = CMSG_FIRSTHDR(&hdr);
    cm->cmsg_level = level;
    cm->cmsg_type = type;
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &val, sizeof(val));
    hdr.msg_controllen = cm->cmsg_len;

    return gro_seg_size(&hdr);
}

static void test_udp_gro_cmsg_is_read(void) {
    char cbuf[CMSG_SPACE(sizeof(int))];
    ASSERT_EQ(parse_with(cbuf, sizeof(cbuf), IPPROTO_UDP, UDP_GRO, 1452), 1452);
}

static void test_full_size_segment(void) {
    /* A full 1500-MTU segment must survive the int/uint16 boundary intact. */
    char cbuf[CMSG_SPACE(sizeof(int))];
    ASSERT_EQ(parse_with(cbuf, sizeof(cbuf), IPPROTO_UDP, UDP_GRO, 1472), 1472);
}

static void test_udp_segment_is_not_gro(void) {
    /* The exact confusion that caused the bug: UDP_SEGMENT is the send-side
     * type and must not be treated as a coalesced-read marker. */
    char cbuf[CMSG_SPACE(sizeof(int))];
    ASSERT_EQ(parse_with(cbuf, sizeof(cbuf), IPPROTO_UDP, UDP_SEGMENT, 1452), 0);
}

static void test_wrong_level_ignored(void) {
    char cbuf[CMSG_SPACE(sizeof(int))];
    ASSERT_EQ(parse_with(cbuf, sizeof(cbuf), IPPROTO_IP, UDP_GRO, 1452), 0);
}

static void test_no_cmsg_means_no_coalescing(void) {
    struct msghdr hdr;
    memset(&hdr, 0, sizeof(hdr));
    ASSERT_EQ(gro_seg_size(&hdr), 0);
}

int main(void) {
    printf("=== gro tests ===\n");
    RUN_TEST(udp_gro_cmsg_is_read);
    RUN_TEST(full_size_segment);
    RUN_TEST(udp_segment_is_not_gro);
    RUN_TEST(wrong_level_ignored);
    RUN_TEST(no_cmsg_means_no_coalescing);
    TEST_MAIN_END();
}
