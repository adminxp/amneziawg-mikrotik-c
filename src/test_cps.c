#include <stdint.h>
#include "test.h"
#include "transform.h"
#include "cps.h"

static void test_parse_static_bytes(void) {
    cps_template_t tmpl;
    ASSERT_EQ(cps_parse("<b 0x0844>", &tmpl), 0);
    ASSERT_EQ(tmpl.nseg, 1);
    ASSERT_EQ(tmpl.segs[0].kind, CPS_STATIC);
    ASSERT_EQ(tmpl.segs[0].data_len, 2);
    ASSERT_EQ(tmpl.static_data[0], 0x08);
    ASSERT_EQ(tmpl.static_data[1], 0x44);
}

static void test_parse_random(void) {
    cps_template_t tmpl;
    ASSERT_EQ(cps_parse("<r 16>", &tmpl), 0);
    ASSERT_EQ(tmpl.nseg, 1);
    ASSERT_EQ(tmpl.segs[0].kind, CPS_RANDOM);
    ASSERT_EQ(tmpl.segs[0].size, 16);
}

static void test_parse_timestamp(void) {
    cps_template_t tmpl;
    ASSERT_EQ(cps_parse("<t>", &tmpl), 0);
    ASSERT_EQ(tmpl.nseg, 1);
    ASSERT_EQ(tmpl.segs[0].kind, CPS_TIMESTAMP);
}

static void test_parse_counter(void) {
    cps_template_t tmpl;
    ASSERT_EQ(cps_parse("<c>", &tmpl), 0);
    ASSERT_EQ(tmpl.nseg, 1);
    ASSERT_EQ(tmpl.segs[0].kind, CPS_COUNTER);
}

static void test_parse_random_chars(void) {
    cps_template_t tmpl;
    ASSERT_EQ(cps_parse("<rc 12>", &tmpl), 0);
    ASSERT_EQ(tmpl.nseg, 1);
    ASSERT_EQ(tmpl.segs[0].kind, CPS_RANDOM_CHARS);
    ASSERT_EQ(tmpl.segs[0].size, 12);
}

static void test_parse_random_digits(void) {
    cps_template_t tmpl;
    ASSERT_EQ(cps_parse("<rd 8>", &tmpl), 0);
    ASSERT_EQ(tmpl.nseg, 1);
    ASSERT_EQ(tmpl.segs[0].kind, CPS_RANDOM_DIGITS);
    ASSERT_EQ(tmpl.segs[0].size, 8);
}

static void test_generate_random_chars(void) {
    cps_template_t tmpl;
    uint8_t buf[64];
    ASSERT_EQ(cps_parse("<rc 20>", &tmpl), 0);
    int n = cps_generate(&tmpl, 0, buf, 64);
    ASSERT_EQ(n, 20);
    for (int i = 0; i < 20; i++) {
        int ok = (buf[i] >= '0' && buf[i] <= '9') ||
                 (buf[i] >= 'A' && buf[i] <= 'Z') ||
                 (buf[i] >= 'a' && buf[i] <= 'z');
        ASSERT(ok);
    }
}

static void test_generate_random_digits(void) {
    cps_template_t tmpl;
    uint8_t buf[64];
    ASSERT_EQ(cps_parse("<rd 10>", &tmpl), 0);
    int n = cps_generate(&tmpl, 0, buf, 64);
    ASSERT_EQ(n, 10);
    for (int i = 0; i < 10; i++) {
        ASSERT(buf[i] >= '0' && buf[i] <= '9');
    }
}

static void test_parse_mixed_rc_rd(void) {
    cps_template_t tmpl;
    uint8_t buf[64];
    ASSERT_EQ(cps_parse("<b 0xDEAD> <rc 8> <t> <rd 4>", &tmpl), 0);
    ASSERT_EQ(tmpl.nseg, 4);
    int n = cps_generate(&tmpl, 0, buf, 64);
    ASSERT_EQ(n, 18); /* 2 + 8 + 4 + 4 */
    ASSERT_EQ(buf[0], 0xDE);
    ASSERT_EQ(buf[1], 0xAD);
    /* bytes [2:10] alphanumeric */
    for (int i = 2; i < 10; i++) {
        int ok = (buf[i] >= '0' && buf[i] <= '9') ||
                 (buf[i] >= 'A' && buf[i] <= 'Z') ||
                 (buf[i] >= 'a' && buf[i] <= 'z');
        ASSERT(ok);
    }
    /* bytes [10:14] timestamp (just check it's 4 bytes, can't verify exact value) */
    /* bytes [14:18] digits */
    for (int i = 14; i < 18; i++) {
        ASSERT(buf[i] >= '0' && buf[i] <= '9');
    }
}

static void test_parse_rc_rd_invalid(void) {
    cps_template_t tmpl;
    ASSERT_EQ(cps_parse("<rc>", &tmpl), -1);
    ASSERT_EQ(cps_parse("<rc abc>", &tmpl), -1);
    ASSERT_EQ(cps_parse("<rc 0>", &tmpl), -1);
    ASSERT_EQ(cps_parse("<rc -1>", &tmpl), -1);
    ASSERT_EQ(cps_parse("<rd>", &tmpl), -1);
    ASSERT_EQ(cps_parse("<rd abc>", &tmpl), -1);
    ASSERT_EQ(cps_parse("<rd 0>", &tmpl), -1);
    ASSERT_EQ(cps_parse("<rd -1>", &tmpl), -1);
}

static void test_parse_multi_segment(void) {
    cps_template_t tmpl;
    ASSERT_EQ(cps_parse("<b 0xAABB> <r 8> <t> <c>", &tmpl), 0);
    ASSERT_EQ(tmpl.nseg, 4);
    ASSERT_EQ(tmpl.segs[0].kind, CPS_STATIC);
    ASSERT_EQ(tmpl.segs[1].kind, CPS_RANDOM);
    ASSERT_EQ(tmpl.segs[2].kind, CPS_TIMESTAMP);
    ASSERT_EQ(tmpl.segs[3].kind, CPS_COUNTER);
}

static void test_parse_empty(void) {
    cps_template_t tmpl;
    ASSERT_EQ(cps_parse("", &tmpl), -1);
}

static void test_parse_invalid(void) {
    cps_template_t tmpl;
    ASSERT_EQ(cps_parse("no tags here", &tmpl), -1);
    ASSERT_EQ(cps_parse("<b>", &tmpl), -1);
    ASSERT_EQ(cps_parse("<b 0x>", &tmpl), -1);
    ASSERT_EQ(cps_parse("<b 0xGG>", &tmpl), -1);
    ASSERT_EQ(cps_parse("<b 0x1>", &tmpl), -1);
    ASSERT_EQ(cps_parse("<r>", &tmpl), -1);
    ASSERT_EQ(cps_parse("<r abc>", &tmpl), -1);
    ASSERT_EQ(cps_parse("<r -5>", &tmpl), -1);
    ASSERT_EQ(cps_parse("<r 0>", &tmpl), -1);
    ASSERT_EQ(cps_parse("<x>", &tmpl), -1);
    ASSERT_EQ(cps_parse("<b 0xFF", &tmpl), -1);
}

static void test_generate_cps(void) {
    cps_template_t tmpl;
    uint8_t buf[64];
    ASSERT_EQ(cps_parse("<b 0xDEAD> <r 4> <c>", &tmpl), 0);
    int n = cps_generate(&tmpl, 42, buf, 64);
    ASSERT_EQ(n, 10); /* 2 + 4 + 4 */
    ASSERT_EQ(buf[0], 0xDE);
    ASSERT_EQ(buf[1], 0xAD);
    /* bytes [2:6] random */
    /* bytes [6:10] counter = 42 LE */
    uint32_t counter;
    memcpy(&counter, buf + 6, 4);
    ASSERT_EQ(counter, 42u);
}

int main(void) {
    fprintf(stderr, "=== cps tests ===\n");
    RUN_TEST(parse_static_bytes);
    RUN_TEST(parse_random);
    RUN_TEST(parse_timestamp);
    RUN_TEST(parse_counter);
    RUN_TEST(parse_random_chars);
    RUN_TEST(parse_random_digits);
    RUN_TEST(generate_random_chars);
    RUN_TEST(generate_random_digits);
    RUN_TEST(parse_mixed_rc_rd);
    RUN_TEST(parse_rc_rd_invalid);
    RUN_TEST(parse_multi_segment);
    RUN_TEST(parse_empty);
    RUN_TEST(parse_invalid);
    RUN_TEST(generate_cps);
    TEST_MAIN_END();
}
