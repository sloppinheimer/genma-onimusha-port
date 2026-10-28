// Xbox controller API (XInitDevices / XInputOpen / XInputGetState ...) on top of SDL3 gamepads (XInput without
// SDL3.dll), with a keyboard fallback. One controller, port 0. Memory units are never present.
#include "host.h"
#include "game.h"
#include <windows.h>
#include <xinput.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_version.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#pragma comment(lib, "xinput.lib")

#define XADDR_DEVICETYPE_GAMEPAD 0x001c9320u   // g_DeviceType_Gamepad (XbSymbolDatabase)

// Xbox analog button slots
enum { XA_A, XA_B, XA_X, XA_Y, XA_BLACK, XA_WHITE, XA_LTRIGGER, XA_RTRIGGER };

#pragma pack(push, 1)
struct x_gamepad { uint16_t buttons; uint8_t analog[8]; int16_t lx, ly, rx, ry; };
#pragma pack(pop)

static uint32_t packet;
static struct x_gamepad last;

static int key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Debug: ONI_INPUT="1000-1010:start,1400-1404:a" holds buttons during those frame ranges (ls_up/down/left/right: left stick).
// Debug: ONI_POKE="9000:547338=10" writes a byte into game memory at that frame; "9000:546bb4+1200" dumps 0x1200
// bytes from there to dump_9000.bin; "9000:546bb4<slot.bin" copies a file (next to the exe) into memory there, e.g. a
// save slot's 0x880 bytes over the loaded game (saves from another console fail the game's signature check). For testing mods.
static void poke(void)
{
    static char spec[4096];
    static int init;
    if (!init++) debug_env("ONI_POKE", spec, sizeof spec);
    uint32_t f = d3d_frame_number();
    for (const char *p = spec; *p;) {
        unsigned at, addr, v;
        if (sscanf(p, "%u:%x=%x", &at, &addr, &v) == 3 && f == at) { *(volatile uint8_t *)addr = (uint8_t)v; xlog("[poke] %08x = %02x", addr, v); }
        if (sscanf(p, "%u:%x+%x", &at, &addr, &v) == 3 && f == at) {   // 9000:546bb4+1200 dumps to dump_9000.bin
            char name[32]; snprintf(name, sizeof name, "dump_%u.bin", f);
            FILE *o = fopen(name, "wb"); if (o) { fwrite((void *)addr, 1, v, o); fclose(o); }
        }
        char file[64];
        if (sscanf(p, "%u:%x<%63[^,]", &at, &addr, file) == 3 && f == at) {
            FILE *in = fopen(file, "rb");
            size_t n = in ? fread((void *)addr, 1, 0x10000, in) : 0;
            if (in) fclose(in);
            xlog("[poke] %s -> %08x, %u bytes", file, addr, (unsigned)n);
        }
        p = strchr(p, ',');
        if (!p) break;
        p++;
    }
}

static void scripted(struct x_gamepad *g)
{
    poke();
    static char spec[4096];
    static int init;
    if (!init++) env_str("ONI_INPUT", spec, sizeof spec);
    uint32_t f = d3d_frame_number();
    for (const char *p = spec; *p;) {
        unsigned a, b;
        char name[16];
        if (sscanf(p, "%u-%u:%15[a-z_]", &a, &b, name) == 3 && f >= a && f <= b) {
            static const char *const digital[] = {"up", "down", "left", "right", "start", "back", "lthumb", "rthumb"};
            static const char *const analog[] = {"a", "b", "x", "y", "black", "white", "lt", "rt"};
            for (int i = 0; i < 8; i++) if (!strcmp(name, digital[i])) g->buttons |= 1 << i;
            for (int i = 0; i < 8; i++) if (!strcmp(name, analog[i])) g->analog[i] = 255;
            if (!strcmp(name, "ls_up")) g->ly = 32767;
            if (!strcmp(name, "ls_down")) g->ly = -32767;
            if (!strcmp(name, "ls_left")) g->lx = -32767;
            if (!strcmp(name, "ls_right")) g->lx = 32767;
        }
        p = strchr(p, ',');
        if (!p) break;
        p++;
    }
}

// PlayStation, Switch and most other controllers come through SDL3 (SDL3.dll next to the exe, delay-loaded). SDL
// gives every pad the Xbox layout by position: the bottom face button is A, so a Switch pad's B acts as A. All
// connected pads drive the game together. Without the DLL, or if SDL won't start, XInput reads Xbox pads.
#define MAX_PADS 4
static SDL_Gamepad *pads[MAX_PADS];
static int sdl_state;   // 0 not tried yet, 1 running, -1 unavailable

static int sdl_start(void)
{
    if (sdl_state) return sdl_state > 0;
    sdl_state = -1;
    if (!LoadLibraryA("SDL3.dll")) { xlog("[input] no SDL3.dll: Xbox controllers only (XInput)"); return 0; }
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");   // like XInput: the pad works without window focus
    if (!SDL_Init(SDL_INIT_GAMEPAD)) { xlog("[input] SDL_Init failed (%s): Xbox controllers only (XInput)", SDL_GetError()); return 0; }
    SDL_SetJoystickEventsEnabled(false);   // no event loop here: the state is polled
    SDL_SetGamepadEventsEnabled(false);
    int v = SDL_GetVersion();
    xlog("[input] SDL %d.%d.%d for controllers", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
    sdl_state = 1;
    return 1;
}

static void sdl_open_pads(void)   // once a second: close unplugged pads, open new ones
{
    static ULONGLONG at;
    ULONGLONG now = GetTickCount64();
    if (at && now - at < 1000) return;
    at = now;
    for (int i = 0; i < MAX_PADS; i++)
        if (pads[i] && !SDL_GamepadConnected(pads[i])) { xlog("[input] controller %d unplugged", i); SDL_CloseGamepad(pads[i]); pads[i] = 0; }
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int k = 0; ids && k < n; k++) {
        int open = 0;
        for (int i = 0; i < MAX_PADS; i++) if (pads[i] && SDL_GetGamepadID(pads[i]) == ids[k]) open = 1;
        for (int i = 0; !open && i < MAX_PADS; i++)
            if (!pads[i] && (pads[i] = SDL_OpenGamepad(ids[k]))) { xlog("[input] controller %d: %s", i, SDL_GetGamepadName(pads[i])); open = 1; }
    }
    SDL_free(ids);
}

static int16_t flip(int16_t v) { return v == -32768 ? 32767 : (int16_t)-v; }   // SDL: y down; Xbox: y up

static void sdl_read(struct x_gamepad *g)
{
    static const SDL_GamepadButton digital[8] = {SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT,
        SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_LEFT_STICK,
        SDL_GAMEPAD_BUTTON_RIGHT_STICK};
    static const SDL_GamepadButton analog[6] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST,
        SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER};   // XA_A .. XA_WHITE
    SDL_UpdateGamepads();
    sdl_open_pads();
    int lmag = 0, rmag = 0;
    for (int i = 0; i < MAX_PADS; i++) {
        SDL_Gamepad *p = pads[i];
        if (!p) continue;
        for (int b = 0; b < 8; b++) if (SDL_GetGamepadButton(p, digital[b])) g->buttons |= 1 << b;
        for (int b = 0; b < 6; b++) if (SDL_GetGamepadButton(p, analog[b])) g->analog[b] = 255;
        int lt = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7, rt = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7;
        if (lt > g->analog[XA_LTRIGGER]) g->analog[XA_LTRIGGER] = (uint8_t)lt;
        if (rt > g->analog[XA_RTRIGGER]) g->analog[XA_RTRIGGER] = (uint8_t)rt;
        int16_t lx = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX), ly = flip(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY));
        int16_t rx = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTX), ry = flip(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTY));
        if (abs(lx) + abs(ly) > lmag) { lmag = abs(lx) + abs(ly); g->lx = lx; g->ly = ly; }   // each stick from the pad pushing it most
        if (abs(rx) + abs(ry) > rmag) { rmag = abs(rx) + abs(ry); g->rx = rx; g->ry = ry; }
    }
}

// g: the controllers, as pressed; k: the test script and the keyboard, already in the game's own layout (Modern
// controls rearranges only the controller).
static void read_pad(struct x_gamepad *g, struct x_gamepad *k)
{
    memset(g, 0, sizeof *g);
    memset(k, 0, sizeof *k);
    static int script_is_pad = -1;   // debug: ONI_PAD_SCRIPT=1 plays the script as a controller (to test Modern controls)
    if (script_is_pad < 0) script_is_pad = env_int("ONI_PAD_SCRIPT", 0) != 0;
    if (host_background()) { scripted(script_is_pad ? g : k); return; }   // tours: only the script drives the game
    XINPUT_STATE s;
    if (sdl_start()) sdl_read(g);
    else if (XInputGetState(0, &s) == ERROR_SUCCESS) {
        const XINPUT_GAMEPAD *p = &s.Gamepad;
        g->buttons = p->wButtons & 0xFF;   // dpad, start, back, thumbs: same bits
        g->analog[XA_A] = p->wButtons & XINPUT_GAMEPAD_A ? 255 : 0;
        g->analog[XA_B] = p->wButtons & XINPUT_GAMEPAD_B ? 255 : 0;
        g->analog[XA_X] = p->wButtons & XINPUT_GAMEPAD_X ? 255 : 0;
        g->analog[XA_Y] = p->wButtons & XINPUT_GAMEPAD_Y ? 255 : 0;
        g->analog[XA_BLACK] = p->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER ? 255 : 0;
        g->analog[XA_WHITE] = p->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER ? 255 : 0;
        g->analog[XA_LTRIGGER] = p->bLeftTrigger;
        g->analog[XA_RTRIGGER] = p->bRightTrigger;
        g->lx = p->sThumbLX; g->ly = p->sThumbLY; g->rx = p->sThumbRX; g->ry = p->sThumbRY;
    }
    scripted(script_is_pad ? g : k);
    g = k;   // the keyboard below goes with the script
    // Keyboard, only while our window has focus.
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId()) return;
    // Bindings from onimusha.ini: d-pad, Start and Back are digital bits 0-5; the rest are analog buttons.
    for (int i = KEY_UP; i <= KEY_BACK; i++) if (key(settings.keys[i])) g->buttons |= 1 << (i - KEY_UP);
    static const int slot[KEY_COUNT] = {[KEY_A] = XA_A, [KEY_B] = XA_B, [KEY_X] = XA_X, [KEY_Y] = XA_Y, [KEY_WHITE] = XA_WHITE,
                                        [KEY_BLACK] = XA_BLACK, [KEY_LTRIGGER] = XA_LTRIGGER, [KEY_RTRIGGER] = XA_RTRIGGER};
    for (int i = KEY_A; i < KEY_COUNT; i++) if (key(settings.keys[i])) g->analog[slot[i]] = 255;
}

static void merge(struct x_gamepad *g, const struct x_gamepad *k)   // k's presses on top of g's
{
    g->buttons |= k->buttons;
    for (int i = 0; i < 8; i++) if (k->analog[i] > g->analog[i]) g->analog[i] = k->analog[i];
    if (k->lx || k->ly) { g->lx = k->lx; g->ly = k->ly; }
    if (k->rx || k->ry) { g->rx = k->rx; g->ry = k->ry; }
}

// Mod (onimusha.ini [mods] modern_controls): the controller laid out as in the Onimusha: Warlords remaster. The game
// is held at its own B-TYPE layout (X attack, Y magic, B absorb souls / cancel, A check), and the rest moves:
//   LB guard (game LT), RB draw weapon (game RT), LT the Gauntlet's power (game White), R3 180-degree turn (game
//   Black), View pause (game Start), Menu inventory (game right stick click). L3 stays the map; RT is the weapon
//   button (weapon_button); the left stick moves Samanosuke where it points (stick_move); the d-pad stays tank controls.
#define G_CONTROL_TYPE 0x0054362fu   // byte: 0 A-TYPE, 1 B-TYPE, 2 C-TYPE (the game's Options > Controls)
static int in_gameplay(void);
static void modern_layout(struct x_gamepad *g)
{
    const struct x_gamepad p = *g;
    g->analog[XA_LTRIGGER] = p.analog[XA_WHITE];
    g->analog[XA_RTRIGGER] = p.analog[XA_BLACK];
    g->analog[XA_WHITE] = p.analog[XA_LTRIGGER];
    g->analog[XA_BLACK] = p.buttons & 0x80 ? 255 : 0;
    g->buttons &= ~0xB0;                        // Start (0x10), Back (0x20), right stick click (0x80)
    if (p.buttons & 0x20) g->buttons |= 0x10;   // View -> Start
    if (p.buttons & 0x10) g->buttons |= 0x80;   // Menu -> right stick click
    // Outside play (title screen, menus, pause, inventory) Menu is Start as well, so "Press Start" takes either button.
    if (!in_gameplay() && (p.buttons & 0x10)) g->buttons |= 0x10;
    *(volatile uint8_t *)G_CONTROL_TYPE = 1;
}

// Mod (onimusha.ini [mods] modern_controls): the left stick walks Samanosuke toward the direction it points on screen:
// his heading is turned to it directly and the game's own "forward" moves him, so its collision, triggers and
// animations stay as they are. In menus the stick acts as a d-pad.
// Game addresses, found by recording memory in play:
#define G_PLAYER_POS 0x00547cd0u     // float x, y, z (y up)
#define G_PLAYER_ANGLE 0x00547d22u   // low 12 bits, 0x1000 = full turn; he walks along (sin, cos) in x/z; d-pad left raises it
#define G_WORLD_TO_SCREEN 0x0053e540u  // 4x4 row-vector matrix, world -> screen pixels (y down)
#define G_PLAYER_ACTION 0x00549234u   // action flags; the game's pose reset clears ACTION_BUSY (bit 31 = attacking)
#define ACTION_BUSY 0x96b10200u
#define G_PLAYER_MODE 0x00547463u    // byte: 1 = the player has control; 2 events and knockdowns, 3 grabbed, 4 dying
#define G_SCENE_FLAGS 0x00543644u    // 0x800000: text box (the pad passes only confirm/cancel); 0x20000000: player frozen

// Whether the game would take the d-pad as movement now: what it filters its own pad by (FUN_00106bd0) and the player
// state it moves in. Heading written straight into memory skips those checks, so the stick turned him in cutscenes,
// text boxes, chest openings and while dying.
static int free_control(void)
{
    return *(volatile uint8_t *)G_PLAYER_MODE == 1 && !(*(volatile uint32_t *)G_MODE_FLAGS & 0x1000) &&
           !(*(volatile uint32_t *)G_SCENE_FLAGS & 0x20800000u);
}

static void to_screen(const float *m, float x, float y, float z, float *sx, float *sy)
{
    float o[4];
    for (int j = 0; j < 4; j++) o[j] = x * m[j] + y * m[4 + j] + z * m[8 + j] + m[12 + j];
    *sx = o[0] / o[3]; *sy = o[1] / o[3];
}

// True while the gameplay frame counter is moving (not in a menu, the title screen or a load).
static int in_gameplay(void)
{
    static uint8_t tick;
    static ULONGLONG tick_ms;
    if (*(volatile uint8_t *)G_PLAY_TICK != tick) { tick = *(volatile uint8_t *)G_PLAY_TICK; tick_ms = GetTickCount64(); }
    return GetTickCount64() - tick_ms < 100;   // the game polls the pad several times a frame
}

// Screen y (Xbox px) of a world point, or 0 if it is behind the camera (or nearly level with it), where the
// projection gives nonsense: walking out of a shot past the camera flips it between huge positive and negative values.
static int screen_y(const float *m, const float *p, float *y)
{
    float w = p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + m[15];
    if (!(w > 1.0f)) return 0;
    *y = (p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13]) / w;
    return 1;
}

// For picture = modern (d3d_modern.c): whether the game is in play, and where Samanosuke's feet are on screen (Xbox px, y down).
// feet_y: NAN while he is behind the camera. cut: the camera itself changed since the last call and moved him on screen
// by more than 24 px or in or out of view (a camera cut, not a tracking camera's slow drift).
int game_view(float *feet_y, int *cut)
{
    static float last[16];
    const float *m = (const float *)G_WORLD_TO_SCREEN, *pos = (const float *)G_PLAYER_POS;
    float y = 0, oy = 0;
    int now = screen_y(m, pos, &y), before = screen_y(last, pos, &oy);
    *cut = memcmp(last, m, sizeof last) && (now != before || (now && fabsf(y - oy) > 24));
    memcpy(last, m, sizeof last);
    *feet_y = now ? y : NAN;
    return in_gameplay();
}

// The world heading (radians, as G_PLAYER_ANGLE: 0 along +z, towards +x) of stick direction (sx, sy; y down)
// through a camera basis b: the camera's right and forward along the ground, {right x, right z, fwd x, fwd z}.
static int world_angle(const float *b, float sx, float sy, float *a)
{
    float wx = sx * b[0] - sy * b[2], wz = sx * b[1] - sy * b[3];
    if (!(wx * wx + wz * wz > 1e-6f)) return 0;   // also NaN
    *a = atan2f(wx, wz);
    return 1;
}

// The camera's directions along the ground: forward (away from it, where its depth grows) and right (where things
// move right on screen), from the world-to-screen matrix. Camera-relative, as 3D games do it: following how his
// screen position moves instead went wrong where the ground is nearly edge-on to the camera (room 125's ladder wall,
// a low camera tilted up), where walking barely moves him on screen and the little it does read backwards.
static int camera_basis(float *b)
{
    const float *m = (const float *)G_WORLD_TO_SCREEN;
    float fx = m[3], fz = m[11], n = sqrtf(fx * fx + fz * fz);
    if (!(n > 1e-6f)) return 0;   // looking straight down (or no camera yet)
    fx /= n; fz /= n;
    float rx = fz, rz = -fx;
    // right: the way screen x grows at the middle of the screen (x 320)
    if ((rx * m[0] + rz * m[8]) - 320 * (rx * m[3] + rz * m[11]) < 0) { rx = -rx; rz = -rz; }
    b[0] = rx; b[1] = rz; b[2] = fx; b[3] = fz;
    return 1;
}

static void stick_move(struct x_gamepad *g)
{
    // The stick's direction on screen becomes a direction in the world through the camera of the moment it was set
    // (basis), so a camera cut mid-walk doesn't turn him round at once, as in the Resident Evil remasters. Once the new
    // camera has held for a quarter of a second (at zone edges the game flips between two cameras for a few frames)
    // the direction blends over half a second to what the stick means with it, so he curves into the new view (only
    // when that is within 90 degrees of the old).
    // Swinging the stick more than 50 degrees from where it was, or letting go, takes the camera of that moment.
    static float basis[4], held_dir, blend, prev[4];   // the latched camera_basis; the stick's angle then; blend 0-1;
    static int latched, steady;                        // last frame's camera_basis; frames the camera has held
    static uint32_t last_frame;
    static int r3_was, turning;   // the 180-degree turn (R3) was pressed with the stick held
    static float turn_dir;
    float sx = g->lx / 32767.0f, sy = -g->ly / 32767.0f;   // screen space: y down
    int playing = in_gameplay(), r3 = (g->buttons & 0x80) != 0;   // R3 as pressed (before modern_layout moves it)
    int pushed = sx * sx + sy * sy >= 0.35f * 0.35f;
    float dir = atan2f(sx, -sy);
    // The 180-degree turn: the game turns him itself, over several frames. Steering him to the stick meanwhile pulled
    // him straight back, so it stops until the stick is let go or swung 50 degrees to somewhere new.
    if (r3 && !r3_was && pushed && playing) { turning = 1; turn_dir = dir; }
    r3_was = r3;
    if (turning && (!pushed || fabsf(fmodf(dir - turn_dir + 9.424778f, 6.2831853f) - 3.1415927f) > 0.87f)) { turning = 0; latched = 0; }
    if (!settings.modern_controls || (g->buttons & 0xF) || !pushed || turning) {
        latched = 0;
        return;
    }
    // Menus and the title screen, text boxes and guarding (LB, the game's LT; tank controls can only step there): the
    // stick is a d-pad (its strongest direction), which the game itself ignores where it should. (Absorbing souls lets
    // tank controls walk, so it doesn't.) Anything else in play that takes control from him (cutscenes, events,
    // knockdowns): nothing, not the d-pad, since the stick held toward the camera is d-pad down, which walks him
    // backwards (crossing a camera cut, he slid back).
    int guard = g->analog[XA_WHITE] > 0x80;
    // The game takes control away for a moment at a camera cut too, so a short break keeps the latched camera: taking
    // the new one there made "toward the camera" point back over the cut, and he bounced between the two.
    static ULONGLONG free_ms;
    if (free_control()) free_ms = GetTickCount64();
    else if (GetTickCount64() - free_ms > 500) latched = 0;
    if (playing && !guard && !free_control() && !(*(volatile uint32_t *)G_SCENE_FLAGS & 0x800000)) return;
    if (!playing || guard || !free_control()) {
        latched = 0;
        if (fabsf(sx) > fabsf(sy)) g->buttons |= sx > 0 ? 1 << 3 : 1 << 2;
        else g->buttons |= sy > 0 ? 1 << 1 : 1 << 0;
        return;
    }
    const float *pos = (const float *)G_PLAYER_POS;
    float now[4], swing = fabsf(fmodf(dir - held_dir + 9.424778f, 6.2831853f) - 3.1415927f);
    if (!camera_basis(now)) {   // no usable camera: keep the last direction, or walk on the way he faces
        if (!latched) goto ahead;
        memcpy(now, basis, sizeof now);
    }
    if (!latched || swing > 0.87f) { memcpy(basis, now, sizeof basis); latched = 1; held_dir = dir; blend = 0; }
    float a_old, a_new;
    if (!world_angle(basis, sx, sy, &a_old)) goto ahead;
    if (!world_angle(now, sx, sy, &a_new)) a_new = a_old;
    float gap = fmodf(a_new - a_old + 9.424778f, 6.2831853f) - 3.1415927f;
    // Same camera (or a tracking camera's drift): nothing to blend. A camera more than 90 degrees round from the
    // latched one (two facing each other across a cut): no blend either, the stick keeps its old meaning until let go
    // or swung. Blending there turned him back over the cut, into the first camera, and back again.
    if (fabsf(gap) < 0.035f || fabsf(gap) > 1.5708f) blend = 0;
    int target = (int)((a_old + gap * blend) * (4096 / 6.2831853f)) & 0xFFF;
    volatile uint16_t *ang = (volatile uint16_t *)G_PLAYER_ANGLE;
    int diff = ((target - (*ang & 0xFFF) + 0x800) & 0xFFF) - 0x800;
    // During an attack (or anything else the game commits him to) he keeps the heading he started it with, as with
    // tank controls: no spinning a combo round. The stick still holds the d-pad, since a direction held is what lets
    // the game end an attack's recovery early (about 20 frames after a swing instead of 46): forward when it points
    // the way he faces, else a turn toward it (in place; he turns to the stick once free). Never back: back after an
    // attack is the backstep.
    if (*(volatile uint32_t *)G_PLAYER_ACTION & ACTION_BUSY) {
        g->buttons |= diff > -0x555 && diff < 0x555 ? 1 << 0 : diff > 0 ? 1 << 2 : 1 << 3;
        return;
    }
    // He turns to the stick himself, not with the tank turn: up to 0x100 (22.5 degrees) a frame, so a full about-turn
    // takes 8 frames. Once per game frame; the game reads the pad several times a frame.
    uint32_t f = d3d_frame_number();
    if (f != last_frame) {
        last_frame = f;
        float change = 0, size = 0;
        for (int i = 0; i < 4; i++) { change += fabsf(now[i] - prev[i]); size += fabsf(now[i]); }
        steady = change < 0.05f * size ? steady + 1 : 0;
        memcpy(prev, now, sizeof prev);
        if (blend > 0 || (fabsf(gap) >= 0.035f && fabsf(gap) <= 1.5708f && steady >= 15)) {   // then half a second over to the new camera
            blend += 1.0f / 30;
            if (blend >= 1) { memcpy(basis, now, sizeof basis); held_dir = dir; blend = 0; }
        }
        int step = diff > 0x100 ? 0x100 : diff < -0x100 ? -0x100 : diff;
        *ang = (uint16_t)((*ang & 0xF000) | ((*ang + step) & 0xFFF));
        diff -= step;
    }
    if (diff > -0x555 && diff < 0x555) g->buttons |= 1 << 0;   // forward, unless still turning from more than 120 off
    return;
ahead:
    g->buttons |= 1 << 0;
}

// The weapon button during play: RT with Modern controls, B (otherwise unused in play) without. With Modern controls a
// tap equips the next weapon you own (swords, then the bow and the Matchlock). Mod (onimusha.ini [mods] weapon_wheel):
// holding it opens a weapon wheel (drawn by d3d_overlay.c, game slowed meanwhile): point the left stick or d-pad, LB/RB change
// a launcher's ammo, let go to equip. LT/RT (or a flick of the right stick) switch to the items page: point at a Herb
// or Medicine and press A to use it.
// Equipping goes through the game's own routine, as the equip screen does (FUN_0015fe10).
// Weapon ids: element * 3 + level for the orb swords (0 Raizan, 1 Enryuu, 2 Shippuu), 9 = Normal Sword, 0x0b + k =
// a launcher loaded with ammo item 0x17 + k: 0x0b/0x0c the bow, 0x0d/0x0e the Matchlock. Each launcher is one slot.
#define G_PLAYER 0x00547440u
#define G_WEAPON 0x005471ecu         // equipped weapon id
#define G_WEAPON_MAGIC 0x005471eeu   // its element, 0xff for anything but an orb sword

// Cleared before equipping, as the equip screen does (FUN_0015fe10): with it 0 the player update that follows runs
// the reset state (FUN_00110830), which puts him back in control (1) standing, with the new weapon.
#define G_EQUIP_RESET G_PLAYER_MODE
#define G_PLAYER_PREV_POS 0x00547ce0u
#define WHEEL_HOLD_MS 250
// ponytail: the Normal Sword is assumed always owned; ammo items 0x18-0x1a are taken to be Fire Arrow, Bullet and
// Burst Bullet (the order the weapons list on the Onimusha wiki gives); only Normal Arrow has been seen in game.

static int launcher(int id) { return id >= 0x0b && id <= 0x0e ? (id - 0x0b) / 2 : -1; }   // 0 bow, 1 Matchlock

static int owned_ammo(int l, int *ids)   // the launcher's weapon ids for the ammo you carry
{
    int n = 0;
    for (int k = 0; k < 2; k++) if (item_count(0x17 + l * 2 + k) > 0) ids[n++] = 0x0b + l * 2 + k;
    return n;
}

static int ammo_choice[2] = {-1, -1};   // per launcher: the ammo last equipped or picked on the wheel

static int chosen_ammo(int l)   // -1 if you have no ammo for it
{
    int a[2], n = owned_ammo(l, a), cur = *(volatile uint8_t *)G_WEAPON;
    if (launcher(cur) == l) ammo_choice[l] = cur;
    for (int i = 0; i < n; i++) if (a[i] == ammo_choice[l]) return a[i];
    return n ? (ammo_choice[l] = a[0]) : -1;
}

static int owned_weapons(int *ids)   // orb swords, Normal Sword, then each launcher once, with its chosen ammo
{
    const uint8_t *lvl = (const uint8_t *)G_SWORD_LEVEL;
    int n = 0;
    for (int e = 0; e < 3; e++) if (lvl[e] < 3) ids[n++] = e * 3 + lvl[e];
    ids[n++] = 9;
    // a launcher only once you have it (bow 0x15, Matchlock 0x16): ammo alone put the Matchlock on the wheel, picking
    // up bullets before the gun
    for (int l = 0; l < 2; l++) { int a = item_count(0x15 + l) > 0 ? chosen_ammo(l) : -1; if (a >= 0) ids[n++] = a; }
    return n;
}

static void wheel_slot(int i, int id)   // fills wheel slot i for weapon id
{
    static const char *const orb[] = {"Raizan", "Enryuu", "Shippuu"};
    static const char *const ammo[] = {"Normal Arrow", "Fire Arrow", "Bullet", "Burst Bullet"};
    int a[2], l = launcher(id);
    wheel.id[i] = id;
    wheel.sub[i][0] = wheel.badge[i][0] = 0;
    wheel.cycle[i] = l >= 0 && owned_ammo(l, a) > 1;
    if (id < 9) { snprintf(wheel.name[i], 24, "%s", orb[id / 3]); snprintf(wheel.sub[i], 24, "Lv %d", id % 3 + 1); }
    else if (id == 9) snprintf(wheel.name[i], 24, "Normal Sword");
    else {
        int count = item_count(0x17 + id - 0x0b);
        snprintf(wheel.name[i], 24, "%s", l ? "Matchlock" : "Bow");
        snprintf(wheel.sub[i], 24, "%s  x%d", ammo[id - 0x0b], count);
        snprintf(wheel.badge[i], 8, "x%d", count);
    }
}

// The game's menu sounds (FUN_0015c440, as its menus call it: FUN_000ecbc0 moves the cursor, decides and cancels;
// FUN_0015ef60 opens the equip screen).
#define SND_MOVE 0x0001000fu
#define SND_DECIDE 0x0201000fu   // the equip sound (the menu's "decide" on a new choice)
#define SND_CANCEL 0x0101000fu   // the close-the-menu sound
#define SND_OPEN 0x0401000fu
#define SND_CANT 0x0301000fu
void game_sound(uint32_t id) { ((void(__cdecl *)(uint32_t, uint32_t, uint32_t, uint32_t))0x15c440)(id, 0, 0, 1); }

// Items page: the quick-use items, with what the game's Item screen does for them (FUN_0015e430 / FUN_00160ad0):
// Herb (0x01) heals 0x40, Medicine (0x02) heals fully. Others (Talisman 0x03 revives by itself, upgrades 0x05/0x06,
// key items) stay in the Item screen.
static const int quick_items[] = {0x01, 0x02};

static int owned_items(int *ids)
{
    int n = 0;
    for (int k = 0; k < (int)(sizeof quick_items / sizeof *quick_items); k++)
        if (item_count(quick_items[k]) > 0) ids[n++] = 0x100 + quick_items[k];
    return n;
}

static void item_slot(int i, int id)
{
    wheel.id[i] = id;
    wheel.sub[i][0] = wheel.badge[i][0] = 0;
    wheel.cycle[i] = 0;
    if (id < 0) { snprintf(wheel.name[i], 24, "No items"); return; }
    snprintf(wheel.name[i], 24, "%s", (id & 0xff) == 1 ? "Herb" : "Medicine");
    snprintf(wheel.sub[i], 24, "x %d", item_count(id & 0xff));
}

static void use_item(int item)   // as the Item screen does: heal, use one up, update the health bar, play its sound
{
    volatile int16_t *hp = (volatile int16_t *)G_HP, max = hp[-1];   // maximum at G_MAX_HP, just before
    if (*hp >= max || item_count(item) <= 0) { game_sound(SND_CANT); return; }
    *hp = item == 1 && *hp + 0x40 < max ? *hp + 0x40 : max;
    ((int(__cdecl *)(int, int))0x15ebd0)(item, -1);
    ((void(__cdecl *)(void))0x193c0)();
    game_sound(SND_DECIDE);
    xlog("[mod] used item %02x, health %d/%d", item, *hp, max);
}

static void equip(int id)
{
    *(volatile uint8_t *)G_WEAPON_MAGIC = id < 9 ? (uint8_t)(id / 3) : 0xff;
    *(volatile uint8_t *)G_EQUIP_RESET = 0;
    *(volatile uint8_t *)G_WEAPON = (uint8_t)id;
    ((void(__cdecl *)(uint32_t, uint32_t, int, int, int))0x10cc50)(G_PLAYER, ((uint8_t *)G_PLAYER)[2], -1, id, 0);
    float prev[4];
    memcpy(prev, (void *)G_PLAYER_PREV_POS, sizeof prev);
    ((void(__cdecl *)(void))0x115120)();
    memcpy((void *)G_PLAYER_PREV_POS, prev, sizeof prev);
    xlog("[mod] equipped weapon %d", id);
}

// Randomizer: an orb level as an item (docs/randomizer.md, 4.5). The game names and draws each orb at each level
// (items 0x63-0x74) but has no use for them: picked up, one takes its orb's inventory slot (3 + element) and nothing
// rises. Seen there, it counts as one level up: the orb goes back in its slot, its orb and sword levels rise by one (to
// 2, the most), the souls put into it rise to that level's mark (the enhance screen goes by those, not the levels),
// and the sword is equipped again at the new level if it's in hand. ponytail: a level item for an orb not yet owned is
// dropped (logged) there. A randomizer run gives its six level items (0x64/0x65, 0x67/0x68, 0x6a/0x6b) slots of their own
// (LEVEL_SLOTS, unused by any item; randomizer.c): there one waits until its orb is held, then counts and goes.
#define G_ORB_LEVEL G_LEVELS      // 3 bytes, as G_SWORD_LEVEL (the game keeps them equal)
#define G_ORB_SOULS 0x005473b8u   // u32 per orb: souls put in at the enhance screen
#define ORB_MARKS 0x001febd0u     // u32 x3 per orb (12 bytes apart): souls for levels 2 and 3, then 64000
#define LEVEL_SLOTS RANDO_LEVEL_SLOT0   // 49-54: the run's level items (randomizer.c gives them these slots)
static int level_up(int e, int id)   // one level for orb e; returns the weapon to equip again, or -1
{
    volatile uint8_t *orb = (volatile uint8_t *)G_ORB_LEVEL + e, *sword = (volatile uint8_t *)G_SWORD_LEVEL + e;
    int was = *sword, now = was < 2 ? was + 1 : 2;
    *orb = *sword = (uint8_t)now;
    volatile uint32_t *souls = (volatile uint32_t *)G_ORB_SOULS + e;
    uint32_t mark = now ? ((const uint32_t *)(ORB_MARKS + 12 * e))[now - 1] : 0;
    if (*souls < mark) *souls = mark;
    xlog("[orb] level item %02x: orb %d level %d -> %d", id, e, was + 1, now + 1);
    return *(volatile uint8_t *)G_WEAPON == e * 3 + was && now != was ? e * 3 + now : -1;
}
void orb_items_frame(void)
{
    static uint8_t tick;
    static int requip = -1;
    rando_orb_levels();   // before a level item waiting for that orb counts
    uint8_t t = *(volatile uint8_t *)G_PLAY_TICK;
    if (t == tick) return;   // in play only (not in menus, where the pickup screen is)
    tick = t;
    if (requip >= 0 && *(volatile uint8_t *)G_PLAYER_MODE == 1 && !(*(volatile uint32_t *)G_PLAYER_ACTION & ACTION_BUSY))
        equip(requip), requip = -1;
    for (int e = 0; e < 3; e++) {
        volatile uint8_t *slot = (volatile uint8_t *)(G_INVENTORY + 4 * (3 + e));
        int id = slot[0];
        if (id < 0x63 || id > 0x74 || (id - 0x63) % 9 / 3 != e) continue;
        if (*((volatile uint8_t *)G_SWORD_LEVEL + e) > 2) { slot[0] = slot[1] = 0; xlog("[orb] level item %02x for orb %d, not owned: dropped", id, e); continue; }
        int r = level_up(e, id);
        slot[0] = (uint8_t)(0x11 + e), slot[1] = 0xff;
        if (r >= 0) requip = r;
    }
    for (int k = LEVEL_SLOTS; k < LEVEL_SLOTS + 6; k++) {   // a run's level items, in slots of their own
        volatile uint8_t *slot = (volatile uint8_t *)(G_INVENTORY + 4 * k);
        int id = slot[0];
        if (id < 0x63 || id > 0x74) continue;
        int e = (id - 0x63) % 9 / 3;
        if (*((volatile uint8_t *)G_SWORD_LEVEL + e) > 2) continue;   // its orb isn't held yet: it waits
        int r = level_up(e, id);
        slot[0] = slot[1] = slot[2] = slot[3] = 0;
        if (r >= 0) requip = r;
    }
}

// Fills the wheel with page 0 (weapons, the current one selected) or 1 (items; "No items" if there are none).
static int fill_wheel(int page, int *ids, int cur)
{
    int n = page ? owned_items(ids) : owned_weapons(ids);
    if (!n) ids[n++] = -1;
    wheel.page = page;
    wheel.sel = 0;
    for (int i = 0; i < n; i++) {
        if (page) item_slot(i, ids[i]); else wheel_slot(i, ids[i]);
        if (!page && ids[i] == cur) wheel.sel = i;
    }
    return n;
}

static void weapon_button(struct x_gamepad *g)
{
    static int was_down, target = -1, use = -1, ids[8], n, lb_was, rb_was, lt_was, rt_was, rs_was, a_was, held_after_a;
    static ULONGLONG down_ms;
    int modern = settings.modern_controls, use_wheel = settings.weapon_wheel, slot = modern ? XA_RTRIGGER : XA_B;
    int down = g->analog[slot] > 0x80;
    if ((!modern && !use_wheel) || !in_gameplay()) { was_down = 0; target = use = -1; wheel.n = 0; return; }
    g->analog[slot] = 0;   // the button belongs to the mods during play
    // Kaede fights with her own dagger and has no weapons to change to (equipping one of Samanosuke's put his sword in
    // her hands): for her the wheel is its Items page alone and a tap does nothing. The inventory is the one they share,
    // and the health it heals is whoever is being played.
    int kaede = *(volatile uint8_t *)G_PLAYER_ID != 0;
    ULONGLONG now = GetTickCount64();
    int cur = *(volatile uint8_t *)G_WEAPON;
    if (down && !was_down) down_ms = now;
    if (!down) held_after_a = 0;
    if (use_wheel && down && !wheel.n && !held_after_a && now - down_ms >= WHEEL_HOLD_MS) {   // opens on the weapons page
        n = fill_wheel(kaede, ids, cur);
        wheel.n = n;
        lt_was = g->analog[XA_LTRIGGER] > 0x80; rt_was = g->analog[XA_RTRIGGER] > 0x80; rs_was = abs(g->rx) > 0x4000;
        a_was = g->analog[XA_A] > 0x80;
        game_sound(SND_OPEN);
    }
    if (wheel.n) {   // slot 0 at the top, clockwise; the stick (or d-pad) points at one
        // The other page: LT or RT, or a flick of the right stick left or right (with Modern controls RT holds the
        // wheel open, so the stick is the way there). The stick re-arms once it is back near the middle.
        int lt = g->analog[XA_LTRIGGER] > 0x80, rt = g->analog[XA_RTRIGGER] > 0x80;
        int rs = abs(g->rx) > 0x5000 ? 1 : abs(g->rx) < 0x3000 ? 0 : rs_was;
        if (!kaede && ((lt && !lt_was) || (rt && !rt_was) || (rs && !rs_was))) { wheel.n = n = fill_wheel(!wheel.page, ids, cur); game_sound(SND_MOVE); }
        lt_was = lt; rt_was = rt; rs_was = rs;
        float sx = g->lx / 32767.0f, sy = g->ly / 32767.0f;
        if (g->buttons & 0xF) { sx = (g->buttons & 8 ? 1.f : 0) - (g->buttons & 4 ? 1.f : 0); sy = (g->buttons & 1 ? 1.f : 0) - (g->buttons & 2 ? 1.f : 0); }
        if (sx * sx + sy * sy > 0.25f) {
            float a = atan2f(sx, sy);   // 0 = up, clockwise
            if (a < 0) a += 6.2831853f;
            int sel = (int)(a / (6.2831853f / wheel.n) + 0.5f) % wheel.n;
            if (sel != wheel.sel) game_sound(SND_MOVE);
            wheel.sel = sel;
        }
        int lb = g->analog[XA_WHITE] > 0x80, rb = g->analog[XA_BLACK] > 0x80, l = launcher(ids[wheel.sel]), a[2], m;
        if (((lb && !lb_was) || (rb && !rb_was)) && l >= 0 && (m = owned_ammo(l, a)) > 1) {   // LB/RB: the launcher's ammo
            int k = 0;
            while (k < m && a[k] != ids[wheel.sel]) k++;
            ids[wheel.sel] = ammo_choice[l] = a[(k + (rb && !rb_was ? 1 : m - 1)) % m];
            wheel_slot(wheel.sel, ids[wheel.sel]);
            game_sound(SND_MOVE);
        }
        lb_was = lb; rb_was = rb;
        // A: use the highlighted item, or equip the highlighted weapon, and close. Items only go through A, so letting
        // go of the wheel button by itself never uses one by accident.
        int a_now = g->analog[XA_A] > 0x80;
        if (a_now && !a_was && ids[wheel.sel] >= 0) {
            if (wheel.page) use = ids[wheel.sel] & 0xff;
            else { target = ids[wheel.sel]; game_sound(target != cur ? SND_DECIDE : SND_CANCEL); }
            wheel.n = 0;
            held_after_a = down;   // don't reopen while the wheel button is still held
        }
        a_was = a_now;
        memset(g, 0, sizeof *g);   // the game sees nothing while the wheel is open
    }
    // Next weapon: on the press itself without the wheel; with it, on letting go of a tap (a hold opens the wheel)
    int tap = modern && !kaede && (use_wheel ? !down && was_down && now - down_ms < WHEEL_HOLD_MS : down && !was_down);
    if (!down && was_down && wheel.n) {
        if (wheel.page) game_sound(SND_CANCEL);   // items: letting go only closes (A uses one)
        else { target = ids[wheel.sel]; game_sound(target != cur ? SND_DECIDE : SND_CANCEL); }
        wheel.n = 0;
    }
    else if (tap) {
        if ((n = owned_weapons(ids)) >= 2) {
            int i = 0;
            while (i < n && ids[i] != cur) i++;
            target = ids[i < n ? (i + 1) % n : 0];
            game_sound(SND_DECIDE);
        }
    }
    was_down = down;
    // A choice made during an attack, a hit or a knockdown waits for it to finish instead of cancelling it: equipping
    // resets him to standing (G_EQUIP_RESET), so done while he wasn't in control it cut short whatever he was in.
    int ready = *(volatile uint8_t *)G_PLAYER_MODE == 1 && !(*(volatile uint32_t *)G_PLAYER_ACTION & ACTION_BUSY);
    if (target >= 0 && ready) {
        if (target != cur) equip(target);
        target = -1;
    }
    if (use >= 0 && ready) { use_item(use); use = -1; }
}

static int is_gamepad(uint32_t type) { return type == XADDR_DEVICETYPE_GAMEPAD; }

static void __stdcall x_XInitDevices(uint32_t count, uint32_t types) {}
static uint32_t __stdcall x_XGetDevices(uint32_t type) { return is_gamepad(type) ? 1 : 0; }
static uint32_t __stdcall x_XGetDeviceChanges(uint32_t type, uint32_t pIns, uint32_t pRem)
{
    static int reported;
    *(uint32_t *)pIns = *(uint32_t *)pRem = 0;
    if (!is_gamepad(type) || reported) return 0;
    reported = 1;
    *(uint32_t *)pIns = 1;
    return 1;
}
static uint32_t __stdcall x_XInputOpen(uint32_t type, uint32_t port, uint32_t slot, uint32_t poll)
{
    if (!is_gamepad(type) || port != 0) { SetLastError(ERROR_DEVICE_NOT_CONNECTED); return 0; }
    return 0x5A0;   // any non-null handle
}
static uint32_t __stdcall x_XInputClose(uint32_t h) { return 0; }
static uint32_t __stdcall x_XInputGetCapabilities(uint32_t h, uint32_t pCaps)
{
    uint8_t *c = (uint8_t *)pCaps;   // BYTE SubType; WORD Reserved; XINPUT_GAMEPAD In; XINPUT_RUMBLE Out
    memset(c, 0xFF, 26);
    c[0] = 1;                        // XINPUT_DEVSUBTYPE_GC_GAMEPAD
    c[1] = c[2] = c[3] = 0;
    return 0;
}
// A or Start skips the Capcom logo and the violence warning. The start-up (FUN_00015a80, its state at 0x29bca8: stage,
// step, ..., a frame count at +6) holds each picture through steps 2-3 (the logo) and 5-6 (the warning), copying the
// first rooms to the cache drive in between; a press counts the rest of that picture's holds down to one frame, so the
// copy and the fade out still happen. (The hard disk messages before them already skip with A.)
static void skip_logo(const struct x_gamepad *g, const struct x_gamepad *k)
{
    static int was, skipping;   // skipping: the picture (1 logo, 2 warning) a press asked to skip
    volatile uint8_t *st = (volatile uint8_t *)0x0029bca8;
    int press = g->analog[XA_A] > 0x80 || k->analog[XA_A] > 0x80 || ((g->buttons | k->buttons) & 0x30);
    int pic = st[0] != 1 ? 0 : st[1] == 2 || st[1] == 3 ? 1 : st[1] == 5 || st[1] == 6 ? 2 : 0;
    if (press && !was && pic) skipping = pic;
    was = press;
    if (!pic || pic != skipping) { if (!pic) skipping = 0; return; }
    // Not while it is fading in (fade state 0x3161ea: 1 = fading; 2 just done, 0 idle again): cut short mid-fade, the
    // step that follows asks for a fade out the fader refuses, and the next one waits for it for ever (a hang on the
    // warning). The press is kept until the fade ends.
    if (*(volatile uint8_t *)0x003161ea != 1 && *(volatile int16_t *)(st + 6) > 1) *(volatile int16_t *)(st + 6) = 1;
}

static uint32_t __stdcall x_XInputGetState(uint32_t h, uint32_t pState)
{
    struct x_gamepad g, k;
    read_pad(&g, &k);
    warp_frame();
    skip_logo(&g, &k);
    if (!settings.modern_controls) merge(&g, &k);   // the keyboard's B opens the wheel too
    weapon_button(&g);   // first: while the wheel is open it hides the pad from stick_move and the game
    stick_move(&g);
    if (settings.modern_controls) { modern_layout(&g); if (!wheel.n) merge(&g, &k); }   // the open wheel hides the keyboard too
    if (memcmp(&g, &last, sizeof g)) { last = g; packet++; }
    *(uint32_t *)pState = packet;
    memcpy((uint8_t *)pState + 4, &g, sizeof g);
    return 0;
}
// XINPUT_FEEDBACK: header { DWORD dwStatus; HANDLE hEvent; BYTE Reserved[58] }, then rumble { WORD left, right }.
static uint32_t __stdcall x_XInputSetState(uint32_t h, uint32_t pFeedback)
{
    uint8_t *f = (uint8_t *)pFeedback;
    XINPUT_VIBRATION v = {*(uint16_t *)(f + 66), *(uint16_t *)(f + 68)};
    if (sdl_state > 0) { for (int i = 0; i < MAX_PADS; i++) if (pads[i]) SDL_RumbleGamepad(pads[i], v.wLeftMotorSpeed, v.wRightMotorSpeed, 0xFFFF); }   // ~65 s: XInput rumble lasts until the game changes it
    else XInputSetState(0, &v);
    *(uint32_t *)f = 0;
    if (*(HANDLE *)(f + 4)) SetEvent(*(HANDLE *)(f + 4));
    return ERROR_IO_PENDING;
}
static uint32_t __stdcall x_XMountMUA(uint32_t port, uint32_t slot, uint32_t pDrive) { return ERROR_DEVICE_NOT_CONNECTED; }
static uint32_t __stdcall x_XUnmountMU(uint32_t port, uint32_t slot) { return 0; }

const struct hle_impl input_impls[] = {
    {"XInitDevices", x_XInitDevices}, {"XGetDevices", x_XGetDevices}, {"XGetDeviceChanges", x_XGetDeviceChanges},
    {"XInputOpen", x_XInputOpen}, {"XInputClose", x_XInputClose}, {"XInputGetCapabilities", x_XInputGetCapabilities},
    {"XInputGetState", x_XInputGetState}, {"XInputSetState", x_XInputSetState}, {"XMountMUA", x_XMountMUA},
    {"XUnmountMU", x_XUnmountMU},
    {0, 0},
};
