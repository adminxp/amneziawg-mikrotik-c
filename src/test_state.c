/* Learned transport preference persisted across restarts. The file is the only
 * thing that survives a container restart, so its exact contract matters:
 * anything that is not a literal '6' must read as "no preference" and leave the
 * stock IPv4-first order alone. */
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "test.h"
#include "proxy.h"

#define PATH "/tmp/awg_test_state"

static void put(const char *s) {
    int fd = open(PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    (void)!write(fd, s, strlen(s));
    close(fd);
}

static void test_missing_file_means_no_preference(void) {
    unlink(PATH);
    ASSERT_EQ(state_read_prefer6(PATH), 0);
}

static void test_empty_path_is_ignored(void) {
    ASSERT_EQ(state_read_prefer6(""), 0);
    ASSERT_EQ(state_read_prefer6(NULL), 0);
    /* Writing nowhere is an error, not a silent success. */
    ASSERT_EQ(state_write_prefer6("", 1), -1);
    ASSERT_EQ(state_write_prefer6(NULL, 1), -1);
}

static void test_ipv6_round_trip(void) {
    unlink(PATH);
    ASSERT_EQ(state_write_prefer6(PATH, 1), 0);
    ASSERT_EQ(state_read_prefer6(PATH), 1);
}

/* Unlearning has to work as well as learning: an IPv6 route that stops
 * carrying traffic must not keep winning the dial order forever. Note this
 * restores the stock order rather than disabling IPv6 — the probe still runs. */
static void test_ipv4_overwrites_ipv6(void) {
    ASSERT_EQ(state_write_prefer6(PATH, 1), 0);
    ASSERT_EQ(state_write_prefer6(PATH, 0), 0);
    ASSERT_EQ(state_read_prefer6(PATH), 0);
}

static void test_one_byte_only(void) {
    ASSERT_EQ(state_write_prefer6(PATH, 1), 0);
    int fd = open(PATH, O_RDONLY);
    ASSERT(fd >= 0);
    char buf[8];
    ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);
    ASSERT_EQ((int)n, 1);
    ASSERT_EQ(buf[0], '6');
}

/* A truncated or garbled file must not be read as "prefer IPv6" — the safe
 * direction is always back to the stock order. */
static void test_garbage_reads_as_no_preference(void) {
    put("");
    ASSERT_EQ(state_read_prefer6(PATH), 0);
    put("x");
    ASSERT_EQ(state_read_prefer6(PATH), 0);
    put("46");
    ASSERT_EQ(state_read_prefer6(PATH), 0);
    put("6");
    ASSERT_EQ(state_read_prefer6(PATH), 1);
}

static void test_unwritable_path_reports_failure(void) {
    ASSERT_EQ(state_write_prefer6("/proc/nonexistent/awg.state", 1), -1);
}

int main(void) {
    fprintf(stderr, "=== state file tests ===\n");
    RUN_TEST(missing_file_means_no_preference);
    RUN_TEST(empty_path_is_ignored);
    RUN_TEST(ipv6_round_trip);
    RUN_TEST(ipv4_overwrites_ipv6);
    RUN_TEST(one_byte_only);
    RUN_TEST(garbage_reads_as_no_preference);
    RUN_TEST(unwritable_path_reports_failure);
    unlink(PATH);
    TEST_MAIN_END();
}
