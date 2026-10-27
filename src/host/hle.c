// Replaces the statically linked SDK functions (D3D8, DirectSound, input/memory-unit) with our own.
// Each listed function's entry gets a jmp. Functions without a native implementation yet jump to a
// stub that logs the call and returns 0 (EDX:EAX) with the right stack cleanup, so the game keeps going.
#include "host.h"
#include "game.h"
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "patch.h"

struct sdk_fn { uint32_t addr; const char *lib, *name; uint8_t nstack; uint32_t calls; };

static struct sdk_fn sdk[] = {   // all stdcall/fastcall/thiscall: the callee pops nstack dwords
#define SDK(addr, lib, name, conv, nstack) {addr, #lib, #name, nstack},
#include "sdk_symbols.h"
#undef SDK
};

// ---- XAPI --------------------------------------------------------------------------------------------
// BOOL XMountUtilityDrive(BOOL fFormatClean): picks a cache partition and formats it at sector level.
// Our Z: is virtual (kernel.c, UTILITY_DRIVE): the disc under another name, so there's nothing to format.
static uint32_t __stdcall x_XMountUtilityDrive(uint32_t fFormatClean)
{
    kernel_add_link("\\??\\Z:", "\\Device\\Harddisk0\\Partition5");
    xlog("[xapi] XMountUtilityDrive(%u) -> Z: = the disc (virtual cache)", fFormatClean & 0xFF);
    return 1;
}

static const struct hle_impl xapi_impls[] = {
    {"XMountUtilityDrive", x_XMountUtilityDrive},
    {0, 0},
};

// ---- debug: room warp --------------------------------------------------------------------------------------
// ONI_WARP=305 makes the next save you load put you in room 305 (stage 3, room 05), at the saved position.
// ONI_WARP_SLOT=<file of 0x880 bytes> (with ONI_WARP; 0 keeps that slot's own room) loads that save slot instead of the
// one picked, e.g. a slot cut out of another save.dat: the load menu always picks the first.
// FUN_001493a0(save, state) checks the save block's checksums (word 0 = sum, word 0x21f = xor of words 1..0x21e)
// and copies stage (+0x28), room (+0x2a) and camera cut (+0x2c, state 0x546aec) into the game state. Its one call
// site, at 0x14ae40, is redirected here: edit the fields, fix the checksums, then run the original. The cut must be
// one the room has: it indexes the room's camera table (0x54347c, 0x28 bytes each) for the background to load, and
// an index past the end read a nonsense file size, so the load never finished. Cut 0 always exists.
static int warp_target = -1;
static uint32_t __fastcall warp_load(uint32_t *save, uint32_t state)
{
    uint32_t sum, x;
    save_slot_sums(save, &sum, &x);   // game.h
    if (warp_target >= 0 && save[0] == sum && save[0x21f] == x) {
        uint8_t *b = (uint8_t *)save;
        char slot[MAX_PATH] = "";
        if (debug_env("ONI_WARP_SLOT", slot, sizeof slot) && slot[0]) {
            FILE *f = fopen(slot, "rb");
            size_t n = f ? fread(b, 1, SAVE_SLOT_SIZE, f) : 0;
            if (f) fclose(f);
            xlog("[warp] slot %s: %u bytes", slot, (unsigned)n);
        }
        if (warp_target > 0) {
            xlog("[warp] save says room %u%02u, loading room %03d instead", *(uint16_t *)(b + 0x28), *(uint16_t *)(b + 0x2a), warp_target);
            *(uint16_t *)(b + 0x28) = (uint16_t)(warp_target / 100);
            *(uint16_t *)(b + 0x2a) = (uint16_t)(warp_target % 100);
            *(uint16_t *)(b + 0x2c) = 0;
        }
        char v[128] = "";   // debug: ONI_WARP_SET=18:0,16:3 writes 16-bit values at those byte offsets of the save
        debug_env("ONI_WARP_SET", v, sizeof v);
        for (const char *p = v; *p;) {
            unsigned off, val;
            if (sscanf(p, "%x:%x", &off, &val) == 2 && off + 2 <= SAVE_SLOT_SIZE) *(uint16_t *)(b + off) = (uint16_t)val;
            p = strchr(p, ',');
            if (!p) break;
            p++;
        }
        save_slot_sums(save, &sum, &x);
        save[0] = sum; save[0x21f] = x;
        warp_target = -1;   // once per run
    }
    return ((uint32_t(__fastcall *)(uint32_t *, uint32_t))0x1493a0)(save, state);
}

// ---- fix: the "locked" message for the Green Key door ---------------------------------------------------------------
// The door handler (FUN_00151650) picks the locked message by key and by who is played, from two jump tables (key -
// 0x30: red, blue, green, Shinobi Kit, Secret key). Samanosuke's green entry and Kaede's blue one point at the case
// that picks message 0, "You have used the <last item>.", so trying the Green Key door (105 > 108) without the key
// said that, with whatever was picked up last. They now point at the cases for message 4 ("It is marked by green ink")
// and 3 ("marked by blue ink"). Seen in a randomizer playtest; the original room file does it too.
static void fix_key_messages(void)
{
    uint32_t *sama = (uint32_t *)0x1517d0, *kaede = (uint32_t *)0x1517e4;
    if (sama[2] != 0x151788 || kaede[1] != 0x151788 || kaede[2] != 0x151781 || sama[1] != 0x151759) {
        xlog("[fix] key message tables moved: not patched");
        return;
    }
    sama[2] = 0x151781;    // mov eax, 4
    kaede[1] = 0x151759;   // mov eax, 3
}

static void install_warp(void)
{
    char v[8] = "";
    if (!debug_env("ONI_WARP", v, sizeof v) || !v[0]) return;
    if (!patch_call(0x14ae40, 0x1493a0, warp_load)) { xlog("[warp] call site moved, warp disabled"); return; }   // patch.h
    warp_target = atoi(v);
    xlog("[warp] armed: next save loaded goes to room %03d", warp_target);
}

// ---- debug: scripted warp ----------------------------------------------------------------------------------
// ONI_WARP_AT=4600:102 (k after the room: switch who is played) sends you to that room at that frame, mid-game, as a
// door does: a door's trigger (FUN_00151650) leaves a pointer to its record at 0x546ba4 and sets the byte 0x546bb1;
// play (FUN_000db320) then turns the mode to 3, whose first step (FUN_000d9820) takes the stage (+8), room (+9),
// camera (+0xa) and arrival x, y, z (s16 at +0, +2, +4) and facing (+6) from the record, and the game's own fade and
// room load follow. The record here is where warp_table.h says: the arrival of a door into the room, or a spot of the
// room's own (tools/rando/warp_table.py). Room scripts clear 0x546bb1 as they run, so it is set again each poll until
// the mode turns. (It was on F8 too, for playtests; taken out with F9's fast-forward.)
static const struct { uint16_t room; int16_t x, y, z, dir; uint8_t cam, kind, how; } warp_spots[] = {
#include "warp_table.h"
};
static int warp_pending;
static uint8_t warp_rec[0x14];

static void warp_go(int room, int switch_character)
{
    int i = 0, n = sizeof warp_spots / sizeof *warp_spots;
    while (i < n && warp_spots[i].room != room) i++;
    if (i == n) { xlog("[warp] no room %03d", room); return; }
    if ((*(volatile uint32_t *)G_STATE & 0xff) != 2 || (*(volatile uint32_t *)G_MODE_FLAGS & 0x1000)) { xlog("[warp] not while you can't move"); return; }
    if (switch_character) ((void(__cdecl *)(void))0x10a140)();   // the character swap, as script op 0x31 does it
    memset(warp_rec, 0, sizeof warp_rec);
    int16_t pos[4] = {warp_spots[i].x, warp_spots[i].y, warp_spots[i].z, warp_spots[i].dir};
    memcpy(warp_rec, pos, sizeof pos);
    warp_rec[8] = (uint8_t)(room / 100); warp_rec[9] = (uint8_t)(room % 100); warp_rec[0xa] = warp_spots[i].cam;
    warp_rec[0x11] = warp_spots[i].kind;
    warp_pending = 120;
    xlog("[warp] to room %03d (%s)", room, warp_spots[i].how == 1 ? "where its door leads" : warp_spots[i].how == 2 ? "a spot in the room" : "no spot known: the room's origin");
}

void warp_frame(void)   // from each pad poll
{
    static unsigned at, to;
    static int init, sw;
    if (!init++) { char v[32] = ""; debug_env("ONI_WARP_AT", v, sizeof v); if (sscanf(v, "%u:%u", &at, &to) == 2) sw = strchr(v, 'k') != 0; }
    if (at && d3d_frame_number() >= at) { at = 0; warp_go((int)to, sw); }
    if (!warp_pending) return;
    if ((*(volatile uint32_t *)G_STATE & 0xff) == 3) warp_pending = 0;   // taken
    else if (!--warp_pending) xlog("[warp] the game didn't take it");
    else { *(volatile uint32_t *)0x546ba4 = (uint32_t)(uintptr_t)warp_rec; *(volatile uint8_t *)0x546bb1 = 1; }
}

// ---- skip the intro (onimusha.ini [game] skip_intro) ------------------------------------------------------
// Start-up runs FUN_00015a80: the hard disk check (FUN_00149790: "Checking the hard disk." for 60 frames, then, when
// the system data holds any unlock, "Game completion data has been loaded." for 60-180), the Capcom logo, then the
// violence warning, each held 64 frames and at least 2 s while the first rooms are copied to the cache drive, with
// fades between. On: both pictures become the black one the game shows before them (image 0x10), every hold one
// frame, and the fades between them take 9 frames instead of 64: fades between black and black, which left the
// window black for two seconds before the title. The fade outs (FUN_000d8b60(0, step): 0x400 down by step a frame)
// get a larger step; the fade in (FUN_000d8c70 sets the step 0x10 itself, for every fade in of the game) gets it only
// until the first one has started (intro_frame, below). The cache copies are skipped anyway (install_cache_copy).
// {address, original byte, new byte}.
static int intro_fade;   // the fade-in step patch (below) is in place
static void install_skip_intro(void)
{
    static const uint32_t p[][3] = {
        {0x15adc, 0x08, 0x10}, {0x15cc7, 0x07, 0x10},   // push 8 (CROGO.BG) / push 7 (E_GRO.BG) -> KURO.BG
        {0x15af0, 0x40, 0x01}, {0x15ce7, 0x40, 0x01},   // hold 64 frames -> 1
        {0x15c55, 0x3c, 0x00},   // what is left of the 2 s copy time, in frames: x60 -> x0 (then 1 frame); the 2 s itself
                                 // stays, since a copy longer than it takes a debug-print path that ends in int 0x2d
        {0x15c9f, 0x10, 0x7f}, {0x15e65, 0x10, 0x7f},   // fade-out step 0x10 -> 0x7f (64 frames -> 9)
        {0x1497e4, 0x3c, 0x01},                          // "Checking the hard disk." 60 -> 1
        {0x1498da, 0x3c, 0x01}, {0x1498e9, 0xb4, 0x01},  // "Game completion data..." 60 / 180 -> 1
    };
    if (!settings.skip_intro) return;
    for (size_t i = 0; i < sizeof p / sizeof *p; i++)
        if (*(uint8_t *)p[i][0] != p[i][1]) { xlog("[intro] code at %06x is not as expected: skip_intro ignored", p[i][0]); return; }
    for (size_t i = 0; i < sizeof p / sizeof *p; i++) *(uint8_t *)p[i][0] = (uint8_t)p[i][2];
    if (*(uint8_t *)0xd8cd5 == 0x10) { *(uint8_t *)0xd8cd5 = 0x7f; intro_fade = 1; }   // the first fade in: step 0x10 -> 0x7f
    xlog("[intro] skipping the logos");
}

// ---- the Z: cache copies ------------------------------------------------------------------------------------------
// FUN_0001d590(src, dst, sectors) copies a file whole: GetFileSize, then one ReadFile of all of it into the buffer at
// *0x53c4d4, then one WriteFile; on failure it deletes dst. The game copies each room's backgrounds (and at start-up,
// behind the logos, a few more files) from D: to its Z: cache with it (callers below). That buffer is 3 MB and a
// room's BGS can be 13.8 MB (room007.bgs), so the read runs past it into whatever was allocated after it. Where that
// was writable it was overwritten: the room 000 -> 007 cutscene froze (2026-10-27, Steam Deck) with room 007's .rdt,
// loaded just before, full of background pixels, and the game asking for a background past the end of the file.
// Where it wasn't, the copy failed and its clean-up deleted Z:\IMAGE\BGS\ROOM007.BGS, which was game\'s own file
// (2026-10-26, Windows; kernel.c no longer lets a delete through). A copy into our Z: is thrown away anyway
// (kernel.c, UTILITY_DRIVE), so it isn't made: reported done (1), the game reads the Z: name, the disc's file.
static int __cdecl cache_copy(const char *src, const char *dst, int sectors)
{
    if ((dst[0] | 0x20) == 'z' && dst[1] == ':') {
        xlog("[cache] copy to %s not made (Z: is the disc)", dst);
        return 1;
    }
    return ((int(__cdecl *)(const char *, const char *, int))0x1d590)(src, dst, sectors);
}
static void install_cache_copy(void)
{
    static const uint32_t sites[] = {0x15bba, 0x15daa, 0x1dd64, 0x1df14};   // logos (2), room loads (2)
    int n = 0;
    for (size_t i = 0; i < sizeof sites / sizeof *sites; i++) n += patch_call(sites[i], 0x1d590, cache_copy);
    if (n != (int)(sizeof sites / sizeof *sites)) xlog("[cache] only %d of %u copy calls found: the rest still copy", n, (unsigned)(sizeof sites / sizeof *sites));
}

// Each frame: once the first fade in has taken the larger step, every later one (the title's, rooms') gets the game's own.
void intro_frame(void)
{
    if (intro_fade && *(volatile uint16_t *)0x3161f2 == 0x7f) { *(uint8_t *)0xd8cd5 = 0x10; intro_fade = 0; }
}

// ---- text language (onimusha.ini [game] text_language = french|german) -----------------------------------
// The word at 0x546ae6 is the text language: 0 Japanese, 1 English, 2 French, 3 German. The USA game only ever
// sets 1, and only part of its European code survived: the message functions (0xeb000-0xef000: item names, options,
// menus, prompts) still switch on all four and the executable has the French and German strings, but some menu
// layouts keep only Japanese/English positions (e.g. 0x160ff0) and would draw at garbage ones. So the variable stays
// English and only the message functions' 23 reads of it are pointed at the chosen language. Files are swapped in
// kernel.c (text_language): the rest of the text is drawn into the F/G images on the disc.
//
// Messages are lists: a table of 16-bit offsets from the list's text. The USA version added messages to the end of
// three English lists that the French and German ones never got (Voice Volume in the options, the Ogre Armor, Secret
// key and Petit Panda, a few prompts); asking for one reads past the list, shows garbage and overflows the text
// buffer, so the rest of the screen stays empty. The F/G lists are copied with those added (our own translations).
// Text: ' ' is 0x00, a letter 0xfa + (char - 0x20), accented letters 0xfa + their code in MOJIST_P, '\n' is 0xf2.
// The control codes around each one (page, wait-for-button, end) are taken from the English message.
static const struct msg_list {
    uint32_t e_tab, e_base, n_e, n;   // English list, its length; the F/G lists' length
    uint32_t at[2][2];                // F, G: where the lookup code holds the list's table and text addresses
    const char *add[4][2];            // French, German
} msg_lists[] = {
    {G_NAME_OFFS, G_NAME_TEXT, G_NAMES, 163, {{0xecb88, 0xecb8d}, {0xecb99, 0xecb9e}}, {   // names and labels
        {"Armure d'ogre", "Oger-R\xfcstung"},
        {"Cl\xe9 secr\xe8te", "Geheimschl\xfcssel"},
        {"Petit Panda", "Kleiner Panda"},
        {"Volume voix", "Stimmen-Lautst."}}},
    {0x205f50, 0x204610, 108, 105, {{0xec462, 0xec468}, {0xec47f, 0xec485}}, {   // item descriptions
        {"L'armure de\nd\xe9" "fense ultime\ndu guerrier\nde la tour.", "Die st\xe4rkste\nSchutzr\xfcstung\ndes Kriegers\naus dem Turm."},
        {"La t\xeate de\ncette cl\xe9 a la\nforme d'une\ncroix.", "Der Griff des\nSchl\xfcssels hat\ndie Form eines\nPluszeichens."},
        {"Un bon compagnon\nqui aime jouer\navec les autres.", "Ein guter Freund,\nder gern mit\nanderen spielt."}}},
    {0x2045dc, 0x203bb8, 24, 20, {{0xec73f, 0xec745}, {0xec75c, 0xec761}}, {   // prompts
        {"C'est verrouill\xe9. La serrure a la\nforme d'une croix.", "Verschlossen. Das Schl\xfcsselloch hat\ndie Form eines Pluszeichens."},
        {"Si vous enregistrez ces donn\xe9" "es, le\n\"Mode Difficile\" sera disponible.", "Speichere die Spieldaten, dann steht\ndir der \"Schwer-Modus\" zur Verf\xfcgung."},
        {"Si vous enregistrez ces donn\xe9" "es, vous\npourrez jouer avec l'\xe9quipement \"Ultime\".", "Speichere die Spieldaten, dann kannst du\nmit der \"Ultimativ\"-Ausr\xfcstung spielen."},
        {"Veuillez reconnecter la manette\net appuyer sur START pour continuer.", "Bitte schlie\xdf" "e den Controller wieder an\nund dr\xfc" "cke START, um fortzufahren."}}},
};

static uint8_t *msg_encode(uint8_t *o, const char *s)
{
    static const char latin[] = "\xc4\xe4\xd6\xf6\xdc\xfc\xdf\xc0\xe0\xc2\xe2\xc8\xe8\xc9\xe9\xca\xea\xcf\xef\xce\xee\xd4\xf4\xd9\xf9\xc7\xe7";
    static const uint8_t code[] = {0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91, 0x92,
                                   0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c};   // in the order of latin[]
    for (; *s; s++) {
        const char *l = (uint8_t)*s >= 0x80 ? strchr(latin, *s) : 0;
        if (*s == '\n') *o++ = 0xf2;
        else if (*s == ' ') *o++ = 0;
        else { *o++ = 0xfa; *o++ = l ? code[l - latin] : (uint8_t)(*s - 0x20); }
    }
    return o;
}

static void extend_messages(const struct msg_list *m, int lang)
{
    uint32_t *tab_at = (uint32_t *)m->at[lang][0], *base_at = (uint32_t *)m->at[lang][1];
    const uint16_t *etab = (const uint16_t *)m->e_tab, *tab = (const uint16_t *)*tab_at;
    const uint8_t *e = (const uint8_t *)m->e_base, *base = (const uint8_t *)*base_at;
    size_t len = (const uint8_t *)tab - base;
    uint8_t *text = malloc(len + 1024), *o = text + len;   // kept for the whole run
    uint16_t *offs = malloc(m->n_e * 2);
    if (!text || !offs) { free(text); free(offs); return; }   // out of memory at start: the list stays as the USA game has it
    memcpy(text, base, len);
    memcpy(offs, tab, m->n * 2);
    for (uint32_t k = m->n; k < m->n_e; k++) {
        const uint8_t *s = e + etab[k], *end = k + 1 < m->n_e ? e + etab[k + 1] : (const uint8_t *)m->e_tab, *first = s, *last = 0;
        while (first < end && *first != 0xfa) first++;       // control codes before the text
        for (const uint8_t *p = first; p < end; p += *p == 0xfa ? 2 : 1) if (*p == 0xfa) last = p + 2;   // and after it
        if (!last) last = end;   // no text: it is all control codes
        offs[k] = (uint16_t)(o - text);
        memcpy(o, s, first - s); o += first - s;
        o = msg_encode(o, m->add[k - m->n][lang]);
        memcpy(o, last, end - last); o += end - last;
    }
    *tab_at = (uint32_t)offs;
    *base_at = (uint32_t)text;
}

// Glyph {offset, width} by code for the Western font. The game's table (0x2008e0) stops at 0x84, so the accented
// letters (0x82-0xb0, laid out by kernel.c european_font) get the width of the letter they're built on.
static int8_t glyph_width[0xb2][2];
static int install_glyph_widths(void)
{
    static const char base[] = "AaOoUuBAaAaEeEeE" "eIiIiOoUuCcNnEeA" "aIiOoUuIiOoNnUu";   // MOJIST_P's accent rows
    uint32_t *sites[21];
    int n = 0, offsets = 0;
    for (uint8_t *p = (uint8_t *)0xeb000; p < (uint8_t *)0xef000 && n < 21; p++)   // 8 reads of offsets, 12 of widths
        if (*(uint32_t *)p == 0x2008e0 || *(uint32_t *)p == 0x2008e1) { offsets += *(uint32_t *)p == 0x2008e0; sites[n++] = (uint32_t *)p; }
    if (n != 20 || offsets != 8) { xlog("[lang] glyph table reads aren't the expected 8 + 12: text_language ignored"); return 0; }
    memcpy(glyph_width, (void *)0x2008e0, 0x85 * 2);
    for (int i = 0; base[i]; i++) memcpy(glyph_width[0x82 + i], glyph_width[base[i] - 0x20], 2);
    for (int i = 0; i < n; i++) *sites[i] = (uint32_t)&glyph_width[0][*sites[i] - 0x2008e0];
    return 1;
}

static uint32_t text_lang_word;   // read as a word and, by one compare, as a dword
static void install_text_language(void)
{
    if (settings.text_lang < LANG_FRENCH) return;
    static const uint8_t lang_addr[] = {0xE6, 0x6A, 0x54, 0x00};
    uint8_t *sites[32];
    int n = 0;
    for (uint8_t *p = (uint8_t *)0xeb000; p < (uint8_t *)0xef000 && n < 32; p++)
        if (!memcmp(p, lang_addr, 4)) sites[n++] = p;
    if (n != 23) { xlog("[lang] %d reads of the text language in the message code, expected 23: text_language ignored", n); return; }
    for (size_t i = 0; i < sizeof msg_lists / sizeof *msg_lists; i++)
        for (int l = 0; l < 2; l++)
            if (*(uint32_t *)msg_lists[i].at[l][0] <= *(uint32_t *)msg_lists[i].at[l][1]) { xlog("[lang] message lists moved: text_language ignored"); return; }
    if (!install_glyph_widths()) return;
    for (size_t i = 0; i < sizeof msg_lists / sizeof *msg_lists; i++) extend_messages(&msg_lists[i], settings.text_lang - LANG_FRENCH);
    text_lang_word = (uint32_t)settings.text_lang;
    for (int i = 0; i < n; i++) *(uint32_t *)sites[i] = (uint32_t)&text_lang_word;
    xlog("[lang] text language %s", settings.text_lang == LANG_FRENCH ? "french" : "german");
}

static const struct hle_impl *const impl_tables[] = {xapi_impls, d3d_impls, input_impls, dsound_impls};

static void *find_impl(const char *name)
{
    for (size_t t = 0; t < sizeof impl_tables / sizeof *impl_tables; t++)
        for (const struct hle_impl *i = impl_tables[t]; i->name; i++)
            if (!strcmp(i->name, name)) return i->fn;
    return 0;
}

static void __stdcall hle_trace(uint32_t i)
{
    if (sdk[i].calls++ < 8) xlog("[sdk] UNIMPL %s %s (%u)", sdk[i].lib, sdk[i].name, sdk[i].calls);
    if (sdk[i].calls == 1) xfeature(1, "sdk: unimplemented %s %s", sdk[i].lib, sdk[i].name);
}

static void write_jmp(uint8_t *at, const void *to) { patch_rel32(at, 0xE9, to); }

void hle_install(void)
{
    enum { STUB = 32 };
    size_t n = sizeof sdk / sizeof *sdk;
    uint8_t *stubs = VirtualAlloc(0, n * STUB, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stubs) xfatal("can't allocate the SDK stubs: %lu", GetLastError());
    int native = 0;
    for (size_t i = 0; i < n; i++) {
        void *fn = find_impl(sdk[i].name);
        if (fn) { write_jmp((uint8_t *)sdk[i].addr, fn); native++; continue; }
        uint8_t *s = stubs + i * STUB;
        s[0] = 0x68; memcpy(s + 1, &(uint32_t){(uint32_t)i}, 4);                    // push i
        s[5] = 0xE8; memcpy(s + 6, &(int32_t){(int32_t)((uint8_t *)hle_trace - (s + 10))}, 4);  // call hle_trace
        s[10] = 0x31; s[11] = 0xC0; s[12] = 0x31; s[13] = 0xD2;                     // xor eax, eax; xor edx, edx
        if (sdk[i].nstack) { s[14] = 0xC2; memcpy(s + 15, &(uint16_t){(uint16_t)(sdk[i].nstack * 4)}, 2); }
        else s[14] = 0xC3;                                                          // ret [n*4]
        write_jmp((uint8_t *)sdk[i].addr, s);
    }
    xlog("hooked %u SDK functions (%d native, rest stubbed)", (unsigned)n, native);
    install_warp();
    fix_key_messages();
    install_skip_intro();
    install_cache_copy();
    install_text_language();
    cheats_install();
    rando_install();
}
