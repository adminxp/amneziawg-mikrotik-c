#include <stdint.h>
#include "test.h"
#include "base64.h"

static void test_decode_valid(void) {
    unsigned char out[64];
    int n;

    /* "SGVsbG8=" -> "Hello" */
    n = base64_decode("SGVsbG8=", 8, out, 64);
    ASSERT_EQ(n, 5);
    ASSERT(memcmp(out, "Hello", 5) == 0);

    /* WG key: 44 chars -> 32 bytes */
    n = base64_decode("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=", 44, out, 64);
    ASSERT_EQ(n, 32);
    for (int i = 0; i < 32; i++) ASSERT_EQ(out[i], 0);

    /* "/+/+" -> 0xFF, 0xEF, 0xFE */
    n = base64_decode("/+/+", 4, out, 64);
    ASSERT_EQ(n, 3);
}

static void test_decode_padding(void) {
    unsigned char out[64];
    int n;

    /* With padding */
    n = base64_decode("YQ==", 4, out, 64);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(out[0], 'a');

    /* Without padding (stripped) */
    n = base64_decode("YQ", 2, out, 64);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(out[0], 'a');

    /* Two-byte remainder */
    n = base64_decode("YWI=", 4, out, 64);
    ASSERT_EQ(n, 2);
    ASSERT(memcmp(out, "ab", 2) == 0);

    n = base64_decode("YWI", 3, out, 64);
    ASSERT_EQ(n, 2);
    ASSERT(memcmp(out, "ab", 2) == 0);
}

static void test_decode_invalid(void) {
    unsigned char out[64];

    /* Single char (invalid: len%4==1 after stripping) */
    ASSERT_EQ(base64_decode("A", 1, out, 64), -1);

    /* Invalid character */
    ASSERT_EQ(base64_decode("A@AA", 4, out, 64), -1);

    /* Output buffer too small */
    ASSERT_EQ(base64_decode("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=", 44, out, 2), -1);
}

int main(void) {
    fprintf(stderr, "=== base64 tests ===\n");
    RUN_TEST(decode_valid);
    RUN_TEST(decode_padding);
    RUN_TEST(decode_invalid);
    TEST_MAIN_END();
}
