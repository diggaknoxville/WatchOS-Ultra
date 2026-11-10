#pragma once
#include <stdio.h>

// Compare "a.b.c" version strings (a leading 'v' is ignored, missing parts are 0).
// Returns <0, 0, >0 like strcmp.
inline int semverCompare(const char* a, const char* b) {
    int x[3] = {0, 0, 0}, y[3] = {0, 0, 0};
    if (a && (*a == 'v' || *a == 'V')) a++;
    if (b && (*b == 'v' || *b == 'V')) b++;
    if (a) sscanf(a, "%d.%d.%d", &x[0], &x[1], &x[2]);
    if (b) sscanf(b, "%d.%d.%d", &y[0], &y[1], &y[2]);
    for (int i = 0; i < 3; i++) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}
