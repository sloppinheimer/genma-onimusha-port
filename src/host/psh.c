// Xbox (NV2A) register-combiner pixel shader (X_D3DPIXELSHADERDEF) -> HLSL ps_2_0 source.
//
// Semantics as xemu's hw/xbox/nv2a/pgraph/glsl/psh.c documents them (itself based on Cxbx/Dxbx); the code is this
// port's own (THIRD-PARTY.txt).
// Inputs: COLOR0/1 (v0, v1), TEXCOORD0..3. Constants: c[0..7] per-stage C0, c[8..15] per-stage C1,
// c[16]/c[17] final combiner C0/C1, c[18] fog colour.
#include "host.h"
#include "shader_out.h"
#include <stdio.h>
#include <string.h>

// X_D3DPIXELSHADERDEF dword indices
enum { ALPHA_IN = 0, FINAL_ABCD = 8, FINAL_EFG = 9, ALPHA_OUT = 26, RGB_IN = 34, COMPARE = 42, RGB_OUT = 45,
       COUNT = 53, TEXMODES = 54 };

// Register code -> HLSL variable. C0/C1 depend on the stage when the shader asks for unique constants.
static const char *reg(int r, int c0, int c1, char *buf)
{
    static const char *const names[16] = {"zero", 0, 0, "fog", "v0", "v1", "zero", "zero", "t0", "t1", "t2", "t3", "r0", "r1", "v1r0", "efprod"};
    if (r == 1) { sprintf(buf, "c[%d]", c0); return buf; }
    if (r == 2) { sprintf(buf, "c[%d]", c1); return buf; }
    return names[r & 15];
}

// One combiner input byte as an HLSL expression. alpha: alpha portion (channel bit picks .a or .b).
static void input(char *dst, uint32_t in, int alpha, int c0, int c1)
{
    char buf[16], v[48];
    const char *sw = alpha ? ((in & 0x10) ? ".aaaa" : ".bbbb") : ((in & 0x10) ? ".aaaa" : "");
    sprintf(v, "%s%s", reg(in & 15, c0, c1, buf), sw);
    switch (in & 0xE0) {
    case 0x00: sprintf(dst, "max(%s, 0)", v); break;               // unsigned identity
    case 0x20: sprintf(dst, "(1 - saturate(%s))", v); break;       // unsigned invert
    case 0x40: sprintf(dst, "(2 * max(%s, 0) - 1)", v); break;     // expand normal
    case 0x60: sprintf(dst, "(1 - 2 * max(%s, 0))", v); break;     // expand negate
    case 0x80: sprintf(dst, "(max(%s, 0) - 0.5)", v); break;       // half-bias normal
    case 0xA0: sprintf(dst, "(0.5 - max(%s, 0))", v); break;       // half-bias negate
    case 0xC0: sprintf(dst, "%s", v); break;                       // signed identity
    default:   sprintf(dst, "(-%s)", v); break;                    // signed negate
    }
}

static const char *out_map(uint32_t flags)
{
    switch (flags & 0x38) {
    case 0x08: return "(%s - 0.5)";
    case 0x10: return "(%s * 2)";
    case 0x18: return "((%s - 0.5) * 2)";
    case 0x20: return "(%s * 4)";
    case 0x30: return "(%s / 2)";
    }
    return "%s";
}

// One half (rgb or alpha) of a combiner stage: computes into ab_/cd_/sum_ temporaries named with `tag`.
static void stage_half(struct out *o, int st, uint32_t in, uint32_t out, int alpha, int c0, int c1, uint32_t mux_msb)
{
    char a[80], b[80], c[80], d[80], e[96];
    const char *tag = alpha ? "a" : "c";
    input(a, in >> 24 & 0xFF, alpha, c0, c1); input(b, in >> 16 & 0xFF, alpha, c0, c1);
    input(c, in >> 8 & 0xFF, alpha, c0, c1);  input(d, in & 0xFF, alpha, c0, c1);
    uint32_t flags = out >> 12;
    const char *map = out_map(flags);
    emit(o, "  float4 ab%s%d = %s(%s, %s);\n", tag, st, (!alpha && (flags & 2)) ? "dot3" : "mul4", a, b);
    emit(o, "  float4 cd%s%d = %s(%s, %s);\n", tag, st, (!alpha && (flags & 1)) ? "dot3" : "mul4", c, d);
    if (flags & 4)
        emit(o, "  float4 sum%s%d = %s ? cd%s%d : ab%s%d;\n", tag, st, mux_msb ? "(r0.a >= 0.5)" : "(fmod(floor(r0.a * 255 + 0.5), 2) >= 1)", tag, st, tag, st);
    else
        emit(o, "  float4 sum%s%d = ab%s%d + cd%s%d;\n", tag, st, tag, st, tag, st);
    const char *names[3] = {"ab", "cd", "sum"};
    for (int i = 0; i < 3; i++) {
        sprintf(e, "%s%s%d", names[i], tag, st);
        emit(o, "  %s%s%d = clamp(", names[i], tag, st);
        emit(o, map, e);
        emit(o, ", -1, 1);\n");
    }
}

static void stage_write(struct out *o, int st, uint32_t out, int alpha)
{
    static const int shifts[3] = {4, 0, 8};   // ab, cd, sum destination fields
    static const char *const names[3] = {"ab", "cd", "sum"};
    const char *tag = alpha ? "a" : "c", *mask = alpha ? ".a" : ".rgb";
    char buf[16];
    for (int i = 0; i < 3; i++) {
        int dst = out >> shifts[i] & 15;
        if (dst) emit(o, "  %s%s = %s%s%d%s;\n", reg(dst, 0, 0, buf), mask, names[i], tag, st, mask);
    }
    if (!alpha && (out >> 12 & 0x80) && (out >> 4 & 15)) emit(o, "  %s.a = abc%d.b;\n", reg(out >> 4 & 15, 0, 0, buf), st);
    if (!alpha && (out >> 12 & 0x40) && (out & 15)) emit(o, "  %s.a = cdc%d.b;\n", reg(out & 15, 0, 0, buf), st);
}

// Final combiner input: only unsigned identity / invert are valid. alpha: G input (.a or .b).
static void final_input(char *dst, uint32_t in, int alpha)
{
    char buf[16], v[48];
    const char *sw = alpha ? ((in & 0x10) ? ".a" : ".b") : ((in & 0x10) ? ".aaa" : ".rgb");
    sprintf(v, "%s%s", reg(in & 15, 16, 17, buf), sw);
    if (in & 0x20) sprintf(dst, "(1 - saturate(%s))", v); else sprintf(dst, "saturate(%s)", v);
}

char *psh_to_hlsl(const uint32_t *def)
{
    struct out o = {0};
    uint32_t count = def[COUNT] & 0xFF, mux_msb = def[COUNT] & 0x100, unique_c0 = def[COUNT] & 0x1000, unique_c1 = def[COUNT] & 0x10000;
    if (count > 8) count = 8;
    emit(&o, "float4 c[19] : register(c0);\n");
    emit(&o, "sampler2D s0 : register(s0); sampler2D s1 : register(s1); sampler2D s2 : register(s2); sampler2D s3 : register(s3);\n");
    emit(&o, "float4 mul4(float4 a, float4 b) { return a * b; }\n"
             "float4 dot3(float4 a, float4 b) { return dot(a.rgb, b.rgb); }\n");
    emit(&o, "struct PSIN { float4 d0 : COLOR0; float4 d1 : COLOR1; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1;"
             " float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; };\n");
    emit(&o, "float4 main(PSIN i) : COLOR {\n");
    emit(&o, "  float4 zero = 0, v0 = i.d0, v1 = i.d1, fog = float4(c[18].rgb, 1), t0 = 0, t1 = 0, t2 = 0, t3 = 0;\n");
    for (int st = 0; st < 4; st++) {
        uint32_t mode = def[TEXMODES] >> (st * 5) & 31;
        switch (mode) {
        case 0: break;
        case 1: case 2: emit(&o, "  t%d = tex2D(s%d, i.t%d.xy);\n", st, st, st); break;   // ponytail: PROJECT* sampled unprojected; 3D textures unsupported
        case 3: emit(&o, "  t%d = 0;\n", st); xlog("[psh] TODO cube map texture mode"); break;
        case 4: emit(&o, "  t%d = saturate(i.t%d);\n", st, st); break;                       // PASSTHRU
        case 5:                                                                                 // CLIPPLANE
            for (int j = 0; j < 4; j++)
                emit(&o, "  clip(i.t%d.%c %s 0 ? -1 : 1);\n", st, "xyzw"[j], (def[COMPARE] >> (st * 4 + j) & 1) ? ">=" : "<");
            break;
        default: xlog("[psh] TODO texture mode %u on stage %d", mode, st); break;
        }
    }
    emit(&o, "  float4 r0 = 0, r1 = 0;\n  r0.a = t0.a;\n");
    for (uint32_t st = 0; st < count; st++) {
        int c0 = unique_c0 ? st : 0, c1 = 8 + (unique_c1 ? st : 0);
        stage_half(&o, st, def[RGB_IN + st], def[RGB_OUT + st], 0, c0, c1, mux_msb);
        stage_half(&o, st, def[ALPHA_IN + st], def[ALPHA_OUT + st], 1, c0, c1, mux_msb);
        stage_write(&o, st, def[RGB_OUT + st], 0);
        stage_write(&o, st, def[ALPHA_OUT + st], 1);
    }
    uint32_t abcd = def[FINAL_ABCD], efg = def[FINAL_EFG];
    if (abcd || efg) {
        char a[64], b[64], c[64], d[64], e[64], f[64], g[64];
        emit(&o, "  float4 v1r0 = 0, efprod = 0;\n");
        final_input(e, efg >> 24 & 0xFF, 0); final_input(f, efg >> 16 & 0xFF, 0);
        emit(&o, "  efprod.rgb = %s * %s;\n", e, f);
        emit(&o, "  v1r0.rgb = %s + %s;\n", (efg & 0x40) ? "(1 - v1.rgb)" : "v1.rgb", (efg & 0x20) ? "(1 - r0.rgb)" : "r0.rgb");
        if (efg & 0x80) emit(&o, "  v1r0 = saturate(v1r0);\n");
        final_input(a, abcd >> 24 & 0xFF, 0); final_input(b, abcd >> 16 & 0xFF, 0);
        final_input(c, abcd >> 8 & 0xFF, 0);  final_input(d, abcd & 0xFF, 0);
        final_input(g, efg >> 8 & 0xFF, 1);
        emit(&o, "  return saturate(float4(%s * %s + (1 - %s) * %s + %s, %s));\n}\n", a, b, a, c, d, g);
    } else {
        emit(&o, "  return saturate(r0);\n}\n");
    }
    return o.s;
}
