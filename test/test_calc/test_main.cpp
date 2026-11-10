// Host tests for include/CalcParser.h (pio test -e native)
#include <unity.h>
#include "CalcParser.h"

void setUp(void) {}
void tearDown(void) {}

static const double EPS = 1e-9;

// Evaluates in degree mode with Ans = 0 and fails the test on a parse error
static double ev(const char* expr) {
    double out = -12345.0;
    TEST_ASSERT_TRUE_MESSAGE(calc::evaluate(expr, true, 0.0, out), expr);
    return out;
}

static bool fails(const char* expr) {
    double out = 0.0;
    return !calc::evaluate(expr, true, 0.0, out);
}

static void test_precedence(void) {
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 14.0, ev("2+3*4"));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 14.0, ev("2 + 3 * 4"));     // operator keys insert spaces
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 4.0, ev("10-2*3"));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 3.0, ev("10-4-3"));         // left-associative
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 2.5, ev("10/2/2"));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 19.0, ev("1+2*3^2"));
}

static void test_parentheses(void) {
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 20.0, ev("(2+3)*4"));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 9.0, ev("((1+2))*(4-1)"));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 20.0, ev("(2+3)*(4"));      // unclosed parenthesis closes implicitly
}

static void test_power(void) {
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 1024.0, ev("2^10"));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 64.0, ev("2^3^2"));         // left-associative: (2^3)^2
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 0.25, ev("4^(-1)"));        // the 1/x key
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 3.0, ev("9^0.5"));
}

static void test_unary_minus(void) {
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 2.0, ev("-3+5"));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, -6.0, ev("2*-3"));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 3.0, ev("--3"));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, -5.0, ev("-(2+3)"));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 5.0, ev("+5"));
}

static void test_trig_degrees_and_radians(void) {
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 0.5, ev("sin(30)"));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 0.5, ev("cos(60)"));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 1.0, ev("tan(45)"));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 30.0, ev("asin(0.5)"));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 45.0, ev("atan(1)"));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 0.5, ev("sin 30"));         // argument without parentheses

    double out = 0.0;
    TEST_ASSERT_TRUE(calc::evaluate("sin(pi/6)", false, 0.0, out));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 0.5, out);
    TEST_ASSERT_TRUE(calc::evaluate("acos(0)", false, 0.0, out));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 1.5707963267948966, out);
}

static void test_functions_and_constants(void) {
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 4.0, ev("sqrt(16)"));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 5.0, ev("sqrt(9+16)"));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 1.0, ev("ln(e)"));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 3.0, ev("log(1000)"));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 7.0, ev("abs(-7)"));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 3.141592653589793, ev("pi"));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 2.718281828459045, ev("e"));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 6.283185307179586, ev("2*\xCF\x80"));   // UTF-8 pi
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 3.0, ev("6\xC3\x97" "2\xC3\xB7" "4"));  // UTF-8 multiply / divide
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 150000.0, ev("1.5e5"));
}

// A '%' directly after a number or a closing parenthesis is "percent" (x0.01),
// it is not relative to the left operand. Anywhere else it is modulo.
static void test_percent(void) {
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 0.5, ev("50%"));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 30.0, ev("200*15%"));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 200.15, ev("200+15%"));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 0.1, ev("(4+6)%"));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 1.0, ev("abs(7)%3"));       // modulo
    TEST_ASSERT_TRUE(fails("10%3"));                           // percent, then a stray operand
}

static void test_factorial(void) {
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 120.0, ev("5!"));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 1.0, ev("0!"));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 7.0, ev("3!+1"));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 12.0, ev("2*3!"));
    TEST_ASSERT_DOUBLE_WITHIN(1.0, 2432902008176640000.0, ev("20!"));
    TEST_ASSERT_DOUBLE_WITHIN(1.0, 2432902008176640000.0, ev("25!"));  // clamped to 20!
}

static void test_ans(void) {
    double out = 0.0;
    TEST_ASSERT_TRUE(calc::evaluate("Ans", true, 42.0, out));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 42.0, out);
    TEST_ASSERT_TRUE(calc::evaluate("ans*2+1", true, 42.0, out));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 85.0, out);
    TEST_ASSERT_TRUE(calc::evaluate("ANS-2", true, -1.5, out));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, -3.5, out);
}

static void test_empty_and_incomplete(void) {
    double out = 1.0;
    TEST_ASSERT_TRUE(calc::evaluate("", true, 0.0, out));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 0.0, out);
    TEST_ASSERT_TRUE(calc::evaluate(nullptr, true, 0.0, out));
    // a trailing operator while typing keeps the live preview alive
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 2.0, ev("2 + "));
    TEST_ASSERT_DOUBLE_WITHIN(EPS, 0.0, ev("2 * "));
}

static void test_syntax_errors(void) {
    TEST_ASSERT_TRUE(fails("2+*"));
    TEST_ASSERT_TRUE(fails("2+*3"));
    TEST_ASSERT_TRUE(fails("*2"));
    TEST_ASSERT_TRUE(fails("2+3)"));
    TEST_ASSERT_TRUE(fails("()"));
    TEST_ASSERT_TRUE(fails("2 3"));
    TEST_ASSERT_TRUE(fails("2+x"));
    TEST_ASSERT_TRUE(fails("1..2"));
}

static void test_non_finite(void) {
    TEST_ASSERT_TRUE(fails("1/0"));
    TEST_ASSERT_TRUE(fails("0/0"));
    TEST_ASSERT_TRUE(fails("5/(2-2)"));
    TEST_ASSERT_TRUE(fails("atan(1/0)"));      // must not hide behind a finite result
    TEST_ASSERT_TRUE(fails("abs(7)%0"));
    TEST_ASSERT_TRUE(fails("sqrt(-1)"));
    TEST_ASSERT_TRUE(fails("ln(0)"));
    TEST_ASSERT_TRUE(fails("10^400"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_precedence);
    RUN_TEST(test_parentheses);
    RUN_TEST(test_power);
    RUN_TEST(test_unary_minus);
    RUN_TEST(test_trig_degrees_and_radians);
    RUN_TEST(test_functions_and_constants);
    RUN_TEST(test_percent);
    RUN_TEST(test_factorial);
    RUN_TEST(test_ans);
    RUN_TEST(test_empty_and_incomplete);
    RUN_TEST(test_syntax_errors);
    RUN_TEST(test_non_finite);
    return UNITY_END();
}
