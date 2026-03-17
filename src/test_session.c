#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "test.h"
#include "transform.h"
#include "blake2s.h"

/* Minimal session table test without socket headers.
 * Tests transform_inbound MAC1 recompute in reverse mode,
 * and session table logic using portable types. */

/* --- Session table unit test using portable struct --- */

#define SESSION_TABLE_SIZE 4096
#define SESSION_TABLE_MASK (SESSION_TABLE_SIZE - 1)

typedef struct {
    uint32_t sender_index;
    uint32_t ip;
    uint16_t port;
    int valid;
} test_session_t;

static test_session_t g_sessions[SESSION_TABLE_SIZE];

static void session_put(uint32_t index, uint32_t ip, uint16_t port) {
    uint32_t slot = index & SESSION_TABLE_MASK;
    for (int i = 0; i < 4; i++) {
        uint32_t s = (slot + i) & SESSION_TABLE_MASK;
        if (!g_sessions[s].valid || g_sessions[s].sender_index == index) {
            g_sessions[s].sender_index = index;
            g_sessions[s].ip = ip;
            g_sessions[s].port = port;
            g_sessions[s].valid = 1;
            return;
        }
    }
    g_sessions[slot].sender_index = index;
    g_sessions[slot].ip = ip;
    g_sessions[slot].port = port;
    g_sessions[slot].valid = 1;
}

static test_session_t *session_get(uint32_t index) {
    uint32_t slot = index & SESSION_TABLE_MASK;
    for (int i = 0; i < 4; i++) {
        uint32_t s = (slot + i) & SESSION_TABLE_MASK;
        if (g_sessions[s].valid && g_sessions[s].sender_index == index)
            return &g_sessions[s];
    }
    return NULL;
}

static void reset_sessions(void) {
    memset(g_sessions, 0, sizeof(g_sessions));
}

/* 1. Basic put/get */
static void test_session_basic(void) {
    reset_sessions();
    session_put(42, 0x0A000001, 12345);
    test_session_t *got = session_get(42);
    ASSERT(got != NULL);
    ASSERT_EQ(got->ip, 0x0A000001u);
    ASSERT_EQ(got->port, 12345);
}

/* 2. Get non-existent */
static void test_session_miss(void) {
    reset_sessions();
    ASSERT(session_get(999) == NULL);
}

/* 3. Update existing */
static void test_session_update(void) {
    reset_sessions();
    session_put(100, 0x0A000001, 12345);
    session_put(100, 0x0A000002, 54321);
    test_session_t *got = session_get(100);
    ASSERT(got != NULL);
    ASSERT_EQ(got->ip, 0x0A000002u);
    ASSERT_EQ(got->port, 54321);
}

/* 4. Multiple entries */
static void test_session_multiple(void) {
    reset_sessions();
    for (int i = 0; i < 10; i++)
        session_put((uint32_t)(i * 1000 + 7), 0x0A000000 + i, 1000 + i);

    for (int i = 0; i < 10; i++) {
        test_session_t *got = session_get((uint32_t)(i * 1000 + 7));
        ASSERT(got != NULL);
        ASSERT_EQ(got->ip, (uint32_t)(0x0A000000 + i));
        ASSERT_EQ(got->port, 1000 + i);
    }
}

/* 5. Collision handling (same lower bits) */
static void test_session_collision(void) {
    reset_sessions();
    uint32_t idx1 = 5;
    uint32_t idx2 = 5 + SESSION_TABLE_SIZE;

    session_put(idx1, 0x0A000001, 1111);
    session_put(idx2, 0x0A000002, 2222);

    test_session_t *got1 = session_get(idx1);
    test_session_t *got2 = session_get(idx2);
    ASSERT(got1 != NULL);
    ASSERT(got2 != NULL);
    ASSERT_EQ(got1->port, 1111);
    ASSERT_EQ(got2->port, 2222);
}

/* 6. Eviction on full probe */
static void test_session_eviction(void) {
    reset_sessions();
    /* Fill 4 consecutive slots with same hash bucket */
    for (int i = 0; i < 4; i++)
        session_put((uint32_t)(5 + i * SESSION_TABLE_SIZE), 0x01010100 + i, 100 + i);

    /* 5th entry should evict slot 5 */
    session_put((uint32_t)(5 + 4 * SESSION_TABLE_SIZE), 0x02020200, 9999);
    test_session_t *got = session_get((uint32_t)(5 + 4 * SESSION_TABLE_SIZE));
    ASSERT(got != NULL);
    ASSERT_EQ(got->port, 9999);
}

/* 7. transform_inbound recomputes MAC1 for init in reverse mode */
static void test_reverse_inbound_init_mac1(void) {
    awg_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.s1 = 20; cfg.s2 = 20;
    cfg.h1 = (hrange_t){1234567890, 1234567890};
    cfg.h2 = (hrange_t){1234567891, 1234567891};
    cfg.h3 = (hrange_t){1234567892, 1234567892};
    cfg.h4 = (hrange_t){1234567893, 1234567893};
    memset(cfg.server_pub, 0xAA, 32);
    memset(cfg.client_pub, 0xBB, 32);
    cfg.mode = AWG_MODE_REVERSE;
    config_compute(&cfg);

    uint8_t buf[20 + WG_INIT_SIZE];
    memset(buf, 0x55, 20);
    uint32_t h1 = cfg.h1.min;
    memcpy(buf + 20, &h1, 4);
    for (int i = 4; i < WG_INIT_SIZE; i++)
        buf[20 + i] = (uint8_t)i;

    int out_len;
    uint8_t *out = transform_inbound(buf, 20 + WG_INIT_SIZE, &cfg, &out_len);
    ASSERT(out != NULL);
    ASSERT_EQ(out_len, WG_INIT_SIZE);

    uint32_t msg_type;
    memcpy(&msg_type, out, 4);
    ASSERT_EQ(msg_type, WG_HANDSHAKE_INIT);

    /* Verify MAC1 was recomputed with server key (responder = WG server) */
    uint8_t expected_mac1[16];
    blake2s_128mac(cfg.mac1key_server, out, 116, expected_mac1);
    ASSERT_MEM_EQ(out + 116, expected_mac1, 16);
}

/* 8. Normal mode DOES recompute MAC1 for inbound init (client key) */
static void test_normal_inbound_init_mac1(void) {
    awg_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.s1 = 20; cfg.s2 = 20;
    cfg.h1 = (hrange_t){1234567890, 1234567890};
    cfg.h2 = (hrange_t){1234567891, 1234567891};
    cfg.h3 = (hrange_t){1234567892, 1234567892};
    cfg.h4 = (hrange_t){1234567893, 1234567893};
    memset(cfg.client_pub, 0xBB, 32);
    cfg.mode = AWG_MODE_NORMAL;
    config_compute(&cfg);

    uint8_t buf[20 + WG_INIT_SIZE];
    memset(buf, 0x55, 20);
    uint32_t h1 = cfg.h1.min;
    memcpy(buf + 20, &h1, 4);
    for (int i = 4; i < WG_INIT_SIZE; i++)
        buf[20 + i] = (uint8_t)i;

    int out_len;
    uint8_t *out = transform_inbound(buf, 20 + WG_INIT_SIZE, &cfg, &out_len);
    ASSERT(out != NULL);
    ASSERT_EQ(out_len, WG_INIT_SIZE);

    /* Verify MAC1 was recomputed with client key (recipient = WG client) */
    uint8_t expected_mac1[16];
    blake2s_128mac(cfg.mac1key_client, out, 116, expected_mac1);
    ASSERT_MEM_EQ(out + 116, expected_mac1, 16);
}

/* 9. Server mode also recomputes MAC1 for inbound init */
static void test_server_inbound_init_mac1(void) {
    awg_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.s1 = 20; cfg.s2 = 20;
    cfg.h1 = (hrange_t){1234567890, 1234567890};
    cfg.h2 = (hrange_t){1234567891, 1234567891};
    cfg.h3 = (hrange_t){1234567892, 1234567892};
    cfg.h4 = (hrange_t){1234567893, 1234567893};
    memset(cfg.server_pub, 0xAA, 32);
    memset(cfg.client_pub, 0xBB, 32);
    cfg.mode = AWG_MODE_SERVER;
    config_compute(&cfg);

    uint8_t buf[20 + WG_INIT_SIZE];
    memset(buf, 0x55, 20);
    uint32_t h1 = cfg.h1.min;
    memcpy(buf + 20, &h1, 4);
    for (int i = 4; i < WG_INIT_SIZE; i++)
        buf[20 + i] = (uint8_t)i;

    int out_len;
    uint8_t *out = transform_inbound(buf, 20 + WG_INIT_SIZE, &cfg, &out_len);
    ASSERT(out != NULL);
    ASSERT_EQ(out_len, WG_INIT_SIZE);

    uint8_t expected_mac1[16];
    blake2s_128mac(cfg.mac1key_server, out, 116, expected_mac1);
    ASSERT_MEM_EQ(out + 116, expected_mac1, 16);
}

int main(void) {
    fprintf(stderr, "=== session & reverse tests ===\n");
    RUN_TEST(session_basic);
    RUN_TEST(session_miss);
    RUN_TEST(session_update);
    RUN_TEST(session_multiple);
    RUN_TEST(session_collision);
    RUN_TEST(session_eviction);
    RUN_TEST(reverse_inbound_init_mac1);
    RUN_TEST(normal_inbound_init_mac1);
    RUN_TEST(server_inbound_init_mac1);
    TEST_MAIN_END();
}
