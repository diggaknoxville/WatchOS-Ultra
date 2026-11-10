// Host tests for include/SemVer.h (pio test -e native)
#include <unity.h>
#include "SemVer.h"

void setUp(void) {}
void tearDown(void) {}

static void test_equal(void) {
    TEST_ASSERT_EQUAL_INT(0, semverCompare("1.4.2", "1.4.2"));
    TEST_ASSERT_EQUAL_INT(0, semverCompare("0.0.0", "0.0.0"));
}

static void test_ordering(void) {
    TEST_ASSERT_TRUE(semverCompare("1.4.2", "1.4.3") < 0);   // patch
    TEST_ASSERT_TRUE(semverCompare("1.4.3", "1.4.2") > 0);
    TEST_ASSERT_TRUE(semverCompare("1.4.9", "1.5.0") < 0);   // minor
    TEST_ASSERT_TRUE(semverCompare("1.5.0", "1.4.9") > 0);
    TEST_ASSERT_TRUE(semverCompare("1.9.9", "2.0.0") < 0);   // major
    TEST_ASSERT_TRUE(semverCompare("2.0.0", "1.9.9") > 0);
}

static void test_leading_v(void) {
    TEST_ASSERT_EQUAL_INT(0, semverCompare("v1.4.2", "1.4.2"));
    TEST_ASSERT_EQUAL_INT(0, semverCompare("1.4.2", "V1.4.2"));
    TEST_ASSERT_TRUE(semverCompare("v1.4.2", "v1.5.0") < 0);
}

static void test_missing_parts(void) {
    TEST_ASSERT_EQUAL_INT(0, semverCompare("1.4", "1.4.0"));
    TEST_ASSERT_EQUAL_INT(0, semverCompare("2", "2.0.0"));
    TEST_ASSERT_TRUE(semverCompare("1.4", "1.4.1") < 0);
    TEST_ASSERT_TRUE(semverCompare("", "0.0.1") < 0);
    TEST_ASSERT_TRUE(semverCompare(nullptr, "0.0.1") < 0);
}

static void test_numeric_not_lexical(void) {
    TEST_ASSERT_TRUE(semverCompare("1.10.0", "1.9.9") > 0);
    TEST_ASSERT_TRUE(semverCompare("1.9.9", "1.10.0") < 0);
    TEST_ASSERT_TRUE(semverCompare("10.0.0", "9.99.99") > 0);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_equal);
    RUN_TEST(test_ordering);
    RUN_TEST(test_leading_v);
    RUN_TEST(test_missing_parts);
    RUN_TEST(test_numeric_not_lexical);
    return UNITY_END();
}
