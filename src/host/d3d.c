// Xbox Direct3D 8 (XDK 3948) on top of PC Direct3D 9.
//
// Model: Xbox resources keep their Xbox layout in Xbox-visible memory (headers + data the game reads and
// writes, often without telling us). Host D3D9 objects are a cache built from them at draw time.
// Render and texture-stage state live in the SDK's own arrays (game code writes them inline), and are read
// and translated just before each draw. The rest of the renderer is in the other d3d_*.c files (d3d_internal.h).
#define COBJMACROS
#include "host.h"
#include "xd3d.h"
#include "game.h"
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <d3dcompiler.h>
#include "d3d_internal.h"
#pragma comment(lib, "d3dcompiler.lib")

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "gdi32.lib")


static IDirect3D9 *d3d;
IDirect3DDevice9 *dev;
int dev_ex;   // dev is a D3D9Ex device (Windows): flip-model presents, no D3DPOOL_MANAGED (tex_create)
HWND hwnd;
int bb_w = 640, bb_h = 480;            // the game's backbuffer, in Xbox pixels

// The game renders into scene_rt (bb * scale, optionally multisampled). Each Present resolves it into
// frame_tex and draws that into the window's own swap chain as settings.picture says, so the window can be any size.
int scale = 1;                          // settings.scale, fixed for the session
static int rt_scale = 1;                       // scale of the current render target (1 for game textures)
IDirect3DSurface9 *scene_rt;
IDirect3DSurface9 *scene_ds;
static IDirect3DSurface9 *tex_ds;
IDirect3DTexture9 *frame_tex, *post_tex;
IDirect3DSwapChain9 *swapchain;
UINT swap_w, swap_h;
static D3DPRESENT_PARAMETERS dev_pp;           // for Reset after the device is lost
static D3DMULTISAMPLE_TYPE scene_ms;

// ---- window ------------------------------------------------------------------------------------------
#ifdef _WIN32
static RECT windowed_rect;
static void set_borderless(int on)
{
    settings.borderless = on;
    if (on) {
        GetWindowRect(hwnd, &windowed_rect);
        MONITORINFO mi = {sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(hwnd, GWL_STYLE, WS_POPUP | (IsWindowVisible(hwnd) ? WS_VISIBLE : 0));   // at start-up: shown after
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_NOTOPMOST, windowed_rect.left, windowed_rect.top, windowed_rect.right - windowed_rect.left,
                     windowed_rect.bottom - windowed_rect.top, SWP_FRAMECHANGED);
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CLOSE) { xlog("[d3d] window closed"); ExitProcess(0); }
    if ((m == WM_SYSKEYDOWN && w == VK_RETURN) || (m == WM_KEYDOWN && w == VK_F11)) { set_borderless(!settings.borderless); return 0; }
    if (m == WM_SYSCHAR && w == VK_RETURN) return 0;   // no beep for Alt+Enter
    if ((m == WM_SYSKEYDOWN || m == WM_SYSKEYUP) && w == VK_F10) return 0;   // F10 (compare shot) would open the window menu
    return DefWindowProcW(h, m, w, l);
}

static DWORD WINAPI window_thread(void *ready)
{
    WNDCLASSW wc = {0, wndproc, 0, 0, GetModuleHandleW(0), 0, LoadCursor(0, IDC_ARROW), (HBRUSH)GetStockObject(BLACK_BRUSH), 0, L"GenmaOnimusha"};
    RegisterClassW(&wc);
    RECT r = {0, 0, settings.window_w, settings.window_h};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    int bg = host_background();   // tours: off-screen, never takes focus, kept off the taskbar
    hwnd = CreateWindowExW(bg ? WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW : 0, L"GenmaOnimusha", L"Genma Onimusha", WS_OVERLAPPEDWINDOW,
                           bg ? -10000 : CW_USEDEFAULT, bg ? -10000 : CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, 0, 0, wc.hInstance, 0);
    {   // the icon (host.rc), large and small, for the title bar and taskbar
        HMODULE me = 0;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)window_thread, &me);
        HANDLE big = LoadImageW(me, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
        HANDLE little = LoadImageW(me, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
        if (big) SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)big);
        if (little) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)little);
    }
    // Full screen before it is first shown: shown first, the window-sized frame flashed at the top left on start-up.
    if (settings.borderless && !bg) set_borderless(1);
    ShowWindow(hwnd, bg ? SW_SHOWNOACTIVATE : SW_SHOW);
    SetEvent(ready);
    MSG msg;
    while (GetMessageW(&msg, 0, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
#else   // Linux: an SDL3 window (src/linux/compat_win.c), which DXVK Native draws into; Alt+Enter and F11 as on Windows
// DXVK draws with Vulkan, and the window is a Vulkan one: no driver (or no 32-bit one) stops the game at either.
#define NO_VULKAN "Genma Onimusha on Linux draws with Vulkan: install your graphics card's Vulkan driver, its 32-bit " \
                  "part too (the game is 32-bit)."
static void set_borderless(int on) { settings.borderless = on; compat_window_borderless(hwnd, on); }
static void toggle_borderless(void) { set_borderless(!settings.borderless); }
static DWORD WINAPI window_thread(void *ready)
{
    int bg = host_background();   // tours: hidden
    hwnd = compat_window_create(L"Genma Onimusha", settings.window_w, settings.window_h, settings.borderless && !bg, bg);
    if (!hwnd) xfatal("The game window couldn't be opened (%s). %s", compat_window_error(), NO_VULKAN);
    SetEvent(ready);
    compat_window_loop(hwnd, toggle_borderless);
    return 0;
}
#endif

// ---- device state shadowed on our side ---------------------------------------------------------------
XSurf backbuffer_hdr;
static XSurf depth_hdr;
XSurf *cur_rt;
XSurf *cur_z;
static const XPix *stage_tex[4];
const XPix *bound_tex[4];       // stage_tex[st] if its header is readable, else 0 (set per draw by ready_to_draw)
XRes *stage_pal[4];
static XPix switch_tex[4];             // D3DDevice_SwitchTexture targets
// Stage st's palette when it and its 256 colours can be read, else 0.
const uint32_t *stage_palette(int st)
{
    const XRes *r = stage_pal[st];
    if (!r || !readable_bytes(r, 12) || readable_bytes((const void *)r->Data, 1024) < 1024) return 0;
    return (const uint32_t *)r->Data;
}
// x's texture cache key. The game draws one paletted texture with several palettes in a frame (HUD, menus), so a
// paletted texture gets one entry per palette: a key ending in binary 10, which can't collide with a header (00) or
// SwitchTexture (x1, odd, so it can't collide with a 4-aligned header address) key. ponytail: hashed key, a
// collision would show the wrong palette.
void *cache_key(const XPix *x, const uint32_t *pal)
{
    if (pal) return (void *)(((x->Data * 2654435761u) ^ (uint32_t)pal) & ~3u | 2);
    return x >= switch_tex && x < switch_tex + 4 ? (void *)(x->Data | 1) : (void *)x;
}
static struct { XRes *vb; uint32_t stride; } streams[16];
static XRes *index_buffer;
static uint32_t base_vertex_index;
uint32_t vertex_shader;         // FVF, or (struct vshader *) | 1
uint32_t pixel_shader;
static uint32_t shader_constant_mode;
static float vs_constants[192][4];

static D3DVIEWPORT9 viewport;
int fast_mode;
static void set_viewport_constants(void);
static IDirect3DTexture9 *gpu_texture_for(const XPix *tex);
int passes_ran;                 // set by post_pass: device state was changed behind the caches
static int max_aniso = 1;              // settings.anisotropy, limited to what the GPU supports

// ---- state translation -------------------------------------------------------------------------------
static DWORD cmp_func(uint32_t v) { return v >= 0x200 && v <= 0x207 ? v - 0x200 + 1 : D3DCMP_ALWAYS; }
static DWORD blend(uint32_t v)
{
    switch (v) {
    case 0: return D3DBLEND_ZERO; case 1: return D3DBLEND_ONE;
    case 0x300: return D3DBLEND_SRCCOLOR; case 0x301: return D3DBLEND_INVSRCCOLOR;
    case 0x302: return D3DBLEND_SRCALPHA; case 0x303: return D3DBLEND_INVSRCALPHA;
    case 0x304: return D3DBLEND_DESTALPHA; case 0x305: return D3DBLEND_INVDESTALPHA;
    case 0x306: return D3DBLEND_DESTCOLOR; case 0x307: return D3DBLEND_INVDESTCOLOR;
    case 0x308: return D3DBLEND_SRCALPHASAT;
    case 0x8001: return D3DBLEND_BLENDFACTOR; case 0x8002: return D3DBLEND_INVBLENDFACTOR;
    }
    return D3DBLEND_ONE;
}
static DWORD blend_op(uint32_t v)
{
    switch (v) {
    case 0x800a: return D3DBLENDOP_SUBTRACT; case 0x800b: case 0xF005: return D3DBLENDOP_REVSUBTRACT;
    case 0x8007: return D3DBLENDOP_MIN; case 0x8008: return D3DBLENDOP_MAX;
    }
    return D3DBLENDOP_ADD;
}
static DWORD stencil_op(uint32_t v)
{
    switch (v) {
    case 0: return D3DSTENCILOP_ZERO; case 0x1e01: return D3DSTENCILOP_REPLACE;
    case 0x1e02: return D3DSTENCILOP_INCRSAT; case 0x1e03: return D3DSTENCILOP_DECRSAT;
    case 0x150a: return D3DSTENCILOP_INVERT; case 0x8507: return D3DSTENCILOP_INCR; case 0x8508: return D3DSTENCILOP_DECR;
    }
    return D3DSTENCILOP_KEEP;
}
static DWORD wrap_flags(uint32_t v) { return (v & 0x10 ? 1 : 0) | (v & 0x1000 ? 2 : 0) | (v & 0x100000 ? 4 : 0); }
static DWORD color_write(uint32_t v)
{
    return (v & 0x00010000 ? D3DCOLORWRITEENABLE_RED : 0) | (v & 0x00000100 ? D3DCOLORWRITEENABLE_GREEN : 0)
         | (v & 0x00000001 ? D3DCOLORWRITEENABLE_BLUE : 0) | (v & 0x01000000 ? D3DCOLORWRITEENABLE_ALPHA : 0);
}
// XDK 3948 D3DTOP numbering: 1-21 match PC; 22-26 are DOT3, MULTIPLYADD, LERP, BUMPENVMAP, BUMPENVMAPLUMINANCE.
static DWORD tex_op(uint32_t v)
{
    static const DWORD hi[] = {D3DTOP_DOTPRODUCT3, D3DTOP_MULTIPLYADD, D3DTOP_LERP, D3DTOP_BUMPENVMAP, D3DTOP_BUMPENVMAPLUMINANCE};
    if (v >= 1 && v <= 21) return v;
    if (v >= 22 && v <= 26) return hi[v - 22];
    return D3DTOP_DISABLE;
}
static DWORD tex_address(uint32_t v) { return v >= 1 && v <= 4 ? v : D3DTADDRESS_CLAMP; }
static DWORD tex_filter(uint32_t v) { return v <= 3 ? v : D3DTEXF_LINEAR; }

static DWORD applied_rs[256], applied_ss[4][16], applied_tss[4][40];
static uint32_t xbr_w, xbr_h;   // stage 0 is drawing a magnified copy (hud_filter's super-xBR, or a pack's HD
                                // version) of a w x h screen texture (0 = not): its gauges are sampled at one texel
static int text_bound;          // enhanced_text: stage 0 is drawing the redrawn font sheet
static int draw_quads;          // the draw being fixed up is a quad list (Xbox primitive 8)
static float inset_uv[4][2];   // per stage: half a texel in u and v, or 0 (set where sampler state is applied)
int rs_valid;

static void set_rs(D3DRENDERSTATETYPE s, DWORD v)
{
    if (rs_valid && applied_rs[s] == v) return;
    applied_rs[s] = v;
    IDirect3DDevice9_SetRenderState(dev, s, v);
}
static void set_ss(int st, D3DSAMPLERSTATETYPE s, DWORD v)
{
    if (rs_valid && applied_ss[st][s] == v) return;
    applied_ss[st][s] = v;
    IDirect3DDevice9_SetSamplerState(dev, st, s, v);
}
static void set_tss(int st, D3DTEXTURESTAGESTATETYPE s, DWORD v)
{
    if (rs_valid && applied_tss[st][s] == v) return;
    applied_tss[st][s] = v;
    IDirect3DDevice9_SetTextureStageState(dev, st, s, v);
}

static void apply_render_state(void)
{
    set_rs(D3DRS_ZENABLE, RS[XRS_ZENABLE] ? D3DZB_TRUE : D3DZB_FALSE);
    set_rs(D3DRS_ZWRITEENABLE, RS[XRS_ZWRITEENABLE] != 0);
    set_rs(D3DRS_ZFUNC, cmp_func(RS[XRS_ZFUNC]));
    // Xbox ALPHAKILL discards pixels whose texture alpha is 0. Stand-in when the game hasn't enabled an alpha test
    // itself: test the final alpha > 0. ponytail: final alpha, not the stage's texture alpha; exact needs a shader.
    int kill = 0;
    for (int st = 0; st < 4; st++) if (TS(st, XTSS_COLOROP) > 1 && TS(st, XTSS_ALPHAKILL)) kill = 1;
    int own_test = RS[XRS_ALPHATESTENABLE] != 0;
    set_rs(D3DRS_ALPHATESTENABLE, own_test || kill);
    set_rs(D3DRS_ALPHAFUNC, own_test ? cmp_func(RS[XRS_ALPHAFUNC]) : D3DCMP_GREATER);
    set_rs(D3DRS_ALPHAREF, own_test ? RS[XRS_ALPHAREF] & 0xFF : 0);
    set_rs(D3DRS_ALPHABLENDENABLE, RS[XRS_ALPHABLENDENABLE] != 0);
    set_rs(D3DRS_SRCBLEND, blend(RS[XRS_SRCBLEND]));
    set_rs(D3DRS_DESTBLEND, blend(RS[XRS_DESTBLEND]));
    set_rs(D3DRS_BLENDOP, blend_op(RS[XRS_BLENDOP]));
    set_rs(D3DRS_BLENDFACTOR, RS[XRS_BLENDCOLOR]);
    set_rs(D3DRS_COLORWRITEENABLE, color_write(RS[XRS_COLORWRITEENABLE]));
    set_rs(D3DRS_DITHERENABLE, RS[XRS_DITHERENABLE] != 0);
    set_rs(D3DRS_SHADEMODE, RS[XRS_SHADEMODE] == 0x1D00 ? D3DSHADE_FLAT : D3DSHADE_GOURAUD);
    set_rs(D3DRS_FILLMODE, RS[XRS_FILLMODE] == 0x1B00 ? D3DFILL_POINT : RS[XRS_FILLMODE] == 0x1B01 ? D3DFILL_WIREFRAME : D3DFILL_SOLID);
    DWORD cull = D3DCULL_NONE;
    if (RS[XRS_CULLMODE] == 0x900 || RS[XRS_CULLMODE] == 0x901) {
        int cw = RS[XRS_CULLMODE] == 0x900;
        if (RS[XRS_FRONTFACE] == 0x901) cw = !cw;
        cull = cw ? D3DCULL_CW : D3DCULL_CCW;
    }
    set_rs(D3DRS_CULLMODE, cull);
    set_rs(D3DRS_STENCILENABLE, RS[XRS_STENCILENABLE] != 0);
    set_rs(D3DRS_STENCILFUNC, cmp_func(RS[XRS_STENCILFUNC]));
    set_rs(D3DRS_STENCILREF, RS[XRS_STENCILREF] & 0xFF);
    set_rs(D3DRS_STENCILMASK, RS[XRS_STENCILMASK] & 0xFF);
    set_rs(D3DRS_STENCILWRITEMASK, RS[XRS_STENCILWRITEMASK] & 0xFF);
    set_rs(D3DRS_STENCILFAIL, stencil_op(RS[XRS_STENCILFAIL]));
    set_rs(D3DRS_STENCILZFAIL, stencil_op(RS[XRS_STENCILZFAIL]));
    set_rs(D3DRS_STENCILPASS, stencil_op(RS[XRS_STENCILPASS]));
    set_rs(D3DRS_FOGENABLE, RS[XRS_FOGENABLE] != 0);
    set_rs(D3DRS_FOGTABLEMODE, RS[XRS_FOGTABLEMODE] & 3);
    set_rs(D3DRS_FOGSTART, RS[XRS_FOGSTART]);
    set_rs(D3DRS_FOGEND, RS[XRS_FOGEND]);
    set_rs(D3DRS_FOGDENSITY, RS[XRS_FOGDENSITY]);
    set_rs(D3DRS_RANGEFOGENABLE, RS[XRS_RANGEFOGENABLE] != 0);
    set_rs(D3DRS_FOGCOLOR, swap_rb(RS[XRS_FOGCOLOR]));   // ponytail: Cxbx notes the Xbox fog colour is RGB-swapped; unverified here
    for (int i = 0; i < 4; i++) set_rs(D3DRS_WRAP0 + i, wrap_flags(RS[XRS_WRAP0 + i]));
    set_rs(D3DRS_LIGHTING, RS[XRS_LIGHTING] != 0);
    set_rs(D3DRS_SPECULARENABLE, RS[XRS_SPECULARENABLE] != 0);
    set_rs(D3DRS_LOCALVIEWER, RS[XRS_LOCALVIEWER] != 0);
    set_rs(D3DRS_COLORVERTEX, RS[XRS_COLORVERTEX] != 0);
    set_rs(D3DRS_SPECULARMATERIALSOURCE, RS[XRS_SPECULARMATERIALSOURCE] & 3);
    set_rs(D3DRS_DIFFUSEMATERIALSOURCE, RS[XRS_DIFFUSEMATERIALSOURCE] & 3);
    set_rs(D3DRS_AMBIENTMATERIALSOURCE, RS[XRS_AMBIENTMATERIALSOURCE] & 3);
    set_rs(D3DRS_EMISSIVEMATERIALSOURCE, RS[XRS_EMISSIVEMATERIALSOURCE] & 3);
    set_rs(D3DRS_AMBIENT, RS[XRS_AMBIENT]);
    set_rs(D3DRS_NORMALIZENORMALS, RS[XRS_NORMALIZENORMALS] != 0);
    set_rs(D3DRS_TEXTUREFACTOR, RS[XRS_TEXTUREFACTOR]);
    set_rs(D3DRS_POINTSIZE, RS[XRS_POINTSIZE]);
    set_rs(D3DRS_POINTSPRITEENABLE, RS[XRS_POINTSPRITEENABLE] != 0);
    // Shadow volumes (stencil on, depth test on, no depth or colour writes) are pushed back a little, so a volume
    // surface lying on the model it was made from doesn't flicker in and out of shadow (speckled rims along the
    // characters' outlines). The game sets no depth bias itself. Kept tiny: a room's depth image spans only about
    // 0.0004 and the volume meets the floor at the feet, so more cuts the cast shadow off the floor.
    // Debug ONI_SHADOW_BIAS=<float> overrides, 0 = off.
    static float bias = -1;
    if (bias < 0) { char v[16]; bias = env_str("ONI_SHADOW_BIAS", v, sizeof v) ? (float)atof(v) : 0.00001f; }
    int no_colour = !RS[XRS_COLORWRITEENABLE] || (RS[XRS_ALPHABLENDENABLE] && RS[XRS_SRCBLEND] == 0 && RS[XRS_DESTBLEND] == 1);
    int volume = RS[XRS_STENCILENABLE] && RS[XRS_ZENABLE] && !RS[XRS_ZWRITEENABLE] && no_colour;
    draw_is_volume = volume;
    float b = volume ? bias : 0, slope = volume && bias ? 1.0f : 0;
    set_rs(D3DRS_DEPTHBIAS, *(DWORD *)&b);
    set_rs(D3DRS_SLOPESCALEDEPTHBIAS, *(DWORD *)&slope);
}

static void apply_stage_state(int st)
{
    set_tss(st, D3DTSS_COLOROP, tex_op(TS(st, XTSS_COLOROP)));
    set_tss(st, D3DTSS_COLORARG0, TS(st, XTSS_COLORARG0));
    set_tss(st, D3DTSS_COLORARG1, TS(st, XTSS_COLORARG1));
    set_tss(st, D3DTSS_COLORARG2, TS(st, XTSS_COLORARG2));
    // Alpha PREMODULATE (17) with the next stage's alpha off: the XDK builds the same combiner as SELECTARG1 (checked
    // on its code by genma-recomp: the hair's alpha is the texture's). D3D9 drivers treat 17 their own way, if at all.
    DWORD aop = tex_op(TS(st, XTSS_ALPHAOP));
    if (aop == D3DTOP_PREMODULATE && (st == 3 || TS(st + 1, XTSS_ALPHAOP) <= D3DTOP_DISABLE)) aop = D3DTOP_SELECTARG1;
    // Xbox alpha kill drops texels whose alpha is 0 whatever else is on. With the game's own alpha test on, the stand-in
    // (apply_render_state) is that test, on the final alpha; with this stage's alpha off and no blending, the final
    // alpha is the vertex colour's (opaque), so nothing was dropped: the game's subtitle strip under in-game movies
    // (links04) drew its transparent background as a black box. There the stage passes on the texture's alpha instead:
    // not blended, it only feeds the test, and the game's test (alpha != 0) then drops what the kill would.
    int kill_alpha = TS(st, XTSS_ALPHAKILL) && TS(st, XTSS_COLOROP) > 1 && aop <= D3DTOP_DISABLE
                     && RS[XRS_ALPHATESTENABLE] && !RS[XRS_ALPHABLENDENABLE];
    if (kill_alpha) aop = D3DTOP_SELECTARG1;
    set_tss(st, D3DTSS_ALPHAOP, aop);
    set_tss(st, D3DTSS_ALPHAARG0, TS(st, XTSS_ALPHAARG0));
    set_tss(st, D3DTSS_ALPHAARG1, kill_alpha ? D3DTA_TEXTURE : TS(st, XTSS_ALPHAARG1));
    set_tss(st, D3DTSS_ALPHAARG2, TS(st, XTSS_ALPHAARG2));
    set_tss(st, D3DTSS_RESULTARG, TS(st, XTSS_RESULTARG) == D3DTA_TEMP ? D3DTA_TEMP : D3DTA_CURRENT);   // only valid targets
    set_tss(st, D3DTSS_TEXTURETRANSFORMFLAGS, TS(st, XTSS_TEXTURETRANSFORMFLAGS));
    set_tss(st, D3DTSS_TEXCOORDINDEX, TS(st, XTSS_TEXCOORDINDEX));
    set_tss(st, D3DTSS_BUMPENVMAT00, TS(st, XTSS_BUMPENVMAT00));
    set_tss(st, D3DTSS_BUMPENVMAT01, TS(st, XTSS_BUMPENVMAT01));
    set_tss(st, D3DTSS_BUMPENVMAT10, TS(st, XTSS_BUMPENVMAT10));
    set_tss(st, D3DTSS_BUMPENVMAT11, TS(st, XTSS_BUMPENVMAT11));
    set_tss(st, D3DTSS_BUMPENVLSCALE, TS(st, XTSS_BUMPENVLSCALE));
    set_tss(st, D3DTSS_BUMPENVLOFFSET, TS(st, XTSS_BUMPENVLOFFSET));
    set_ss(st, D3DSAMP_ADDRESSU, tex_address(TS(st, XTSS_ADDRESSU)));
    set_ss(st, D3DSAMP_ADDRESSV, tex_address(TS(st, XTSS_ADDRESSV)));
    set_ss(st, D3DSAMP_ADDRESSW, tex_address(TS(st, XTSS_ADDRESSW)));
    // Screen-space (pre-transformed) draws: the 640x480 background, HUD, text, menus.
    int screen = !(vertex_shader & 1) && (vertex_shader & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
    // Text and UI are paletted sprites drawn without blending, scaled ~1.5x with UVs inset half a texel. Filtered
    // bilinearly, their edge rows pick up ~18% of the neighbouring glyph in the font sheet, which the alpha test
    // (NOTEQUAL 0) turns into solid stray marks. Nearest-texel sampling removes them (compared on the save screen).
    // Screen-space only: 3D models also use unblended paletted textures and keep filtering.
    // ponytail: unverified whether NV2A filters paletted textures at all; blended paletted effects keep filtering.
    int paletted = bound_tex[st] && XFMT_CODE(bound_tex[st]->Format) == 0x0b;
    int p8 = screen && !RS[XRS_ALPHABLENDENABLE] && paletted;
    if (settings.filter == FILTER_SHARP && screen) p8 = 1;   // settings: background_filter = sharp
    // Some rooms draw their background (or its animated parts) nearest-texel (room 123). At 640x480 that is the same
    // as bilinear, but scaled up it turns blocky, so screen-space non-paletted images follow background_filter.
    // (Blended paletted sprites, the health HUD's digits and bars, keep the game's own filter: smoothed and inset half a
    // texel, the soul count's digits lost their edge rows and columns.)
    int bg_smooth = screen && !p8 && !paletted && settings.filter != FILTER_SHARP && scale > 1;
    // Smoothing a rectangle cut from a sheet of animation frames would blend in the next frame along its edges
    // (a 1-pixel seam), so fix_texcoords pulls those rectangles' texture coordinates in by half a texel.
    inset_uv[st][0] = inset_uv[st][1] = 0;
    if (bg_smooth && tex_filter(TS(st, XTSS_MAGFILTER)) == D3DTEXF_POINT && bound_tex[st]) {
        struct xtex_info ti = tex_info(bound_tex[st]->Format, bound_tex[st]->Size);
        if (ti.w && ti.h) { inset_uv[st][0] = 0.5f / ti.w; inset_uv[st][1] = 0.5f / ti.h; }
    }
    DWORD minf = p8 ? D3DTEXF_POINT : bg_smooth ? D3DTEXF_LINEAR : tex_filter(TS(st, XTSS_MINFILTER));
    if (!screen && minf == D3DTEXF_LINEAR && max_aniso > 1) minf = D3DTEXF_ANISOTROPIC;   // settings: anisotropy
    set_ss(st, D3DSAMP_MAGFILTER, p8 ? D3DTEXF_POINT : bg_smooth ? D3DTEXF_LINEAR : tex_filter(TS(st, XTSS_MAGFILTER)));
    set_ss(st, D3DSAMP_MINFILTER, minf);
    set_ss(st, D3DSAMP_MIPFILTER, tex_filter(TS(st, XTSS_MIPFILTER)));
    set_ss(st, D3DSAMP_MIPMAPLODBIAS, TS(st, XTSS_MIPMAPLODBIAS));
    set_ss(st, D3DSAMP_MAXMIPLEVEL, TS(st, XTSS_MAXMIPLEVEL));
    set_ss(st, D3DSAMP_MAXANISOTROPY, minf == D3DTEXF_ANISOTROPIC ? max_aniso : TS(st, XTSS_MAXANISOTROPY) ? TS(st, XTSS_MAXANISOTROPY) : 1);
    set_ss(st, D3DSAMP_BORDERCOLOR, TS(st, XTSS_BORDERCOLOR));

    // Stages that are off may hold stale pointers, and the game unloads resource bundles while textures from
    // them are still bound (harmless on Xbox, where freed memory stays readable). Skip what we can't read.
    // With a pixel shader bound, PSTextureModes says which stages sample; otherwise the stage's colour op does.
    int used = pixel_shader ? (RS[XRS_PSTEXTUREMODES] >> (st * 5) & 31) != 0 : TS(st, XTSS_COLOROP) > 1;
    const XPix *x = used ? bound_tex[st] : 0;
    if (x && (!x->Data || !readable_bytes((void *)x->Data, 1))) x = 0;
    IDirect3DTexture9 *t = 0;
    if (st == 0) xbr_w = xbr_h = text_bound = 0;
    if (x && x->Data) {
        const uint32_t *pal = paletted ? stage_palette(st) : 0;
        if (pal) note_hud_sheet(x, stage_pal[st]);
        struct tex_entry *h = host_for(cache_key(x, pal));
        if (h->gpu) {
            t = gpu_texture_for(x);   // never from Xbox memory, even when a device reset dropped the D3D9 texture
        } else {
            // The game often fills texture memory without a Lock (file loads straight into it), so check the
            // contents once per frame while the texture is in use.
            if (h->hash_frame != frame_no) {
                h->hash_frame = frame_no;
                uint64_t hv = content_hash(x, pal);
                if (hv != h->hash) { h->hash = hv; h->dirty = 1; }
            }
            if (h->pack_wait && h->pack_wait_gen != pack_ready) h->dirty = 1;   // a pack texture was made: maybe this one
            int reup = !h->tex || h->dirty || h->data != x->Data || h->format != x->Format || h->size != x->Size;
            if (reup && recording && h->tex && h->used_rec == rec_serial) frames[rec_cur].broken = 1;   // rewritten while in use (menu text)
            if (reup) { LONGLONG tu = qpc(); t = upload_texture(x, pal, h); st_tex += qpc() - tu; }
            else t = h->tex;
            if (h->screen2d) {
                if (screen2d_frame != frame_no) screen2d_band[0] = screen2d_band[1] = 0;
                screen2d_frame = frame_no;
                if (screen2d_names[h->screen2d - 1].y1 > 0) { screen2d_band[0] = screen2d_names[h->screen2d - 1].y0; screen2d_band[1] = screen2d_names[h->screen2d - 1].y1; }
            }
            h->used_rec = rec_serial;
            int own = t && !h->replaced;   // a texture pack's replacement is drawn as it is: none of the upscaling below
            if (own && screen && !p8 && !paletted && h->uploaded_frame == frame_no) dump_image(h);
            if (own && screen && !p8 && !paletted && settings.filter == FILTER_ENHANCED && scale > 1) t = upscaled(h, 0);
            // settings: hud_filter. Only images drawn on more than one frame: palette fades make a new cache entry
            // (palette) per frame, each drawn once, and upscaling every one of them is far too slow.
            if (!h->first_frame) h->first_frame = frame_no;
            IDirect3DTexture9 *font = own && screen && paletted && settings.enhanced_text && scale > 1
                                    && frame_no > h->first_frame ? font_sheet_texture(h, x, pal) : 0;
            if (font) {   // drawn about 1:1 (text_k): smooth sampling, not the nearest texel the original's pixels need
                t = font;
                if (st == 0) text_bound = 1;
                if (st == 0 && h->text == 2 && !RS[XRS_ALPHABLENDENABLE]) {   // soft edges: blend by the coverage
                    set_rs(D3DRS_ALPHABLENDENABLE, TRUE);
                    set_rs(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
                    set_rs(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
                    set_rs(D3DRS_BLENDOP, D3DBLENDOP_ADD);
                    // The game's text colour is 0x80808080, 0x80 meaning full: it doubles the colour (MODULATE2X) but
                    // not the alpha, which only fed an on/off alpha test. Blended, the alpha must be doubled too.
                    if (TS(0, XTSS_ALPHAOP) == D3DTOP_MODULATE) set_tss(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE2X);
                }
                set_ss(st, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                set_ss(st, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            }
            else if (t && h->replaced && h->soft_alpha && p8 && st == 0 && !RS[XRS_ALPHABLENDENABLE]) {
                // A smooth-edged replacement in a draw the game makes with an alpha test and no blending (menu digits
                // and names): drawn that way every texel with any alpha is solid and taken nearest, so its anti-aliased
                // edge turns to steps and its faint fringe to stray pixels. Blend by the file's alpha (coverage)
                // instead, sampled smoothly. The alpha test stays, so the depth and colour writes it gated still skip
                // empty texels. The game's own alpha stays in it, doubled as for the font (0x80 is full): taking the
                // texture's alone showed what the game hides with a zero alpha (the save screen's "memory unit" label
                // over AVAILABLE: a palette of the font sheet itself goes past 0x80, so its HD file counts as soft).
                set_rs(D3DRS_ALPHABLENDENABLE, TRUE);
                set_rs(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
                set_rs(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
                set_rs(D3DRS_BLENDOP, D3DBLENDOP_ADD);
                if (TS(0, XTSS_ALPHAOP) == D3DTOP_MODULATE) set_tss(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE2X);
                set_ss(st, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                set_ss(st, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            }
            else if (own && screen && paletted && settings.hud_xbr && scale > 1 && frame_no > h->first_frame) {
                LONGLONG u0 = qpc();
                t = upscaled(h, p8 ? 2 : 1);
                double ums = qpc_ms(qpc() - u0);
                if (ums > 10) {   // stalls.txt: an xbr upscale of a menu sheet on the game thread (one per palette version)
                    struct xtex_info ti = tex_info(x->Format, x->Size);
                    char un[64], uw[64];
                    tex_name(x, pal, &ti, un, uw, sizeof un);
                    stall_note("frame %u xbr upscale %s %.1f ms", frame_no, un, ums);
                }
                if (st == 0 && t != h->tex) { struct xtex_info ti = tex_info(x->Format, x->Size); xbr_w = ti.w; xbr_h = ti.h; }
                if (t != h->tex) dump_xbr(h, x, pal, t);
            }
            // A pack's HD version of a 2D sheet: its gauges are stretched slivers too, and the HD sliver's neighbouring
            // detail smears across the bar (the HUD's and the save screen's bars went streaky). Same treatment as xbr.
            if (st == 0 && t && h->replaced && screen && paletted) { struct xtex_info ti = tex_info(x->Format, x->Size); xbr_w = ti.w; xbr_h = ti.h; }
        }
    }
    IDirect3DDevice9_SetTexture(dev, st, (IDirect3DBaseTexture9 *)t);
    // a pack's HD picture has mip levels where the game's own may have none (d3d_pack.c): on a model, use them
    if (t && !screen && tex_filter(TS(st, XTSS_MIPFILTER)) == D3DTEXF_NONE && IDirect3DTexture9_GetLevelCount(t) > 1)
        set_ss(st, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
}

// ---- primitives --------------------------------------------------------------------------------------
static int prim_count(uint32_t xprim, uint32_t n, D3DPRIMITIVETYPE *pc)
{
    if (xprim <= 4 || xprim == 10)
        xfeature(xprim == 3, "draw: primitive type %u%s", xprim, xprim == 3 ? " (line loop drawn open)" : "");
    switch (xprim) {
    case 1: *pc = D3DPT_POINTLIST; return n;
    case 2: *pc = D3DPT_LINELIST; return n / 2;
    case 3: case 4: *pc = D3DPT_LINESTRIP; return n - 1;   // ponytail: LINELOOP drawn open
    case 5: *pc = D3DPT_TRIANGLELIST; return n / 3;
    case 6: case 9: *pc = D3DPT_TRIANGLESTRIP; return n - 2;   // quad strip == triangle strip order
    case 7: case 10: *pc = D3DPT_TRIANGLEFAN; return n - 2;
    case 8: *pc = D3DPT_TRIANGLELIST; return n / 4 * 2;        // quad list: drawn with generated indices
    }
    return 0;
}

uint32_t fvf_stride(uint32_t fvf)
{
    static const uint8_t pos[] = {0, 12, 16, 16, 20, 24, 28, 32};   // D3DFVF_POSITION_MASK >> 1 (XYZ..XYZB5)
    uint32_t s = pos[(fvf & 0xE) >> 1];
    if (fvf & D3DFVF_NORMAL) s += 12;
    if (fvf & D3DFVF_PSIZE) s += 4;
    if (fvf & D3DFVF_DIFFUSE) s += 4;
    if (fvf & D3DFVF_SPECULAR) s += 4;
    uint32_t ntex = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    for (uint32_t i = 0; i < ntex; i++) {
        static const uint8_t sz[] = {8, 12, 16, 4};
        s += sz[(fvf >> (16 + i * 2)) & 3];
    }
    return s;
}

// ---- feature report (report.c): what each draw uses that the port ignores or only approximates ------------
static const char *kind_name(int k)
{
    static const char *const n[] = {"L8", "AL8", "A1R5G5B5", "X1R5G5B5", "A4R4G4B4", "R5G6B5", "A8R8G8B8", "X8R8G8B8", "P8", "A8",
                                    "A8L8", "A8B8G8R8", "B8G8R8A8", "R8G8B8A8", "DXT1", "DXT3", "DXT5", "YUY2", "R5G5B5A1", "R4G4B4A4", "depth"};
    return k >= 0 && k < (int)(sizeof n / sizeof *n) ? n[k] : "?";
}

static void report_draw(void)
{
    static uint32_t last_sig;   // most draws repeat the previous draw's state: skip them cheaply
    uint32_t sig = vertex_shader ^ pixel_shader * 31;
    for (int i = XRS_ZFUNC; i < XRS_COUNT; i++) sig = sig * 16777619u ^ RS[i];
    for (int i = 0; i < 4 * XTSS_STAGE_SIZE; i++) sig = sig * 16777619u ^ TSS[i];
    for (int st = 0; st < 4; st++) sig = sig * 16777619u ^ (uint32_t)stage_tex[st];
    if (sig == last_sig) return;
    last_sig = sig;

    int vs = vertex_shader & 1;
    const char *path = vs ? "vertex shader" : (vertex_shader & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW ? "pre-transformed" : "fixed-function T&L";
    float f;
    if (RS[XRS_VERTEXBLEND]) xfeature(!vs, "state: vertex blend %u (%s)", RS[XRS_VERTEXBLEND], path);
    if (RS[XRS_LOGICOP]) xfeature(1, "state: logic op %x", RS[XRS_LOGICOP]);
    if (RS[XRS_ZBIAS]) xfeature(1, "state: z bias %d", (int)RS[XRS_ZBIAS]);
    if (RS[XRS_SOLIDOFFSETENABLE] || RS[XRS_WIREFRAMEOFFSETENABLE] || RS[XRS_POINTOFFSETENABLE]) xfeature(1, "state: polygon offset");
    if (RS[XRS_TWOSIDEDLIGHTING]) xfeature(1, "state: two-sided lighting");
    if (RS[XRS_BACKFILLMODE] != RS[XRS_FILLMODE]) xfeature(1, "state: back fill mode %x differs from front %x", RS[XRS_BACKFILLMODE], RS[XRS_FILLMODE]);
    if (RS[XRS_YUVENABLE]) xfeature(1, "state: YUV enable");
    if (RS[XRS_POINTSCALEENABLE]) xfeature(1, "state: point scale");
    memcpy(&f, &RS[XRS_LINEWIDTH], 4);
    if (f != 1.0f) xfeature(1, "state: line width %g", f);
    if (RS[XRS_EDGEANTIALIAS]) xfeature(0, "state: edge antialias");
    if (RS[XRS_OCCLUSIONCULLENABLE] || RS[XRS_STENCILCULLENABLE]) xfeature(0, "state: occlusion/stencil cull");
    if (RS[XRS_FOGENABLE]) xfeature(0, "state: fog table mode %u (%s)", RS[XRS_FOGTABLEMODE], path);
    if (RS[XRS_LIGHTING] && !vs && (vertex_shader & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW && (vertex_shader & D3DFVF_NORMAL))
        xfeature(1, "state: fixed-function lighting with normals (lights not implemented)");
    if (RS[XRS_STENCILENABLE]) xfeature(0, "state: stencil func %x ops %x/%x/%x", RS[XRS_STENCILFUNC], RS[XRS_STENCILFAIL], RS[XRS_STENCILZFAIL], RS[XRS_STENCILPASS]);
    if (RS[XRS_ALPHABLENDENABLE]) {
        int odd = RS[XRS_SRCBLEND] == 0x8003 || RS[XRS_SRCBLEND] == 0x8004 || RS[XRS_DESTBLEND] == 0x8003 || RS[XRS_DESTBLEND] == 0x8004
               || RS[XRS_BLENDOP] == 0xF005 || RS[XRS_BLENDOP] == 0xF006;
        xfeature(odd, "state: blend %x/%x op %x", RS[XRS_SRCBLEND], RS[XRS_DESTBLEND], RS[XRS_BLENDOP]);
    }
    if (RS[XRS_COLORWRITEENABLE] != 0x01010101) xfeature(0, "state: colour write mask %08x", RS[XRS_COLORWRITEENABLE]);
    if (pixel_shader && RS[XRS_PSCOMPAREMODE]) xfeature(1, "pixel shader: compare mode %x", RS[XRS_PSCOMPAREMODE]);

    for (int st = 0; st < 4; st++) {
        int used = pixel_shader ? (RS[XRS_PSTEXTUREMODES] >> (st * 5) & 31) != 0 : TS(st, XTSS_COLOROP) > 1;
        if (!used) continue;
        const XPix *x = bound_tex[st];
        if (x) {
            struct xtex_info t = tex_info(x->Format, x->Size);
            int code = XFMT_CODE(x->Format), cube = XFMT_CUBEMAP(x->Format) != 0, vol = XFMT_DIMS(x->Format) == 3;
            xfeature(!t.ok || cube || vol || t.kind == F_DEPTH, "texture: format %02x %s%s%s%s", code, t.ok ? kind_name(t.kind) : "unknown",
                     t.linear ? " linear" : "", cube ? " cubemap" : "", vol ? " volume" : "");
        }
        if (pixel_shader) continue;   // the stage ops below are replaced by the pixel shader
        uint32_t cop = TS(st, XTSS_COLOROP), aop = TS(st, XTSS_ALPHAOP);
        int bump = cop == 25 || cop == 26;
        xfeature(bump, "stage %d: colour op %u, alpha op %u", st, cop, aop);
        if (TS(st, XTSS_RESULTARG) != D3DTA_CURRENT) xfeature(0, "stage %d: result to %x", st, TS(st, XTSS_RESULTARG));
        if (TS(st, XTSS_TEXTURETRANSFORMFLAGS)) xfeature(vs || (vertex_shader & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW,
                                                          "stage %d: texture transform %x (%s)", st, TS(st, XTSS_TEXTURETRANSFORMFLAGS), path);
        if (TS(st, XTSS_TEXCOORDINDEX) >> 16) xfeature(vs, "stage %d: texcoord generation %x (%s)", st, TS(st, XTSS_TEXCOORDINDEX) >> 16, path);
        if (TS(st, XTSS_COLORKEYOP)) xfeature(1, "stage %d: colour key op %u", st, TS(st, XTSS_COLORKEYOP));
        if (TS(st, XTSS_COLORSIGN)) xfeature(1, "stage %d: colour sign %08x", st, TS(st, XTSS_COLORSIGN));
        if (TS(st, XTSS_ALPHAKILL)) xfeature(RS[XRS_ALPHATESTENABLE] != 0, "stage %d: alpha kill%s", st,
                                             RS[XRS_ALPHATESTENABLE] ? " with the game's own alpha test (kill ignored)" : " (as alpha test > 0)");
        if (TS(st, XTSS_ADDRESSU) == 5 || TS(st, XTSS_ADDRESSV) == 5) xfeature(0, "stage %d: clamp-to-edge addressing", st);
        if (TS(st, XTSS_ADDRESSU) > 5 || TS(st, XTSS_ADDRESSV) > 5) xfeature(1, "stage %d: unknown address mode %u/%u", st, TS(st, XTSS_ADDRESSU), TS(st, XTSS_ADDRESSV));
        if (TS(st, XTSS_MAGFILTER) > 3 || TS(st, XTSS_MINFILTER) > 3) xfeature(1, "stage %d: filter %u/%u (quincunx/gaussian approximated)", st, TS(st, XTSS_MAGFILTER), TS(st, XTSS_MINFILTER));
    }
    if (!vs) xfeature(0, "draw: FVF %x", vertex_shader);
}

static uint16_t quad_indices[0x30000];
// Quad list -> two triangles per quad, into quad_indices (idx: the draw's indices, or 0). Returns the index count,
// 0 if the draw doesn't fit (unindexed, vertex numbers must fit 16 bits).
static uint32_t quad_list(const uint16_t *idx, uint32_t n)
{
    uint32_t q = n / 4;
    if (q * 6 > sizeof quad_indices / 2 || (!idx && n > 0x10000)) return 0;
    for (uint32_t i = 0; i < q; i++) {
        uint16_t a = idx ? idx[i * 4] : i * 4, b = idx ? idx[i * 4 + 1] : i * 4 + 1;
        uint16_t c = idx ? idx[i * 4 + 2] : i * 4 + 2, d = idx ? idx[i * 4 + 3] : i * 4 + 3;
        uint16_t *o = quad_indices + i * 6;
        o[0] = a; o[1] = b; o[2] = c; o[3] = a; o[4] = c; o[5] = d;
    }
    return q * 6;
}
struct draw_stats stats;   // world: fixed-function 3D draws; depth: room depth uploads

static int ready_to_draw(void)
{
    if (!dev) return 0;
    for (int st = 0; st < 4; st++) bound_tex[st] = stage_tex[st] && readable_bytes(stage_tex[st], sizeof(XPix)) ? stage_tex[st] : 0;
    for (int again = 0; again < 2; again++) {   // a background upscale pass mid-way clobbers state: apply it all again
        passes_ran = 0;
        apply_render_state();
        for (int st = 0; st < 4; st++) apply_stage_state(st);
        if (!passes_ran) break;
    }
    apply_pixel_shader();
    report_draw();
    rs_valid = 1;
    static int plain = -1, only = -1;   // debug: ONI_PLAIN=1 untextured/unblended; ONI_ONLY=n draws only the n-th draw of each frame
    if (plain < 0) plain = env_int("ONI_PLAIN", 0) == 1, only = env_int("ONI_ONLY", 0);
    if (only && stats.drawn + 1 != (uint32_t)only) { stats.drawn++; return 0; }
    if (plain) {
        rs_valid = 0;
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, 0); IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, 0); IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHATESTENABLE, 0);
        IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, (vertex_shader & D3DFVF_DIFFUSE) ? D3DTA_DIFFUSE : D3DTA_TEXTURE);
        IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    }
    if (vertex_shader & 1) return 1;   // caller sets the shader and declaration
    IDirect3DDevice9_SetVertexShader(dev, 0);
    IDirect3DDevice9_SetFVF(dev, vertex_shader);
    stats.drawn++;
    if ((vertex_shader & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW) stats.world++;
    for (int i = 0; i < 8; i++) if (!stats.fvf[i] || stats.fvf[i] == vertex_shader) { stats.fvf[i] = vertex_shader; break; }
    return 1;
}

// Linear (non-power-of-two) textures on Xbox are addressed in texels, not 0..1. For FVF draws, copy the
// vertices and divide those texture coordinates by the texture size. Returns verts unchanged if not needed.
static uint8_t *scratch_verts;
static uint32_t scratch_size;
// For each texture coordinate set: 1/size if a stage samples a linear (texel-addressed) texture with it, else 0.
// With a pixel shader, stage n reads coordinate set n; otherwise TEXCOORDINDEX picks the set.
static int linear_texcoord_scales(float sx[4], float sy[4])
{
    int any = 0;
    for (int st = 0; st < 4; st++) {
        const XPix *x = bound_tex[st];
        int used = pixel_shader ? (RS[XRS_PSTEXTUREMODES] >> (st * 5) & 31) != 0 : TS(st, XTSS_COLOROP) > 1;
        uint32_t set = pixel_shader ? (uint32_t)st : TS(st, XTSS_TEXCOORDINDEX);
        if (!used || !x || !x->Size || set >= 4) continue;   // set >= 4 also skips TCI generation modes
        sx[set] = 1.0f / XSIZE_W(x->Size); sy[set] = 1.0f / XSIZE_H(x->Size); any = 1;
    }
    return any;
}

// Also: the Xbox GPU puts pixel centres at .5 (OpenGL style), D3D9 at whole numbers. Pre-transformed (XYZRHW)
// positions move by -0.5 so texels line up as on Xbox; otherwise sprites sampled at exact texel edges pick the
// neighbouring texel (seen as 1-pixel "jiggling" in flipbook-animated water).
const uint8_t *fix_texcoords(const uint8_t *verts, uint32_t stride, uint32_t count)
{
    uint32_t fvf = vertex_shader, ntex = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    float sx[8] = {0}, sy[8] = {0};
    int rhw = (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
    if (!linear_texcoord_scales(sx, sy) && !rhw) return verts;
    if (ntex > 4) ntex = 4;
    if (scratch_size < count * stride) {
        free(scratch_verts);
        scratch_size = (scratch_verts = malloc(count * stride * 2)) ? count * stride * 2 : 0;
        if (!scratch_verts) return verts;   // out of memory: drawn with the coordinates as they are
    }
    memcpy(scratch_verts, verts, count * stride);
    if (rhw) for (uint32_t i = 0; i < count; i++) {   // Xbox pixels -> render target pixels, then the half-pixel shift
        float *p = (float *)(scratch_verts + i * stride);
        if (hud_draw) { p[0] = p[0] * hud_map[0] + hud_map[1]; p[1] = p[1] * hud_map[2] + hud_map[3]; }
        p[0] = p[0] * rt_scale - 0.5f; p[1] = p[1] * rt_scale - 0.5f;
    }
    uint32_t off = fvf_stride(fvf & ~D3DFVF_TEXCOUNT_MASK);   // texture coordinates come last
    for (uint32_t set = 0; set < ntex; set++) {
        static const uint8_t sz[] = {8, 12, 16, 4};
        // Pre-transformed draws from a linear texture use texel coordinates with the texel's centre on the whole number
        // (the game's full-screen copies draw x 0-640 with u 0.5-640.5, exactly 1:1 on Xbox); D3D9 puts it at .5, so
        // as they were each copy moved the picture half a texel up and left. Twice before a camera cut (the frame is
        // drawn back from its copy), which showed as a jump of the whole picture.
        float c = rhw ? 0.5f : 0;
        if (sx[set]) for (uint32_t i = 0; i < count; i++) {
            float *t = (float *)(scratch_verts + i * stride + off);
            t[0] = (t[0] - c) * sx[set]; t[1] = (t[1] - c) * sy[set];
        }
        off += sz[(fvf >> (16 + set * 2)) & 3];
    }
    // Screen-space rectangles cut from a sheet of animation frames (room 001's water): the game's texture coordinates
    // land up to about 0.1 texel off the cell edges, by a different amount per frame (0.40635 for 0.40625). Invisible
    // at the sheet's own size, but an HD replacement magnifies it into the strip hopping half a screen pixel each time
    // the frame changes. Snap coordinates within 0.2 texel of a whole texel onto it (texture set 0, normalized only).
    if (rhw && ntex && !sx[0] && (TS(0, XTSS_TEXCOORDINDEX) & 0xFFFF) == 0 && bound_tex[0]) {
        struct xtex_info ti = tex_info(bound_tex[0]->Format, bound_tex[0]->Size);
        uint32_t uv = fvf_stride(fvf & ~D3DFVF_TEXCOUNT_MASK);
        if (ti.w && ti.h) for (uint32_t i = 0; i < count; i++) {
            float *t = (float *)(scratch_verts + i * stride + uv);
            for (int c = 0; c < 2; c++) {
                float n = c ? (float)ti.h : (float)ti.w, k = t[c] * n, r = floorf(k + 0.5f);
                if (fabsf(k - r) < 0.2f) t[c] = r / n;
            }
        }
    }
    // hud_filter = xbr (or a pack's HD sheet): the HUD's gauges are a sliver of the sheet (the magic bar: 1.5 texels
    // wide) stretched across the bar. super-xBR reshapes the sliver's edges row by row, so sampling across it, stretched
    // sideways, shows as stripes. A quad that stretches fewer than 4 texels to over 4x its size samples one texel's
    // centre on that axis.
    if (rhw && ntex && xbr_w && draw_quads && count % 4 == 0 && (TS(0, XTSS_TEXCOORDINDEX) & 0xFFFF) == 0) {
        uint32_t uv = fvf_stride(fvf & ~D3DFVF_TEXCOUNT_MASK);
        for (uint32_t g = 0; g < count; g += 4)
            for (int c = 0; c < 2; c++) {
                float n = c ? (float)xbr_h : (float)xbr_w, lo = 1e9f, hi = -1e9f, plo = 1e9f, phi = -1e9f;
                for (uint32_t i = g; i < g + 4; i++) {
                    float t = ((float *)(scratch_verts + i * stride + uv))[c] * n, p = ((float *)(scratch_verts + i * stride))[c];
                    lo = fminf(lo, t); hi = fmaxf(hi, t); plo = fminf(plo, p); phi = fmaxf(phi, p);
                }
                if (hi - lo <= 0 || hi - lo >= 4 || phi - plo <= 4 * (hi - lo) * rt_scale) continue;
                float mid = (floorf((lo + hi) / 2) + 0.5f) / n;
                for (uint32_t i = g; i < g + 4; i++) ((float *)(scratch_verts + i * stride + uv))[c] = mid;
            }
    }
    // enhanced_text: the game draws each glyph from half a texel in on the left and top of its box (the original's
    // outline loses half a column there, unnoticed), and the redrawn sheet's outline sits in that strip and up to half a
    // texel past the right. Widen each glyph's quad to cover it, in texture and on screen alike so nothing is squeezed:
    // half a texel left and up, up to half a texel right but never into the next 16-texel cell.
    if (rhw && ntex && text_bound && draw_quads && count % 4 == 0 && (TS(0, XTSS_TEXCOORDINDEX) & 0xFFFF) == 0) {
        uint32_t uv = fvf_stride(fvf & ~D3DFVF_TEXCOUNT_MASK);
        for (uint32_t g = 0; g < count; g += 4)
            for (int c = 0; c < 2; c++) {
                float lo = 1e9f, hi = -1e9f, plo = 0, phi = 0;   // texel range, and the screen position at each end
                for (uint32_t i = g; i < g + 4; i++) {
                    float t = ((float *)(scratch_verts + i * stride + uv))[c] * 256, p = ((float *)(scratch_verts + i * stride))[c];
                    if (t < lo) { lo = t; plo = p; }
                    if (t > hi) { hi = t; phi = p; }
                }
                if (hi - lo <= 0.5f || hi - lo > 16.01f) continue;
                float f = lo - floorf(lo), k = (phi - plo) / (hi - lo);   // screen pixels per texel (signed)
                float m = 1.0f / text_k();   // smooth sampling reaches up to a sheet pixel at this scale: stay that far inside the cell
                float grow_lo = f > 0.4f && f < 0.6f ? 0.5f - m : 0;
                float grow_hi = c == 0 ? fminf(0.5f, floorf(lo / 16) * 16 + 16 - m - hi) : 0;
                if (grow_hi < 0) grow_hi = 0;
                for (uint32_t i = g; i < g + 4; i++) {
                    float *t = (float *)(scratch_verts + i * stride + uv) + c, *p = (float *)(scratch_verts + i * stride) + c;
                    if (*t * 256 <= lo + 0.01f) { *t = (lo - grow_lo) / 256; *p -= grow_lo * k; }
                    else if (*t * 256 >= hi - 0.01f) { *t = (hi + grow_hi) / 256; *p += grow_hi * k; }
                }
            }
    }
    // Half-texel inset for smoothed screen-space rectangles (see inset_uv), per quad when the draw is whole quads.
    // ponytail: stage 0 / texture set 0 only, the case the game uses (room 123's animated background).
    if (rhw && ntex && inset_uv[0][0] && (TS(0, XTSS_TEXCOORDINDEX) & 0xFFFF) == 0) {
        uint32_t uv = fvf_stride(fvf & ~D3DFVF_TEXCOUNT_MASK), group = count % 4 == 0 ? 4 : count;
        for (uint32_t g = 0; g + group <= count; g += group)
            for (int c = 0; c < 2; c++) {
                float lo = 1e9f, hi = -1e9f, in = inset_uv[0][c];
                for (uint32_t i = g; i < g + group; i++) { float t = ((float *)(scratch_verts + i * stride + uv))[c]; if (t < lo) lo = t; if (t > hi) hi = t; }
                if (hi - lo <= 2 * in) continue;
                for (uint32_t i = g; i < g + group; i++) {
                    float *t = (float *)(scratch_verts + i * stride + uv) + c;
                    if (*t <= lo + in * 0.01f) *t += in; else if (*t >= hi - in * 0.01f) *t -= in;
                }
            }
    }
    return scratch_verts;
}

static void draw_body(uint32_t xprim, const uint8_t *verts, uint32_t nverts, uint32_t stride, const uint16_t *idx, uint32_t nidx, int32_t base);
static void draw(uint32_t xprim, const uint8_t *verts, uint32_t nverts, uint32_t stride, const uint16_t *idx, uint32_t nidx, int32_t base)
{
    LONGLONG t0 = qpc();
    draw_body(xprim, verts, nverts, stride, idx, nidx, base);
    st_draw += qpc() - t0;
}
static void draw_body(uint32_t xprim, const uint8_t *verts, uint32_t nverts, uint32_t stride, const uint16_t *idx, uint32_t nidx, int32_t base)
{
    D3DPRIMITIVETYPE pt;
    uint32_t n = idx ? nidx : nverts;
    int prims = prim_count(xprim, n, &pt);
    if (prims <= 0 || !ready_to_draw()) return;
    if (capture) { char k[24]; snprintf(k, sizeof k, "fvf %x", vertex_shader); capture_draw(k, xprim, n, idx ? verts + base * (int32_t)stride : verts, stride); }
    if (tex_capture) capture_textures();
    if (wide && xprim == 8) letterbox_check(verts, stride, idx, n, base);
    if (xprim == 8 && settings.picture == PICTURE_MODERN) wide_picture_check(verts, stride, idx, n, base);
    if (n == 4 && !idx && !(vertex_shader & 1) && (vertex_shader & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW && bound_tex[0]
        && XFMT_CODE(bound_tex[0]->Format) == 0x0c) {
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (int k = 0; k < 4; k++) {
            const float *p = (const float *)(verts + k * stride);
            if (p[0] < x0) x0 = p[0]; if (p[0] > x1) x1 = p[0]; if (p[1] < y0) y0 = p[1]; if (p[1] > y1) y1 = p[1];
        }
        if (x0 <= 1 && x1 >= 639 && y0 <= ART_Y0 + 1 && y1 >= ART_Y1 - 1) fullpic_frame = frame_no;
    }
    if (xprim == 8) {
        if (!(nidx = quad_list(idx, n))) return;
        idx = quad_indices;
    }
    uint32_t lo = 0, hi = nverts ? nverts - 1 : 0;
    if (idx) {
        lo = 0xFFFF; hi = 0;
        for (uint32_t i = 0; i < nidx; i++) { if (idx[i] < lo) lo = idx[i]; if (idx[i] > hi) hi = idx[i]; }
        verts += base * (int32_t)stride;
    }
    if (hold_loading_text(verts + lo * stride, stride)) return;
    IDirect3DTexture9 *orb_tex = 0;
    const uint8_t *moved = xprim == 8 && lo == 0 ? orb_layout(verts, hi + 1, stride, &orb_tex) : 0;
    if (moved) verts = moved;
    else if (xprim == 8 && lo == 0 && (moved = title_trim(verts, hi + 1, stride))) verts = moved;
    else if (xprim == 8 && lo == 0 && (moved = still_text(verts, hi + 1, stride))) verts = moved;
    // The pack's frame is drawn smoothly and blended by its own alpha (doubled: 0x80 is full), not cut out, so its soft
    // edge shows no dark fringe; the states are put back after.
    IDirect3DBaseTexture9 *was_tex = 0;
    static const D3DRENDERSTATETYPE rs_k[] = {D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND};
    static const D3DTEXTURESTAGESTATETYPE ts_k[] = {D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2};
    DWORD was_mag = 0, was_min = 0, was_rs[3], was_ts[3];
    if (orb_tex) {
        IDirect3DDevice9_GetTexture(dev, 0, &was_tex);
        IDirect3DDevice9_GetSamplerState(dev, 0, D3DSAMP_MAGFILTER, &was_mag);
        IDirect3DDevice9_GetSamplerState(dev, 0, D3DSAMP_MINFILTER, &was_min);
        for (int i = 0; i < 3; i++) IDirect3DDevice9_GetRenderState(dev, rs_k[i], &was_rs[i]), IDirect3DDevice9_GetTextureStageState(dev, 0, ts_k[i], &was_ts[i]);
        IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)orb_tex);
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_ADD);
        IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG2, D3DTA_TEXTURE);
    }
    int hud = wide && hud_begin(verts + lo * stride, stride, hi + 1 - lo);
    int fade = wide && !hud && hud_drawn && hud_surf && is_fade(verts + lo * stride, stride, hi + 1 - lo);
    const uint8_t *raw = verts;
    draw_quads = xprim == 8;
    verts = fix_texcoords(verts, stride, idx ? hi + 1 : nverts);
    draw_quads = 0;
    dev_draw(pt, lo, hi + 1 - lo, prims, idx, verts, stride, 0, 0);
    if (orb_tex) {
        IDirect3DDevice9_SetTexture(dev, 0, was_tex);
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, was_mag);
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, was_min);
        for (int i = 0; i < 3; i++) IDirect3DDevice9_SetRenderState(dev, rs_k[i], was_rs[i]), IDirect3DDevice9_SetTextureStageState(dev, 0, ts_k[i], was_ts[i]);
        if (was_tex) IDirect3DBaseTexture9_Release(was_tex);
    }
    if (hud) hud_end();
    if (fade) hud_fade(pt, lo, hi + 1 - lo, prims, idx, raw, stride, idx ? hi + 1 : nverts);
}

// ---- HLE: device -------------------------------------------------------------------------------------
static void init_default_state(void)
{
    // What the real Direct3D_CreateDevice puts in the state arrays (Xbox defaults, GL-style values).
    RS[XRS_ZFUNC] = 0x203; RS[XRS_ALPHAFUNC] = 0x207; RS[XRS_SRCBLEND] = 1; RS[XRS_DESTBLEND] = 0;
    RS[XRS_ZWRITEENABLE] = 1; RS[XRS_SHADEMODE] = 0x1D01; RS[XRS_COLORWRITEENABLE] = 0x01010101;
    RS[XRS_STENCILFUNC] = 0x207; RS[XRS_STENCILMASK] = 0xFF; RS[XRS_STENCILWRITEMASK] = 0xFF;
    RS[XRS_STENCILZFAIL] = RS[XRS_STENCILPASS] = RS[XRS_STENCILFAIL] = 0x1e00; RS[XRS_BLENDOP] = 0x8006;
    RS[XRS_FILLMODE] = RS[XRS_BACKFILLMODE] = 0x1B02; RS[XRS_FRONTFACE] = 0x900; RS[XRS_CULLMODE] = 0x901;
    RS[XRS_ZENABLE] = 1; RS[XRS_LIGHTING] = 1; RS[XRS_COLORVERTEX] = 1; RS[XRS_DIFFUSEMATERIALSOURCE] = 1;
    RS[XRS_SPECULARMATERIALSOURCE] = 2; RS[XRS_MULTISAMPLEMASK] = 0xFFFFFFFF;
    float one = 1.0f;
    memcpy(&RS[XRS_POINTSIZE], &one, 4); memcpy(&RS[XRS_LINEWIDTH], &one, 4); memcpy(&RS[XRS_FOGEND], &one, 4);
    for (int st = 0; st < 4; st++) {
        TS(st, XTSS_COLOROP) = st ? 1 : 4; TS(st, XTSS_ALPHAOP) = st ? 1 : 2;
        TS(st, XTSS_COLORARG1) = TS(st, XTSS_ALPHAARG1) = D3DTA_TEXTURE;
        TS(st, XTSS_COLORARG2) = TS(st, XTSS_ALPHAARG2) = D3DTA_CURRENT;
        TS(st, XTSS_ADDRESSU) = TS(st, XTSS_ADDRESSV) = TS(st, XTSS_ADDRESSW) = 1;
        TS(st, XTSS_MAGFILTER) = TS(st, XTSS_MINFILTER) = 1;
        TS(st, XTSS_TEXCOORDINDEX) = st; TS(st, XTSS_MAXANISOTROPY) = 1; TS(st, XTSS_RESULTARG) = D3DTA_CURRENT;
    }
}

// The scene's render targets, `scale` times the Xbox size. D3DPOOL_DEFAULT: made again after a device reset.
static int create_targets(void)
{
    UINT rw = bb_w * scale, rh = bb_h * scale;
    return SUCCEEDED(IDirect3DDevice9_CreateRenderTarget(dev, rw, rh, D3DFMT_X8R8G8B8, scene_ms, 0, FALSE, &scene_rt, 0))
        && SUCCEEDED(IDirect3DDevice9_CreateDepthStencilSurface(dev, rw, rh, D3DFMT_D24S8, scene_ms, 0, FALSE, &scene_ds, 0))
        && SUCCEEDED(IDirect3DDevice9_CreateDepthStencilSurface(dev, rw, rh, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &tex_ds, 0))
        && SUCCEEDED(IDirect3DDevice9_CreateTexture(dev, rw, rh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &frame_tex, 0));
}

// Textures the CPU fills. D3D9Ex has no D3DPOOL_MANAGED: there each is made in D3DPOOL_DEFAULT with a system-memory
// twin, kept in its private data. Locks go to the twin, and unlocking the last level copies it to the GPU, as MANAGED
// did by itself (levels are filled in order, so a partial fill isn't shown half-done).
static const GUID tex_twin_guid = {0x6f1d2b7a, 0x3c4e, 0x4b8f, {0x9a, 0x51, 0x2e, 0x7d, 0x0c, 0x63, 0xb4, 0x18}};

HRESULT tex_create(UINT w, UINT h, UINT levels, D3DFORMAT fmt, IDirect3DTexture9 **out)
{
    if (!dev_ex) return IDirect3DDevice9_CreateTexture(dev, w, h, levels, 0, fmt, D3DPOOL_MANAGED, out, 0);
    IDirect3DTexture9 *t = 0, *twin = 0;
    HRESULT hr = IDirect3DDevice9_CreateTexture(dev, w, h, levels, 0, fmt, D3DPOOL_DEFAULT, &t, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_CreateTexture(dev, w, h, IDirect3DTexture9_GetLevelCount(t), 0, fmt, D3DPOOL_SYSTEMMEM, &twin, 0);
    if (SUCCEEDED(hr)) hr = IDirect3DTexture9_SetPrivateData(t, &tex_twin_guid, twin, sizeof twin, D3DSPD_IUNKNOWN);
    if (twin) IDirect3DTexture9_Release(twin);   // the private data holds it now (or nothing does)
    if (FAILED(hr) && t) { IDirect3DTexture9_Release(t); t = 0; }
    *out = t;
    return hr;
}

static IDirect3DTexture9 *tex_twin(IDirect3DTexture9 *t)   // t's own reference: not to be released
{
    IDirect3DTexture9 *twin = 0;
    DWORD n = sizeof twin;
    if (!dev_ex || FAILED(IDirect3DTexture9_GetPrivateData(t, &tex_twin_guid, &twin, &n)) || !twin) return 0;
    IDirect3DTexture9_Release(twin);   // GetPrivateData AddRefs an IUnknown
    return twin;
}

HRESULT tex_lock(IDirect3DTexture9 *t, UINT level, D3DLOCKED_RECT *lr, DWORD flags)
{
    IDirect3DTexture9 *twin = tex_twin(t);
    return IDirect3DTexture9_LockRect(twin ? twin : t, level, lr, 0, flags);
}

void tex_unlock(IDirect3DTexture9 *t, UINT level)
{
    IDirect3DTexture9 *twin = tex_twin(t);
    if (!twin) { IDirect3DTexture9_UnlockRect(t, level); return; }
    IDirect3DTexture9_UnlockRect(twin, level);
    if (level + 1 < IDirect3DTexture9_GetLevelCount(t)) return;
    IDirect3DTexture9_AddDirtyRect(twin, 0);   // every level, not just the first's dirty region
    IDirect3DDevice9_UpdateTexture(dev, (IDirect3DBaseTexture9 *)twin, (IDirect3DBaseTexture9 *)t);
}

// HRESULT Direct3D_CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, X_D3DPRESENT_PARAMETERS *pp, IDirect3DDevice8 **ppDevice)
static uint32_t __stdcall Direct3D_CreateDevice(uint32_t Adapter, uint32_t DeviceType, uint32_t hFocusWindow, uint32_t BehaviorFlags, uint32_t pp, uint32_t ppDevice)
{
    uint32_t *p = (uint32_t *)pp;
    bb_w = p[0] ? p[0] : 640; bb_h = p[1] ? p[1] : 480;
    xlog("[d3d] CreateDevice %ux%u fmt %x count %u ms %x autodepth %u depthfmt %x flags %x interval %x",
         p[0], p[1], p[2], p[3], p[4], p[7], p[8], p[9], p[11]);
    HANDLE ready = CreateEventW(0, TRUE, FALSE, 0);
    CloseHandle(CreateThread(0, 0, window_thread, ready, 0, 0));
    WaitForSingleObject(ready, INFINITE);
    CloseHandle(ready);

#ifdef _WIN32
    // D3D9Ex where it can be had, for flip-model presents (present_surface): with the old copy-to-desktop present,
    // G-SYNC and FreeSync stayed off in the full-screen window. Plain D3D9 otherwise.
    IDirect3D9Ex *d3d_ex9 = 0;
    if (SUCCEEDED(Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d_ex9))) d3d = (IDirect3D9 *)d3d_ex9;
    else if (!(d3d = Direct3DCreate9(D3D_SDK_VERSION))) xfatal("[d3d] Direct3DCreate9 failed");
#else   // DXVK finds no adapter without a (32-bit) Vulkan driver
    if (!(d3d = Direct3DCreate9(D3D_SDK_VERSION)) || !IDirect3D9_GetAdapterCount(d3d))
        xfatal("No Vulkan graphics driver was found. %s Details in logs/onimusha_d3d9.log.", NO_VULKAN);
#endif
    fast_mode = env_int("ONI_FAST", 0) == 1;   // debug: ONI_FAST=1 drops vsync and vblank waits so scripted runs go faster than real time
    // The implicit swap chain is never presented: frames go to the window through `swapchain` (see present_frame).
    D3DPRESENT_PARAMETERS pp9 = {0};
    pp9.BackBufferWidth = 16; pp9.BackBufferHeight = 16; pp9.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp9.BackBufferCount = 1; pp9.hDeviceWindow = hwnd; pp9.Windowed = TRUE; pp9.SwapEffect = D3DSWAPEFFECT_DISCARD;
    dev_pp = pp9;
    DWORD dev_flags = D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE;
    HRESULT hr = E_FAIL;
#ifdef _WIN32
    if (d3d_ex9) {
        IDirect3DDevice9Ex *dev_ex9 = 0;
        hr = IDirect3D9Ex_CreateDeviceEx(d3d_ex9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, dev_flags, &pp9, 0, &dev_ex9);
        if (SUCCEEDED(hr)) { dev = (IDirect3DDevice9 *)dev_ex9; dev_ex = 1; }
        else {
            xlog("[d3d] CreateDeviceEx failed (%08lx): plain D3D9", hr);
            IDirect3D9Ex_Release(d3d_ex9);
            if (!(d3d = Direct3DCreate9(D3D_SDK_VERSION))) xfatal("[d3d] Direct3DCreate9 failed");
        }
    }
    if (!dev_ex) hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, dev_flags, &pp9, &dev);
    if (FAILED(hr)) xfatal("[d3d] IDirect3D9::CreateDevice failed %08lx", hr);
    xlog("[d3d] %s", dev_ex ? "D3D9Ex device: flip-model presents" : "D3D9 device");
#else
    hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, dev_flags, &pp9, &dev);
    if (FAILED(hr)) xfatal("[d3d] The graphics device couldn't be created (%08lx). The Vulkan driver may be too old for "
                           "DXVK 3.1, or lack its 32-bit part. Details in logs/onimusha_d3d9.log.", hr);
#endif

    scale = rt_scale = settings.scale;
    D3DCAPS9 caps;
    IDirect3DDevice9_GetDeviceCaps(dev, &caps);
    max_aniso = settings.anisotropy < (int)caps.MaxAnisotropy ? settings.anisotropy : (int)caps.MaxAnisotropy;
    recording = settings.interp > 1 && !fast_mode;
    if (recording) xlog("[d3d] frame interpolation on: %d frames per game step", settings.interp);
    UINT rw = bb_w * scale, rh = bb_h * scale;
    D3DMULTISAMPLE_TYPE ms = settings.msaa >= 2 && settings.msaa <= 16 ? (D3DMULTISAMPLE_TYPE)settings.msaa : D3DMULTISAMPLE_NONE;
    while (ms && (FAILED(IDirect3D9_CheckDeviceMultiSampleType(d3d, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, TRUE, ms, 0))
                  || FAILED(IDirect3D9_CheckDeviceMultiSampleType(d3d, 0, D3DDEVTYPE_HAL, D3DFMT_D24S8, TRUE, ms, 0))))
        ms = ms > 2 ? (D3DMULTISAMPLE_TYPE)max(ms / 2, 2) : D3DMULTISAMPLE_NONE;   // never 1: that is NONMASKABLE
    scene_ms = ms;
    if (!create_targets())
        xfatal("[d3d] can't create %ux%u render targets (scale %d, msaa %d) - lower scale/msaa in onimusha.ini", rw, rh, scale, ms);
    IDirect3DDevice9_SetRenderTarget(dev, 0, scene_rt);
    IDirect3DDevice9_SetDepthStencilSurface(dev, scene_ds);
    xlog("[d3d] rendering at %ux%u (scale %d), msaa %d, anisotropy %d", rw, rh, scale, ms, max_aniso);

    backbuffer_hdr = (XSurf){XCOMMON_SURFACE | XCOMMON_D3DCREATED | 1, (uint32_t)xalloc(bb_w * bb_h * 4), 0,
        1 | 2 << 4 | 0x12 << 8 | 1 << 16, (bb_w - 1) | (bb_h - 1) << 12 | ((bb_w * 4) / 64 - 1) << 24, 0};
    depth_hdr = (XSurf){XCOMMON_SURFACE | XCOMMON_D3DCREATED | 1, (uint32_t)xalloc(bb_w * bb_h * 4), 0,
        1 | 2 << 4 | 0x2e << 8 | 1 << 16, backbuffer_hdr.Size, 0};
    cur_rt = &backbuffer_hdr; cur_z = &depth_hdr;
    init_default_state();
    viewport = (D3DVIEWPORT9){0, 0, (DWORD)bb_w, (DWORD)bb_h, 0.0f, 1.0f};
    set_viewport_constants();
    apply_viewport();
    IDirect3DDevice9_BeginScene(dev);
    *(uint32_t *)ppDevice = 0x0D3D0D3D;   // the game only ever calls the global D3DDevice_* functions
    return 0;
}

static uint32_t __stdcall D3D_SetPushBufferSize(uint32_t PushBufferSize, uint32_t KickOffSize) { return 0; }

// The emulated vertical blank (NTSC, 60000/1001 Hz). The game advances one fixed step per frame and waits here once
// per frame (FUN_000e3b30), so this sets the game's speed. Blanks sit on a fixed grid, so a frame that runs a little
// late doesn't push every later one back; more than a frame behind, the grid restarts instead of rushing to catch
// up. The wait is a high-resolution timer to within half a millisecond, then a spin (Sleep(1) on the default
// Windows timer overslept enough to run the game at ~53 fps).
static LONGLONG vb_period, vb_half_ms, vb_freq;
static HANDLE vb_timer;

static void sleep_until(LONGLONG when)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    LONGLONG left = when - now.QuadPart;
    if (left > vb_half_ms && vb_timer) {
        LARGE_INTEGER due = {.QuadPart = -(left - vb_half_ms) * 10000000 / vb_freq};   // relative, 100 ns units
        if (SetWaitableTimer(vb_timer, &due, 0, 0, 0, FALSE)) WaitForSingleObject(vb_timer, INFINITE);
    }
    do QueryPerformanceCounter(&now); while (now.QuadPart < when);
}

static void wait_vblank_body(void);
static void wait_vblank(void)
{
    LONGLONG t0 = qpc();
    wait_vblank_body();
    st_wait += qpc() - t0;
}
static void wait_vblank_body(void)
{
    static LONGLONG next;
    LARGE_INTEGER now;
    if (!vb_period) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        vb_freq = f.QuadPart;
        vb_period = vb_freq * 1001 / 60000;
        vb_half_ms = vb_freq / 2000;
        vb_timer = CreateWaitableTimerExW(0, 0, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }
    // While the weapon wheel is open the game runs at a third of its speed: each step gets three refreshes, filled
    // with in-betweens (recorded for the purpose even with frame interpolation off), so the picture keeps its rate.
    // (It used to stall each step instead, which dropped the picture to 20 fps and felt like lag.)
    int slow = wheel.n ? WHEEL_SLOW : 1;
    LONGLONG slot = vb_period * slow;
    QueryPerformanceCounter(&now);
    next += slot;
    if (now.QuadPart - next > slot) next = now.QuadPart;   // first call, or more than a slot behind
    if (held) {   // frame interpolation: in-betweens spread evenly through the slot, the real frame at its end
        // An in-between that wouldn't be done by the next one's turn (or the real frame's) is skipped: replaying costs
        // a few microseconds a draw, and the hundreds of souls left after a fight made three of them overrun the
        // slot, which slowed the game itself. `cost` is the recent replay's length.
        static LONGLONG cost;
        int n = (settings.interp > 1 ? settings.interp : 1) * slow;
        for (int k = 1; k < n; k++) {
            LONGLONG due = next - slot * (n - k) / n, after = next - slot * (n - k - 1) / n;
            if (qpc() + cost > after) { inb_skipped++; cost -= cost / 8; continue; }   // eased, so a lighter scene tries again
            sleep_until(due);
            LONGLONG ti = qpc();
            int ok = show_inbetween((float)k / n);
            LONGLONG d = qpc() - ti;
            st_inb += d;
            if (!ok) break;   // not interpolable this frame: just the real one
            cost = cost ? (cost * 3 + d) / 4 : d;
        }
        sleep_until(next);
        show_held();
        return;
    }
    sleep_until(next);
}

// Stalls: a game step over 45 ms from one Present to the next (movies run at 33) is logged, with frame number;
// ONI_HITCH=1 logs from 25 ms. With the pack texture loads (load_replacement) it shows what a stutter was. Each is split
// into where its time went: our Present (the previous frame's, which the gap includes), our drawing, texture uploads,
// in-betweens, the frame-pacing wait, the game thread's file I/O, and the rest (the game's own code, or anything not
// timed); "cpu" is the game thread's CPU time in the gap: far under the gap's length, the thread was waiting or held
// off the CPU (another program, a lock), not working.
// A slow second: 60 steps in a row, none of them a stall or a movie's (they run at 30), that took over 1050 ms (under 57 fps) are logged
// the same way, as the average step, with the draws per frame: the souls left after a fight slowed the game by a few
// ms a frame, which no single-step limit catches.
static void stall_check(LONGLONG now)
{
    static int lim = -1;
    static LONGLONG last;
    static ULONGLONG cpu_last;
    if (lim < 0) lim = env_int("ONI_HITCH", 0) == 1 ? 25 : 45;
    stall_thread = GetCurrentThreadId();
    FILETIME c0, e0, k0, u0;
    ULONGLONG cpu = GetThreadTimes(GetCurrentThread(), &c0, &e0, &k0, &u0)
        ? ((ULONGLONG)k0.dwHighDateTime << 32 | k0.dwLowDateTime) + ((ULONGLONG)u0.dwHighDateTime << 32 | u0.dwLowDateTime) : 0;
    static double sec[8];   // the slow-second window: ms, present, draw, in-betweens, wait, file I/O, cpu, draws
    static int sec_n, sec_max;
    if (last) {
        double k = qpc_ms(1), ms = (now - last) * k;
        double pr = st_present * k, dr = st_draw * k, tx = st_tex * k, ib = st_inb * k, io = stall_io_ticks * k;
        double wt = (st_wait - st_inb) * k, rest = ms - pr - dr - ib - wt - io;   // texture uploads happen inside draws
        double cms = cpu_last && cpu ? (cpu - cpu_last) / 10000.0 : -1.0;
        int stall = ms > lim;
        if (stall) {
            xlog("[hitch] frame %u: %.1f ms", frame_no, ms);
            stall_note("frame %u stall %.1f ms (mode %02x, room %s) - present %.0f, draw %.0f (textures %.0f of it), in-betweens %.0f, "
                       "wait %.0f, file I/O %.0f, rest %.0f; cpu %.0f", frame_no, ms, *(volatile uint8_t *)G_STATE, last_room,
                       pr, dr, tx, ib, wt, io, rest, cms);
        }
        if (stall || frame_no - movie_seen < 2) sec_n = 0;   // any screen: the Oni Spirits game is mode 0x0c
        else {
            if (!sec_n) memset(sec, 0, sizeof sec), sec_max = 0;
            double v[8] = { ms, pr, dr, ib, wt, io, cms, stats.drawn };
            for (int i = 0; i < 8; i++) sec[i] += v[i];
            if ((int)stats.drawn > sec_max) sec_max = stats.drawn;
            if (++sec_n == 60) {
                if (sec[0] > 1050) {
                    double a = 1.0 / 60, r = (sec[0] - sec[1] - sec[2] - sec[3] - sec[4] - sec[5]) * a;
                    stall_note("frame %u slow second %.0f ms (%.0f fps, room %s) - per step: present %.1f, draw %.1f, in-betweens %.1f, "
                               "wait %.1f, file I/O %.1f, rest %.1f; cpu %.1f; draws %.0f (most %d)", frame_no, sec[0], 60000 / sec[0],
                               last_room, sec[1] * a, sec[2] * a, sec[3] * a, sec[4] * a, sec[5] * a, r, sec[6] * a, sec[7] * a, sec_max);
                }
                sec_n = 0;
            }
        }
    }
    last = now; cpu_last = cpu;
    st_draw = st_tex = st_inb = st_wait = 0; stall_io_ticks = 0;
}
static uint32_t __stdcall present_body(uint32_t a, uint32_t b, uint32_t c, uint32_t d);
static uint32_t __stdcall D3DDevice_Present(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    if (!dev) return 0;
    LONGLONG t0 = qpc();
    stall_check(t0);
    uint32_t r = present_body(a, b, c, d);
    st_present = qpc() - t0;
    return r;
}
static uint32_t __stdcall present_body(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    if (!dev) return 0;
    start_prefetch();
    {   // debug: ONI_WATCH=546ae0:32 logs that Xbox memory range (hex address : bytes) whenever it changes
        static uint8_t *watch, last[256];
        static uint32_t wlen = ~0u;
        if (wlen == ~0u) {
            char v[64] = "";
            uint32_t a = 0;
            wlen = 0;
            if (debug_env("ONI_WATCH", v, sizeof v) && sscanf(v, "%x:%u", &a, &wlen) == 2 && wlen <= 256) watch = (uint8_t *)a;
            else wlen = 0;
        }
        if (wlen && memcmp(last, watch, wlen)) {
            char hex[800];
            int o = 0;
            for (uint32_t i = 0; i < wlen; i++) o += snprintf(hex + o, sizeof hex - o, "%s%02x", i % 4 ? "" : " ", watch[i]);
            xlog("[watch] f%u %p:%s", frame_no, watch, hex);
            memcpy(last, watch, wlen);
        }
    }
    if (frame_no % 120 == 10 || frame_no == 1) {   // debug: ONI_WATCHWRITE=546aea:2 - log every write to that address (re-armed for new threads)
        char v[32] = "";
        uint32_t a, l;
        if (debug_env("ONI_WATCHWRITE", v, sizeof v) && sscanf(v, "%x:%u", &a, &l) == 2) kernel_watch_writes(a, l);
    }
    game_frame();
    fps_steps++;
    if (!recording && !overlays_late(wide)) draw_wheel(scene_rt), draw_subtitle(scene_rt), draw_popup(scene_rt);   // with frame interpolation: as the frame is shown (show_held)
    // A frame that draws no HUD at all, as at a camera cut, keeps the last one's HUD layer for up to two frames: in
    // 4:3 the HUD stays from the frame before as well, and showing none blinked it out.
    static int hud_gap;
    hud_gap = hud_drawn ? 0 : hud_gap + 1;
    frame_hud = hud_drawn || (hud_gap <= 2 && hud_tex);
    wide_still_now();
    pres_wide = wide; pres_hud = frame_hud;
    wide_pan();
    maybe_screenshot();
    if (recording) hold_frame(); else present_frame();
    if (cmp_state == 2) { compare_shot(); cmp_state = 0; if (fast_mode) show_held(); }   // scripted runs never wait for the held frame
#ifdef ONI_DEBUG   // F10, the compare shot: development builds only
    {
        static int was_down;
        static uint32_t cmp_at = ~0u;   // debug: ONI_COMPARE=frame takes the F10 compare shot from a script
        if (cmp_at == ~0u) cmp_at = (uint32_t)env_int("ONI_COMPARE", 0);
        int down = ((GetAsyncKeyState(VK_F10) & 0x8000) && GetForegroundWindow() == hwnd) || (cmp_at && frame_no + 1 == cmp_at);
        if (down && !was_down && !cmp_state) cmp_state = 1;
        was_down = down;
    }
#endif
    wide_next_frame();
    if (cmp_state == 1) wide = 0;   // the compare frame is drawn 4:3, HUD in it
    check_device();
    // Record frames for in-betweens with frame interpolation on, and while the wheel is open (its slow motion, see
    // wait_vblank). Switched here, between frames; a fresh start needs two recorded frames before it blends.
    // The F10 compare frame is recorded too.
    int rec = (!fast_mode && (settings.interp > 1 || wheel.n)) || cmp_state == 1;
    if (cmp_state == 1) cmp_state = 2;
    if (rec && !recording) {
        rec_done = rec_prev = -1;
        struct frame_rec *n = &frames[rec_cur];
        n->n = 0; n->used = 0; n->broken = 0; n->depth_hash = 0;
        sb_used = 0;
    }
    recording = rec;
    if (fast_mode && wheel.n) Sleep(33);   // scripted runs have no vblank waits to slow down
    if (frame_no % 300 == 0 || frame_no < 4) {
        int live = 0, tomb = 0;   // the texture cache's slots in use and forgotten (it ends the game when full)
        for (int i = 0; i < HOST_SLOTS; i++) live += hosts[i].key && hosts[i].key != HOST_TOMB, tomb += hosts[i].key == HOST_TOMB;
        xlog("[d3d] frame %u: %u draws, %u skipped (vertex shader); FVFs %x %x %x %x %x %x; texture slots %d used, %d forgotten of %d",
             frame_no, stats.drawn, stats.skipped_vs, stats.fvf[0], stats.fvf[1], stats.fvf[2], stats.fvf[3], stats.fvf[4], stats.fvf[5],
             live, tomb, HOST_SLOTS);
        if (recording && frame_no % 300 == 0) {   // totals over the last 300 frames
            xlog("[d3d] in-betweens: %u shown, %u skipped; ms per frame: capture %.2f replay %.2f present %.2f", inb_shown, inb_skipped,
                 qpc_ms(prof[0]) / 300, qpc_ms(prof[1]) / 300, qpc_ms(prof[2]) / 300);
            inb_shown = inb_skipped = 0;
            memset(prof, 0, sizeof prof);
        }
    }
    memset(&stats, 0, sizeof stats);
    if (frame_no % 600 == 0) host_sweep();
    static int f12_was_down;
#ifndef ONI_DEBUG   // a release: F12 saves the picture as shown (captures\screenshots, PNG), nothing else
    int f12 = (GetAsyncKeyState(VK_F12) & 0x8000) && GetForegroundWindow() == hwnd;
    if (f12 && !f12_was_down) player_shot = 1;
    f12_was_down = f12;
#else   // development builds: F12 logs and saves the next two frames' draws, Shift+F12 the next frame's textures
    if (capture) capture--;
    int shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;   // Shift+F12: the texture save below, not this
    int f12 = (GetAsyncKeyState(VK_F12) & 0x8000) && GetForegroundWindow() == hwnd && !shift;
    static uint32_t cap_at = ~0u;   // debug: ONI_CAPTURE=frame triggers the F12 capture from a script
    if (cap_at == ~0u) cap_at = (uint32_t)env_int("ONI_CAPTURE", 0);
    if (cap_at && frame_no + 1 == cap_at) f12 = 1;
    if (f12 && !f12_was_down) { capture = 2; xlog("[cap] F12: capturing frames %u-%u, room %s", frame_no + 1, frame_no + 2, last_room); }
    f12_was_down = f12;
    if (tex_capture) tex_capture--;
    static int tex_key_was_down;   // Shift+F12 (F11 is full screen)
    int tex_key = (GetAsyncKeyState(VK_F12) & 0x8000) && GetForegroundWindow() == hwnd && shift;
    static uint32_t tex_at = ~0u;   // debug: ONI_TEXCAPTURE=frame does the Shift+F12 save from a script
    if (tex_at == ~0u) tex_at = (uint32_t)env_int("ONI_TEXCAPTURE", 0);
    if (tex_at && frame_no + 1 == tex_at) tex_key = 1;
    if (tex_key && !tex_key_was_down) { tex_capture = 1; tex_capture_frame = frame_no + 1; xlog("[cap] Shift+F12: saving frame %u's textures, room %s", frame_no + 1, last_room); }
    tex_key_was_down = tex_key;
#endif
    frame_no++;
    return 0;
}
static void __stdcall D3DDevice_BlockUntilVerticalBlank(void) { if (!fast_mode) wait_vblank(); }

static uint32_t __stdcall D3DDevice_Clear(uint32_t Count, uint32_t pRects, uint32_t Flags, uint32_t Color, uint32_t Z, uint32_t Stencil)
{
    if (!dev) return 0;
    DWORD f = (Flags & 0xF0 ? D3DCLEAR_TARGET : 0) | (Flags & 1 ? D3DCLEAR_ZBUFFER : 0) | (Flags & 2 ? D3DCLEAR_STENCIL : 0);
    float z;
    memcpy(&z, &Z, 4);
    D3DRECT rects[16];
    const D3DRECT *in = (const D3DRECT *)pRects;
    if (in && Count > 16) Count = 16;
    for (uint32_t i = 0; in && i < Count; i++)   // rects are in Xbox pixels
        rects[i] = (D3DRECT){in[i].x1 * rt_scale, in[i].y1 * rt_scale, in[i].x2 * rt_scale, in[i].y2 * rt_scale};
    if (capture) xlog("[cap] f%u Clear n %u flags %x colour %08x z %g stencil %x", frame_no, Count, Flags, Color, z, Stencil);
    dev_clear(in ? Count : 0, in ? rects : 0, f, Color, z, Stencil);
    return 0;
}

static void addref(XRes *r) { if (r) r->Common = (r->Common & ~XCOMMON_REFCOUNT) | ((r->Common + 1) & XCOMMON_REFCOUNT); }

static uint32_t __stdcall D3DDevice_GetBackBuffer(uint32_t BackBuffer, uint32_t Type, uint32_t ppBackBuffer)
{ addref((XRes *)&backbuffer_hdr); *(XSurf **)ppBackBuffer = &backbuffer_hdr; return 0; }
static uint32_t __stdcall D3DDevice_GetRenderTarget(uint32_t pp) { addref((XRes *)cur_rt); *(XSurf **)pp = cur_rt; return 0; }
static uint32_t __stdcall D3DDevice_GetDepthStencilSurface(uint32_t pp) { addref((XRes *)cur_z); *(XSurf **)pp = cur_z; return 0; }
static uint32_t __stdcall D3DResource_Release(uint32_t pThis);
static uint32_t __stdcall D3DDevice_SetRenderTarget(uint32_t pRenderTarget, uint32_t pNewZStencil)
{
    XSurf *rt = (XSurf *)pRenderTarget, *old_rt = cur_rt, *old_z = cur_z;
    if (capture) xlog("[cap] f%u SetRenderTarget %p z %p", frame_no, rt, (void *)pNewZStencil);
    if (rt && rt != &backbuffer_hdr) xfeature(1, "render target: drawing into a texture surface (format %02x, untested path)", XFMT_CODE(rt->Format));
    // The device holds a reference on its targets, so the game can Release one while it is bound.
    addref((XRes *)rt);
    addref((XRes *)pNewZStencil);
    if (rt) cur_rt = rt;
    cur_z = (XSurf *)pNewZStencil;
    if (rt && old_rt) D3DResource_Release((uint32_t)old_rt);
    if (old_z) D3DResource_Release((uint32_t)old_z);
    // ponytail: rt 0 changes only cur_z here; D3D9 keeps the old depth buffer (what the Xbox does with it is unconfirmed)
    if (!dev || !rt) return 0;
    // A texture surface becomes a D3D9 render-target texture (the same GPU-backed one CopyRects fills, created at
    // `scale` times its Xbox size); anything else draws to the scene target.
    IDirect3DSurface9 *s = 0;
    IDirect3DTexture9 *t = rt != &backbuffer_hdr && rt->Parent ? gpu_texture_for((const XPix *)rt->Parent) : 0;
    if (t) IDirect3DTexture9_GetSurfaceLevel(t, 0, &s);
    else {
        if (rt != &backbuffer_hdr) {
            static int warned;
            if (warned++ < 4) xlog("[d3d] TODO render target surface %p without a texture (fmt %x) - drawing to the scene", rt, rt->Format);
        }
        s = scene_rt;
        IDirect3DSurface9_AddRef(s);
    }
    IDirect3DDevice9_SetRenderTarget(dev, 0, s);
    // ponytail: texture targets share one non-multisampled depth buffer the size of the scene
    IDirect3DDevice9_SetDepthStencilSurface(dev, pNewZStencil ? (t ? tex_ds : scene_ds) : 0);
    rec_target(s, pNewZStencil ? (t ? tex_ds : scene_ds) : 0);
    rt_scale = scale;   // scene and GPU textures are both `scale` times their Xbox size
    D3DSURFACE_DESC d;   // the viewport resets to the whole new target, as in D3D8 (kept in Xbox pixels)
    IDirect3DSurface9_GetDesc(s, &d);
    viewport = (D3DVIEWPORT9){0, 0, d.Width / rt_scale, d.Height / rt_scale, 0.0f, 1.0f};
    set_viewport_constants();
    apply_viewport();
    IDirect3DSurface9_Release(s);
    return 0;
}

static uint32_t __stdcall D3DSurface_GetDesc(uint32_t pThis, uint32_t pDesc)
{
    XSurf *s = (XSurf *)pThis;
    struct xtex_info t = tex_info(s->Format, s->Size);
    uint32_t *d = (uint32_t *)pDesc;   // Format Type Usage Size MultiSampleType Width Height
    d[0] = XFMT_CODE(s->Format); d[1] = 1; d[2] = 0; d[3] = t.linear ? t.pitch * t.h : level_bytes(t.kind, t.bpp, t.w, t.h);
    d[4] = 0x11; d[5] = t.w; d[6] = t.h;
    return 0;
}

static uint32_t __stdcall D3DDevice_GetTile(uint32_t Index, uint32_t pTile) { memset((void *)pTile, 0, 24); return 0; }
static uint32_t __stdcall D3DDevice_SetTile(uint32_t Index, uint32_t pTile) { return 0; }
static uint32_t __stdcall stub_ok1(uint32_t a) { return 0; }

// ---- HLE: resources ----------------------------------------------------------------------------------
static uint32_t __stdcall D3DDevice_CreateVertexBuffer(uint32_t Length, uint32_t Usage, uint32_t FVF, uint32_t Pool, uint32_t pp)
{
    XRes *r = xheader(sizeof *r);
    r->Common = XCOMMON_VB | XCOMMON_D3DCREATED | 1;
    r->Data = (uint32_t)xalloc(Length);
    *(XRes **)pp = r;
    return 0;
}
static uint32_t __stdcall D3DDevice_CreateIndexBuffer(uint32_t Length, uint32_t Usage, uint32_t Format, uint32_t Pool, uint32_t pp)
{
    XRes *r = xheader(sizeof *r);
    r->Common = XCOMMON_IB | XCOMMON_D3DCREATED | 1;
    r->Data = (uint32_t)xalloc(Length);
    *(XRes **)pp = r;
    return 0;
}
static uint32_t __stdcall D3DVertexBuffer_Lock(uint32_t pVB, uint32_t Offset, uint32_t Size, uint32_t ppbData, uint32_t Flags)
{ *(uint32_t *)ppbData = ((XRes *)pVB)->Data + Offset; return 0; }

static uint32_t __stdcall D3DDevice_CreatePalette(uint32_t Size, uint32_t pp)
{
    static const uint32_t entries[] = {256, 128, 64, 32};
    XRes *r = xheader(sizeof *r);
    r->Common = XCOMMON_PALETTE | XCOMMON_D3DCREATED | 1 | (Size & 3) << 30;
    r->Data = (uint32_t)xalloc(entries[Size & 3] * 4);
    *(XRes **)pp = r;
    return 0;
}
static uint32_t __stdcall D3DPalette_Lock(uint32_t pThis, uint32_t ppColors, uint32_t Flags) { *(uint32_t *)ppColors = ((XRes *)pThis)->Data; return 0; }

static uint32_t make_format(uint32_t code, uint32_t w, uint32_t h, uint32_t levels, int linear)
{
    if (linear) return 1 | 2 << 4 | code << 8 | 1 << 16;
    return 1 | 2 << 4 | code << 8 | levels << 16 | ilog2(w) << 20 | ilog2(h) << 24;
}

static uint32_t __stdcall D3DDevice_CreateTexture(uint32_t Width, uint32_t Height, uint32_t Levels, uint32_t Usage, uint32_t Format, uint32_t Pool, uint32_t pp)
{
    int fi = fmt_index(Format);
    if (fi < 0) { xlog("[d3d] CreateTexture: unknown format %x", Format); return 0x8876086Cu; }
    int linear = formats[fi].linear;
    uint32_t w = Width, h = Height;
    if (!linear) { w = 1u << ilog2(Width); h = 1u << ilog2(Height); }
    uint32_t maxlev = 1 + (ilog2(w) > ilog2(h) ? ilog2(w) : ilog2(h));
    if (linear) Levels = 1; else if (!Levels || Levels > maxlev) Levels = maxlev;
    XPix *t = xheader(sizeof *t);
    t->Common = XCOMMON_TEXTURE | XCOMMON_D3DCREATED | 1;
    t->Format = make_format(Format, w, h, Levels, linear);
    uint32_t bytes = 0;
    if (linear) {
        uint32_t pitch = (w * formats[fi].bpp + 63) & ~63u;
        t->Size = (w - 1) | (h - 1) << 12 | (pitch / 64 - 1) << 24;
        bytes = pitch * h;
    } else {
        int bpp = is_dxt(formats[fi].kind) ? 0 : formats[fi].bpp;
        for (uint32_t lv = 0, lw = w, lh = h; lv < Levels; lv++, lw = lw > 1 ? lw / 2 : 1, lh = lh > 1 ? lh / 2 : 1)
            bytes += level_bytes(formats[fi].kind, bpp, lw, lh);
    }
    t->Data = (uint32_t)xalloc(bytes);
    *(XPix **)pp = t;
    return 0;
}

// Byte offset and geometry of mip level `lv`.
static uint32_t level_offset(const XPix *x, uint32_t lv, uint32_t *w, uint32_t *h, uint32_t *pitch)
{
    struct xtex_info t = tex_info(x->Format, x->Size);
    uint32_t off = 0, lw = t.w, lh = t.h;
    for (uint32_t i = 0; i < lv; i++) {
        off += level_bytes(t.kind, t.bpp, lw, lh);
        if (lw > 1) lw >>= 1;
        if (lh > 1) lh >>= 1;
    }
    *w = lw; *h = lh;
    *pitch = t.linear ? t.pitch : is_dxt(t.kind) ? ((lw + 3) / 4) * dxt_block(t.kind) : lw * t.bpp;
    return off;
}

// Bytes from an image's start to rect r's top-left corner, rows pitch bytes apart (DXT: rows of 4x4 blocks).
// ponytail: swizzled images are addressed as if linear.
static uint32_t rect_offset(const struct xtex_info *t, const RECT *r, uint32_t pitch)
{
    if (is_dxt(t->kind)) return r->top / 4 * pitch + r->left / 4 * dxt_block(t->kind);
    return r->top * pitch + r->left * t->bpp;
}

static void mark_dirty(const XPix *x)
{
    struct tex_entry *h = host_for(x);
    h->dirty = h->dynamic = 1;
    h = host_for((void *)(x->Data | 1));   // the same texture as seen through SwitchTexture
    h->dirty = h->dynamic = 1;
}

static uint32_t __stdcall D3DTexture_LockRect(uint32_t pThis, uint32_t Level, uint32_t pLockedRect, uint32_t pRect, uint32_t Flags)
{
    XPix *x = (XPix *)pThis;
    uint32_t w, h, pitch, off = level_offset(x, Level, &w, &h, &pitch);
    uint32_t *lr = (uint32_t *)pLockedRect;   // INT Pitch; void *pBits
    lr[0] = pitch;
    lr[1] = x->Data + off;
    if (pRect) { struct xtex_info t = tex_info(x->Format, x->Size); lr[1] += rect_offset(&t, (const RECT *)pRect, pitch); }
    mark_dirty(x);
    return 0;
}

static uint32_t __stdcall D3DTexture_GetSurfaceLevel(uint32_t pThis, uint32_t Level, uint32_t pp)
{
    XPix *x = (XPix *)pThis;
    uint32_t w, h, pitch, off = level_offset(x, Level, &w, &h, &pitch);
    XSurf *s = xheader(sizeof *s);   // freed, and the texture released, by D3DResource_Release
    s->Common = XCOMMON_SURFACE | XCOMMON_D3DCREATED | 1;
    s->Data = x->Data + off;
    s->Format = x->Size ? x->Format : make_format(XFMT_CODE(x->Format), w, h, 1, 0);
    s->Size = x->Size;
    s->Parent = pThis;
    addref((XRes *)x);
    *(XSurf **)pp = s;
    return 0;
}

static uint32_t __stdcall D3DSurface_LockRect(uint32_t pThis, uint32_t pLockedRect, uint32_t pRect, uint32_t Flags)
{
    XSurf *s = (XSurf *)pThis;
    struct xtex_info t = tex_info(s->Format, s->Size);
    uint32_t *lr = (uint32_t *)pLockedRect;
    lr[0] = t.pitch;
    lr[1] = s->Data;
    if (pRect) lr[1] += rect_offset(&t, (const RECT *)pRect, t.pitch);
    mark_dirty(s->Parent ? (const XPix *)s->Parent : (const XPix *)s);
    return 0;
}

// Xbox D3DResource_Register: the header came from an XPR file; Data is an offset from pBase.
static void __stdcall D3DResource_Register(uint32_t pThis, uint32_t pBase)
{
    XRes *r = (XRes *)pThis;
    r->Data += pBase;
}

static uint32_t __stdcall D3DResource_AddRef(uint32_t pThis) { addref((XRes *)pThis); return ((XRes *)pThis)->Common & XCOMMON_REFCOUNT; }
static uint32_t __stdcall D3DResource_Release(uint32_t pThis)
{
    XRes *r = (XRes *)pThis;
    uint32_t n = r->Common & XCOMMON_REFCOUNT;
    if (n) n--;
    r->Common = (r->Common & ~XCOMMON_REFCOUNT) | n;
    if (!n && (r->Common & XCOMMON_D3DCREATED) && r != (XRes *)&backbuffer_hdr && r != (XRes *)&depth_hdr) {
        uint32_t type = r->Common & XCOMMON_TYPE_MASK, parent = type == XCOMMON_SURFACE ? ((XSurf *)r)->Parent : 0;
        if (type == XCOMMON_TEXTURE) { host_forget(r); host_forget((void *)(r->Data | 1)); }
        // The game releases resources that are still bound; on Xbox the device holds its own reference.
        for (int st = 0; st < 4; st++) {
            if (stage_tex[st] == (XPix *)r || (stage_tex[st] == &switch_tex[st] && switch_tex[st].Data == r->Data)) stage_tex[st] = 0;
            if (stage_pal[st] == r) stage_pal[st] = 0;
        }
        for (int s = 0; s < 16; s++) if (streams[s].vb == r) streams[s].vb = 0;
        if (index_buffer == r) index_buffer = 0;
        if (type != XCOMMON_SURFACE) VirtualFree((void *)r->Data, 0, MEM_RELEASE);   // surfaces point into their parent
        free(r);
        if (parent) D3DResource_Release(parent);   // the reference GetSurfaceLevel took
    }
    return n;
}
static uint32_t __stdcall D3DResource_IsBusy(uint32_t pThis) { return 0; }
static uint32_t __stdcall D3DResource_GetType(uint32_t pThis)
{
    XRes *r = (XRes *)pThis;
    switch (r->Common & XCOMMON_TYPE_MASK) {
    case XCOMMON_VB: return 6; case XCOMMON_IB: return 7; case XCOMMON_PALETTE: return 9; case XCOMMON_SURFACE: return 1;
    case XCOMMON_TEXTURE: { uint32_t f = ((XPix *)r)->Format; return XFMT_CUBEMAP(f) ? 5 : XFMT_DIMS(f) == 3 ? 4 : 3; }
    }
    return 0;
}

static uint32_t __stdcall D3D_AllocContiguousMemory(uint32_t dwSize, uint32_t dwAllocAttributes) { return (uint32_t)xalloc(dwSize); }

// ---- HLE: state setters ------------------------------------------------------------------------------
#define RS_SETTER(fn, idx) static void __stdcall fn(uint32_t Value) { RS[idx] = Value; }
RS_SETTER(D3DDevice_SetRenderState_PSTextureModes, XRS_PSTEXTUREMODES)
RS_SETTER(D3DDevice_SetRenderState_VertexBlend, XRS_VERTEXBLEND)
RS_SETTER(D3DDevice_SetRenderState_FogColor, XRS_FOGCOLOR)
RS_SETTER(D3DDevice_SetRenderState_FillMode, XRS_FILLMODE)
RS_SETTER(D3DDevice_SetRenderState_BackFillMode, XRS_BACKFILLMODE)
RS_SETTER(D3DDevice_SetRenderState_TwoSidedLighting, XRS_TWOSIDEDLIGHTING)
RS_SETTER(D3DDevice_SetRenderState_NormalizeNormals, XRS_NORMALIZENORMALS)
RS_SETTER(D3DDevice_SetRenderState_ZEnable, XRS_ZENABLE)
RS_SETTER(D3DDevice_SetRenderState_StencilEnable, XRS_STENCILENABLE)
RS_SETTER(D3DDevice_SetRenderState_StencilFail, XRS_STENCILFAIL)
RS_SETTER(D3DDevice_SetRenderState_FrontFace, XRS_FRONTFACE)
RS_SETTER(D3DDevice_SetRenderState_CullMode, XRS_CULLMODE)
RS_SETTER(D3DDevice_SetRenderState_TextureFactor, XRS_TEXTUREFACTOR)
RS_SETTER(D3DDevice_SetRenderState_ZBias, XRS_ZBIAS)
RS_SETTER(D3DDevice_SetRenderState_LogicOp, XRS_LOGICOP)
RS_SETTER(D3DDevice_SetRenderState_EdgeAntiAlias, XRS_EDGEANTIALIAS)
RS_SETTER(D3DDevice_SetRenderState_MultiSampleAntiAlias, XRS_MULTISAMPLEANTIALIAS)
RS_SETTER(D3DDevice_SetRenderState_MultiSampleMask, XRS_MULTISAMPLEMASK)
RS_SETTER(D3DDevice_SetRenderState_MultiSampleType, XRS_MULTISAMPLETYPE)
RS_SETTER(D3DDevice_SetRenderState_ShadowFunc, XRS_SHADOWFUNC)
RS_SETTER(D3DDevice_SetRenderState_LineWidth, XRS_LINEWIDTH)
RS_SETTER(D3DDevice_SetRenderState_Dxt1NoiseEnable, XRS_DXT1NOISEENABLE)
RS_SETTER(D3DDevice_SetRenderState_YuvEnable, XRS_YUVENABLE)
RS_SETTER(D3DDevice_SetRenderState_OcclusionCullEnable, XRS_OCCLUSIONCULLENABLE)
RS_SETTER(D3DDevice_SetRenderState_StencilCullEnable, XRS_STENCILCULLENABLE)
RS_SETTER(D3DDevice_SetRenderState_RopZCmpAlwaysRead, XRS_ROPZCMPALWAYSREAD)
RS_SETTER(D3DDevice_SetRenderState_RopZRead, XRS_ROPZREAD)
RS_SETTER(D3DDevice_SetRenderState_DoNotCullUncompressed, XRS_DONOTCULLUNCOMPRESSED)

// Simple states: game code writes D3D__RenderState[] inline and passes the GPU method here; nothing to do.
static void __fastcall D3DDevice_SetRenderState_Simple(uint32_t Method, uint32_t Value) {}
static void __fastcall D3DDevice_SetRenderState_Deferred(uint32_t State, uint32_t Value) { if (State < XRS_COUNT) RS[State] = Value; }
static void __stdcall D3DDevice_SetRenderStateNotInline(uint32_t State, uint32_t Value) { if (State < XRS_COUNT) RS[State] = Value; }

static void __fastcall D3DDevice_SetTextureState_Deferred(uint32_t Stage, uint32_t Type, uint32_t Value) { if (Stage < 4 && Type < 32) TS(Stage, Type) = Value; }
static void __stdcall D3DDevice_SetTextureStageStateNotInline(uint32_t Stage, uint32_t Type, uint32_t Value) { if (Stage < 4 && Type < 32) TS(Stage, Type) = Value; }
static void __stdcall D3DDevice_SetTextureState_TexCoordIndex(uint32_t Stage, uint32_t Value) { TS(Stage & 3, XTSS_TEXCOORDINDEX) = Value; }
static void __stdcall D3DDevice_SetTextureState_BorderColor(uint32_t Stage, uint32_t Value) { TS(Stage & 3, XTSS_BORDERCOLOR) = Value; }
static void __stdcall D3DDevice_SetTextureState_ColorKeyColor(uint32_t Stage, uint32_t Value) { TS(Stage & 3, XTSS_COLORKEYCOLOR) = Value; }
static void __stdcall D3DDevice_SetTextureState_BumpEnv(uint32_t Stage, uint32_t Type, uint32_t Value) { if (Type < 32) TS(Stage & 3, Type) = Value; }

static uint32_t __stdcall D3DDevice_SetTexture(uint32_t Stage, uint32_t pTexture) { if (Stage < 4) stage_tex[Stage] = (const XPix *)pTexture; return 0; }
static uint32_t __stdcall D3DDevice_SetPalette(uint32_t Stage, uint32_t pPalette) { if (Stage < 4) stage_pal[Stage] = (XRes *)pPalette; return 0; }

// SwitchTexture(Method, Data, Format): fast path that repoints a stage's texture. Method is the NV2A
// SET_TEXTURE_OFFSET method for the stage (0x1B00 + 0x40 * stage, low 16 bits).
static void __fastcall D3DDevice_SwitchTexture(uint32_t Method, uint32_t Data, uint32_t Format)
{
    // The SDK's inline SetTexture only takes this path for swizzled/compressed textures, so Size is 0.
    uint32_t stage = (((Method & 0xFFFF) - 0x1B00) / 0x40) & 3;
    XPix *t = &switch_tex[stage];
    t->Common = XCOMMON_TEXTURE | 1; t->Data = Data; t->Format = Format; t->Size = 0;
    stage_tex[stage] = t;
}

static uint32_t __stdcall D3DDevice_SetStreamSource(uint32_t n, uint32_t pStreamData, uint32_t Stride)
{
    if (n < 16) { streams[n].vb = (XRes *)pStreamData; streams[n].stride = Stride; }
    return 0;
}
static uint32_t __stdcall D3DDevice_SetIndices(uint32_t pIndexData, uint32_t BaseVertexIndex)
{
    index_buffer = (XRes *)pIndexData;
    base_vertex_index = BaseVertexIndex;
    *(uint32_t *)XADDR_INDEXBASE = index_buffer ? index_buffer->Data : 0;
    return 0;
}

static uint32_t __stdcall D3DDevice_SetTransform(uint32_t State, uint32_t pMatrix)
{
    static const D3DTRANSFORMSTATETYPE map[] = {D3DTS_VIEW, D3DTS_PROJECTION, D3DTS_TEXTURE0, D3DTS_TEXTURE1, D3DTS_TEXTURE2,
                                               D3DTS_TEXTURE3, D3DTS_WORLD, D3DTS_WORLD1, D3DTS_WORLD2, D3DTS_WORLD3};
    if (dev && State < 10) IDirect3DDevice9_SetTransform(dev, map[State], (const D3DMATRIX *)pMatrix);
    return 0;
}
// Xbox D3D keeps the viewport transform in reserved vertex shader constants c-38 (scale) and c-37 (offset);
// vertex programs use them to produce screen-space positions. Z is in depth-buffer units (24-bit).
static void set_viewport_constants(void)
{
    if (shader_constant_mode & 0x10) return;   // D3DSCM_NORESERVEDCONSTANTS
    const D3DVIEWPORT9 *v = &viewport;
    float zs = 16777215.0f;
    float scale[4] = {v->Width * 0.5f, v->Height * -0.5f, (v->MaxZ - v->MinZ) * zs, 1.0f};
    float offset[4] = {v->X + v->Width * 0.5f, v->Y + v->Height * 0.5f, v->MinZ * zs, 0.0f};
    memcpy(vs_constants[58], scale, 16);
    memcpy(vs_constants[59], offset, 16);
}

void apply_viewport(void)   // `viewport` is in Xbox pixels; the render target is rt_scale times larger
{
    D3DVIEWPORT9 v = viewport;
    v.X *= rt_scale; v.Y *= rt_scale; v.Width *= rt_scale; v.Height *= rt_scale;
    if (dev) IDirect3DDevice9_SetViewport(dev, &v);
}

static uint32_t __stdcall D3DDevice_SetViewport(uint32_t pViewport)
{
    viewport = *(const D3DVIEWPORT9 *)pViewport;
    set_viewport_constants();
    apply_viewport();
    return 0;
}

// ---- HLE: shaders ------------------------------------------------------------------------------------
static uint32_t *copy_tokens(const uint32_t *src, uint32_t *len, int decl)
{
    if (!src) { *len = 0; return 0; }
    uint32_t n = 0;
    if (decl) while (src[n] != 0xFFFFFFFF && n < 4096) n++;          // D3DVSD_END
    else n = 1 + (src[0] >> 16) * 4;                                  // header word + NumInst * 4
    *len = n;
    uint32_t *d = malloc((n + 1) * 4);
    memcpy(d, src, (n + 1) * 4);
    return d;
}
static uint32_t __stdcall D3DDevice_CreateVertexShader(uint32_t pDeclaration, uint32_t pFunction, uint32_t pHandle, uint32_t Usage)
{
    struct vshader *vs = calloc(1, sizeof *vs);
    vs->decl = copy_tokens((const uint32_t *)pDeclaration, &vs->decl_len, 1);
    vs->function = copy_tokens((const uint32_t *)pFunction, &vs->function_len, 0);
    vs->usage = Usage;
    parse_declaration(vs);
    *(uint32_t *)pHandle = (uint32_t)vs | 1;
    return 0;
}
static uint32_t __stdcall D3DDevice_SetVertexShader(uint32_t Handle) { vertex_shader = Handle; return 0; }
static uint32_t __stdcall D3DDevice_SetShaderConstantMode(uint32_t Mode) { shader_constant_mode = Mode; set_viewport_constants(); return 0; }
static uint32_t __stdcall D3DDevice_SetVertexShaderConstant(uint32_t Register, uint32_t pConstantData, uint32_t ConstantCount)
{
    int r = (int32_t)Register + 96;
    for (uint32_t i = 0; i < ConstantCount; i++, r++)
        if (r >= 0 && r < 192) memcpy(vs_constants[r], (const float *)pConstantData + i * 4, 16);
    return 0;
}
static uint32_t __stdcall D3DDevice_CreatePixelShader(uint32_t pPSDef, uint32_t pHandle)
{
    struct pshader *ps = calloc(1, sizeof *ps);
    ps->ref = ps->one = 1;
    ps->def_ptr = (uint32_t)ps->def;
    memcpy(ps->def, (const void *)pPSDef, sizeof ps->def);
    *(uint32_t *)pHandle = (uint32_t)ps;
    return 0;
}
static uint32_t __stdcall D3DDevice_SetPixelShader(uint32_t Handle)
{
    pixel_shader = Handle;
    if (Handle) {   // as the SDK does: the combiner program lives in the render-state array while bound
        const uint32_t *def = ((struct pshader *)Handle)->def;
        memcpy(RS, def, 57 * 4);
        RS[XRS_PSTEXTUREMODES] = def[54];
    }
    return 0;
}

static float *vs_verts;
static uint32_t vs_verts_cap;

// Draw with a programmable vertex shader. Xbox streams are merged on the CPU into one buffer of float4
// attributes (D3D9 UP draws take a single stream), covering vertices [0, last] after base/start offsets.
static void draw_vs(uint32_t xprim, uint32_t start, uint32_t count, const uint16_t *idx, uint32_t nidx, const uint8_t *up, uint32_t up_stride)
{
    struct vshader *vs = (struct vshader *)(vertex_shader & ~1u);
    D3DPRIMITIVETYPE pt;
    uint32_t n = idx ? nidx : count;
    int prims = prim_count(xprim, n, &pt);
    if (prims <= 0 || !dev || !compile_vshader(vs)) { stats.skipped_vs++; return; }
    if (!ready_to_draw()) return;
    int32_t first = idx ? (int32_t)base_vertex_index : (int32_t)start;   // vertex 0 of the draw in stream terms
    if (xprim == 8) {
        if (!(nidx = quad_list(idx, n))) return;
        idx = quad_indices;
    }
    uint32_t lo = 0, hi = count ? count - 1 : 0;
    if (idx) { lo = 0xFFFF; hi = 0; for (uint32_t i = 0; i < nidx; i++) { if (idx[i] < lo) lo = idx[i]; if (idx[i] > hi) hi = idx[i]; } }

    int ninputs = 0;
    uint8_t in_reg[16];
    for (int i = 0; i < 16; i++) if (vs->inputs & (1u << i)) in_reg[ninputs++] = (uint8_t)i;
    int nslots = ninputs ? ninputs : 1;
    uint32_t need = (hi + 1) * nslots * 4;
    if (vs_verts_cap < need) {
        free(vs_verts);
        vs_verts_cap = (vs_verts = malloc(need * 2 * 4)) ? need * 2 : 0;
        if (!vs_verts) { stats.skipped_vs++; return; }   // out of memory: this draw skipped, not a crash
    }
    for (int k = 0; k < ninputs; k++) {
        const struct vattr *a = 0;
        for (int j = 0; j < vs->nattrs; j++) if (vs->attrs[j].reg == in_reg[k]) a = &vs->attrs[j];
        const uint8_t *base = 0;
        uint32_t stride = 0;
        if (a) {
            if (up && a->stream == 0) { base = up; stride = up_stride; }
            else if (streams[a->stream].vb) { base = (const uint8_t *)streams[a->stream].vb->Data; stride = streams[a->stream].stride; base += first * (int32_t)stride; }
        }
        for (uint32_t v = lo; v <= hi; v++) {
            float *o = vs_verts + (v * nslots + k) * 4;
            if (base) vsdt_fetch(a->type, base + v * stride + a->offset, o);
            else { o[0] = o[1] = o[2] = 0; o[3] = 1; }
        }
    }
    if (capture) {
        char k[24];
        snprintf(k, sizeof k, "vs %p", (void *)vs);
        capture_draw(k, xprim, idx ? nidx : count, 0, 0);
    }
    if (tex_capture) capture_textures();
    if (capture) {
        for (int j = 0; j < vs->nattrs; j++)
            xlog("[cap]    v%u <- stream %u +%u type %02x (stride %u)", vs->attrs[j].reg, vs->attrs[j].stream, vs->attrs[j].offset, vs->attrs[j].type,
                 up ? up_stride : streams[vs->attrs[j].stream].stride);
        if (ninputs) xlog("[cap]    vert0 v%u = %.3f %.3f %.3f %.3f", in_reg[0], vs_verts[lo * nslots * 4], vs_verts[lo * nslots * 4 + 1],
                             vs_verts[lo * nslots * 4 + 2], vs_verts[lo * nslots * 4 + 3]);
    }
    IDirect3DDevice9_SetVertexShader(dev, vs->hs);
    IDirect3DDevice9_SetVertexDeclaration(dev, vs->hdecl);
    IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, &vs_constants[0][0], 192);
    // c[192..195]: per texture coordinate set scale for linear textures (see linear_texcoord_scales; vsh.c applies it)
    float sx[4] = {0}, sy[4] = {0}, tc[4][4];
    if (linear_texcoord_scales(sx, sy) && vs->warned++ < 1)
        xlog("[d3d] vertex shader %p samples a linear texture: texel coords rescaled (%g %g | %g %g)", vs, sx[0], sy[0], sx[1], sy[1]);
    for (int k = 0; k < 4; k++) { tc[k][0] = sx[k] ? sx[k] : 1; tc[k][1] = sy[k] ? sy[k] : 1; tc[k][2] = tc[k][3] = 1; }
    IDirect3DDevice9_SetVertexShaderConstantF(dev, 192, &tc[0][0], 4);
    float half[4] = {0.5f / rt_scale, 0.5f / rt_scale, 0, 0};   // c[196]: half a render-target pixel, in Xbox pixels
    IDirect3DDevice9_SetVertexShaderConstantF(dev, 196, half, 1);
    // frame interpolation matches this draw in the next frame by shader, vertex buffer, texture and size
    uint32_t key[4] = {(uint32_t)vs, streams[0].vb ? (uint32_t)streams[0].vb : (uint32_t)(up != 0), (uint32_t)stage_tex[0], (uint32_t)prims};
    // MSAA: the characters' meshes have hairline gaps between some triangles (as made; at 640x480 they are far below a
    // pixel). Multisampled, a gap now and then crosses a sample along the whole edge, and thin dark lines ran across
    // faces where the head's inside showed through (seen at scale 5 on Kaede's cheek; gone with MSAA off for these draws
    // only, and with every 3D draw flat white; not the textures, the shadow volumes or the depth precision). An opaque
    // draw is made twice: first without multisampling, which fills every sample of a pixel whose centre it covers, gaps
    // included, then multisampled on top for the edges. Outlines lose a little of their smoothing on the inside half.
    // Debug: ONI_GAPFILL=0 turns it off.
    static int gapfill = -1;
    if (gapfill < 0) gapfill = env_int("ONI_GAPFILL", 1) != 0;
    if (gapfill && scene_ms != D3DMULTISAMPLE_NONE && cur_rt == &backbuffer_hdr && !RS[XRS_ALPHABLENDENABLE] && RS[XRS_ZENABLE]
        && RS[XRS_ZWRITEENABLE] && RS[XRS_ZFUNC] == 0x203 && RS[XRS_COLORWRITEENABLE] && !draw_is_volume) {
        IDirect3DDevice9_SetRenderState(dev, D3DRS_MULTISAMPLEANTIALIAS, FALSE);
        dev_draw(pt, lo, hi + 1 - lo, prims, idx, vs_verts, nslots * 16, &vs_constants[0][0], key);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_MULTISAMPLEANTIALIAS, TRUE);
    }
    dev_draw(pt, lo, hi + 1 - lo, prims, idx, vs_verts, nslots * 16, &vs_constants[0][0], key);
    stats.drawn++; stats.vs++;
}

// ---- HLE: draws --------------------------------------------------------------------------------------
static uint32_t __stdcall D3DDevice_DrawVertices(uint32_t PrimitiveType, uint32_t StartVertex, uint32_t VertexCount)
{
    if (vertex_shader & 1) { draw_vs(PrimitiveType, StartVertex, VertexCount, 0, 0, 0, 0); return 0; }
    if (!streams[0].vb) return 0;
    uint32_t stride = streams[0].stride ? streams[0].stride : fvf_stride(vertex_shader);
    draw(PrimitiveType, (const uint8_t *)streams[0].vb->Data + StartVertex * stride, VertexCount, stride, 0, 0, 0);
    return 0;
}
static uint32_t __stdcall D3DDevice_DrawIndexedVertices(uint32_t PrimitiveType, uint32_t VertexCount, uint32_t pIndexData)
{
    if (vertex_shader & 1) { draw_vs(PrimitiveType, 0, 0, (const uint16_t *)pIndexData, VertexCount, 0, 0); return 0; }
    if (!streams[0].vb) return 0;
    uint32_t stride = streams[0].stride ? streams[0].stride : fvf_stride(vertex_shader);
    draw(PrimitiveType, (const uint8_t *)streams[0].vb->Data, 0, stride, (const uint16_t *)pIndexData, VertexCount, base_vertex_index);
    return 0;
}
static uint32_t __stdcall D3DDevice_DrawVerticesUP(uint32_t PrimitiveType, uint32_t VertexCount, uint32_t pData, uint32_t Stride)
{
    if (vertex_shader & 1) { draw_vs(PrimitiveType, 0, VertexCount, 0, 0, (const uint8_t *)pData, Stride); return 0; }
    draw(PrimitiveType, (const uint8_t *)pData, VertexCount, Stride, 0, 0, 0);
    return 0;
}

// A render-target texture standing in for an Xbox texture that the GPU writes (CopyRects from the backbuffer).
static IDirect3DTexture9 *gpu_texture_for(const XPix *tex)
{
    struct tex_entry *h = host_for(tex);
    if (h->gpu && h->tex) return h->tex;
    struct xtex_info t = tex_info(tex->Format, tex->Size);
    if (h->tex) { IDirect3DTexture9_Release(h->tex); h->tex = 0; }
    // `scale` times the Xbox size, like the scene: copies of the backbuffer keep the extra resolution. The game
    // samples these with normalised coordinates (linear_texcoord_scales), so the size difference is invisible to it.
    if (FAILED(IDirect3DDevice9_CreateTexture(dev, t.w * scale, t.h * scale, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &h->tex, 0))) {
        xlog("[d3d] render-target texture %ux%u failed", t.w * scale, t.h * scale);
        return 0;
    }
    xlog("[d3d] texture %p is now GPU-backed (%ux%u)", tex, t.w, t.h);
    h->gpu = 1; h->data = tex->Data; h->format = tex->Format; h->size = tex->Size;
    return h->tex;
}

// The game keeps each room's pre-rendered depth as an image and CopyRects it into the depth buffer every frame,
// so 3D objects hide behind background scenery. D3D9 depth buffers can't be written from the CPU: upload the
// depth as a float texture and draw a full-screen pass that writes only depth.
static IDirect3DTexture9 *depth_tex;
static IDirect3DPixelShader9 *depth_ps;

// A pack's depth image: depth\<hash>.f32 in an enabled mod replaces the room depth image whose contents hash to it
// (FNV-1a 64 over its rows, as Shift+F12 names the raw copy it saves). Float depth (1 = far), the original's size or
// a whole number of times it. Made offline (tools/hdpack/depth_clean.py: the original without its thin near lines,
// which cut what is drawn in front into dotted lines).
static IDirect3DTexture9 *depth_mod_tex;
static uint64_t depth_mod_key, depth_mod_quick = 1;
static int depth_mod_have;
static uint64_t depth_hash(const XSurf *s, const struct xtex_info *t)
{
    uint64_t hv = 1469598103934665603ull;
    for (uint32_t y = 0; y < t->h; y++)
        for (uint32_t i = 0; i < t->pitch; i++) hv = (hv ^ ((const uint8_t *)s->Data)[y * t->pitch + i]) * 1099511628211ull;
    return hv;
}
static IDirect3DTexture9 *depth_mod(const XSurf *s, const struct xtex_info *t)
{
    uint64_t quick = 1469598103934665603ull;   // a few samples first: the whole image is hashed only when they change
    const uint8_t *data = (const uint8_t *)s->Data;
    for (uint32_t i = 0; i < 256; i++) quick = (quick ^ data[(uint64_t)i * 4801 % (t->pitch * t->h)]) * 1099511628211ull;
    if (quick == depth_mod_quick) return depth_mod_have ? depth_mod_tex : 0;
    depth_mod_quick = quick;
    uint64_t hv = depth_hash(s, t);
    if (hv == depth_mod_key && depth_mod_tex) return depth_mod_have ? depth_mod_tex : 0;
    depth_mod_key = hv; depth_mod_have = 0;
    wchar_t rel[64], path[MAX_PATH];
    swprintf(rel, 64, L"depth\\%016llx.f32", (unsigned long long)hv);
    if (!mod_file(rel, path, MAX_PATH)) return 0;
    FILE *f = _wfopen(path, L"rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t k = 1;
    while ((long)(t->w * (k + 1) * t->h * (k + 1) * 4) <= n) k++;
    uint32_t W = t->w * k, H = t->h * k;
    if ((long)(W * H * 4) != n) { fclose(f); xlog("[depth] %ls: not %ux%u (or a whole number of times it) floats", path, t->w, t->h); return 0; }
    D3DSURFACE_DESC dd = {0};
    if (depth_mod_tex) IDirect3DTexture9_GetLevelDesc(depth_mod_tex, 0, &dd);
    if (depth_mod_tex && (dd.Width != W || dd.Height != H)) { IDirect3DTexture9_Release(depth_mod_tex); depth_mod_tex = 0; }
    if (!depth_mod_tex && FAILED(IDirect3DDevice9_CreateTexture(dev, W, H, 1, D3DUSAGE_DYNAMIC, D3DFMT_R32F, D3DPOOL_DEFAULT, &depth_mod_tex, 0))) {
        depth_mod_tex = 0; fclose(f); return 0;
    }
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DTexture9_LockRect(depth_mod_tex, 0, &lr, 0, D3DLOCK_DISCARD))) { fclose(f); return 0; }
    for (uint32_t y = 0; y < H; y++) fread((uint8_t *)lr.pBits + y * lr.Pitch, 4, W, f);
    IDirect3DTexture9_UnlockRect(depth_mod_tex, 0);
    fclose(f);
    depth_mod_have = 1;
    xlog("[depth] the pack's depth image %016llx (%ux%u)", (unsigned long long)hv, W, H);
    return depth_mod_tex;
}

static void upload_depth(const XSurf *s)
{
    static int failed;
    static int off = -1;   // debug: ONI_NODEPTH=1 skips this pass
    if (off < 0) off = env_int("ONI_NODEPTH", 0) == 1;
    if (off) return;
    struct xtex_info t = tex_info(s->Format, s->Size);
    int code = XFMT_CODE(s->Format);
    int d16 = code == 0x2c || code == 0x30, d24 = code == 0x2a || code == 0x2e;
    if (failed || !t.ok || !t.linear || (!d16 && !d24) || !readable_bytes((void *)s->Data, t.pitch * t.h)) {
        static int warned;
        if (warned++ < 4) xlog("[d3d] TODO depth upload from format %02x size %ux%u%s", code, t.w, t.h, t.linear ? "" : " (swizzled)");
        xfeature(1, "copy: depth image format %02x%s not uploaded", code, t.linear ? "" : " swizzled");
        return;
    }
    static UINT depth_w, depth_h;
    if (depth_tex && (depth_w != t.w || depth_h != t.h)) { IDirect3DTexture9_Release(depth_tex); depth_tex = 0; }
    if (!depth_tex) {
        // The image is 640x480 and the scene `scale` times that. Taken texel by texel, a floor becomes flat steps one
        // texel high while the game's shadow volumes, which end right at the floor, keep sloping across each step: rows
        // of pixels alternate in and out of shadow and the shadow breaks into horizontal streaks that crawl as anything
        // moves. Where the four nearest texels lie on one plane (a - b - c + d = 0, to within a few steps of 24-bit
        // depth) the depth is blended between them, which rebuilds the plane exactly. At an object's edge it is blended
        // too: the nearest texel there cut characters along the 640x480 grid (square notches through a leg behind grass,
        // `scale` px each) under a background drawn smooth. Blended, the cut runs where the depth crosses the
        // character's, a smooth line within the texel. c0: width, height, 1/width, 1/height; c1.x: the tolerance
        // (huge: always blend; ONI_DEPTH_STEPS=1 brings back the nearest texel at edges, to compare).
        const char *src = "sampler2D s : register(s0);\n"
                          "float4 size : register(c0);\n"
                          "float4 tol : register(c1);\n"
                          "float4 main(float2 uv : TEXCOORD0, out float depth : DEPTH) : COLOR {\n"
                          "    float2 p = uv * size.xy - 0.5, f = frac(p), b = (p - f + 0.5) * size.zw;\n"
                          "    float a = tex2D(s, b).r, c = tex2D(s, b + float2(size.z, 0)).r, d = tex2D(s, b + float2(0, size.w)).r, e = tex2D(s, b + size.zw).r;\n"
                          "    depth = abs(a - c - d + e) < tol.x ? lerp(lerp(a, c, f.x), lerp(d, e, f.x), f.y) : tex2D(s, uv).r;\n"
                          "    return 0;\n"
                          "}\n";
        ID3DBlob *bin = 0, *err = 0;
        HRESULT hr = IDirect3DDevice9_CreateTexture(dev, t.w, t.h, 1, D3DUSAGE_DYNAMIC, D3DFMT_R32F, D3DPOOL_DEFAULT, &depth_tex, 0);
        if (FAILED(hr)) depth_tex = 0;
        else if (!depth_ps && SUCCEEDED(hr = D3DCompile(src, strlen(src), "depth", 0, 0, "main", "ps_2_0", 0, 0, &bin, &err))
                 && FAILED(hr = IDirect3DDevice9_CreatePixelShader(dev, (const DWORD *)ID3D10Blob_GetBufferPointer(bin), &depth_ps))) depth_ps = 0;
        if (FAILED(hr)) xlog("[d3d] depth upload unavailable (%08lx): %s", hr, err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
        if (bin) ID3D10Blob_Release(bin);
        if (err) ID3D10Blob_Release(err);
        if (FAILED(hr)) { failed = 1; return; }
        depth_w = t.w; depth_h = t.h;
    }
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DTexture9_LockRect(depth_tex, 0, &lr, 0, D3DLOCK_DISCARD))) return;
    for (uint32_t y = 0; y < t.h; y++) {
        const uint8_t *row = (const uint8_t *)s->Data + y * t.pitch;
        float *o = (float *)((uint8_t *)lr.pBits + y * lr.Pitch);
        for (uint32_t x = 0; x < t.w; x++)   // D24S8: depth in the top 24 bits; D16: whole word
            o[x] = d24 ? (((const uint32_t *)row)[x] >> 8) / 16777215.0f : ((const uint16_t *)row)[x] / 65535.0f;
    }
    if (capture) {   // debug: the depth image as grey levels, near = dark
        float lo = 1, hi = 0;   // stretch the range below "far" (1.0) so the picture shows something
        for (uint32_t y = 0; y < t.h; y++)
            for (uint32_t x = 0; x < t.w; x++) {
                float z = ((const float *)((const uint8_t *)lr.pBits + y * lr.Pitch))[x];
                if (z < lo) lo = z;
                if (z < 1.0f && z > hi) hi = z;
            }
        xlog("[cap] depth image range %.6f .. %.6f (excluding far)", lo, hi);
        uint32_t *px = malloc(t.w * t.h * 4);
        for (uint32_t y = 0; px && y < t.h; y++)
            for (uint32_t x = 0; x < t.w; x++) {
                float z = ((const float *)((const uint8_t *)lr.pBits + y * lr.Pitch))[x];
                uint8_t g = (uint8_t)(z >= 1.0f || hi <= lo ? 255 : (z - lo) / (hi - lo) * 254);
                px[y * t.w + x] = 0xFF000000u | g * 0x010101u;
            }
        if (px) {
            char file[32], name[MAX_PATH + 64];
            snprintf(file, sizeof file, "depth_%u.bmp", frame_no);
            capture_path(name, sizeof name, "depth", file);
            unique_path(name, sizeof name);
            D3DLOCKED_RECT img = {(INT)(t.w * 4), px};
            write_bmp(name, &img, t.w, t.h);
            free(px);
        }
    }
    if (recording) {   // frame interpolation: a different depth image means a camera cut
        uint32_t hv = 2166136261u;
        for (uint32_t i = 0; i < 64; i++) {
            const uint32_t *row = (const uint32_t *)((const uint8_t *)lr.pBits + (i * t.h / 64) * lr.Pitch);
            hv = (hv ^ row[(i * 37) % t.w]) * 16777619u;
        }
        frames[rec_cur].depth_hash = hv;
    }
    IDirect3DTexture9_UnlockRect(depth_tex, 0);
    if (tex_capture) {   // Shift+F12: the raw depth image too, named as a pack's depth file would be
        static uint64_t saved;
        uint64_t hv = depth_hash(s, &t);
        if (hv != saved) {
            saved = hv;
            char file[64], path[MAX_PATH + 160];
            snprintf(file, sizeof file, "depth_%016llx_%ux%u_%s.raw", (unsigned long long)hv, t.w, t.h, d24 ? "d24" : "d16");
            capture_path(path, sizeof path, tex_capture_dir(), file);
            FILE *f = ufopen(path, L"wb");
            if (f) { for (uint32_t y = 0; y < t.h; y++) fwrite((const uint8_t *)s->Data + y * t.pitch, 1, t.w * (d24 ? 4 : 2), f); fclose(f); }
        }
    }

    IDirect3DSurface9 *cur = 0;   // the pass covers the whole current render target (scaled)
    D3DSURFACE_DESC cd = {0};
    IDirect3DDevice9_GetRenderTarget(dev, 0, &cur);
    if (cur) { IDirect3DSurface9_GetDesc(cur, &cd); IDirect3DSurface9_Release(cur); }
    float w = cd.Width ? (float)cd.Width : (float)t.w, h = cd.Height ? (float)cd.Height : (float)t.h;
    struct { float x, y, z, rhw, u, v; } q[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {w - 0.5f, -0.5f, 0, 1, 1, 0},
                                                {-0.5f, h - 0.5f, 0, 1, 0, 1}, {w - 0.5f, h - 0.5f, 0, 1, 1, 1}};
    quad_state(1, D3DTEXF_POINT);
    IDirect3DDevice9_SetPixelShader(dev, depth_ps);
    static int steps = -1;
    if (steps < 0) steps = env_int("ONI_DEPTH_STEPS", 0) == 1;
    float tol = !steps ? 1e30f : d16 ? 3.0f / 65535 : 3.0f / 16777215 + 2.5e-7f;   // 3 steps, + float rounding
    IDirect3DTexture9 *dm = depth_mod(s, &t);
    D3DSURFACE_DESC dmd = {0};
    if (dm) IDirect3DTexture9_GetLevelDesc(dm, 0, &dmd);
    float tw = dm ? (float)dmd.Width : (float)t.w, th = dm ? (float)dmd.Height : (float)t.h;
    float pc[2][4] = {{tw, th, 1.0f / tw, 1.0f / th}, {tol}};
    IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, &pc[0][0], 2);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)(dm ? dm : depth_tex));
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZFUNC, D3DCMP_ALWAYS);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0);
    dev_draw(D3DPT_TRIANGLESTRIP, 0, 4, 2, 0, q, sizeof q[0], 0, 0);
    rs_valid = 0;   // everything above gets re-applied from the Xbox state on the next draw
}

// Everything in D3DPOOL_DEFAULT is released, the device reset, and the scene targets made again. The rest is
// rebuilt on use: GPU-backed textures by gpu_texture_for (refilled by the game's next CopyRects), upscaled images
// by upscaled(), the swap chain by present_frame; device state is re-applied from the Xbox state on the next draw.
void reset_device(void)
{
    IDirect3DSurface9 *bb = 0;   // nothing of ours may stay bound
    IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    IDirect3DDevice9_SetRenderTarget(dev, 0, bb);
    if (bb) IDirect3DSurface9_Release(bb);
    IDirect3DDevice9_SetDepthStencilSurface(dev, 0);
    for (int st = 0; st < 4; st++) IDirect3DDevice9_SetTexture(dev, st, 0);
#define DROP(p) do { if (p) { ULONG left_ = IUnknown_Release((IUnknown *)(p)); if (left_) xlog("[d3d] reset: %s still has %lu references", #p, left_); (p) = 0; } } while (0)
    interp_drop();   // first: it holds references to the scene targets
    DROP(swapchain); DROP(scene_rt); DROP(scene_ds); DROP(tex_ds); DROP(frame_tex); DROP(post_tex);
    DROP(depth_tex); DROP(depth_ps); DROP(depth_mod_tex); depth_mod_quick = 1; depth_mod_key = 0;   // the pack's is read again
    xbr_pool_drop();
    // An upscaled copy is made again on its next use (big_upload no longer matching); the font sheet's redrawn copy
    // (text) is MANAGED and stays.
    for (int i = 0; i < HOST_SLOTS; i++) {
        if (!hosts[i].text && hosts[i].big) { DROP(hosts[i].big); hosts[i].big_upload = hosts[i].uploads - 1; }
        if (hosts[i].gpu) DROP(hosts[i].tex);
    }
    if (plate.big) { DROP(plate.big); plate.big_upload = plate.uploads - 1; }
    DROP(dark_tiny);
#undef DROP
    // Lost again before the reset went through (screen still locked): wait. The game pauses meanwhile, which is
    // what it would look like anyway.
    HRESULT hr;
    while ((hr = IDirect3DDevice9_Reset(dev, &dev_pp)) == D3DERR_DEVICELOST) Sleep(100);
    xlog("[d3d] frame %u: device reset %s (%08lx)", frame_no, SUCCEEDED(hr) ? "done" : "failed", hr);
    if (FAILED(hr)) xfatal("[d3d] device reset failed (%08lx): something still holds a D3DPOOL_DEFAULT resource", hr);
    if (!create_targets()) xfatal("[d3d] can't create the render targets again after a device reset");
    D3DVIEWPORT9 v = viewport;   // binding the target resets the viewport, as in D3D8: keep the game's
    D3DDevice_SetRenderTarget((uint32_t)cur_rt, (uint32_t)cur_z);
    viewport = v;
    set_viewport_constants();
    apply_viewport();
    rs_valid = 0;
}

// CopyRects(pSourceSurface, pSourceRectsArray, cRects, pDestinationSurface, pDestPointsArray)
static uint32_t __stdcall D3DDevice_CopyRects(uint32_t src, uint32_t rects, uint32_t n, uint32_t dst, uint32_t points)
{
    {
        const XSurf *dd = (const XSurf *)dst;
        int fi = fmt_index(XFMT_CODE(dd->Format));
        if (dd == cur_z || (fi >= 0 && formats[fi].kind == F_DEPTH)) {
            if (capture) xlog("[cap] f%u CopyRects to depth: src fmt %08x size %08x", frame_no, ((const XSurf *)src)->Format, ((const XSurf *)src)->Size);
            upload_depth((const XSurf *)src);
            stats.depth++;
            return 0;
        }
    }
    XSurf *s = (XSurf *)src, *d = (XSurf *)dst;
    const RECT *r = (const RECT *)rects;
    if (capture) xlog("[cap] f%u CopyRects %p -> %p (parent %p) n %u rect0 %ld,%ld-%ld,%ld", frame_no, s, d, (void *)d->Parent, n,
                      r && n ? r->left : 0, r && n ? r->top : 0, r && n ? r->right : 0, r && n ? r->bottom : 0);
    const POINT *pt = (const POINT *)points;
    if (s == &backbuffer_hdr && d->Parent) {
        IDirect3DTexture9 *t = gpu_texture_for((const XPix *)d->Parent);
        IDirect3DSurface9 *ds = 0;
        if (!t) return 0;
        IDirect3DTexture9_GetSurfaceLevel(t, 0, &ds);
        // ponytail: no rects stretches the whole scene onto the whole texture; CopyRects may mean 1:1 at the top-left
        // when the sizes differ (unconfirmed what the game relies on)
        if (!r || !n) dev_copy(scene_rt, 0, ds, 0);
        for (uint32_t i = 0; r && i < n; i++) {   // rects in Xbox pixels; both surfaces are `scale` times larger
            RECT sr = {r[i].left * scale, r[i].top * scale, r[i].right * scale, r[i].bottom * scale}, dr = sr;
            if (pt) OffsetRect(&dr, (pt[i].x - r[i].left) * scale, (pt[i].y - r[i].top) * scale);
            dev_copy(scene_rt, &sr, ds, &dr);
        }
        IDirect3DSurface9_Release(ds);
        return 0;
    }
    // Memory to memory, same format (both linear as far as seen).
    struct xtex_info a = tex_info(s->Format, s->Size), b = tex_info(d->Format, d->Size);
    if (!a.ok || !b.ok || !a.linear || !b.linear || a.bpp != b.bpp) {
        static int warned;
        if (warned++ < 4) xlog("[d3d] TODO CopyRects %p (fmt %x size %x) -> %p (fmt %x size %x)", s, s->Format, s->Size, d, d->Format, d->Size);
        xfeature(1, "copy: CopyRects format %02x -> %02x not handled", XFMT_CODE(s->Format), XFMT_CODE(d->Format));
        return 0;
    }
    RECT full = {0, 0, (LONG)a.w, (LONG)a.h};
    for (uint32_t i = 0; i < (r && n ? n : 1); i++) {
        RECT sr = r && n ? r[i] : full;
        LONG dx = pt ? pt[i].x : sr.left, dy = pt ? pt[i].y : sr.top;
        // clipped to both surfaces
        if (sr.left < 0) { dx -= sr.left; sr.left = 0; }
        if (sr.top < 0) { dy -= sr.top; sr.top = 0; }
        if (dx < 0) { sr.left -= dx; dx = 0; }
        if (dy < 0) { sr.top -= dy; dy = 0; }
        sr.right = min(sr.right, min((LONG)a.w, sr.left + (LONG)b.w - dx));
        sr.bottom = min(sr.bottom, min((LONG)a.h, sr.top + (LONG)b.h - dy));
        if (sr.right <= sr.left) continue;
        for (LONG y = sr.top; y < sr.bottom; y++)
            memcpy((uint8_t *)d->Data + (dy + y - sr.top) * b.pitch + dx * b.bpp, (const uint8_t *)s->Data + y * a.pitch + sr.left * a.bpp,
                   (sr.right - sr.left) * a.bpp);
    }
    mark_dirty(d->Parent ? (const XPix *)d->Parent : (const XPix *)d);
    return 0;
}

const struct hle_impl d3d_impls[] = {
#define F(name) {#name, name},
    F(Direct3D_CreateDevice) F(D3D_SetPushBufferSize) F(D3DDevice_Present) F(D3DDevice_BlockUntilVerticalBlank)
    F(D3DDevice_Clear) F(D3DDevice_GetBackBuffer) F(D3DDevice_GetRenderTarget) F(D3DDevice_GetDepthStencilSurface)
    F(D3DDevice_SetRenderTarget) F(D3DSurface_GetDesc) F(D3DDevice_GetTile) F(D3DDevice_SetTile)
    F(D3DDevice_CreateVertexBuffer) F(D3DDevice_CreateIndexBuffer) F(D3DVertexBuffer_Lock) F(D3DDevice_CreatePalette)
    F(D3DPalette_Lock) F(D3DDevice_CreateTexture) F(D3DTexture_LockRect) F(D3DTexture_GetSurfaceLevel)
    F(D3DSurface_LockRect) F(D3DResource_Register) F(D3DResource_AddRef) F(D3DResource_Release) F(D3DResource_IsBusy)
    F(D3DResource_GetType) F(D3D_AllocContiguousMemory)
    F(D3DDevice_SetRenderState_PSTextureModes) F(D3DDevice_SetRenderState_VertexBlend) F(D3DDevice_SetRenderState_FogColor)
    F(D3DDevice_SetRenderState_FillMode) F(D3DDevice_SetRenderState_BackFillMode) F(D3DDevice_SetRenderState_TwoSidedLighting)
    F(D3DDevice_SetRenderState_NormalizeNormals) F(D3DDevice_SetRenderState_ZEnable) F(D3DDevice_SetRenderState_StencilEnable)
    F(D3DDevice_SetRenderState_StencilFail) F(D3DDevice_SetRenderState_FrontFace) F(D3DDevice_SetRenderState_CullMode)
    F(D3DDevice_SetRenderState_TextureFactor) F(D3DDevice_SetRenderState_ZBias) F(D3DDevice_SetRenderState_LogicOp)
    F(D3DDevice_SetRenderState_EdgeAntiAlias) F(D3DDevice_SetRenderState_MultiSampleAntiAlias)
    F(D3DDevice_SetRenderState_MultiSampleMask) F(D3DDevice_SetRenderState_MultiSampleType) F(D3DDevice_SetRenderState_ShadowFunc)
    F(D3DDevice_SetRenderState_LineWidth) F(D3DDevice_SetRenderState_Dxt1NoiseEnable) F(D3DDevice_SetRenderState_YuvEnable)
    F(D3DDevice_SetRenderState_OcclusionCullEnable) F(D3DDevice_SetRenderState_StencilCullEnable)
    F(D3DDevice_SetRenderState_RopZCmpAlwaysRead) F(D3DDevice_SetRenderState_RopZRead)
    F(D3DDevice_SetRenderState_DoNotCullUncompressed) F(D3DDevice_SetRenderState_Simple) F(D3DDevice_SetRenderState_Deferred)
    F(D3DDevice_SetRenderStateNotInline) F(D3DDevice_SetTextureState_Deferred) F(D3DDevice_SetTextureStageStateNotInline)
    F(D3DDevice_SetTextureState_TexCoordIndex) F(D3DDevice_SetTextureState_BorderColor) F(D3DDevice_SetTextureState_ColorKeyColor)
    F(D3DDevice_SetTextureState_BumpEnv) F(D3DDevice_SetTexture) F(D3DDevice_SetPalette) F(D3DDevice_SwitchTexture)
    F(D3DDevice_SetStreamSource) F(D3DDevice_SetIndices) F(D3DDevice_SetTransform) F(D3DDevice_SetViewport)
    F(D3DDevice_CreateVertexShader) F(D3DDevice_SetVertexShader) F(D3DDevice_SetShaderConstantMode)
    F(D3DDevice_SetVertexShaderConstant) F(D3DDevice_CreatePixelShader) F(D3DDevice_SetPixelShader)
    F(D3DDevice_DrawVertices) F(D3DDevice_DrawIndexedVertices) F(D3DDevice_DrawVerticesUP) F(D3DDevice_CopyRects)
#undef F
    {"D3DDevice_SetSoftDisplayFilter", stub_ok1}, {"D3DDevice_SetFlickerFilter", stub_ok1},
    {0, 0},
};

uint32_t d3d_frame_number(void) { return frame_no; }
