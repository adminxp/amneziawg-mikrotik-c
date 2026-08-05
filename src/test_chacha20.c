#include <stdint.h>
#include <string.h>
#include "test.h"
#include "chacha20.h"

/* RFC 8439 §2.3.2 / §2.4.2 test vectors. */

static const char *KEY_HEX =
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";

/* 1. §2.3.2 — single block at counter 1 */
static void test_rfc8439_block(void) {
    const char *nonce_hex = "000000090000004a00000000";
    const char *want_hex =
        "10f1e7e4d13b5915500fdd1fa32071c4"
        "c7d1f4c733c068030422aa9ac3d46c4e"
        "d2826446079faa0914c2d705d98b02a2"
        "b5129cd1de164eb9cbd083e8a2503c4e";

    uint8_t key[32], nonce[12], want[64], got[64];
    ASSERT_EQ(hex_decode(KEY_HEX, key, 32), 32);
    ASSERT_EQ(hex_decode(nonce_hex, nonce, 12), 12);
    ASSERT_EQ(hex_decode(want_hex, want, 64), 64);

    chacha20_block(key, nonce, 1, got);
    ASSERT_MEM_EQ(got, want, 64);
}

/* 2. §2.4.2 — 114-byte encryption at counter 1 */
static void test_rfc8439_encrypt(void) {
    const char *nonce_hex = "000000000000004a00000000";
    const char *plain =
        "Ladies and Gentlemen of the class of '99: If I could offer you "
        "only one tip for the future, sunscreen would be it.";
    const char *want_hex =
        "6e2e359a2568f98041ba0728dd0d6981"
        "e97e7aec1d4360c20a27afccfd9fae0b"
        "f91b65c5524733ab8f593dabcd62b357"
        "1639d624e65152ab8f530c359f0861d8"
        "07ca0dbf500d6a6156a38e088a22b65e"
        "52bc514d16ccf806818ce91ab7793736"
        "5af90bbf74a35be6b40b8eedf2785e42"
        "874d";

    uint8_t key[32], nonce[12], want[114], buf[114], ks[114];
    int len = (int)strlen(plain);
    ASSERT_EQ(len, 114);
    ASSERT_EQ(hex_decode(KEY_HEX, key, 32), 32);
    ASSERT_EQ(hex_decode(nonce_hex, nonce, 12), 12);
    ASSERT_EQ(hex_decode(want_hex, want, 114), 114);

    memcpy(buf, plain, 114);
    chacha20_keystream(key, nonce, 1, ks, 114);
    for (int i = 0; i < 114; i++) buf[i] ^= ks[i];
    ASSERT_MEM_EQ(buf, want, 114);
}

/* 3. chacha20_xor starts at counter 0 and runs a continuous keystream */
static void test_xor_is_keystream_from_zero(void) {
    uint8_t key[32], nonce[12], ks[200], buf[200], plain[200];
    ASSERT_EQ(hex_decode(KEY_HEX, key, 32), 32);
    for (int i = 0; i < 12; i++) nonce[i] = (uint8_t)(0xA0 + i);
    for (int i = 0; i < 200; i++) plain[i] = (uint8_t)i;

    memcpy(buf, plain, 200);
    chacha20_xor(key, nonce, buf, 200);
    chacha20_keystream(key, nonce, 0, ks, 200);
    for (int i = 0; i < 200; i++)
        ASSERT_EQ(buf[i], (uint8_t)(plain[i] ^ ks[i]));
}

/* 4. The first block of the keystream equals chacha20_block(counter=0) —
 * this is what the inbound header-protection fast path relies on. */
static void test_block_matches_keystream_head(void) {
    uint8_t key[32], nonce[12], blk[64], ks[64];
    ASSERT_EQ(hex_decode(KEY_HEX, key, 32), 32);
    for (int i = 0; i < 12; i++) nonce[i] = (uint8_t)(i * 7 + 1);

    chacha20_block(key, nonce, 0, blk);
    chacha20_keystream(key, nonce, 0, ks, 64);
    ASSERT_MEM_EQ(blk, ks, 64);
}

/* 5. XOR is an involution — encrypt/decrypt round-trip at every length */
static void test_roundtrip_all_lengths(void) {
    uint8_t key[32], nonce[12], plain[300], buf[300];
    ASSERT_EQ(hex_decode(KEY_HEX, key, 32), 32);
    for (int i = 0; i < 12; i++) nonce[i] = (uint8_t)(0x5A ^ i);
    for (int i = 0; i < 300; i++) plain[i] = (uint8_t)(i * 3);

    for (int len = 0; len <= 300; len++) {
        memcpy(buf, plain, 300);
        chacha20_xor(key, nonce, buf, len);
        if (len > 0) ASSERT(memcmp(buf, plain, (size_t)len) != 0);
        chacha20_xor(key, nonce, buf, len);
        ASSERT_MEM_EQ(buf, plain, 300);
    }
}

/* 6. Distinct nonces give distinct keystreams (nonce is the only per-packet
 * input in header protection) */
static void test_nonce_separation(void) {
    uint8_t key[32], n1[12], n2[12], b1[64], b2[64];
    ASSERT_EQ(hex_decode(KEY_HEX, key, 32), 32);
    memset(n1, 0, 12);
    memset(n2, 0, 12);
    n2[11] = 1;

    chacha20_block(key, n1, 0, b1);
    chacha20_block(key, n2, 0, b2);
    ASSERT(memcmp(b1, b2, 64) != 0);
}

int main(void) {
    RUN_TEST(rfc8439_block);
    RUN_TEST(rfc8439_encrypt);
    RUN_TEST(xor_is_keystream_from_zero);
    RUN_TEST(block_matches_keystream_head);
    RUN_TEST(roundtrip_all_lengths);
    RUN_TEST(nonce_separation);
    TEST_MAIN_END();
}
