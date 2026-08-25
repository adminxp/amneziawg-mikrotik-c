#ifndef AWG_TEST_H
#define AWG_TEST_H

#include <stdio.h>
#include <string.h>

static int g_tests_run, g_tests_passed, g_tests_failed;
static const char *g_current_test;

/* ASSERT returns out of the test function, not out of RUN_TEST, so a failed
 * test used to be counted and printed as PASS right after its own FAIL line —
 * the summary then read like "23/23 tests passed, 1 FAILED". Compare the
 * failure counter across the call and only claim PASS when it did not move. */
#define RUN_TEST(name) do { \
    int failed_before = g_tests_failed; \
    g_current_test = #name; g_tests_run++; \
    test_##name(); \
    if (g_tests_failed == failed_before) { \
        g_tests_passed++; \
        fprintf(stderr, "  PASS  %s\n", #name); \
    } \
} while(0)

#define ASSERT(cond) do { if (!(cond)) { \
    fprintf(stderr, "  FAIL  %s: %s:%d: %s\n", g_current_test, __FILE__, __LINE__, #cond); \
    g_tests_failed++; g_tests_run--; return; \
}} while(0)

#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_MEM_EQ(a, b, n) ASSERT(memcmp((a), (b), (n)) == 0)

#define TEST_MAIN_END() do { \
    fprintf(stderr, "\n%d/%d tests passed", g_tests_passed, g_tests_run + g_tests_failed); \
    if (g_tests_failed) fprintf(stderr, ", %d FAILED", g_tests_failed); \
    fprintf(stderr, "\n"); \
    return g_tests_failed ? 1 : 0; \
} while(0)

__attribute__((unused))
static int hex2byte(char hi, char lo) {
    int a = (hi >= '0' && hi <= '9') ? hi - '0' : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10 : -1;
    int b = (lo >= '0' && lo <= '9') ? lo - '0' : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10 : -1;
    return (a < 0 || b < 0) ? -1 : (a << 4) | b;
}

__attribute__((unused))
static int hex_decode(const char *hex, uint8_t *out, int maxlen) {
    int len = 0;
    for (int i = 0; hex[i] && hex[i+1]; i += 2) {
        if (len >= maxlen) return -1;
        int v = hex2byte(hex[i], hex[i+1]);
        if (v < 0) return -1;
        out[len++] = (uint8_t)v;
    }
    return len;
}

#endif
