// Code patches in the game's image: a CALL or JMP pointed at the port's own code. The rel32 arithmetic once, for
// hle.c (the SDK hooks, the warp), cheats.c and randomizer.c. Needs <string.h> and <stdint.h> before it.
#pragma once

static void patch_rel32(uint8_t *at, uint8_t op, const void *to)   // op (0xE8 CALL, 0xE9 JMP) rel32 at at, to to
{
    at[0] = op;
    int32_t rel = (int32_t)((const uint8_t *)to - (at + 5));
    memcpy(at + 1, &rel, 4);
}
// Where the CALL rel32 at at goes, or 0 when at isn't one.
static const uint8_t *patch_call_target(const uint8_t *at)
{
    int32_t rel;
    memcpy(&rel, at + 1, 4);
    return at[0] == 0xE8 ? at + 5 + rel : 0;
}
// Points the CALL at at, if it still calls expect, to to: 1; 0 (nothing written) when the code isn't as expected,
// a game other than the one these addresses were read from.
static int patch_call(uint32_t at, uint32_t expect, const void *to)
{
    if (patch_call_target((const uint8_t *)at) != (const uint8_t *)expect) return 0;
    patch_rel32((uint8_t *)at, 0xE8, to);
    return 1;
}
