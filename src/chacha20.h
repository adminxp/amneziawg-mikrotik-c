#ifndef AWG_CHACHA20_H
#define AWG_CHACHA20_H

#include <stdint.h>

#define CHACHA20_KEY_SIZE   32
#define CHACHA20_NONCE_SIZE 12
#define CHACHA20_BLOCK_SIZE 64

/* One 64-byte keystream block. Hot primitive: AWG 3.0 header protection needs
 * exactly one block per transport packet (only 16 header bytes are covered). */
void chacha20_block(const uint8_t key[CHACHA20_KEY_SIZE],
                    const uint8_t nonce[CHACHA20_NONCE_SIZE],
                    uint32_t counter, uint8_t out[CHACHA20_BLOCK_SIZE]);

/* len bytes of keystream starting at the given block counter. */
void chacha20_keystream(const uint8_t key[CHACHA20_KEY_SIZE],
                        const uint8_t nonce[CHACHA20_NONCE_SIZE],
                        uint32_t counter, uint8_t *out, int len);

/* XOR buf in place with the keystream from block counter 0 (what AmneziaWG
 * uses for header protection). */
void chacha20_xor(const uint8_t key[CHACHA20_KEY_SIZE],
                  const uint8_t nonce[CHACHA20_NONCE_SIZE],
                  uint8_t *buf, int len);

/* XOR up to one block against an already-computed keystream block. */
static inline void chacha20_xor_ks(uint8_t *buf, const uint8_t *ks, int len) {
    for (int i = 0; i < len; i++)
        buf[i] ^= ks[i];
}

#endif
