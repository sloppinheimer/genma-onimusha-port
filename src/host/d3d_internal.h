// What the d3d files share. d3d.c is the D3D8 API itself (device, state translation, resources, shaders, draws,
// Present); d3d_texture.c the texture cache, formats and uploads; d3d_pack.c the HD pack's textures; d3d_shaders.c
// the game's shaders; d3d_interp.c frame interpolation (and the F10 compare shot, a replay of one frame); d3d_modern.c picture = modern; d3d_post.c the full-screen
// passes and presenting the frame; d3d_capture.c the debug captures; d3d_overlay.c the wheel, popups, FPS and
// subtitles. Everything else stays static in its file.
#pragma once
#include <stdint.h>
#include <windows.h>
#include <d3d9.h>
#include "xd3d.h"

// An Xbox texture format as d3d.c handles it (tex_info): which kind, size, mip levels, pitch.
enum { F_L8, F_AL8, F_1555, F_X555, F_4444, F_565, F_8888, F_X888, F_P8, F_A8, F_A8L8, F_ABGR, F_BGRA, F_RGBA,
       F_DXT1, F_DXT3, F_DXT5, F_YUY2, F_5551, F_4444R, F_DEPTH, F_UNKNOWN };
struct xtex_info { uint32_t w, h, levels, pitch; int kind, bpp, linear, ok; };

// The texture cache's entry (d3d.c hosts[]); the overlays keep one of their own for the plate art.
struct tex_entry {
    const void *key;
    IDirect3DTexture9 *tex;
    uint32_t data, format, size;    // what the cached texture was built from
    uint32_t dirty;                 // set by Lock calls; game may write without telling us after that
    uint32_t dynamic;               // ever locked: re-upload once per frame while in use
    uint32_t uploaded_frame;
    uint32_t gpu;                   // render-target texture filled on the GPU (CopyRects): never uploaded from Xbox memory
    uint64_t hash;                  // contents (+ palette) when last checked
    uint32_t hash_frame;
    IDirect3DTexture9 *big;         // background_filter = enhanced: tex upscaled to the render scale
    uint32_t uploads, big_upload;   // upload count; the one the upscaled copy was made from (several uploads can happen in a frame)
    uint32_t first_frame;           // first frame it was drawn with
    uint32_t used_rec;              // frame interpolation: last recorded frame it was bound in
    uint32_t text_upload;           // enhanced_text: the upload `text` was decided for (0: not yet)
    int text;                       // 1: this is the font sheet and big is its redrawn copy
    int replaced;                   // tex is an HD texture pack's replacement (texpack.c), made from contents pack_key
    int soft_alpha;                 // that replacement has alpha above 0x80: smooth-edged, its alpha is coverage (see set_texture)
    uint64_t pack_key;
    int screen2d;                   // a sheet only a full-screen 2D screen of play uses (picture = modern: screen2d_names)
    const uint32_t *pal;            // the palette it was last uploaded with (the F10 compare shot uploads the game's own again)
    int pack_wait;                  // its pack texture is being made off the game thread (load_replacement returned -1)
    LONG pack_wait_gen;             // pack_ready when it last tried: it tries again once that moves
};

// d3d.c
extern IDirect3DDevice9 *dev;
extern int scale;
extern uint32_t frame_no;
extern int rs_valid;
uint32_t ilog2(uint32_t v);
void swizzle_masks(uint32_t w, uint32_t h, uint32_t *mx, uint32_t *my);
uint32_t pdep(uint32_t v, uint32_t mask);
void write_bmp(const char *name, const D3DLOCKED_RECT *lr, int w, int h);
// captures\dumps\<file> (UTF-8, made on first use): where the ONI_DUMP_* files and failed shaders go, not the current
// directory (bin\, the install folder, or wherever the game was started from: beside the files an update replaces).
void dump_path(char *out, size_t cap, const char *file);
IDirect3DPixelShader9 *compile_ps(const char *name, const char *src);
// vsh.c, psh.c: an Xbox vertex shader (*inputs: a bit per v register it reads) or register-combiner pixel shader as
// malloc'd HLSL source
char *vsh_to_hlsl(const uint32_t *fn, uint32_t len, uint32_t *inputs);
char *psh_to_hlsl(const uint32_t *def);
// d3d_shaders.c: the game's vertex shaders (their declaration and program) and register-combiner pixel shaders
// as D3D9 ones. A vertex shader handle is (struct vshader *) | 1; a pixel shader handle is a struct pshader.
struct vattr { uint8_t stream, reg, type; uint16_t offset; };
struct vshader {
    uint32_t *decl, decl_len, *function, function_len, usage, warned;
    struct vattr attrs[16]; int nattrs;         // from the Xbox vertex declaration
    uint32_t inputs;                            // v registers the program reads
    int failed;
    IDirect3DVertexShader9 *hs;
    IDirect3DVertexDeclaration9 *hdecl;
};
// Same layout as the SDK's handle: {refcount, 1, &def, X_D3DPIXELSHADERDEF (60 dwords)}, plus our host shader.
struct pshader { uint32_t ref, one, def_ptr, def[60]; IDirect3DPixelShader9 *hs; int failed; };
extern uint32_t pixel_shader;   // the bound struct pshader *, or 0
void vsdt_fetch(uint32_t t, const uint8_t *p, float *o);   // an Xbox vertex attribute of type t -> float4
void parse_declaration(struct vshader *vs);
int compile_vshader(struct vshader *vs);   // 1: vs->hs is ready (compiled once, on first use)
void apply_pixel_shader(void);
static inline uint32_t swap_rb(uint32_t c) { return (c & 0xFF00FF00u) | (c & 0xFF) << 16 | (c >> 16 & 0xFF); }   // ARGB <-> ABGR
FILE *ufopen(const char *path, const wchar_t *mode);   // fopen of a UTF-8 path
void quad_state(int stages, DWORD filter);
void apply_viewport(void);
uint64_t content_hash(const XPix *x, const uint32_t *pal);
// The HD pack's texture names, made the same way wherever a texture is named (tex_name for uploads; the read-ahead of
// backgrounds and animated sheets, d3d_pack.c; the wheel's item pictures, d3d_overlay.c), so a change reaches them all:
// WxH_<format>_<texel hash>[_<palette hash>], with _$ in place of the palette hash matching every palette.
uint64_t pack_texel_hash(const void *texels, uint32_t bytes);   // FNV-1a 64 over its 64-bit words (a partial last one left out)
uint32_t pack_pal_hash(const uint32_t *pal);                    // FNV-1a 32 over 256 ARGB entries
// name (and wild, if given: the _$ form, or the same name when not paletted) from a texture's size, format and hashes.
void pack_name(char *name, char *wild, size_t n, uint32_t w, uint32_t h, uint32_t code, uint64_t texels, int paletted, uint32_t pal);
#define HUD_SHEET_HASH 0x080376b10b292b4dull   // the game's HUD sheet (8-bit 256x256): its texels' pack hash
uint32_t readable_bytes(const void *p, uint32_t want);
IDirect3DTexture9 *upscaled(struct tex_entry *h, int hud);
extern int bb_w, bb_h;
extern int pres_wide;

// d3d_overlay.c: drawn at Present; note_hud_sheet is told of the HUD sheet when the game uploads it
extern unsigned fps_steps;
extern struct tex_entry plate;
void note_hud_sheet(const XPix *x, const XRes *pal);
void draw_wheel(IDirect3DSurface9 *target);
void draw_popup(IDirect3DSurface9 *target);
void draw_fps(IDirect3DSurface9 *target);
void draw_subtitle(IDirect3DSurface9 *target);

// d3d_pack.c: the HD pack's version of a texture into h->tex (1: replaced); the room the game opened last; the
// stall and per-room texture lists kept next to the exe
extern char last_room[16];
int load_replacement(struct tex_entry *h, const struct xtex_info *t, const char *name, const char *wild);   // -1: being made
extern volatile LONG pack_ready;   // +1 each time a texture made off the game thread is kept
void room_note(const char *name);
void stall_note(const char *fmt, ...);

// d3d.c and d3d_post.c (the full-screen passes, resolving and presenting the frame)
static inline int is_dxt(int kind) { return kind == F_DXT1 || kind == F_DXT3 || kind == F_DXT5; }
static inline uint32_t dxt_block(int kind) { return kind == F_DXT1 ? 8 : 16; }   // bytes in one 4x4 block
// The 4:3 art's rectangle in the Xbox frame (640x480): picture = modern crops from it
#define ART_X0 21.3333f
#define ART_X1 618.6667f
#define ART_Y0 16.0f
#define ART_Y1 464.0f
extern HWND hwnd;
extern IDirect3DSurface9 *scene_rt;
extern IDirect3DTexture9 *frame_tex, *post_tex, *hud_tex;
extern IDirect3DSwapChain9 *swapchain;
extern UINT swap_w, swap_h;
extern int fullpic_on, screen2d_on, pres_hud, passes_ran, fast_mode, recording;
extern float screen2d_band[2], pan_y;
struct xtex_info tex_info(uint32_t format, uint32_t size);
uint64_t tex_name(const XPix *x, const uint32_t *pal, const struct xtex_info *t, char *out, char *wild, size_t n);
void dxt_decode(int kind, const uint8_t *src, int pitch, uint32_t w, uint32_t h, uint32_t *out);
void reset_device(void);
// d3d_post.c
void check_device(void);
void dump_xbr(struct tex_entry *h, const XPix *x, const uint32_t *pal, IDirect3DTexture9 *t);
IDirect3DTexture9 *font_sheet_texture(struct tex_entry *h, const XPix *x, const uint32_t *pal);
int text_k(void);
void dump_image(struct tex_entry *h);
IDirect3DSurface9 *resolve_frame(void);
int menu_fill(UINT cw, UINT ch);
int overlays_late(int wide);
void compose_fill(IDirect3DSurface9 *frame, IDirect3DSurface9 *dst, UINT w, UINT h);
void compose_wide(IDirect3DSurface9 *frame, IDirect3DSurface9 *dst, UINT w, UINT h);
void present_surface(IDirect3DSurface9 *frame);
void present_frame(void);
void xbr_pool_drop(void);

// d3d.c and d3d_capture.c (debug captures)
// The SDK's render-state and texture-stage-state arrays (the game writes them inline; read at draw time)
static uint32_t *const RS = (uint32_t *)XADDR_RENDERSTATE;
static uint32_t *const TSS = (uint32_t *)XADDR_TEXTURESTATE;
#define TS(stage, type) TSS[(stage) * XTSS_STAGE_SIZE + (type)]
// world: fixed-function 3D draws; depth: room depth uploads
struct draw_stats { uint32_t drawn, skipped_vs, prims, vs, world, depth; uint32_t fvf[8]; };
extern struct draw_stats stats;
extern XSurf *cur_rt;
void save_frame(const char *name);
#ifdef ONI_DEV_TOOLS
void texdump_texture(IDirect3DTexture9 *tex, const struct xtex_info *t, const char *name);   // src/host/dev/
#endif
extern int player_shot;   // F12 in a release: save the picture at the next Present (maybe_screenshot)
// captures\<kind>\<file> in the install folder (UTF-8; the folders made on first use)
void capture_path(char *out, size_t cap, const char *kind, const char *file);
extern const XPix *bound_tex[4];
void *cache_key(const XPix *x, const uint32_t *pal);
extern int capture;
void capture_draw(const char *kind, uint32_t xprim, uint32_t n, const uint8_t *verts, uint32_t stride);
void capture_textures(void);
void decode_level(const struct xtex_info *t, const uint8_t *src, uint32_t w, uint32_t h, const uint32_t *pal, uint8_t *out, int pitch);
struct tex_entry *host_find(const void *key);
void maybe_screenshot(void);
extern uint32_t mid_shot;
extern XRes *stage_pal[4];
const uint32_t *stage_palette(int st);
extern int tex_capture;
const char *tex_capture_dir(void);
extern uint32_t tex_capture_frame;
uint32_t texture_bytes(const struct xtex_info *t);
int uexists(const char *path);
void unique_path(char *path, size_t cap);
int utf8_wide(const char *path, wchar_t *w, int n);
extern uint32_t vertex_shader;
extern float wide_aspect;

// d3d.c and d3d_modern.c (picture = modern)
extern XSurf *cur_z;
extern IDirect3DSurface9 *dark_tiny;
void dev_clear(DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil);
void dev_draw(D3DPRIMITIVETYPE pt, UINT minv, UINT nverts, UINT prims, const uint16_t *idx, const void *verts, UINT stride, const float *consts, const uint32_t key[4]);
const uint8_t *fix_texcoords(const uint8_t *verts, uint32_t stride, uint32_t count);
extern uint32_t fullpic_frame;
uint32_t fvf_stride(uint32_t fvf);
int grow_buf(void **p, size_t *cap, size_t n);
extern int held;
extern int held_wide;
int hold_loading_text(const uint8_t *v, uint32_t stride);
int hud_begin(const uint8_t *verts, uint32_t stride, uint32_t n);
extern int hud_draw;
extern int hud_drawn;
void hud_end(void);
void hud_fade(D3DPRIMITIVETYPE pt, UINT minv, UINT nverts, UINT prims, const uint16_t *idx, const uint8_t *verts, UINT stride, UINT count);
extern float hud_map[4];
extern IDirect3DSurface9 *hud_surf;
int is_fade(const uint8_t *verts, uint32_t stride, uint32_t n);
void letterbox_check(const uint8_t *verts, uint32_t stride, const uint16_t *idx, uint32_t n, int32_t base);
extern uint32_t movie_seen;
const uint8_t *orb_layout(const uint8_t *verts, uint32_t n, uint32_t stride, IDirect3DTexture9 **tex);
void rec_target(IDirect3DSurface9 *rt, IDirect3DSurface9 *ds);
extern uint32_t screen2d_frame;
const uint8_t *still_text(const uint8_t *verts, uint32_t n, uint32_t stride);
uint32_t texel_argb(int kind, const uint8_t *p, const uint32_t *pal);
const uint8_t *title_trim(const uint8_t *verts, uint32_t n, uint32_t stride);
void wide_next_frame(void);
void wide_pan(void);
void wide_picture_check(const uint8_t *verts, uint32_t stride, const uint16_t *idx, uint32_t n, int32_t base);
void wide_still_now(void);
extern int wide;   // the frame being drawn is a modern one (decided at the end of each Present)
extern int held_hud, frame_hud;
extern uint32_t inb_shown, inb_skipped;
extern XSurf backbuffer_hdr;
extern uint32_t movie_w, movie_h;
extern IDirect3DSurface9 *scene_ds;

// d3d.c and d3d_interp.c (frame interpolation)
// A recorded frame: its ops (draws, clears, target changes, copies) and their data; broken: not replayable
struct op;
struct frame_rec { struct op *ops; int n, cap; uint8_t *data; size_t used, size; int broken; uint32_t depth_hash; };
#define WHEEL_SLOW 3   // the game runs at 1/3 speed while the weapon wheel is open
extern int rec_cur, rec_done, rec_prev;   // frames[]: being recorded, last finished, the one before (-1: none)
// Stall breakdown: where each frame's time went, in QPC ticks, summed between Presents (the stall line splits a stall
// into these): our Present, our drawing, texture uploads, in-between frames, the frame-pacing wait.
extern LONGLONG st_present, st_draw, st_tex, st_inb, st_wait;
void dev_copy(IDirect3DSurface9 *src, const RECT *sr, IDirect3DSurface9 *dst, const RECT *dr);
extern int draw_is_volume;
extern struct frame_rec frames[3];
void hold_frame(void);
void interp_drop(void);
extern LONGLONG prof[4];
extern uint32_t rec_serial;
extern int sb_used;
void show_held(void);
int show_inbetween(float t);

// d3d.c and d3d_texture.c (textures)
#define HOST_SLOTS 8192
#define HOST_TOMB ((const void *)~0u)   // a forgotten slot, never a real key: keeps probe chains intact; host_for reuses it
struct xfmt { uint8_t code, kind, bpp, linear; };   // an Xbox texture format: its code, F_* kind, bytes per texel
extern const struct xfmt formats[];
// picture = modern: full-screen 2D screens recognised by a picture they draw (y0, y1: the rows that are the screen)
struct screen2d_name { const char *name; float y0, y1; };
extern const struct screen2d_name screen2d_names[];
int fmt_index(uint32_t code);
struct tex_entry *host_for(const void *key);
void host_forget(const void *key);
void host_sweep(void);
extern struct tex_entry hosts[HOST_SLOTS];
uint32_t level_bytes(int kind, int bpp, uint32_t w, uint32_t h);
IDirect3DTexture9 *upload_texture(const XPix *x, const uint32_t *pal, struct tex_entry *h);
void *xalloc(uint32_t n);
void *xheader(uint32_t n);

// d3d_interp.c and d3d.c: the F10 compare shot (the frame as the HD pack draws it and as the original game does)
extern int cmp_state;   // 1: F10 pressed, the next frame is the compare frame; 2: it is being drawn
extern int cmp_plain;   // d3d_texture.c: uploading a texture as the game has it (no pack, no HD movie frame)
void compare_shot(void);
