#include <stdint.h>
#include "test.h"
#include "blake2s.h"

static void test_blake2s_256_empty(void) {
    uint8_t out[32], expected[32];
    hex_decode("69217a3079908094e11121d042354a7c1f55b6482ca1a51e1b250dfd1ed0eef9", expected, 32);
    blake2s_256(NULL, 0, out);
    ASSERT_MEM_EQ(out, expected, 32);
}

static void test_blake2s_256_abc(void) {
    uint8_t out[32], expected[32];
    hex_decode("508c5e8c327c14e2e1a72ba34eeb452f37458b209ed63a294d999b4c86675982", expected, 32);
    blake2s_256("abc", 3, out);
    ASSERT_MEM_EQ(out, expected, 32);
}

static void test_blake2s_256_long(void) {
    uint8_t data[200], out[32], expected[32];
    for (int i = 0; i < 200; i++) data[i] = (uint8_t)i;
    hex_decode("6d244e1a06ce4ef578dd0f63aff0936706735119ca9c8d22d86c801414ab9741", expected, 32);
    blake2s_256(data, 200, out);
    ASSERT_MEM_EQ(out, expected, 32);
}

static void test_blake2s_256_exact_block(void) {
    uint8_t data[64], out[32], expected[32];
    for (int i = 0; i < 64; i++) data[i] = (uint8_t)i;
    hex_decode("56f34e8b96557e90c1f24b52d0c89d51086acf1b00f634cf1dde9233b8eaaa3e", expected, 32);
    blake2s_256(data, 64, out);
    ASSERT_MEM_EQ(out, expected, 32);
}

static void test_blake2s_256_two_blocks(void) {
    uint8_t data[128], out[32], expected[32];
    for (int i = 0; i < 128; i++) data[i] = (uint8_t)i;
    hex_decode("1fa877de67259d19863a2a34bcc6962a2b25fcbf5cbecd7ede8f1fa36688a796", expected, 32);
    blake2s_256(data, 128, out);
    ASSERT_MEM_EQ(out, expected, 32);
}

static void test_blake2s_128_keyed(void) {
    uint8_t key[32], out[16], expected[16];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
    hex_decode("b0c007dc5d354f7b3c65c08ff124ba08", expected, 16);
    blake2s_128mac(key, "test data for keyed blake2s", 27, out);
    ASSERT_MEM_EQ(out, expected, 16);
}

static void test_blake2s_128_keyed_empty(void) {
    uint8_t key[32], out[16], expected[16];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)(i + 0x80);
    hex_decode("111595da88fc8f625eca3f6a1043234c", expected, 16);
    blake2s_128mac(key, NULL, 0, out);
    ASSERT_MEM_EQ(out, expected, 16);
}

static void test_blake2s_128_keyed_long(void) {
    uint8_t key[32], data[300], out[16], expected[16];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)(i * 3);
    for (int i = 0; i < 300; i++) data[i] = (uint8_t)i;
    hex_decode("b112a0843c4677eb4e326d4e9aaaeff5", expected, 16);
    blake2s_128mac(key, data, 300, out);
    ASSERT_MEM_EQ(out, expected, 16);
}

static void test_compute_mac1_key(void) {
    uint8_t pubkey[32], mac1key[32], expected[32];
    for (int i = 0; i < 32; i++) pubkey[i] = (uint8_t)(i + 1);
    hex_decode("121b33018813efaa1d3128cdec1392897828e98831f01822c250ddfdf7090183", expected, 32);
    compute_mac1_key(pubkey, mac1key);
    ASSERT_MEM_EQ(mac1key, expected, 32);
}

static void test_recompute_mac1(void) {
    uint8_t serverPub[32], mac1key[32], buf[148];
    uint8_t expected_mac1[16];
    for (int i = 0; i < 32; i++) serverPub[i] = (uint8_t)(i + 0x10);
    compute_mac1_key(serverPub, mac1key);

    /* Build fake handshake init */
    memset(buf, 0, 148);
    uint32_t type_val = 1234567890;
    memcpy(buf, &type_val, 4);
    for (int i = 4; i < 116; i++) buf[i] = (uint8_t)i;
    /* buf[116:132] = MAC1 (to be computed), buf[132:148] = MAC2 (zeros) */

    hex_decode("a5186b65c1d54a0c234a7e374db1551c", expected_mac1, 16);
    recompute_mac1(buf, mac1key);
    ASSERT_MEM_EQ(buf + 116, expected_mac1, 16);
}

static void test_blake2s_incremental(void) {
    uint8_t data[200], out[32], expected[32];
    for (int i = 0; i < 200; i++) data[i] = (uint8_t)i;

    blake2s_256(data, 200, out);

    hex_decode("6d244e1a06ce4ef578dd0f63aff0936706735119ca9c8d22d86c801414ab9741", expected, 32);
    ASSERT_MEM_EQ(out, expected, 32);
}

int main(void) {
    fprintf(stderr, "=== blake2s tests ===\n");
    RUN_TEST(blake2s_256_empty);
    RUN_TEST(blake2s_256_abc);
    RUN_TEST(blake2s_256_long);
    RUN_TEST(blake2s_256_exact_block);
    RUN_TEST(blake2s_256_two_blocks);
    RUN_TEST(blake2s_128_keyed);
    RUN_TEST(blake2s_128_keyed_empty);
    RUN_TEST(blake2s_128_keyed_long);
    RUN_TEST(compute_mac1_key);
    RUN_TEST(recompute_mac1);
    RUN_TEST(blake2s_incremental);
    TEST_MAIN_END();
}
