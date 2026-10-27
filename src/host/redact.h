// The user's profile folder (C:\Users\<name>, and its 8.3 form) out of a log line: players attach logs\onimusha.log
// to public issues, and every path in it ran through their Windows user name. Header-only so tests/selftest.cpp
// checks it. Needs <string.h> before it.
#pragma once

// in (n bytes) into out (cap bytes, NUL-terminated) with each of forms[0..nforms) replaced by %USERPROFILE%, ignoring
// case, and only where the next character can't continue that folder's name (a letter, digit, _ - $ ~ or a non-ASCII
// byte would: so C:\Users\Al is not C:\Users\Alex, and is followed by \ , : ; . ) ] or anything else). Returns out's length.
static size_t redact_profile(const char *in, size_t n, char *out, size_t cap, const char *const *forms, int nforms)
{
    static const char tag[] = "%USERPROFILE%";
    size_t o = 0;
    if (!cap) return 0;
    for (size_t i = 0; i < n;) {
        size_t hit = 0;
        for (int f = 0; f < nforms && !hit; f++) {
            size_t len = forms[f] ? strlen(forms[f]) : 0;
            if (len && i + len <= n && !_strnicmp(in + i, forms[f], len)) {
                unsigned char next = i + len < n ? (unsigned char)in[i + len] : 0;
                int more = (next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') || (next >= '0' && next <= '9')
                         || next == '_' || next == '-' || next == '$' || next == '~' || next >= 0x80;
                if (!more) hit = len;
            }
        }
        if (hit) {
            for (const char *t = tag; *t && o + 1 < cap; t++) out[o++] = *t;
            i += hit;
        } else {
            if (o + 1 < cap) out[o++] = in[i];
            i++;
        }
    }
    out[o] = 0;
    return o;
}
