#pragma once
// Recursive-descent expression parser of the calculator page. Pure logic
// without Arduino / LVGL dependencies so it can be unit-tested on the host.
//
//   expression := term   { ('+' | '-') term }
//   term       := factor { ('*' | '/' | '%') factor }     '%' here = modulo
//   factor     := primary { '^' primary }                 left-associative
//   primary    := ('+' | '-') primary | '(' expression ')' ['%'] | constant
//               | function argument | number ['!'] ['%']  '%' here = percent (x0.01)
//
// A missing operand at the very end ("2 + ") counts as 0 and an unclosed
// parenthesis is closed implicitly, so the live preview works while typing.
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace calc {
namespace detail {

static const double kPi = 3.14159265358979323846;
static const double kE  = 2.71828182845904523536;

struct Ctx {
    const char* p;
    bool deg;
    double ans;
    bool err;
};

inline void skipWs(Ctx& c) {
    while (*c.p == ' ' || *c.p == '\t') c.p++;
}

inline bool eat(Ctx& c, const char* word) {
    size_t n = strlen(word);
    if (strncmp(c.p, word, n) != 0) return false;
    c.p += n;
    return true;
}

inline double toRad(const Ctx& c, double a)   { return c.deg ? (a * kPi / 180.0) : a; }
inline double fromRad(const Ctx& c, double r) { return c.deg ? (r * 180.0 / kPi) : r; }

inline double parseExpression(Ctx& c);
inline double parsePrimary(Ctx& c);

inline double parseFuncArg(Ctx& c) {
    skipWs(c);
    if (*c.p == '(') {
        c.p++;
        double v = parseExpression(c);
        skipWs(c);
        if (*c.p == ')') c.p++;
        return v;
    }
    return parsePrimary(c);
}

inline double parsePrimary(Ctx& c) {
    skipWs(c);
    if (*c.p == 0) return 0.0;

    // Unary plus / minus
    if (*c.p == '+') { c.p++; return parsePrimary(c); }
    if (*c.p == '-') { c.p++; return -parsePrimary(c); }

    // Parentheses
    if (*c.p == '(') {
        c.p++;
        double val = parseExpression(c);
        skipWs(c);
        if (*c.p == ')') c.p++;
        skipWs(c);
        if (*c.p == '%') { c.p++; val *= 0.01; }
        return val;
    }

    // Constants
    if (eat(c, "pi") || eat(c, "PI")) return kPi;
    if ((*c.p == 'e' || *c.p == 'E') && (c.p[1] < '0' || c.p[1] > '9')) { c.p++; return kE; }
    if (eat(c, "ans") || eat(c, "ANS") || eat(c, "Ans")) return c.ans;

    // Functions
    if (eat(c, "sin"))  return sin(toRad(c, parseFuncArg(c)));
    if (eat(c, "cos"))  return cos(toRad(c, parseFuncArg(c)));
    if (eat(c, "tan"))  return tan(toRad(c, parseFuncArg(c)));
    if (eat(c, "asin")) return fromRad(c, asin(parseFuncArg(c)));
    if (eat(c, "acos")) return fromRad(c, acos(parseFuncArg(c)));
    if (eat(c, "atan")) return fromRad(c, atan(parseFuncArg(c)));
    if (eat(c, "sqrt")) return sqrt(parseFuncArg(c));
    if (eat(c, "ln"))   return log(parseFuncArg(c));
    if (eat(c, "log"))  return log10(parseFuncArg(c));
    if (eat(c, "abs"))  return fabs(parseFuncArg(c));

    // Number literal
    char* endp = nullptr;
    double val = strtod(c.p, &endp);
    if (endp && endp != c.p) {
        c.p = endp;
        skipWs(c);
        if (*c.p == '!') {
            c.p++;
            int n = (int)val;
            if (n < 0) n = 0;
            if (n > 20) n = 20;
            double f = 1.0;
            for (int i = 2; i <= n; i++) f *= i;
            val = f;
        }
        skipWs(c);
        if (*c.p == '%') {
            c.p++;
            val *= 0.01;
        }
        return val;
    }

    c.err = true; // operand expected
    return 0.0;
}

inline double parseFactor(Ctx& c) {
    double left = parsePrimary(c);
    skipWs(c);
    while (*c.p == '^') {
        c.p++;
        double right = parsePrimary(c);
        left = pow(left, right);
        skipWs(c);
    }
    return left;
}

inline double parseTerm(Ctx& c) {
    double left = parseFactor(c);
    skipWs(c);
    while (*c.p == '*' || *c.p == '/' || *c.p == '%') {
        char op = *c.p++;
        double right = parseFactor(c);
        if (op == '*') left *= right;
        else if (right == 0.0) c.err = true; // division / modulo by zero
        else if (op == '/') left /= right;
        else left = fmod(left, right);
        skipWs(c);
    }
    return left;
}

inline double parseExpression(Ctx& c) {
    double left = parseTerm(c);
    skipWs(c);
    while (*c.p == '+' || *c.p == '-') {
        char op = *c.p++;
        double right = parseTerm(c);
        if (op == '+') left += right;
        else left -= right;
        skipWs(c);
    }
    return left;
}

} // namespace detail

// Evaluates expr (at most 125 bytes are read). degMode selects degrees for the
// trigonometric functions, lastAns is the value of "ans". Returns false on a
// syntax error or a non-finite result; an empty expression is 0.
inline bool evaluate(const char* expr, bool degMode, double lastAns, double& out) {
    out = 0.0;
    if (!expr || expr[0] == 0) return true;

    // Clean string: replace UTF-8 multiplication / division signs and pi
    char clean[128];
    size_t ci = 0;
    for (size_t i = 0; expr[i] && ci + 2 < sizeof(clean); i++) {
        unsigned char ch = (unsigned char)expr[i];
        unsigned char nx = (unsigned char)expr[i + 1];
        if (ch == 0xC3 && nx == 0x97) { clean[ci++] = '*'; i++; }                         // U+00D7
        else if (ch == 0xC3 && nx == 0xB7) { clean[ci++] = '/'; i++; }                    // U+00F7
        else if (ch == 0xCF && nx == 0x80) { clean[ci++] = 'p'; clean[ci++] = 'i'; i++; } // U+03C0
        else clean[ci++] = expr[i];
    }
    clean[ci] = 0;

    detail::Ctx c = { clean, degMode, lastAns, false };
    double res = detail::parseExpression(c);
    detail::skipWs(c);
    if (c.err || *c.p != 0 || isnan(res) || isinf(res)) return false;
    out = res;
    return true;
}

} // namespace calc
