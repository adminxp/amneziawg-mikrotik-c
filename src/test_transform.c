#include <stdint.h>
#include <stdlib.h>
#include "test.h"
#include "transform.h"
#include "blake2s.h"
#include "fastrand.h"

/* Shared test config: Jc=3, Jmin=30, Jmax=500, S1=S2=20, H1-H4 point values */
static awg_config_t make_test_config(void) {
    awg_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.jc = 3;
    cfg.jmin = 30;
    cfg.jmax = 500;
    cfg.s1 = 20;
    cfg.s2 = 20;
    cfg.s3 = 0;
    cfg.s4 = 0;
    cfg.h1 = (hrange_t){1234567890, 1234567890};
    cfg.h2 = (hrange_t){1234567891, 1234567891};
    cfg.h3 = (hrange_t){1234567892, 1234567892};
    cfg.h4 = (hrange_t){1234567893, 1234567893};
    config_compute(&cfg);
    return cfg;
}

static void fill_seq(uint8_t *buf, int n) {
    for (int i = 0; i < n; i++) buf[i] = (uint8_t)i;
}

static void write32_le(uint8_t *p, uint32_t v) {
    memcpy(p, &v, 4);
}

static uint32_t read32_le(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* 1. Outbound handshake init */
static void test_outbound_handshake_init(void) {
    awg_config_t cfg = make_test_config();
    uint8_t buf[256 + WG_INIT_SIZE];
    int dataoff = 256;
    memset(buf, 0, sizeof(buf));
    uint8_t *data = buf + dataoff;
    write32_le(data, WG_HANDSHAKE_INIT);
    fill_seq(data + 4, WG_INIT_SIZE - 4);

    int out_len, sendJunk;
    uint8_t *out = transform_outbound(buf, dataoff, WG_INIT_SIZE, &cfg, 12345, &out_len, &sendJunk);
    ASSERT_EQ(out_len, cfg.s1 + WG_INIT_SIZE);
    ASSERT_EQ(sendJunk, 1);
    /* Type at offset S1 should be H1 */
    uint32_t h = read32_le(out + cfg.s1);
    ASSERT(hrange_contains(&cfg.h1, h));
}

/* 2. Outbound handshake response */
static void test_outbound_handshake_response(void) {
    awg_config_t cfg = make_test_config();
    uint8_t buf[256 + WG_RESP_SIZE];
    int dataoff = 256;
    memset(buf, 0, sizeof(buf));
    uint8_t *data = buf + dataoff;
    write32_le(data, WG_HANDSHAKE_RESPONSE);
    fill_seq(data + 4, WG_RESP_SIZE - 4);

    int out_len, sendJunk;
    uint8_t *out = transform_outbound(buf, dataoff, WG_RESP_SIZE, &cfg, 12345, &out_len, &sendJunk);
    ASSERT_EQ(out_len, cfg.s2 + WG_RESP_SIZE);
    ASSERT_EQ(sendJunk, 0);
    uint32_t h = read32_le(out + cfg.s2);
    ASSERT(hrange_contains(&cfg.h2, h));
}

/* 3. Outbound cookie reply */
static void test_outbound_cookie_reply(void) {
    awg_config_t cfg = make_test_config();
    uint8_t buf[256 + WG_COOKIE_SIZE];
    int dataoff = 256;
    memset(buf, 0, sizeof(buf));
    uint8_t *data = buf + dataoff;
    write32_le(data, WG_COOKIE_REPLY);
    fill_seq(data + 4, WG_COOKIE_SIZE - 4);

    int out_len, sendJunk;
    uint8_t *out = transform_outbound(buf, dataoff, WG_COOKIE_SIZE, &cfg, 12345, &out_len, &sendJunk);
    ASSERT_EQ(out_len, WG_COOKIE_SIZE); /* S3=0, no padding */
    ASSERT_EQ(sendJunk, 0);
    uint32_t h = read32_le(out);
    ASSERT(hrange_contains(&cfg.h3, h));
}

/* 4. Outbound transport data */
static void test_outbound_transport_data(void) {
    awg_config_t cfg = make_test_config();
    uint8_t buf[256 + 100];
    int dataoff = 256;
    memset(buf, 0, sizeof(buf));
    uint8_t *data = buf + dataoff;
    write32_le(data, WG_TRANSPORT_DATA);
    fill_seq(data + 4, 96);

    int out_len, sendJunk;
    uint8_t *out = transform_outbound(buf, dataoff, 100, &cfg, 12345, &out_len, &sendJunk);
    ASSERT_EQ(out_len, 100); /* S4=0, no padding */
    ASSERT_EQ(sendJunk, 0);
    uint32_t h = read32_le(out);
    ASSERT(hrange_contains(&cfg.h4, h));
}

/* 5. Inbound handshake init */
static void test_inbound_handshake_init(void) {
    awg_config_t cfg = make_test_config();
    int total = cfg.s1 + WG_INIT_SIZE;
    uint8_t buf[256];
    memset(buf, 0xAA, sizeof(buf));
    /* S1 bytes padding + H1 type + payload */
    write32_le(buf + cfg.s1, cfg.h1.min);
    fill_seq(buf + cfg.s1 + 4, WG_INIT_SIZE - 4);

    int out_len;
    uint8_t *out = transform_inbound(buf, total, &cfg, &out_len);
    ASSERT(out != NULL);
    ASSERT_EQ(out_len, WG_INIT_SIZE);
    ASSERT_EQ(read32_le(out), WG_HANDSHAKE_INIT);
}

/* 6. Inbound handshake response */
static void test_inbound_handshake_response(void) {
    awg_config_t cfg = make_test_config();
    int total = cfg.s2 + WG_RESP_SIZE;
    uint8_t buf[256];
    memset(buf, 0xAA, sizeof(buf));
    write32_le(buf + cfg.s2, cfg.h2.min);
    fill_seq(buf + cfg.s2 + 4, WG_RESP_SIZE - 4);

    int out_len;
    uint8_t *out = transform_inbound(buf, total, &cfg, &out_len);
    ASSERT(out != NULL);
    ASSERT_EQ(out_len, WG_RESP_SIZE);
    ASSERT_EQ(read32_le(out), WG_HANDSHAKE_RESPONSE);
}

/* 7. Inbound cookie reply */
static void test_inbound_cookie_reply(void) {
    awg_config_t cfg = make_test_config();
    uint8_t buf[WG_COOKIE_SIZE];
    memset(buf, 0xAA, sizeof(buf));
    write32_le(buf, cfg.h3.min);

    int out_len;
    uint8_t *out = transform_inbound(buf, WG_COOKIE_SIZE, &cfg, &out_len);
    ASSERT(out != NULL);
    ASSERT_EQ(out_len, WG_COOKIE_SIZE);
    ASSERT_EQ(read32_le(out), WG_COOKIE_REPLY);
}

/* 8. Inbound transport data */
static void test_inbound_transport_data(void) {
    awg_config_t cfg = make_test_config();
    uint8_t buf[100];
    memset(buf, 0xAA, sizeof(buf));
    write32_le(buf, cfg.h4.min);

    int out_len;
    uint8_t *out = transform_inbound(buf, 100, &cfg, &out_len);
    ASSERT(out != NULL);
    ASSERT_EQ(out_len, 100);
    ASSERT_EQ(read32_le(out), WG_TRANSPORT_DATA);
}

/* 9. Roundtrip handshake init */
static void test_roundtrip_init(void) {
    awg_config_t cfg = make_test_config();
    uint8_t orig[WG_INIT_SIZE];
    write32_le(orig, WG_HANDSHAKE_INIT);
    fill_seq(orig + 4, WG_INIT_SIZE - 4);

    uint8_t buf[256 + WG_INIT_SIZE];
    int dataoff = 256;
    memset(buf, 0, sizeof(buf));
    memcpy(buf + dataoff, orig, WG_INIT_SIZE);

    int out_len, sendJunk;
    uint8_t *out = transform_outbound(buf, dataoff, WG_INIT_SIZE, &cfg, 99, &out_len, &sendJunk);

    int in_len;
    uint8_t *result = transform_inbound(out, out_len, &cfg, &in_len);
    ASSERT(result != NULL);
    ASSERT_EQ(in_len, WG_INIT_SIZE);
    ASSERT_EQ(read32_le(result), WG_HANDSHAKE_INIT);
    ASSERT_MEM_EQ(result + 4, orig + 4, WG_INIT_SIZE - 4);
}

/* 10. Roundtrip handshake response */
static void test_roundtrip_response(void) {
    awg_config_t cfg = make_test_config();
    uint8_t orig[WG_RESP_SIZE];
    write32_le(orig, WG_HANDSHAKE_RESPONSE);
    fill_seq(orig + 4, WG_RESP_SIZE - 4);

    uint8_t buf[256 + WG_RESP_SIZE];
    int dataoff = 256;
    memset(buf, 0, sizeof(buf));
    memcpy(buf + dataoff, orig, WG_RESP_SIZE);

    int out_len, sendJunk;
    uint8_t *out = transform_outbound(buf, dataoff, WG_RESP_SIZE, &cfg, 99, &out_len, &sendJunk);

    int in_len;
    uint8_t *result = transform_inbound(out, out_len, &cfg, &in_len);
    ASSERT(result != NULL);
    ASSERT_EQ(in_len, WG_RESP_SIZE);
    ASSERT_EQ(read32_le(result), WG_HANDSHAKE_RESPONSE);
}

/* 11. Roundtrip cookie */
static void test_roundtrip_cookie(void) {
    awg_config_t cfg = make_test_config();
    uint8_t orig[WG_COOKIE_SIZE];
    write32_le(orig, WG_COOKIE_REPLY);
    fill_seq(orig + 4, WG_COOKIE_SIZE - 4);

    uint8_t buf[256 + WG_COOKIE_SIZE];
    int dataoff = 256;
    memset(buf, 0, sizeof(buf));
    memcpy(buf + dataoff, orig, WG_COOKIE_SIZE);

    int out_len, sendJunk;
    uint8_t *out = transform_outbound(buf, dataoff, WG_COOKIE_SIZE, &cfg, 99, &out_len, &sendJunk);

    int in_len;
    uint8_t *result = transform_inbound(out, out_len, &cfg, &in_len);
    ASSERT(result != NULL);
    ASSERT_EQ(read32_le(result), WG_COOKIE_REPLY);
}

/* 12. Roundtrip transport */
static void test_roundtrip_transport(void) {
    awg_config_t cfg = make_test_config();
    uint8_t orig[200];
    write32_le(orig, WG_TRANSPORT_DATA);
    fill_seq(orig + 4, 196);

    uint8_t buf[256 + 200];
    int dataoff = 256;
    memset(buf, 0, sizeof(buf));
    memcpy(buf + dataoff, orig, 200);

    int out_len, sendJunk;
    uint8_t *out = transform_outbound(buf, dataoff, 200, &cfg, 99, &out_len, &sendJunk);

    int in_len;
    uint8_t *result = transform_inbound(out, out_len, &cfg, &in_len);
    ASSERT(result != NULL);
    ASSERT_EQ(in_len, 200);
    ASSERT_EQ(read32_le(result), WG_TRANSPORT_DATA);
    ASSERT_MEM_EQ(result + 4, orig + 4, 196);
}

/* 13. Generate junk packets */
static void test_generate_junk(void) {
    awg_config_t cfg = make_test_config();
    uint8_t jbuf[3 * 500];
    int sizes[3];
    fastrand_t rng;
    fastrand_init(&rng, 42);
    fastrand_fill(&rng, jbuf, sizeof(jbuf));

    int n = generate_junk(&cfg, jbuf, sizes);
    ASSERT_EQ(n, 3);
    for (int i = 0; i < 3; i++) {
        ASSERT(sizes[i] >= 30);
        ASSERT(sizes[i] <= 500);
    }
}

/* 14. Junk Jc=0 */
static void test_generate_junk_zero_jc(void) {
    awg_config_t cfg = make_test_config();
    cfg.jc = 0;
    uint8_t jbuf[4];
    int sizes[1];
    int n = generate_junk(&cfg, jbuf, sizes);
    ASSERT_EQ(n, 0);
}

/* 15. Inbound drops unknown type */
static void test_inbound_drops_unknown(void) {
    awg_config_t cfg = make_test_config();
    uint8_t buf[100];
    memset(buf, 0, sizeof(buf));
    write32_le(buf, 99999);

    int out_len;
    ASSERT(transform_inbound(buf, 100, &cfg, &out_len) == NULL);
}

/* 16. Inbound drops too short */
static void test_inbound_drops_too_short(void) {
    awg_config_t cfg = make_test_config();
    uint8_t buf[3] = {1, 0, 0};
    int out_len;
    ASSERT(transform_inbound(buf, 3, &cfg, &out_len) == NULL);
}

/* 17. S1=0 */
static void test_no_padding_s1_zero(void) {
    awg_config_t cfg = make_test_config();
    cfg.s1 = 0;
    config_compute(&cfg);

    uint8_t buf[256 + WG_INIT_SIZE];
    int dataoff = 256;
    memset(buf, 0, sizeof(buf));
    uint8_t *data = buf + dataoff;
    write32_le(data, WG_HANDSHAKE_INIT);
    fill_seq(data + 4, WG_INIT_SIZE - 4);

    int out_len, sendJunk;
    uint8_t *out = transform_outbound(buf, dataoff, WG_INIT_SIZE, &cfg, 12345, &out_len, &sendJunk);
    ASSERT_EQ(out_len, WG_INIT_SIZE);
    ASSERT_EQ(sendJunk, 1);
    ASSERT(hrange_contains(&cfg.h1, read32_le(out)));

    /* Roundtrip */
    int in_len;
    uint8_t *result = transform_inbound(out, out_len, &cfg, &in_len);
    ASSERT(result != NULL);
    ASSERT_EQ(read32_le(result), WG_HANDSHAKE_INIT);
}

/* 18. S2=0 */
static void test_no_padding_s2_zero(void) {
    awg_config_t cfg = make_test_config();
    cfg.s2 = 0;
    config_compute(&cfg);

    uint8_t buf[256 + WG_RESP_SIZE];
    int dataoff = 256;
    memset(buf, 0, sizeof(buf));
    uint8_t *data = buf + dataoff;
    write32_le(data, WG_HANDSHAKE_RESPONSE);
    fill_seq(data + 4, WG_RESP_SIZE - 4);

    int out_len, sendJunk;
    uint8_t *out = transform_outbound(buf, dataoff, WG_RESP_SIZE, &cfg, 12345, &out_len, &sendJunk);
    ASSERT_EQ(out_len, WG_RESP_SIZE);

    /* Roundtrip */
    int in_len;
    uint8_t *result = transform_inbound(out, out_len, &cfg, &in_len);
    ASSERT(result != NULL);
    ASSERT_EQ(read32_le(result), WG_HANDSHAKE_RESPONSE);
}

/* 19. Outbound too short */
static void test_outbound_too_short(void) {
    awg_config_t cfg = make_test_config();
    uint8_t buf[256 + 2];
    int dataoff = 256;
    buf[dataoff] = 0xAA;
    buf[dataoff + 1] = 0xBB;

    int out_len, sendJunk;
    uint8_t *out = transform_outbound(buf, dataoff, 2, &cfg, 0, &out_len, &sendJunk);
    ASSERT_EQ(sendJunk, 0);
    ASSERT_EQ(out_len, 2);
    ASSERT_EQ(out[0], 0xAA);
    ASSERT_EQ(out[1], 0xBB);
}

/* 20. HRange pick/contains */
static void test_hrange_pick_contains(void) {
    /* Point range */
    hrange_t r1 = {42, 42};
    for (int i = 0; i < 100; i++) {
        ASSERT_EQ(hrange_pick(&r1, (uint64_t)i), 42u);
    }

    /* Wide range */
    hrange_t r2 = {100, 200};
    for (int i = 0; i < 100; i++) {
        uint32_t v = hrange_pick(&r2, (uint64_t)(i * 7919));
        ASSERT(v >= 100 && v <= 200);
    }

    /* contains */
    hrange_t r3 = {10, 20};
    ASSERT(hrange_contains(&r3, 10));
    ASSERT(hrange_contains(&r3, 15));
    ASSERT(hrange_contains(&r3, 20));
    ASSERT(!hrange_contains(&r3, 9));
    ASSERT(!hrange_contains(&r3, 21));
}

/* 21. Outbound cookie with S3 */
static void test_outbound_cookie_with_s3(void) {
    awg_config_t cfg = make_test_config();
    cfg.s3 = 49;
    config_compute(&cfg);

    uint8_t buf[256 + WG_COOKIE_SIZE];
    int dataoff = 256;
    memset(buf, 0, sizeof(buf));
    uint8_t *data = buf + dataoff;
    write32_le(data, WG_COOKIE_REPLY);
    fill_seq(data + 4, WG_COOKIE_SIZE - 4);

    int out_len, sendJunk;
    uint8_t *out = transform_outbound(buf, dataoff, WG_COOKIE_SIZE, &cfg, 12345, &out_len, &sendJunk);
    ASSERT_EQ(out_len, 49 + WG_COOKIE_SIZE);
    ASSERT_EQ(sendJunk, 0);
    uint32_t h = read32_le(out + 49);
    ASSERT(hrange_contains(&cfg.h3, h));
}

/* 22. Outbound transport with S4 */
static void test_outbound_transport_with_s4(void) {
    awg_config_t cfg = make_test_config();
    cfg.s4 = 17;
    config_compute(&cfg);

    uint8_t buf[256 + 100];
    int dataoff = 256;
    memset(buf, 0, sizeof(buf));
    uint8_t *data = buf + dataoff;
    write32_le(data, WG_TRANSPORT_DATA);
    fill_seq(data + 4, 96);

    int out_len, sendJunk;
    uint8_t *out = transform_outbound(buf, dataoff, 100, &cfg, 12345, &out_len, &sendJunk);
    ASSERT_EQ(out_len, 17 + 100);
    ASSERT_EQ(sendJunk, 0);
    uint32_t h = read32_le(out + 17);
    ASSERT(hrange_contains(&cfg.h4, h));
}

/* 23. Inbound scanning with S3/S4 */
static void test_inbound_scanning_s3(void) {
    awg_config_t cfg = make_test_config();
    cfg.s3 = 49;
    config_compute(&cfg);

    int total = 49 + WG_COOKIE_SIZE;
    uint8_t buf[256];
    memset(buf, 0xBB, sizeof(buf));
    write32_le(buf + 49, cfg.h3.min);

    int out_len;
    uint8_t *out = transform_inbound(buf, total, &cfg, &out_len);
    ASSERT(out != NULL);
    ASSERT_EQ(out_len, WG_COOKIE_SIZE);
    ASSERT_EQ(read32_le(out), WG_COOKIE_REPLY);
}

static void test_inbound_scanning_s4(void) {
    awg_config_t cfg = make_test_config();
    cfg.s4 = 17;
    config_compute(&cfg);

    int total = 17 + 100;
    uint8_t buf[256];
    memset(buf, 0xBB, sizeof(buf));
    write32_le(buf + 17, cfg.h4.min);

    int out_len;
    uint8_t *out = transform_inbound(buf, total, &cfg, &out_len);
    ASSERT(out != NULL);
    ASSERT_EQ(out_len, 100);
    ASSERT_EQ(read32_le(out), WG_TRANSPORT_DATA);
}

/* 24. Inbound H range / reject */
static void test_inbound_hrange_accept(void) {
    awg_config_t cfg = make_test_config();
    cfg.h4 = (hrange_t){1000, 2000};
    config_compute(&cfg);

    uint8_t buf[100];
    memset(buf, 0, sizeof(buf));
    write32_le(buf, 1500);

    int out_len;
    uint8_t *out = transform_inbound(buf, 100, &cfg, &out_len);
    ASSERT(out != NULL);
    ASSERT_EQ(read32_le(out), WG_TRANSPORT_DATA);
}

static void test_inbound_hrange_reject(void) {
    awg_config_t cfg = make_test_config();
    cfg.h4 = (hrange_t){1000, 2000};
    config_compute(&cfg);

    uint8_t buf[100];
    memset(buf, 0, sizeof(buf));
    write32_le(buf, 999);

    int out_len;
    ASSERT(transform_inbound(buf, 100, &cfg, &out_len) == NULL);
}

/* 25. Roundtrip v2 (S3, S4, H ranges) */
static void test_roundtrip_v2(void) {
    awg_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.jc = 3; cfg.jmin = 30; cfg.jmax = 500;
    cfg.s1 = 20; cfg.s2 = 20; cfg.s3 = 49; cfg.s4 = 17;
    cfg.h1 = (hrange_t){100000, 200000};
    cfg.h2 = (hrange_t){300000, 400000};
    cfg.h3 = (hrange_t){500000, 600000};
    cfg.h4 = (hrange_t){700000, 800000};
    config_compute(&cfg);

    /* Handshake init roundtrip */
    {
        uint8_t orig[WG_INIT_SIZE];
        write32_le(orig, WG_HANDSHAKE_INIT);
        fill_seq(orig + 4, WG_INIT_SIZE - 4);

        uint8_t buf[256 + WG_INIT_SIZE];
        int dataoff = 256;
        memset(buf, 0, sizeof(buf));
        memcpy(buf + dataoff, orig, WG_INIT_SIZE);

        int out_len, sendJunk;
        uint8_t *out = transform_outbound(buf, dataoff, WG_INIT_SIZE, &cfg, 42, &out_len, &sendJunk);
        ASSERT_EQ(out_len, 20 + WG_INIT_SIZE);

        int in_len;
        uint8_t *r = transform_inbound(out, out_len, &cfg, &in_len);
        ASSERT(r != NULL);
        ASSERT_EQ(read32_le(r), WG_HANDSHAKE_INIT);
        ASSERT_MEM_EQ(r + 4, orig + 4, WG_INIT_SIZE - 4);
    }

    /* Cookie reply roundtrip */
    {
        uint8_t orig[WG_COOKIE_SIZE];
        write32_le(orig, WG_COOKIE_REPLY);
        fill_seq(orig + 4, WG_COOKIE_SIZE - 4);

        uint8_t buf[256 + WG_COOKIE_SIZE];
        int dataoff = 256;
        memset(buf, 0, sizeof(buf));
        memcpy(buf + dataoff, orig, WG_COOKIE_SIZE);

        int out_len, sendJunk;
        uint8_t *out = transform_outbound(buf, dataoff, WG_COOKIE_SIZE, &cfg, 77, &out_len, &sendJunk);
        ASSERT_EQ(out_len, 49 + WG_COOKIE_SIZE);

        int in_len;
        uint8_t *r = transform_inbound(out, out_len, &cfg, &in_len);
        ASSERT(r != NULL);
        ASSERT_EQ(read32_le(r), WG_COOKIE_REPLY);
    }

    /* Transport data roundtrip */
    {
        uint8_t orig[200];
        write32_le(orig, WG_TRANSPORT_DATA);
        fill_seq(orig + 4, 196);

        uint8_t buf[256 + 200];
        int dataoff = 256;
        memset(buf, 0, sizeof(buf));
        memcpy(buf + dataoff, orig, 200);

        int out_len, sendJunk;
        uint8_t *out = transform_outbound(buf, dataoff, 200, &cfg, 123, &out_len, &sendJunk);
        ASSERT_EQ(out_len, 17 + 200);

        int in_len;
        uint8_t *r = transform_inbound(out, out_len, &cfg, &in_len);
        ASSERT(r != NULL);
        ASSERT_EQ(in_len, 200);
        ASSERT_EQ(read32_le(r), WG_TRANSPORT_DATA);
        ASSERT_MEM_EQ(r + 4, orig + 4, 196);
    }
}

/* 26. V1 backward compatibility */
static void test_v1_backward(void) {
    awg_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.jc = 2; cfg.jmin = 10; cfg.jmax = 50;
    cfg.s1 = 46; cfg.s2 = 122; cfg.s3 = 0; cfg.s4 = 0;
    cfg.h1 = (hrange_t){1033089720, 1033089720};
    cfg.h2 = (hrange_t){1336452505, 1336452505};
    cfg.h3 = (hrange_t){1858775673, 1858775673};
    cfg.h4 = (hrange_t){332219739, 332219739};
    config_compute(&cfg);

    /* Handshake init */
    {
        uint8_t buf[256 + WG_INIT_SIZE];
        int dataoff = 256;
        memset(buf, 0, sizeof(buf));
        uint8_t *data = buf + dataoff;
        write32_le(data, WG_HANDSHAKE_INIT);
        fill_seq(data + 4, WG_INIT_SIZE - 4);

        int out_len, sendJunk;
        transform_outbound(buf, dataoff, WG_INIT_SIZE, &cfg, 0, &out_len, &sendJunk);
        ASSERT_EQ(out_len, 46 + WG_INIT_SIZE);
        ASSERT_EQ(sendJunk, 1);
    }

    /* Transport data: no S4 padding */
    {
        uint8_t buf[256 + 100];
        int dataoff = 256;
        memset(buf, 0, sizeof(buf));
        uint8_t *data = buf + dataoff;
        write32_le(data, WG_TRANSPORT_DATA);

        int out_len, sendJunk;
        transform_outbound(buf, dataoff, 100, &cfg, 0, &out_len, &sendJunk);
        ASSERT_EQ(out_len, 100);
    }

    /* Cookie: no S3 padding */
    {
        uint8_t buf[256 + WG_COOKIE_SIZE];
        int dataoff = 256;
        memset(buf, 0, sizeof(buf));
        uint8_t *data = buf + dataoff;
        write32_le(data, WG_COOKIE_REPLY);

        int out_len, sendJunk;
        transform_outbound(buf, dataoff, WG_COOKIE_SIZE, &cfg, 0, &out_len, &sendJunk);
        ASSERT_EQ(out_len, WG_COOKIE_SIZE);
    }
}

/* 27. V2 false positive regression */
static void test_v2_false_positive(void) {
    awg_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.jc = 2; cfg.jmin = 10; cfg.jmax = 50;
    cfg.s1 = 46; cfg.s2 = 62; cfg.s3 = 30; cfg.s4 = 17;
    cfg.h1 = (hrange_t){100000, 200000};
    cfg.h2 = (hrange_t){300000, 400000};
    cfg.h3 = (hrange_t){500000, 600000};
    cfg.h4 = (hrange_t){0, 1073741823}; /* wide: 25% of uint32 space */
    config_compute(&cfg);

    /* Handshake response: poison padding with H4 value */
    {
        int total = 62 + WG_RESP_SIZE;
        uint8_t buf[256];
        memset(buf, 0, sizeof(buf));
        /* Poison padding with a value in H4 range */
        write32_le(buf, 500000000); /* in H4 range */
        write32_le(buf + 62, cfg.h2.min);
        fill_seq(buf + 62 + 4, WG_RESP_SIZE - 4);

        int out_len;
        uint8_t *out = transform_inbound(buf, total, &cfg, &out_len);
        ASSERT(out != NULL);
        ASSERT_EQ(read32_le(out), WG_HANDSHAKE_RESPONSE);
    }

    /* Handshake init: poison padding with H4 value */
    {
        int total = 46 + WG_INIT_SIZE;
        uint8_t buf[256];
        memset(buf, 0, sizeof(buf));
        write32_le(buf, 500000000);
        write32_le(buf + 46, cfg.h1.min);
        fill_seq(buf + 46 + 4, WG_INIT_SIZE - 4);

        int out_len;
        uint8_t *out = transform_inbound(buf, total, &cfg, &out_len);
        ASSERT(out != NULL);
        ASSERT_EQ(read32_le(out), WG_HANDSHAKE_INIT);
    }

    /* Cookie reply: poison padding */
    {
        int total = 30 + WG_COOKIE_SIZE;
        uint8_t buf[256];
        memset(buf, 0, sizeof(buf));
        write32_le(buf, 500000000);
        write32_le(buf + 30, cfg.h3.min);

        int out_len;
        uint8_t *out = transform_inbound(buf, total, &cfg, &out_len);
        ASSERT(out != NULL);
        ASSERT_EQ(read32_le(out), WG_COOKIE_REPLY);
    }

    /* Transport data */
    {
        int total = 17 + 100;
        uint8_t buf[256];
        memset(buf, 0, sizeof(buf));
        write32_le(buf + 17, cfg.h4.min);

        int out_len;
        uint8_t *out = transform_inbound(buf, total, &cfg, &out_len);
        ASSERT(out != NULL);
        ASSERT_EQ(read32_le(out), WG_TRANSPORT_DATA);
    }

    /* Junk packet: too small for anything */
    {
        uint8_t buf[17];
        memset(buf, 0, sizeof(buf));
        write32_le(buf, 500000000);

        int out_len;
        ASSERT(transform_inbound(buf, 17, &cfg, &out_len) == NULL);
    }
}

int main(void) {
    fprintf(stderr, "=== transform tests ===\n");
    RUN_TEST(outbound_handshake_init);
    RUN_TEST(outbound_handshake_response);
    RUN_TEST(outbound_cookie_reply);
    RUN_TEST(outbound_transport_data);
    RUN_TEST(inbound_handshake_init);
    RUN_TEST(inbound_handshake_response);
    RUN_TEST(inbound_cookie_reply);
    RUN_TEST(inbound_transport_data);
    RUN_TEST(roundtrip_init);
    RUN_TEST(roundtrip_response);
    RUN_TEST(roundtrip_cookie);
    RUN_TEST(roundtrip_transport);
    RUN_TEST(generate_junk);
    RUN_TEST(generate_junk_zero_jc);
    RUN_TEST(inbound_drops_unknown);
    RUN_TEST(inbound_drops_too_short);
    RUN_TEST(no_padding_s1_zero);
    RUN_TEST(no_padding_s2_zero);
    RUN_TEST(outbound_too_short);
    RUN_TEST(hrange_pick_contains);
    RUN_TEST(outbound_cookie_with_s3);
    RUN_TEST(outbound_transport_with_s4);
    RUN_TEST(inbound_scanning_s3);
    RUN_TEST(inbound_scanning_s4);
    RUN_TEST(inbound_hrange_accept);
    RUN_TEST(inbound_hrange_reject);
    RUN_TEST(roundtrip_v2);
    RUN_TEST(v1_backward);
    RUN_TEST(v2_false_positive);
    TEST_MAIN_END();
}
