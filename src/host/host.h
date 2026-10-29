#pragma once
#include <stdint.h>
#include <stddef.h>

// log.c
void log_init(const wchar_t *path);
void xlog(const char *fmt, ...);
__declspec(noreturn) void xfatal(const char *fmt, ...);
int env_int(const char *name, int dflt);   // debug switches: an ONI_* environment variable as a number, dflt when unset
// An ONI_* environment variable's text in buf (n bytes): its length, or 0 and "" when unset or longer than buf.
uint32_t env_str(const char *name, char *buf, uint32_t n);
long long qpc(void);                // QueryPerformanceCounter, in ticks
double qpc_ms(long long ticks);     // QPC ticks as milliseconds
// The switches that write game memory, saves, achievements or files (ONI_POKE, ONI_WARP*, ONI_ACH, ONI_WATCH*,
// ONI_DUMP_*, ONI_TEST_RESET): read only in development builds (ONI_DEBUG, which CMake sets when ONI_UPDATE_REPO is
// empty). A release build sees them as unset, so a stray variable can't edit a player's game.
uint32_t debug_env(const char *name, char *buf, uint32_t n);   // env_str, in development builds
int debug_env_int(const char *name, int dflt);                 // env_int, in development builds
void xbacktrace(const char *why);   // logs return addresses up the EBP chain

// The "seen once" sets (texpack.c, mapping.c, keyset_add's callers)
// FNV-1a 64 over a string, carrying on from h (start from FNV64_START): the keys of the "seen once" sets below.
#define FNV64_START 1469598103934665603ull
static inline uint64_t fnv64_str(uint64_t h, const char *s) { while (*s) h = (h ^ (uint8_t)*s++) * 1099511628211ull; return h; }
// The slot key has, or would take, in a set of n slots (n a power of two); *absent: it isn't in the set. Keys are
// stored | 1 (0 marks a free slot). The lookup keyset_add, texpack.c and mapping.c share.
static inline uint32_t keyset_slot(const uint64_t *t, uint32_t n, uint64_t key, int *absent)
{
    key |= 1;
    uint32_t i = (uint32_t)key & (n - 1);
    while (t[i] && t[i] != key) i = (i + 1) & (n - 1);
    if (absent) *absent = !t[i];
    return i;
}
// Adds key to a set of n slots (n a power of two): 1 if it was new. Full up to 1024 short of n, then it takes no more.
static inline int keyset_add(uint64_t *t, uint32_t n, uint32_t *count, uint64_t key)
{
    int absent;
    uint32_t i = keyset_slot(t, n, key, &absent);
    if (!absent || *count >= n - 1024) return 0;
    t[i] = key | 1; ++*count;
    return 1;
}

// host.c. read_file: a whole file, NUL-terminated (so text can be parsed as a string), malloc'd; 0 when missing, over
// max bytes or unreadable. size gets its length.
uint8_t *read_file(const wchar_t *path, uint32_t max, uint32_t *size);
// host_path: name in the folder of the exe (out holds MAX_PATH); game_file: a disc file as the game would get it, from
// the highest enabled mod that has it, else game\ (rel like L"image\\etc\\ITEMI.IDA")
void host_path(wchar_t *out, const wchar_t *name);
void game_file(wchar_t *out, const wchar_t *rel);
const wchar_t *host_dir(void);   // the install folder itself (layout.h's root), set first thing in host_main
// Every folder in mods\, on or off, in mods_list.h's order (mod_list): read once, at the first call (settings_load,
// after layout_migrate), and shared by settings.c, texpack.c, kernel.c and the patches. *n gets the count.
struct mod_item;
const struct mod_item *host_mods(int *n);
// The game logic the port adds, once a frame from Present (d3d.c), in one place and this order: achievements, then
// on their in-play checks the randomizer's start items or (outside a run) the map; the cheats' top-ups; the orb
// level items (the randomizer's orb levels first); the intro skip.
void game_frame(void);

// settings.c — onimusha.ini
enum key_action { KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_START, KEY_BACK, KEY_LTHUMB, KEY_RTHUMB, KEY_A, KEY_B, KEY_X, KEY_Y, KEY_WHITE, KEY_BLACK,
                  KEY_LTRIGGER, KEY_RTRIGGER, KEY_COUNT };
enum { FILTER_SMOOTH, FILTER_SHARP, FILTER_ENHANCED };
enum { PICTURE_ORIGINAL, PICTURE_STRETCH, PICTURE_ZOOM, PICTURE_MODERN };
// text_language: the game's own language numbers (hle.c hands it the value), so English is 1, not 0
enum { LANG_ENGLISH = 1, LANG_FRENCH, LANG_GERMAN };
enum { SUBS_OFF, SUBS_TEXT, SUBS_JAPANESE };   // movie_subtitles: off, in text_language, Japanese
// onimusha.ini [cheats], in the order of cheat_names (settings.c)
enum { CHEAT_GOD, CHEAT_MAGIC, CHEAT_SOULS, CHEAT_ONE_HIT, CHEAT_AMMO, CHEAT_ITEMS, CHEAT_ONIMUSHA, CHEAT_VISION, CHEAT_COUNT };
struct settings { int scale, borderless, window_w, window_h, msaa, vsync, filter, anisotropy, fxaa, hud_xbr, interp, modern_controls, weapon_wheel, picture, text_lang, enhanced_text, skip_intro, ach_popups, show_fps, movie_subs, console; int cheat[CHEAT_COUNT]; float denoise; int keys[KEY_COUNT];
                  char rando_run[64]; };   // the randomizer run being played (randomizer.c); "" = the normal game
extern struct settings settings;
extern const char *const cheat_names[CHEAT_COUNT];   // their ini keys
void settings_load(const wchar_t *root);
// Built-in mods (mods_list.h): mods\<folder>\mod.ini with builtin = <id>. builtin_mod_on is the launcher's switch for
// that folder, or fallback when no folder has that id. Call it from settings_load (root is set there).
int builtin_mod_on(const char *id, int fallback);
// The path of rel in the built-in mod folder with that id (mods\<folder>\rel); 0 if there's no such folder
int builtin_mod_file(const char *id, const wchar_t *rel, wchar_t *out, size_t n);

// input.c
int game_view(float *feet_y, int *cut);   // for picture = modern
void game_sound(uint32_t id);             // plays one of the game's own sounds (its FUN_0015c440), on the game's thread
void orb_items_frame(void);               // the randomizer's orb level items, once a game step (docs/randomizer.md)
// input.c -> d3d_overlay.c (defined there): the weapon wheel to draw over the game (n = 0: hidden). d3d.c's Present also slows the game
// while shown. page: 0 weapons, 1 items (LT/RT switch). id: weapon id (see input.c; for the bow or Matchlock, the one
// for its chosen ammo), or 0x100 + item id on the items page, or -1 for an empty slot. name and sub (level, or ammo and
// count) are shown in the middle for the selected slot; badge (ammo count) beside a launcher; cycle: LB/RB change its ammo.
struct wheel { int n, sel, page, id[8], cycle[8]; char name[8][24], sub[8][24], badge[8][8]; };
extern struct wheel wheel;

// d3d.c
uint32_t d3d_frame_number(void);
// d3d_pack.c — reading HD textures ahead, from kernel.c's file opens
void bg_prefetch(const wchar_t *path);   // the game opened a room's .bgs: read its HD backgrounds ahead
void xmov_prefetch(const wchar_t *path); // ... an animated background's xmov*.xpr: read its HD frame sheets ahead
void room_open(const wchar_t *path);     // the game opened a room's .rdt or .bgs (room_textures.txt)
void start_prefetch(void);               // at the first frame, read ahead the textures used before any room or in many

// texpack.c — HD texture packs: mods\<folder>\textures\<name>.png/.dds replace the texture d3d_texture.c names <name>.
// texpack_load returns the replacement as malloc'd BGRA (free it), or 0.
void texpack_init(const wchar_t *root);
int texpack_replacing(void);
void texpack_seen(const char *wild, int replaced);   // coverage: textures_seen.txt and a running count in the log
uint32_t *texpack_load(const char *name, uint32_t *w, uint32_t *h);
// The exact name first (with its palette), in every mod, before any _$ name, as every lookup does: *got
// says which one was found. wild may equal name (not paletted).
uint32_t *texpack_load_either(const char *name, const char *wild, uint32_t *w, uint32_t *h, const char **got);
// A paletted pack name's _$ form (WxH_ff_texels_$) into wild: 1, or 0 for a name without a palette part (wild = name).
int texpack_wild_name(const char *name, char *wild, size_t n);
int texpack_has(const char *name);   // a pack has a file of that name (no reading)
void write_png(const wchar_t *path, const void *bgra, uint32_t w, uint32_t h, uint32_t pitch);   // 32-bit BGRA
#ifdef ONI_DEV_TOOLS   // development builds' own tools (src/host/dev/, not in the public repository)
void texdump_init(const wchar_t *root);
int texpack_dumping(void);
int texpack_ignored(const char *name);
int texpack_dumped(const char *name);
void texpack_dump(const char *name, const void *bgra, uint32_t w, uint32_t h, uint32_t pitch);
#else
#define texpack_dumping() 0
#define texpack_ignored(name) 0
#endif

// movie.c — HD movies: kernel.c calls movie_open when the game opens a .bik that an enabled mod has
// movies\<name>.mp4 for, and movie_close when it closes that file. d3d_texture.c: movie_frame gives the HD frame (W x H BGRA,
// pitch W * 4) for a w x h movie frame texture the game just filled, or 0 (then the game's own); after a frame,
// call movie_frame_done once it's copied.
void movie_bik(const wchar_t *bik, void *file);   // every .bik the game opens (before movie_open / movie_subs)
int movie_open(const wchar_t *path, void *file);   // 0: not used (e.g. wrong frame count)
void movie_subs(const wchar_t *srt);   // that movie's subtitles (.srt, UTF-8)
const wchar_t *movie_subtitle(void);   // the line to show now, or 0
void movie_close(void *file);
const uint32_t *movie_frame(uint32_t w, uint32_t h, uint32_t *W, uint32_t *H);
void movie_frame_done(void);
// dsound.c — a .wav beside a .bik is that movie's sound (for remade movies the game's old Bink can't sound out):
// kernel.c opens/closes it with the .bik, d3d_texture.c starts it at the movie's first frame.
void movie_sound_open(const wchar_t *wav, void *file);
void movie_sound_start(void);
void movie_sound_close(void *file);

// text.c — settings: enhanced_text. idx: a 256x256 paletted texture's indices (linear). If it is the game's font sheet,
// returns it redrawn with Kabuto Sans at k times the size ((256 k)^2 indices, kept by text.c until the next sheet);
// otherwise 0.
const uint8_t *text_sheet(const uint8_t *idx, int k);
// The last sheet's outline coverage, 0-255 a pixel (its anti-aliased edge, drawn blended), or 0: draw it alpha-tested.
const uint8_t *text_coverage(void);
int kabuto_font(void);   // Kabuto Sans loaded for GDI from the DLL's resources (once); 1: there
// achievements.c — achievements_list.h; ach_frame once a frame from game_frame (checks game memory; 1: this was one of
// its in-play checks, a quarter second apart, in the story's play states), ach_unlock from anywhere,
// ach_popup_next: the next unlocked achievement's name to show, or 0
#include "achievements_list.h"
#define X(id, name, desc) ACH_##id,
enum { ACHIEVEMENTS(X) ACH_COUNT };
#undef X
void ach_init(const wchar_t *root);
int ach_frame(void);
void ach_unlock(int a);
const char *ach_popup_next(void);
int ach_popup_pending(void);   // a popup is waiting to be shown
// mapping.c — the randomizer's map in mapping\ (items, flags, rooms, checklist), from what play uncovers;
// mapping_frame from game_frame, on ach_frame's in-play checks
void mapping_init(void);
void mapping_frame(void);

// cheats.c — onimusha.ini [cheats]. cheats_install patches the game's code (from hle_install), cheats_frame tops
// values up once a frame; cheats_any: any cheat is on (achievements.c then unlocks nothing, and marks the saves).
void cheats_install(void);
void cheats_frame(void);
int cheats_any(void);
// randomizer.c — a randomizer run (randomizer\<run>\, made by the launcher): rando_on while one is played;
// rando_install patches the Dark Realm and Ogre Tower item tables (from hle_install)
int rando_on(void);
void rando_install(void);
void rando_frame(void);   // in play (from game_frame, as mapping_frame): hands over the run's start items
void rando_orb_levels(void);   // from orb_items_frame: an orb picked up outside its room gets its levels back

// host.c — ONI_BACKGROUND=1: window off-screen without focus, muted, real controller/keyboard ignored (tours)
int host_background(void);

// report.c — first-seen feature report (features.log); unsupported = 1 marks things the port doesn't handle
void report_init(const wchar_t *root);
void xfeature(int unsupported, const char *fmt, ...);
int report_take_shot(void);

// kernel.c
// Stall breakdown (d3d.c's stall line): time the game thread spent in file opens and reads, counted here for the
// thread d3d.c names (the one that calls Present), in QueryPerformanceCounter ticks.
extern volatile long long stall_io_ticks;
extern volatile uint32_t stall_thread;
// The path of mods\<folder>\<rel> in the highest enabled mod that has it; 0 if none does.
int mod_file(const wchar_t *rel, wchar_t *out, size_t n);
struct kernel_export { uint16_t ordinal; const char *name; void *impl; };
extern const struct kernel_export kernel_exports[];
void kernel_init(const wchar_t *root);
void kernel_attach_boot_thread(void);
void kernel_add_link(const char *name, const char *target);   // e.g. "\??\Z:" -> "\Device\Harddisk0\Partition5"
void kernel_watch_writes(uint32_t addr, uint32_t len);        // debug: hardware write watchpoint on all game threads
extern uint32_t tls_slot_kthread, tls_slot_prcb, tls_slot_irql, tls_slot_anchor;

// hle.c — SDK replacements, by SDK function name; each module exports a {0,0}-terminated table
struct hle_impl { const char *name; void *fn; };
void hle_install(void);
void intro_frame(void);   // skip_intro's fade patch, undone once used
void warp_frame(void);    // the debug warp (ONI_WARP_AT), from each pad poll
extern const struct hle_impl d3d_impls[], input_impls[], dsound_impls[];
