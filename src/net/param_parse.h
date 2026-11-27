// param_parse.h -- strict whole-number parsing for API parameters. Pure logic, no Arduino types, unit tested natively.
//
// Why not just toInt()? It reads "3abc" as 3 and "abc" or "" as 0, so a typo would silently select slot 0 or set
// brightness 0. The rule here: the text must be exactly what printing the number would give -- an optional '-', then
// digits with no leading zeros, nothing else. That also rejects " 5", "+5", "05", "-0" and "5.0", and anything too big
// for an int (instead of wrapping or saturating).

#pragma once
#include <stddef.h>

// True and *out set only if `s` is a canonical whole number within [lo, hi]. Otherwise false and *out is untouched.
static inline bool parse_whole_number(const char *s, int lo, int hi, int *out) {
    if (!s || !*s) return false;
    const char *p = s;
    const bool neg = (*p == '-');
    if (neg) p++;
    if (!*p) return false;                       // "-" alone
    if (*p == '0' && (p[1] || neg)) return false; // leading zero, or "-0"
    long long v = 0;
    size_t digits = 0;
    for (; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        if (++digits > 10) return false;         // cannot fit an int; stops v overflowing
        v = v * 10 + (*p - '0');
    }
    if (neg) v = -v;
    if (v < lo || v > hi) return false;
    *out = (int)v;
    return true;
}
