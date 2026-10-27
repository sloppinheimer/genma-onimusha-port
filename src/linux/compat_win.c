// Linux: the game window (SDL3) and the keyboard as GetAsyncKeyState reads it (compat_win.h).
#include <windows.h>
#include <unistd.h>
#include <SDL3/SDL.h>

static SDL_Window *win;
static char win_error[256];   // why the window couldn't be made (compat_window_error)

HWND compat_window_create(const wchar_t *title, int w, int h, int borderless, int background)
{
    char t[256];
    WideCharToMultiByte(CP_UTF8, 0, title, -1, t, sizeof t, 0, 0);
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        snprintf(win_error, sizeof win_error, "no display: %s", SDL_GetError());
        return 0;
    }
    win = SDL_CreateWindow(t, w, h, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | (borderless ? SDL_WINDOW_FULLSCREEN : 0)
                                    | (background ? SDL_WINDOW_HIDDEN : 0));
    if (!win) snprintf(win_error, sizeof win_error, "%s", SDL_GetError());
    return (HWND)win;
}
const char *compat_window_error(void) { return win_error; }
void compat_window_borderless(HWND w, int on) { SDL_SetWindowFullscreen((SDL_Window *)w, on); }
// Events for the window: closing it ends the game (as WM_CLOSE does), Alt+Enter and F11 switch full screen. The
// events are taken every 5 ms rather than waited for: SDL_WaitEventTimeout never slept on the Steam Deck (the
// controller's stream of events, and pumping X11 and D-Bus for each), and the thread took a whole core in Gaming Mode.
// The game reads the keyboard state and the controllers itself (GetAsyncKeyState, input.c), so 5 ms only delays these.
void compat_window_loop(HWND w, void (*toggle_borderless)(void))
{
    (void)w;
    SDL_Event e;
    for (;;) {
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                void xlog(const char *fmt, ...);
                xlog("[d3d] window closed");
                ExitProcess(0);
            }
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat
                && ((e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT)) || e.key.key == SDLK_F11)) toggle_borderless();
        }
        SDL_Delay(5);
    }
}
BOOL GetClientRect(HWND w, RECT *r)
{
    int x = 0, y = 0;
    if (w) SDL_GetWindowSizeInPixels((SDL_Window *)w, &x, &y);
    *r = (RECT){0, 0, x, y};
    return w != 0;
}
HWND GetForegroundWindow(void) { return win && SDL_GetKeyboardFocus() == win ? (HWND)win : 0; }
DWORD GetWindowThreadProcessId(HWND w, DWORD *pid) { if (pid) *pid = w ? (DWORD)getpid() : 0; return 0; }

static SDL_Scancode scancode(int vk)
{
    if (vk >= 'A' && vk <= 'Z') return (SDL_Scancode)(SDL_SCANCODE_A + vk - 'A');
    if (vk >= '1' && vk <= '9') return (SDL_Scancode)(SDL_SCANCODE_1 + vk - '1');
    if (vk == '0') return SDL_SCANCODE_0;
    if (vk >= VK_F1 && vk <= VK_F1 + 11) return (SDL_Scancode)(SDL_SCANCODE_F1 + vk - VK_F1);
    if (vk >= VK_NUMPAD0 + 1 && vk <= VK_NUMPAD0 + 9) return (SDL_Scancode)(SDL_SCANCODE_KP_1 + vk - VK_NUMPAD0 - 1);
    switch (vk) {
    case VK_NUMPAD0: return SDL_SCANCODE_KP_0;
    case VK_BACK: return SDL_SCANCODE_BACKSPACE;   case VK_TAB: return SDL_SCANCODE_TAB;
    case VK_RETURN: return SDL_SCANCODE_RETURN;    case VK_ESCAPE: return SDL_SCANCODE_ESCAPE;
    case VK_SPACE: return SDL_SCANCODE_SPACE;      case VK_PRIOR: return SDL_SCANCODE_PAGEUP;
    case VK_NEXT: return SDL_SCANCODE_PAGEDOWN;    case VK_END: return SDL_SCANCODE_END;
    case VK_HOME: return SDL_SCANCODE_HOME;        case VK_LEFT: return SDL_SCANCODE_LEFT;
    case VK_UP: return SDL_SCANCODE_UP;            case VK_RIGHT: return SDL_SCANCODE_RIGHT;
    case VK_DOWN: return SDL_SCANCODE_DOWN;        case VK_INSERT: return SDL_SCANCODE_INSERT;
    case VK_DELETE: return SDL_SCANCODE_DELETE;    case VK_PAUSE: return SDL_SCANCODE_PAUSE;
    case VK_LSHIFT: return SDL_SCANCODE_LSHIFT;    case VK_RSHIFT: return SDL_SCANCODE_RSHIFT;
    case VK_LCONTROL: return SDL_SCANCODE_LCTRL;   case VK_RCONTROL: return SDL_SCANCODE_RCTRL;
    case VK_LMENU: return SDL_SCANCODE_LALT;       case VK_RMENU: return SDL_SCANCODE_RALT;
    case VK_MULTIPLY: return SDL_SCANCODE_KP_MULTIPLY; case VK_ADD: return SDL_SCANCODE_KP_PLUS;
    case VK_SUBTRACT: return SDL_SCANCODE_KP_MINUS; case VK_DECIMAL: return SDL_SCANCODE_KP_PERIOD;
    case VK_DIVIDE: return SDL_SCANCODE_KP_DIVIDE;
    case VK_OEM_1: return SDL_SCANCODE_SEMICOLON;  case VK_OEM_PLUS: return SDL_SCANCODE_EQUALS;
    case VK_OEM_COMMA: return SDL_SCANCODE_COMMA;  case VK_OEM_MINUS: return SDL_SCANCODE_MINUS;
    case VK_OEM_PERIOD: return SDL_SCANCODE_PERIOD; case VK_OEM_2: return SDL_SCANCODE_SLASH;
    case VK_OEM_3: return SDL_SCANCODE_GRAVE;      case VK_OEM_4: return SDL_SCANCODE_LEFTBRACKET;
    case VK_OEM_5: return SDL_SCANCODE_BACKSLASH;  case VK_OEM_6: return SDL_SCANCODE_RIGHTBRACKET;
    case VK_OEM_7: return SDL_SCANCODE_APOSTROPHE;
    }
    return SDL_SCANCODE_UNKNOWN;
}
SHORT GetAsyncKeyState(int vk)
{
    if (!win) return 0;
    const bool *k = SDL_GetKeyboardState(0);
    int down;
    if (vk == VK_SHIFT) down = k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT];
    else if (vk == VK_CONTROL) down = k[SDL_SCANCODE_LCTRL] || k[SDL_SCANCODE_RCTRL];
    else if (vk == VK_MENU) down = k[SDL_SCANCODE_LALT] || k[SDL_SCANCODE_RALT];
    else { SDL_Scancode s = scancode(vk); down = s != SDL_SCANCODE_UNKNOWN && k[s]; }
    return down ? (SHORT)0x8000 : 0;
}
