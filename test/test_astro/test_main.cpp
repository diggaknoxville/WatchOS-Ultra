// Host tests for include/Astro.h (pio test -e native)
#include <unity.h>
#include "Astro.h"

void setUp(void) {}
void tearDown(void) {}

// Berlin, June solstice, CEST (UTC+2): sunrise 04:43, sunset 21:33 local
static void test_sun_berlin_june_solstice(void) {
    int rise = -1, set = -1;
    TEST_ASSERT_TRUE(astro::sunTimes(52.52, 13.405, 171, 7200, rise, set));
    TEST_ASSERT_INT_WITHIN(6, 4 * 60 + 43, rise);
    TEST_ASSERT_INT_WITHIN(6, 21 * 60 + 33, set);
}

// Berlin, December solstice, CET (UTC+1): sunrise 08:15, sunset 15:54 local
static void test_sun_berlin_december_solstice(void) {
    int rise = -1, set = -1;
    TEST_ASSERT_TRUE(astro::sunTimes(52.52, 13.405, 354, 3600, rise, set));
    TEST_ASSERT_INT_WITHIN(6, 8 * 60 + 15, rise);
    TEST_ASSERT_INT_WITHIN(6, 15 * 60 + 54, set);
}

// The UTC offset only shifts the result
static void test_sun_utc_offset_shifts(void) {
    int riseUtc = -1, setUtc = -1, rise = -1, set = -1;
    TEST_ASSERT_TRUE(astro::sunTimes(52.52, 13.405, 171, 0, riseUtc, setUtc));
    TEST_ASSERT_TRUE(astro::sunTimes(52.52, 13.405, 171, 7200, rise, set));
    TEST_ASSERT_EQUAL_INT(riseUtc + 120, rise);
    TEST_ASSERT_EQUAL_INT(setUtc + 120, set);
}

static void test_sun_polar_night_and_day(void) {
    int rise = -1, set = -1;
    TEST_ASSERT_FALSE(astro::sunTimes(78.0, 15.0, 355, 3600, rise, set));   // late December
    TEST_ASSERT_FALSE(astro::sunTimes(78.0, 15.0, 171, 7200, rise, set));   // midnight sun
    TEST_ASSERT_EQUAL_INT(-1, rise);                                        // outputs untouched
    TEST_ASSERT_EQUAL_INT(-1, set);
}

static void test_moon_full(void) {
    double age = 0, illum = 0;
    astro::moonPhase((time_t)1706205240, age, illum);   // 2024-01-25 17:54 UTC
    TEST_ASSERT_TRUE(illum > 0.98);
    TEST_ASSERT_DOUBLE_WITHIN(1.0, 29.530588853 / 2.0, age);
}

static void test_moon_new(void) {
    double age = 0, illum = 1;
    astro::moonPhase((time_t)1704974220, age, illum);   // 2024-01-11 11:57 UTC
    TEST_ASSERT_TRUE(illum < 0.02);
    TEST_ASSERT_TRUE(age < 1.0 || age > 28.53);
}

static void test_moon_reference_epoch(void) {
    double age = 1, illum = 1;
    astro::moonPhase((time_t)947182440, age, illum);    // 2000-01-06 18:14 UTC
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0, age);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0, illum);
    astro::moonPhase((time_t)0, age, illum);            // before the reference: still 0..synodic
    TEST_ASSERT_TRUE(age >= 0.0 && age < 29.530588853);
    TEST_ASSERT_TRUE(illum >= 0.0 && illum <= 1.0);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_sun_berlin_june_solstice);
    RUN_TEST(test_sun_berlin_december_solstice);
    RUN_TEST(test_sun_utc_offset_shifts);
    RUN_TEST(test_sun_polar_night_and_day);
    RUN_TEST(test_moon_full);
    RUN_TEST(test_moon_new);
    RUN_TEST(test_moon_reference_epoch);
    return UNITY_END();
}
