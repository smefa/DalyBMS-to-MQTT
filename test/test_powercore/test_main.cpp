#include <initializer_list>
#include <string.h>
#include <unity.h>

#include "powercore.h"

using namespace powercore;

void setUp() {}
void tearDown() {}

static bool parse(const char* s, Mode& m) { return parseMode(s, strlen(s), m); }

void test_parse_names_any_case_and_whitespace() {
    Mode m = Mode::Normal;
    TEST_ASSERT_TRUE(parse("storage", m));
    TEST_ASSERT_EQUAL(Mode::Storage, m);
    TEST_ASSERT_TRUE(parse("HIGH", m));
    TEST_ASSERT_EQUAL(Mode::High, m);
    TEST_ASSERT_TRUE(parse("  Normal\r\n", m));
    TEST_ASSERT_EQUAL(Mode::Normal, m);
}

void test_parse_rejects_and_leaves_output_alone() {
    Mode m = Mode::High;
    TEST_ASSERT_FALSE(parse("", m));
    TEST_ASSERT_FALSE(parse("   ", m));
    TEST_ASSERT_FALSE(parse("fast", m));
    TEST_ASSERT_FALSE(parse("highest", m));
    TEST_ASSERT_FALSE(parse("hig", m));
    TEST_ASSERT_FALSE(parseMode(nullptr, 0, m));
    TEST_ASSERT_EQUAL(Mode::High, m);
}

void test_parse_is_not_nul_terminated() {
    // An MQTT payload is a pointer and a length; bytes after it are not ours.
    const char buf[] = {'h', 'i', 'g', 'h', 'x', 'x'};
    Mode m = Mode::Normal;
    TEST_ASSERT_TRUE(parseMode(buf, 4, m));
    TEST_ASSERT_EQUAL(Mode::High, m);
    TEST_ASSERT_FALSE(parseMode(buf, 5, m));
}

void test_names_round_trip() {
    for (Mode want : {Mode::Storage, Mode::Normal, Mode::High}) {
        Mode got = Mode::Normal;
        const char* n = modeName(want);
        TEST_ASSERT_TRUE(parseMode(n, strlen(n), got));
        TEST_ASSERT_EQUAL(want, got);
    }
}

void test_plan_short_sleep_is_one_chunk() {
    const SleepPlan p = planSleep(600, 4000ULL * 1000000ULL);
    TEST_ASSERT_EQUAL_UINT64(600ULL * 1000000ULL, p.sleep_us);
    TEST_ASSERT_EQUAL_UINT32(0, p.remaining_after_s);
}

void test_plan_day_is_chained_and_adds_up() {
    const uint64_t max_us = 4294967295ULL; // ~71.6 min, the classic hardware limit
    uint32_t remaining = 86400;
    uint64_t slept_s = 0;
    int chunks = 0;
    while (remaining > 0) {
        const SleepPlan p = planSleep(remaining, max_us);
        TEST_ASSERT_TRUE(p.sleep_us > 0);
        TEST_ASSERT_TRUE(p.sleep_us <= max_us);
        slept_s += p.sleep_us / 1000000ULL;
        remaining = p.remaining_after_s;
        TEST_ASSERT_TRUE(++chunks < 100);
    }
    TEST_ASSERT_EQUAL_UINT64(86400ULL, slept_s);
    TEST_ASSERT_EQUAL(21, chunks); // 86400 / 4294 s
}

void test_plan_always_makes_progress() {
    // A max below one second would otherwise plan a zero sleep forever.
    const SleepPlan p = planSleep(10, 500000ULL);
    TEST_ASSERT_EQUAL_UINT64(1000000ULL, p.sleep_us);
    TEST_ASSERT_EQUAL_UINT32(9, p.remaining_after_s);
}

void test_plan_zero_request() {
    const SleepPlan p = planSleep(0, 1000000000ULL);
    TEST_ASSERT_EQUAL_UINT64(0, p.sleep_us);
    TEST_ASSERT_EQUAL_UINT32(0, p.remaining_after_s);
}

void test_mode_record_round_trip() {
    for (Mode want : {Mode::Storage, Mode::Normal, Mode::High}) {
        Mode got = Mode::Normal;
        TEST_ASSERT_TRUE(readModeRecord(makeModeRecord(want), got));
        TEST_ASSERT_EQUAL(want, got);
    }
}

void test_mode_record_rejects_garbage() {
    Mode m = Mode::High;
    ModeRecord erased;
    memset(&erased, 0xFF, sizeof erased); // freshly erased flash
    TEST_ASSERT_FALSE(readModeRecord(erased, m));
    ModeRecord zero;
    memset(&zero, 0, sizeof zero);
    TEST_ASSERT_FALSE(readModeRecord(zero, m));

    ModeRecord r = makeModeRecord(Mode::Storage);
    r.mode = 7; // out of range, even with a matching check word
    r.check = r.magic ^ r.mode ^ 0xA5A5A5A5UL;
    TEST_ASSERT_FALSE(readModeRecord(r, m));

    r = makeModeRecord(Mode::Storage);
    r.mode = 1; // flipped without fixing the check
    TEST_ASSERT_FALSE(readModeRecord(r, m));
    TEST_ASSERT_EQUAL(Mode::High, m);
}

void test_chain_record_round_trip_and_garbage() {
    uint32_t rem = 0;
    TEST_ASSERT_TRUE(readChainRecord(makeChainRecord(12345), rem));
    TEST_ASSERT_EQUAL_UINT32(12345, rem);
    TEST_ASSERT_TRUE(readChainRecord(makeChainRecord(0), rem));
    TEST_ASSERT_EQUAL_UINT32(0, rem);

    ChainRecord junk;
    memset(&junk, 0xA5, sizeof junk);
    rem = 99;
    TEST_ASSERT_FALSE(readChainRecord(junk, rem));
    TEST_ASSERT_EQUAL_UINT32(99, rem);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_parse_names_any_case_and_whitespace);
    RUN_TEST(test_parse_rejects_and_leaves_output_alone);
    RUN_TEST(test_parse_is_not_nul_terminated);
    RUN_TEST(test_names_round_trip);
    RUN_TEST(test_plan_short_sleep_is_one_chunk);
    RUN_TEST(test_plan_day_is_chained_and_adds_up);
    RUN_TEST(test_plan_always_makes_progress);
    RUN_TEST(test_plan_zero_request);
    RUN_TEST(test_mode_record_round_trip);
    RUN_TEST(test_mode_record_rejects_garbage);
    RUN_TEST(test_chain_record_round_trip_and_garbage);
    return UNITY_END();
}
