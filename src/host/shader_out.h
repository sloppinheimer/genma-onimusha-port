// A growing string for the shader translators (vsh.c, psh.c).
#pragma once
#include "host.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

struct out { char *s; size_t n, cap; };
static void emit(struct out *o, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int need = _vscprintf(fmt, ap);
    va_end(ap);
    if (o->n + need + 1 > o->cap) {
        o->cap = (o->n + need + 1) * 2;
        if (!(o->s = realloc(o->s, o->cap))) xfatal("out of memory translating a shader");
    }
    va_start(ap, fmt);
    vsprintf(o->s + o->n, fmt, ap);
    va_end(ap);
    o->n += need;
}
