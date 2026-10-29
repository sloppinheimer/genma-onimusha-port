// onimusha.ini next to the exe. Written with defaults (and comments) the first time; unknown keys are ignored. The
// defaults are the values in that commented file (settings_default.h), read before the real one.
#include "host.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

struct settings settings;

#include "settings_default.h"
#include "mods_list.h"

// Built-in mods: a folder in mods\ whose mod.ini says builtin = <id> (mods_list.h).
static int builtin_folder(const char *id, struct mod_item *it)
{
    int n;
    const struct mod_item *all = host_mods(&n);
    char b[64];
    for (int i = 0; i < n; i++)
        if (mod_ini(host_dir(), all[i].name, "", "builtin", b, sizeof b) && !_stricmp(b, id)) { *it = all[i]; return 1; }
    return 0;
}
int builtin_mod_file(const char *id, const wchar_t *rel, wchar_t *out, size_t n)
{
    struct mod_item it;
    if (!builtin_folder(id, &it)) return 0;
    swprintf(out, n, L"%ls\\mods\\%ls\\%ls", host_dir(), it.name, rel);
    return 1;
}
int builtin_mod_on(const char *id, int fallback)
{
    struct mod_item it;
    if (!builtin_folder(id, &it)) return fallback;
    xlog("[mods] built-in %s: %s (mods\\%ls)", id, it.on ? "on" : "off", it.name);
    return it.on;
}

// In the order of the CHEAT_ enum (host.h).
const char *const cheat_names[CHEAT_COUNT] = {"god_mode", "infinite_magic", "infinite_souls", "one_hit_kill",
                                              "infinite_ammo", "infinite_items", "infinite_onimusha", "vision_staff"};

// In the order of enum key_action (host.h).
static const char *const key_names[KEY_COUNT] = {"up", "down", "left", "right", "start", "back", "left_stick", "right_stick",
                                                  "a", "b", "x", "y",
                                                  "white", "black", "left_trigger", "right_trigger"};

static int vk_from_name(const char *s)
{
    static const struct { const char *n; int vk; } named[] = {
        {"up", VK_UP}, {"down", VK_DOWN}, {"left", VK_LEFT}, {"right", VK_RIGHT}, {"enter", VK_RETURN}, {"return", VK_RETURN},
        {"space", VK_SPACE}, {"escape", VK_ESCAPE}, {"esc", VK_ESCAPE}, {"backspace", VK_BACK}, {"tab", VK_TAB},
        {"shift", VK_SHIFT}, {"ctrl", VK_CONTROL}, {"control", VK_CONTROL}, {"alt", VK_MENU},
    };
    if (!s[0]) return 0;
    if (!s[1] && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= '0' && s[0] <= '9'))) return s[0];
    if (!s[1] && s[0] >= 'a' && s[0] <= 'z') return s[0] - 32;
    for (size_t i = 0; i < sizeof named / sizeof *named; i++) if (!_stricmp(s, named[i].n)) return named[i].vk;
    if ((s[0] == 'F' || s[0] == 'f') && atoi(s + 1) >= 1 && atoi(s + 1) <= 10) return VK_F1 + atoi(s + 1) - 1;
    if (!_strnicmp(s, "numpad", 6) && s[6] >= '0' && s[6] <= '9' && !s[7]) return VK_NUMPAD0 + s[6] - '0';
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return (int)strtol(s, 0, 16);
    return 0;
}


static int seen_modern, seen_wheel;   // this file set the mod by its current name: the earlier name is ignored

static void apply(char *line)
{
    char *k, *v;
    if (!ini_line(line, &k, &v, 1, 0, 0)) return;   // mods_list.h; any section: keys are unique
    if (!_stricmp(k, "scale")) settings.scale = atoi(v) < 1 ? 1 : atoi(v) > 8 ? 8 : atoi(v);
    else if (!_stricmp(k, "window")) settings.borderless = !_stricmp(v, "borderless") || !_stricmp(v, "fullscreen");
    else if (!_stricmp(k, "window_width")) settings.window_w = atoi(v) > 0 ? atoi(v) : settings.window_w;
    else if (!_stricmp(k, "window_height")) settings.window_h = atoi(v) > 0 ? atoi(v) : settings.window_h;
    else if (!_stricmp(k, "msaa")) settings.msaa = atoi(v) < 2 ? 0 : atoi(v) > 16 ? 16 : atoi(v);
    else if (!_stricmp(k, "vsync")) settings.vsync = atoi(v) != 0;
    else if (!_stricmp(k, "background_filter")) settings.filter = !_stricmp(v, "sharp") ? FILTER_SHARP : !_stricmp(v, "enhanced") ? FILTER_ENHANCED : FILTER_SMOOTH;
    else if (!_stricmp(k, "anisotropy")) settings.anisotropy = atoi(v) < 0 ? 0 : atoi(v) > 16 ? 16 : atoi(v);
    else if (!_stricmp(k, "background_denoise")) settings.denoise = (float)(atof(v) < 0 ? 0 : atof(v) > 1 ? 1 : atof(v));
    else if (!_stricmp(k, "hud_filter")) settings.hud_xbr = !_stricmp(v, "xbr");
    else if (!_stricmp(k, "enhanced_text")) settings.enhanced_text = atoi(v) != 0;
    else if (!_stricmp(k, "frame_interpolation")) settings.interp = atoi(v) < 2 ? 0 : atoi(v) > 8 ? 8 : atoi(v);
    else if (!_stricmp(k, "fxaa")) settings.fxaa = atoi(v) != 0;
    else if (!_stricmp(k, "show_fps")) settings.show_fps = atoi(v) != 0;
    else if (!_stricmp(k, "picture")) settings.picture = !_stricmp(v, "stretch") ? PICTURE_STRETCH : !_stricmp(v, "zoom") ? PICTURE_ZOOM : !_stricmp(v, "modern") ? PICTURE_MODERN : PICTURE_ORIGINAL;
    else if (!_stricmp(k, "modern_controls")) { settings.modern_controls = atoi(v) != 0; seen_modern = 1; }
    else if (!_stricmp(k, "weapon_wheel")) { settings.weapon_wheel = atoi(v) != 0; seen_wheel = 1; }
    // the two mods' earlier names (stick movement became part of Modern controls; weapon swap became the wheel)
    else if (!_stricmp(k, "stick_move")) { if (!seen_modern) settings.modern_controls = atoi(v) != 0; }
    else if (!_stricmp(k, "swap_weapon")) { if (!seen_wheel) settings.weapon_wheel = atoi(v) != 0; }
    else if (!_stricmp(k, "skip_intro")) settings.skip_intro = atoi(v) != 0;
    else if (!_stricmp(k, "achievement_popups")) settings.ach_popups = atoi(v) != 0;
    else if (!_stricmp(k, "console")) settings.console = atoi(v) != 0;   // [launcher], set from its About page
    else if (!_stricmp(k, "movie_subtitles")) settings.movie_subs = !_stricmp(v, "off") ? SUBS_OFF : !_stricmp(v, "japanese") ? SUBS_JAPANESE : SUBS_TEXT;
    else if (!_stricmp(k, "randomizer_run")) {   // a folder name in randomizer\ : letters, digits, - and _ only
        int n = 0;
        for (; *v && n < (int)sizeof settings.rando_run - 1; v++) if (isalnum((unsigned char)*v) || *v == '-' || *v == '_') settings.rando_run[n++] = *v;
        settings.rando_run[n] = 0;
    }
    else if (!_stricmp(k, "text_language")) settings.text_lang = !_stricmp(v, "french") ? LANG_FRENCH : !_stricmp(v, "german") ? LANG_GERMAN : LANG_ENGLISH;
    else {
        for (int i = 0; i < CHEAT_COUNT; i++) if (!_stricmp(k, cheat_names[i])) settings.cheat[i] = atoi(v) != 0;
        for (int i = 0; i < KEY_COUNT; i++)
            if (!_stricmp(k, key_names[i])) {
                int vk = vk_from_name(v);
                if (vk) settings.keys[i] = vk; else xlog("[settings] unknown key name '%s' for %s", v, k);
            }
    }
}

void settings_load(const wchar_t *root)
{
    static char defaults[sizeof settings_default_file];
    memcpy(defaults, settings_default_file, sizeof defaults);
    for (char *s = defaults, *nl; s; s = nl) { if ((nl = strchr(s, '\n'))) *nl++ = 0; apply(s); }
    seen_modern = seen_wheel = 0;
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\onimusha.ini", root);
    FILE *f = _wfopen(path, L"r");
    if (!f) {   // a first run writes the defaults; a file that is there but can't be read is left alone
        if (!file_missing(path)) xlog("[settings] onimusha.ini couldn't be read: defaults used this run, the file left as it is");
        else if (write_text_atomic(path, settings_default_file)) xlog("[settings] wrote defaults to onimusha.ini");
        else xlog("[settings] couldn't write onimusha.ini: defaults used this run");
        return;
    }
    char line[256];
    while (fgets(line, sizeof line, f)) apply(line);
    fclose(f);
    static const char *const filters[] = {"smooth", "sharp", "enhanced"};
    static const char *const pictures[] = {"original", "stretch", "zoom", "modern"};
    xlog("[settings] picture %s", pictures[settings.picture]);   // own line: the summary below is long enough
    xlog("[settings] scale %d, %s %dx%d, msaa %d, vsync %d, backgrounds %s (denoise %.1f), hud %s, enhanced_text %d, anisotropy %d, fxaa %d, modern_controls %d, weapon_wheel %d", settings.scale,
         settings.borderless ? "borderless" : "windowed", settings.window_w, settings.window_h, settings.msaa, settings.vsync,
         filters[settings.filter], settings.denoise, settings.hud_xbr ? "xbr" : "pixel", settings.enhanced_text, settings.anisotropy, settings.fxaa, settings.modern_controls, settings.weapon_wheel);
}
