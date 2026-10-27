// Xbox (NV2A) vertex shader microcode -> HLSL source.
//
// Instruction format and semantics as xemu's hw/xbox/nv2a/pgraph/glsl/vsh-prog.c documents them (itself based on
// Cxbx/Dxbx); the field layout is the hardware's, the code is this port's own (THIRD-PARTY.txt). Each instruction is 4 dwords; a MAC and an ILU operation can be paired in one instruction.
// Inputs v0..v15 arrive as float4 TEXCOORD0..15; constants are c[0..191] (Xbox -96..95, plus 96).
// Xbox shaders leave the position in screen space; the epilogue maps it back to clip space using the same
// viewport scale/offset (c[58], c[59]) the shader was given.
#include "host.h"
#include "shader_out.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAC_NOP, MAC_MOV, MAC_MUL, MAC_ADD, MAC_MAD, MAC_DP3, MAC_DPH, MAC_DP4, MAC_DST, MAC_MIN, MAC_MAX, MAC_SLT, MAC_SGE, MAC_ARL };
enum { ILU_NOP, ILU_MOV, ILU_RCP, ILU_RCC, ILU_RSQ, ILU_EXP, ILU_LOG, ILU_LIT };
enum { PARAM_R = 1, PARAM_V = 2, PARAM_C = 3 };

static uint32_t field(const uint32_t *t, int word, int bit, int len) { return (t[word] >> bit) & ((1u << len) - 1); }
#define F_ILU(t)        field(t, 1, 25, 3)
#define F_MAC(t)        field(t, 1, 21, 4)
#define F_CONST(t)      field(t, 1, 13, 8)
#define F_V(t)          field(t, 1, 9, 4)
#define F_A_NEG(t)      field(t, 1, 8, 1)
#define F_A_R(t)        field(t, 2, 28, 4)
#define F_A_MUX(t)      field(t, 2, 26, 2)
#define F_B_NEG(t)      field(t, 2, 25, 1)
#define F_B_R(t)        field(t, 2, 13, 4)
#define F_B_MUX(t)      field(t, 2, 11, 2)
#define F_C_NEG(t)      field(t, 2, 10, 1)
#define F_C_R(t)        (field(t, 2, 0, 2) << 2 | field(t, 3, 30, 2))
#define F_C_MUX(t)      field(t, 3, 28, 2)
#define F_OUT_MAC_MASK(t) field(t, 3, 24, 4)
#define F_OUT_R(t)      field(t, 3, 20, 4)
#define F_OUT_ILU_MASK(t) field(t, 3, 16, 4)
#define F_OUT_O_MASK(t) field(t, 3, 12, 4)
#define F_OUT_ORB(t)    field(t, 3, 11, 1)    // 0 = constant register, 1 = output register
#define F_OUT_ADDR(t)   field(t, 3, 3, 8)
#define F_OUT_MUX(t)    field(t, 3, 2, 1)     // 0 = MAC result, 1 = ILU result
#define F_A0X(t)        field(t, 3, 1, 1)
#define F_FINAL(t)      field(t, 3, 0, 1)

static void swizzle(const uint32_t *t, int word, int bit, int scalar, char *dst)
{
    static const char xyzw[] = "xyzw";
    int s[4];
    for (int i = 0; i < 4; i++) s[i] = field(t, word, bit - 2 * i, 2);
    if (scalar) s[1] = s[2] = s[3] = s[0];
    dst[0] = '.';
    for (int i = 0; i < 4; i++) dst[1 + i] = xyzw[s[i]];
    dst[5] = 0;
}

// Operand A, B or C as an HLSL float4 expression.
static void operand(const uint32_t *t, int which, char *dst, int ilu_scalar)
{
    int mux = which == 0 ? F_A_MUX(t) : which == 1 ? F_B_MUX(t) : F_C_MUX(t);
    int neg = which == 0 ? F_A_NEG(t) : which == 1 ? F_B_NEG(t) : F_C_NEG(t);
    int reg = which == 0 ? F_A_R(t) : which == 1 ? F_B_R(t) : F_C_R(t);
    // swizzle fields sit right after the NEG bit: A at word1 bit 6.., B at word2 bit 23.., C at word2 bit 8..
    char sw[8];
    if (which == 0) swizzle(t, 1, 6, 0, sw);
    else if (which == 1) swizzle(t, 2, 23, 0, sw);
    else swizzle(t, 2, 8, ilu_scalar, sw);
    char base[32];
    if (mux == PARAM_V) sprintf(base, "v%u", F_V(t));
    else if (mux == PARAM_C) {
        uint32_t c = F_CONST(t);   // Xbox c[-96..95] is c[0..191]; there is no 192-255 (our c[192..196] sit there)
        if (F_A0X(t)) sprintf(base, "c[A0 + %u]", c);
        else if (c < 192) sprintf(base, "c[%u]", c);
        else { sprintf(base, "float4(0, 0, 0, 0)"); xfeature(1, "vertex shader: reads constant %u (none there)", c); }
    } else sprintf(base, "R%d", reg);
    sprintf(dst, "%s%s%s", neg ? "-" : "", base, sw);
}

static const char *mask_str(int m)
{
    static const char *const s[16] = {"", ".w", ".z", ".zw", ".y", ".yw", ".yz", ".yzw", ".x", ".xw", ".xz", ".xzw", ".xy", ".xyw", ".xyz", ""};
    return s[m];
}

static const char *const out_names[16] = {"oPos", 0, 0, "oD0", "oD1", "oFog", "oPts", "oB0", "oB1", "oT0", "oT1", "oT2", "oT3", 0, 0, 0};

// Returns malloc'd HLSL, or 0. *inputs gets a bit per v register read.
char *vsh_to_hlsl(const uint32_t *fn, uint32_t len, uint32_t *inputs)
{
    if (len < 1) return 0;
    uint32_t ninst = fn[0] >> 16;
    if (ninst > (len - 1) / 4) ninst = (len - 1) / 4;   // 4 dwords each after the header
    const uint32_t *ins = fn + 1;
    struct out body = {0}, o = {0};
    *inputs = 0;
    int final = 0;
    // Precise depth: the game's projection has its z row almost equal to its w row (z ~ w - 1), so z and w come out
    // of two dp4s whose 1e4-sized terms cancel, each a few 1e-3 off in float32: z/w is about +-10 steps of D24 noise,
    // and close layers of one model (collar and neck, armour and body) swap places from frame to frame. Where both
    // come from a dp4 of one source with a constant row, the epilogue sets clip z = w - dot(src, c[w] - c[z]): the
    // difference of the rows is exact, so one rounding is left. Debug: ONI_PRECISE_DEPTH=0 turns it off.
    static int precise = -1;
    if (precise < 0) { char v[8]; precise = !(env_str("ONI_PRECISE_DEPTH", v, sizeof v) && v[0] == '0'); }
    int cz = -1, cw = -1;   // constant row of the dp4 that last wrote position z / w, -1 = none or something else
    for (uint32_t i = 0; i < ninst && !final; i++, ins += 4) {
        const uint32_t *t = ins;
        int mac = F_MAC(t), ilu = F_ILU(t);
        final = F_FINAL(t);
        if (mac == MAC_NOP && ilu == ILU_NOP) continue;
        static const int ilu_scalar[8] = {0, 0, 1, 1, 1, 1, 1, 0};
        char a[48], b[48], c[48];
        operand(t, 0, a, 0); operand(t, 1, b, 0); operand(t, 2, c, ilu_scalar[ilu]);
        for (int k = 0; k < 3; k++) {
            int mux = k == 0 ? F_A_MUX(t) : k == 1 ? F_B_MUX(t) : F_C_MUX(t);
            if (mux == PARAM_V) *inputs |= 1u << F_V(t);
        }
        emit(&body, "  { // %u\n", i);
        if (mac != MAC_NOP) {
            switch (mac) {
            case MAC_MOV: emit(&body, "    float4 m = %s;\n", a); break;
            case MAC_MUL: emit(&body, "    float4 m = %s * %s;\n", a, b); break;
            case MAC_ADD: emit(&body, "    float4 m = %s + %s;\n", a, c); break;
            case MAC_MAD: emit(&body, "    float4 m = %s * %s + %s;\n", a, b, c); break;
            case MAC_DP3: emit(&body, "    float4 m = dot((%s).xyz, (%s).xyz);\n", a, b); break;
            case MAC_DPH: emit(&body, "    float4 m = dot(float4((%s).xyz, 1), %s);\n", a, b); break;
            case MAC_DP4: emit(&body, "    float4 m = dot(%s, %s);\n", a, b); break;
            case MAC_DST: emit(&body, "    float4 m = float4(1, (%s).y * (%s).y, (%s).z, (%s).w);\n", a, b, a, b); break;
            case MAC_MIN: emit(&body, "    float4 m = min(%s, %s);\n", a, b); break;
            case MAC_MAX: emit(&body, "    float4 m = max(%s, %s);\n", a, b); break;
            case MAC_SLT: emit(&body, "    float4 m = (%s < %s) ? 1 : 0;\n", a, b); break;
            case MAC_SGE: emit(&body, "    float4 m = (%s >= %s) ? 1 : 0;\n", a, b); break;
            case MAC_ARL: emit(&body, "    float4 m = %s;\n", a); break;
            }
        }
        if (ilu != ILU_NOP) {
            switch (ilu) {
            case ILU_MOV: emit(&body, "    float4 u = %s;\n", c); break;
            case ILU_RCP: emit(&body, "    float4 u = 1.0 / (%s).x;\n", c); break;
            case ILU_RCC: emit(&body, "    float u0 = 1.0 / (%s).x; u0 = abs(u0) < 5.42101e-20 ? sign(u0) * 5.42101e-20 : (abs(u0) > 1.84467e19 ? sign(u0) * 1.84467e19 : u0); float4 u = u0;\n", c); break;
            case ILU_RSQ: emit(&body, "    float4 u = rsqrt(abs((%s).x));\n", c); break;
            case ILU_EXP: emit(&body, "    float e0 = (%s).x; float4 u = float4(exp2(floor(e0)), e0 - floor(e0), exp2(e0), 1);\n", c); break;
            case ILU_LOG: emit(&body, "    float l0 = abs((%s).x); float4 u = l0 == 0 ? float4(-1e30, 1, -1e30, 1) : float4(floor(log2(l0)), l0 / exp2(floor(log2(l0))), log2(l0), 1);\n", c); break;
            case ILU_LIT: emit(&body, "    float4 ls = %s; ls.w = clamp(ls.w, -127.996, 127.996); ls.xy = max(ls.xy, 0); float4 u = float4(1, ls.x, ls.x > 0 ? exp2(ls.w * log2(max(ls.y, 1e-30))) : 0, 1);\n", c); break;
            }
        }
        // Register writes: MAC to R[out_r] (paired: R1 writes dropped), ILU to R1 when paired else R[out_r].
        int r = F_OUT_R(t);
        // Position z/w writes, for precise depth (above). The XDK's viewport mapping at the end (mul by c[58], mad
        // with c[59], xyz only) is left out: the epilogue undoes it.
        int viewport = strstr(a, "c[58]") || strstr(b, "c[58]") || strstr(c, "c[59]");
        if (precise && !viewport) {
            int pm = 0, ilu_pos = ilu != ILU_NOP && (mac != MAC_NOP ? 1 : r) == 12 && F_OUT_ILU_MASK(t);
            if (mac != MAC_NOP && mac != MAC_ARL && r == 12 && !(ilu != ILU_NOP && r == 1)) pm |= F_OUT_MAC_MASK(t);
            if (ilu_pos) pm |= F_OUT_ILU_MASK(t);
            if (F_OUT_O_MASK(t) && F_OUT_ORB(t) && (F_OUT_ADDR(t) & 15) == 0) pm |= F_OUT_O_MASK(t), ilu_pos |= F_OUT_MUX(t);
            int ca = F_A_MUX(t) == PARAM_C, cb = F_B_MUX(t) == PARAM_C;
            int row = mac == MAC_DP4 && !ilu_pos && !F_A0X(t) && ca != cb ? (int)F_CONST(t) : -1;
            if (pm & 2) { cz = row; if (row >= 0) emit(&body, "    dsz = %s;\n", ca ? b : a); }
            if (pm & 1) { cw = row; if (row >= 0) emit(&body, "    dsw = %s;\n", ca ? b : a); }
        }
        if (mac == MAC_ARL) emit(&body, "    A0 = (int)floor(m.x + 0.001);\n");
        else if (mac != MAC_NOP && F_OUT_MAC_MASK(t) && !(ilu != ILU_NOP && r == 1))
            emit(&body, "    R%d%s = m%s;\n", r, mask_str(F_OUT_MAC_MASK(t)), mask_str(F_OUT_MAC_MASK(t)));
        if (ilu != ILU_NOP && F_OUT_ILU_MASK(t)) {
            int ir = mac != MAC_NOP ? 1 : r;
            emit(&body, "    R%d%s = u%s;\n", ir, mask_str(F_OUT_ILU_MASK(t)), mask_str(F_OUT_ILU_MASK(t)));
        }
        if (F_OUT_O_MASK(t)) {
            const char *src = F_OUT_MUX(t) ? "u" : "m";
            if ((F_OUT_MUX(t) && ilu == ILU_NOP) || (!F_OUT_MUX(t) && mac == MAC_NOP)) src = 0;
            if (src && F_OUT_ORB(t) == 0) {
                static int warned;
                if (!warned++) xlog("[vsh] TODO shader writes a constant register");
                xfeature(1, "vertex shader: writes a constant register (ignored)");
            } else if (src) {
                const char *name = out_names[F_OUT_ADDR(t) & 15];
                if (!name) name = "oUnused";
                if (!strcmp(name, "oPos")) name = "R12";   // oPos and R12 are the same register
                emit(&body, "    %s%s = %s%s;\n", name, mask_str(F_OUT_O_MASK(t)), src, mask_str(F_OUT_O_MASK(t)));
            }
        }
        emit(&body, "  }\n");
    }

    // c[192..195] are ours: scale for texture coordinate set n when its texture is linear (texel-addressed on Xbox).
    emit(&o, "float4 c[197] : register(c0);\n");
    emit(&o, "struct VSIN {");
    for (int i = 0; i < 16; i++) if (*inputs & (1u << i)) emit(&o, " float4 v%d : TEXCOORD%d;", i, i);
    if (!*inputs) emit(&o, " float4 unused_ : TEXCOORD0;");
    emit(&o, " };\n");
    emit(&o, "struct VSOUT { float4 pos : POSITION; float4 d0 : COLOR0; float4 d1 : COLOR1; float4 t0 : TEXCOORD0;"
             " float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; float fog : FOG; float pts : PSIZE; };\n");
    emit(&o, "VSOUT main(VSIN vin) {\n  int A0 = 0;\n");
    for (int i = 0; i < 16; i++) {
        if (*inputs & (1u << i)) emit(&o, "  float4 v%d = vin.v%d;\n", i, i);
        else emit(&o, "  float4 v%d = float4(0, 0, 0, 1);\n", i);
    }
    for (int i = 0; i <= 12; i++) emit(&o, "  float4 R%d = 0;\n", i);
    emit(&o, "  float4 dsz = 0, dsw = 1;\n");   // dp4 sources of position z and w (precise depth)
    emit(&o, "  float4 oD0 = 1, oD1 = 0, oFog = 1, oPts = 1, oB0 = 0, oB1 = 0, oT0 = 0, oT1 = 0, oT2 = 0, oT3 = 0, oUnused = 0;\n");
    emit(&o, "%s", body.s ? body.s : "");
    char depth[96] = "";
    if (cz >= 0 && cw >= 0)
        snprintf(depth, sizeof depth, "  if (all(dsz == dsw)) r.pos.z = w - dot(dsw, c[%d] - c[%d]);\n", cw, cz);
    // Screen space -> clip space, inverting the viewport transform the shader applied with c[58]/c[59].
    emit(&o, "  VSOUT r;\n"
             "  float w = R12.w;\n"
             "  w = abs(w) < 5.42101e-20 ? 5.42101e-20 : (abs(w) > 1.84467e19 ? sign(w) * 1.84467e19 : w);\n"
             // Xbox pixel centres are at .5, D3D9's at whole numbers: shift half a render-target pixel (c[196], in Xbox pixels).
             "  float3 ndc = (R12.xyz - c[59].xyz - float3(c[196].xy, 0)) / c[58].xyz;\n"
             "  r.pos = float4(ndc * w, w);\n"
             "  r.d0 = saturate(oD0); r.d1 = saturate(oD1); r.t0 = oT0 * c[192]; r.t1 = oT1 * c[193]; r.t2 = oT2 * c[194]; r.t3 = oT3 * c[195];\n"
             "  r.fog = oFog.x; r.pts = oPts.x;\n"
             "%s  return r;\n}\n", depth);
    free(body.s);
    return o.s;
}
