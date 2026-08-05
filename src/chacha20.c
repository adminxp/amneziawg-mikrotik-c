#include "chacha20.h"
#include <string.h>

static inline uint32_t rotl32(uint32_t x, unsigned n) {
    return (x << n) | (x >> (32 - n));
}

static inline uint32_t load32_le(const void *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static inline void store32_le(void *p, uint32_t v) {
    memcpy(p, &v, 4);
}

#define QR(a, b, c, d) do { \
    a += b; d ^= a; d = rotl32(d, 16); \
    c += d; b ^= c; b = rotl32(b, 12); \
    a += b; d ^= a; d = rotl32(d, 8);  \
    c += d; b ^= c; b = rotl32(b, 7);  \
} while (0)

static void chacha20_core(const uint32_t in[16], uint8_t out[64]) {
    uint32_t x[16];
    memcpy(x, in, sizeof(x));

    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[ 8], x[12]);
        QR(x[1], x[5], x[ 9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[ 8], x[13]);
        QR(x[3], x[4], x[ 9], x[14]);
    }

    for (int i = 0; i < 16; i++)
        store32_le(out + i * 4, x[i] + in[i]);
}

static void chacha20_setup(uint32_t st[16], const uint8_t key[32],
                           const uint8_t nonce[12], uint32_t counter) {
    st[0] = 0x61707865; st[1] = 0x3320646E;
    st[2] = 0x79622D32; st[3] = 0x6B206574;
    for (int i = 0; i < 8; i++)
        st[4 + i] = load32_le(key + i * 4);
    st[12] = counter;
    for (int i = 0; i < 3; i++)
        st[13 + i] = load32_le(nonce + i * 4);
}

void chacha20_block(const uint8_t key[32], const uint8_t nonce[12],
                    uint32_t counter, uint8_t out[64]) {
    uint32_t st[16];
    chacha20_setup(st, key, nonce, counter);
    chacha20_core(st, out);
}

void chacha20_keystream(const uint8_t key[32], const uint8_t nonce[12],
                        uint32_t counter, uint8_t *out, int len) {
    uint32_t st[16];
    uint8_t blk[64];

    chacha20_setup(st, key, nonce, counter);
    while (len >= 64) {
        chacha20_core(st, out);
        st[12]++;
        out += 64;
        len -= 64;
    }
    if (len > 0) {
        chacha20_core(st, blk);
        memcpy(out, blk, (size_t)len);
    }
}

void chacha20_xor(const uint8_t key[32], const uint8_t nonce[12],
                  uint8_t *buf, int len) {
    uint32_t st[16];
    uint8_t blk[64];

    chacha20_setup(st, key, nonce, 0);
    while (len > 0) {
        int n = len < 64 ? len : 64;
        chacha20_core(st, blk);
        st[12]++;
        chacha20_xor_ks(buf, blk, n);
        buf += n;
        len -= n;
    }
}
