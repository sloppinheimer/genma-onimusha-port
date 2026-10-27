// The game's shaders as D3D9 ones: programmable vertex shaders (their Xbox declaration, attribute formats and the
// program, translated by vsh.c) and register-combiner pixel shaders (psh.c), with the pixel shader's constants from
// the render-state array. Split out of d3d.c; the draws that use them are still there (draw_vs).
#define COBJMACROS
#include "host.h"
#include "xd3d.h"
#include "game.h"
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <d3dcompiler.h>
#include "d3d_internal.h"

// ---- programmable vertex shaders ---------------------------------------------------------------------

static uint32_t vsdt_size(uint32_t t)
{
    switch (t) {
    case 0x40: case 0x16: return 4;    // D3DCOLOR, NORMPACKED3
    case 0x72: return 12;              // FLOAT2H
    case 0x02: return 0;               // NONE
    }
    uint32_t n = t >> 4;
    switch (t & 0xF) { case 2: return 4 * n; case 1: case 5: return 2 * n; case 4: return n; }
    return 0;
}

// Xbox vertex attribute -> float4, with missing components defaulting to (0, 0, 0, 1).
void vsdt_fetch(uint32_t t, const uint8_t *p, float *o)
{
    o[0] = o[1] = o[2] = 0; o[3] = 1;
    uint32_t n = t >> 4;
    switch (t) {
    case 0x40: for (int i = 0; i < 4; i++) o[i] = p[i == 3 ? 3 : 2 - i] / 255.0f; return;   // BGRA bytes
    case 0x16: {
        uint32_t v = *(const uint32_t *)p;
        int x = (int)(v << 21) >> 21, y = (int)(v << 10) >> 21, z = (int)v >> 22;
        o[0] = x / 1023.0f; o[1] = y / 1023.0f; o[2] = z / 511.0f;
        return;
    }
    case 0x72: { const float *f = (const float *)p; o[0] = f[0]; o[1] = f[1]; o[3] = f[2]; return; }
    case 0x02: return;
    }
    for (uint32_t i = 0; i < n && i < 4; i++) {
        switch (t & 0xF) {
        case 2: o[i] = ((const float *)p)[i]; break;
        case 5: o[i] = ((const int16_t *)p)[i]; break;
        case 1: o[i] = ((const int16_t *)p)[i] / 32767.0f; break;
        case 4: o[i] = p[i] / 255.0f; break;
        }
    }
}

void parse_declaration(struct vshader *vs)
{
    uint32_t stream = 0, offset = 0;
    for (uint32_t i = 0; i < vs->decl_len; i++) {
        uint32_t t = vs->decl[i];
        switch (t >> 29) {
        case 1: stream = t & 0xF; offset = 0; break;                                  // STREAM
        case 2:                                                                       // STREAMDATA
            if (t & 0x10000000) { uint32_t c = (t >> 16) & 0xF; offset += (t & 0x08000000) ? c : c * 4; }
            else if (vs->nattrs < 16) {
                vs->attrs[vs->nattrs++] = (struct vattr){(uint8_t)stream, (uint8_t)(t & 0x1F), (uint8_t)((t >> 16) & 0xFF), (uint16_t)offset};
                offset += vsdt_size((t >> 16) & 0xFF);
            }
            break;
        case 4: {                                                                     // CONSTMEM
            static int warned;
            if (!warned++) xlog("[d3d] TODO constants in a vertex declaration");
            xfeature(1, "vertex shader: declaration loads constants (ignored)");
            i += ((t >> 25) & 0xF) * 4;
            break;
        }
        case 5: i += (t >> 24) & 0x1F; break;                                         // EXT
        }
    }
}

int compile_vshader(struct vshader *vs)
{
    if (vs->hs) return 1;
    if (vs->failed || !vs->function) return 0;
    char *src = vsh_to_hlsl(vs->function, vs->function_len, &vs->inputs);
    ID3DBlob *code = 0, *err = 0;
    HRESULT hr = src ? D3DCompile(src, strlen(src), "xvsh", 0, 0, "main", "vs_2_0", 0, 0, &code, &err) : E_FAIL;
    if (FAILED(hr) && src) {
        if (err) ID3D10Blob_Release(err);
        err = 0;
        hr = D3DCompile(src, strlen(src), "xvsh", 0, 0, "main", "vs_2_a", 0, 0, &code, &err);
    }
    static int dump = -1;   // debug: ONI_DUMP_VSH=1 writes every translated vertex shader to vsh_<handle>.hlsl (failed ones always)
    if (dump < 0) dump = debug_env_int("ONI_DUMP_VSH", 0) == 1;
    if (src && (dump || FAILED(hr))) {
        char file[64], name[MAX_PATH * 3 + 64];
        snprintf(file, sizeof file, "vsh_%p.hlsl", (void *)vs);
        dump_path(name, sizeof name, file);
        FILE *f = ufopen(name, L"w");
        if (f) { fputs(src, f); fclose(f); }
    }
    if (FAILED(hr)) {
        xfeature(1, "vertex shader: failed to compile (draws skipped)");
        xlog("[d3d] vertex shader %p failed to compile: %s", vs, err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "(no source)");
    } else {
        if (SUCCEEDED(hr = IDirect3DDevice9_CreateVertexShader(dev, (const DWORD *)ID3D10Blob_GetBufferPointer(code), &vs->hs))) {
            xfeature(0, "vertex shader: program version %04x", vs->function[0] & 0xFFFF);
            for (int j = 0; j < vs->nattrs; j++) xfeature(0, "vertex shader: input type %02x", vs->attrs[j].type);
            D3DVERTEXELEMENT9 el[18];
            int n = 0;
            for (int i = 0; i < 16; i++)
                if (vs->inputs & (1u << i))
                    el[n] = (D3DVERTEXELEMENT9){0, (WORD)(16 * n), D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, (BYTE)i}, n++;
            if (!n) el[n++] = (D3DVERTEXELEMENT9){0, 0, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 0};
            el[n] = (D3DVERTEXELEMENT9)D3DDECL_END();
            if (FAILED(hr = IDirect3DDevice9_CreateVertexDeclaration(dev, el, &vs->hdecl))) IDirect3DVertexShader9_Release(vs->hs);
        }
        if (FAILED(hr)) { vs->hs = 0; vs->hdecl = 0; xlog("[d3d] vertex shader %p: D3D9 refused it (%08lx)", vs, hr); }
        ID3D10Blob_Release(code);
    }
    vs->failed = !vs->hs;   // tried once only
    if (err) ID3D10Blob_Release(err);
    free(src);
    return vs->hs != 0;
}

// ---- register-combiner pixel shaders -----------------------------------------------------------------
static void argb_float4(uint32_t c, float *o) { o[0] = (c >> 16 & 0xFF) / 255.0f; o[1] = (c >> 8 & 0xFF) / 255.0f; o[2] = (c & 0xFF) / 255.0f; o[3] = (c >> 24) / 255.0f; }

// Bind the pixel shader (compiled on first use) and its constants from the render-state array, which is
// where SetPixelShader and the game's constant writes put them.
void apply_pixel_shader(void)
{
    struct pshader *ps = (struct pshader *)pixel_shader;
    if (ps && !ps->hs && !ps->failed) {
        char *src = psh_to_hlsl(ps->def);
        ID3DBlob *code = 0, *err = 0;
        if (FAILED(D3DCompile(src, strlen(src), "xpsh", 0, 0, "main", "ps_2_0", 0, 0, &code, &err))) {
            ps->failed = 1;
            xfeature(1, "pixel shader: failed to compile");
            xlog("[d3d] pixel shader %p failed to compile: %s\n%s", ps, err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "", src);
        } else {
            if (FAILED(IDirect3DDevice9_CreatePixelShader(dev, (const DWORD *)ID3D10Blob_GetBufferPointer(code), &ps->hs))) {
                ps->hs = 0;
                ps->failed = 1;
                xlog("[d3d] pixel shader %p: D3D9 refused it", ps);
            } else xlog("[d3d] pixel shader %p compiled:\n%s", ps, src);
            ID3D10Blob_Release(code);
        }
        if (err) ID3D10Blob_Release(err);
        free(src);
    }
    IDirect3DDevice9_SetPixelShader(dev, ps ? ps->hs : 0);
    if (!ps || !ps->hs) return;
    float c[19][4];
    for (int i = 0; i < 8; i++) { argb_float4(RS[XRS_PSCONSTANT0_0 + i], c[i]); argb_float4(RS[XRS_PSCONSTANT1_0 + i], c[8 + i]); }
    argb_float4(RS[XRS_PSFINALCOMBINERCONSTANT0], c[16]);
    argb_float4(RS[XRS_PSFINALCOMBINERCONSTANT1], c[17]);
    argb_float4(swap_rb(RS[XRS_FOGCOLOR]), c[18]);
    IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, &c[0][0], 19);
}
