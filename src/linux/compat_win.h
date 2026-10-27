// Linux: the window, keyboard and GDI slice of Win32 (compat_win.c, compat_gdi.c). The window is an SDL3 window, which
// DXVK Native takes as the HWND; keys come from SDL's keyboard state, so GetAsyncKeyState sees what the window sees.
#pragma once

// ---- keys -----------------------------------------------------------------------------------------------------
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_PAUSE 0x13
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_NUMPAD0 0x60
#define VK_MULTIPLY 0x6A
#define VK_ADD 0x6B
#define VK_SUBTRACT 0x6D
#define VK_DECIMAL 0x6E
#define VK_DIVIDE 0x6F
#define VK_F1 0x70
#define VK_F10 0x79
#define VK_F11 0x7A
#define VK_F12 0x7B
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU 0xA4
#define VK_RMENU 0xA5
#define VK_OEM_1 0xBA
#define VK_OEM_PLUS 0xBB
#define VK_OEM_COMMA 0xBC
#define VK_OEM_MINUS 0xBD
#define VK_OEM_PERIOD 0xBE
#define VK_OEM_2 0xBF
#define VK_OEM_3 0xC0
#define VK_OEM_4 0xDB
#define VK_OEM_5 0xDC
#define VK_OEM_6 0xDD
#define VK_OEM_7 0xDE
SHORT GetAsyncKeyState(int vk);
HWND GetForegroundWindow(void);
DWORD GetWindowThreadProcessId(HWND w, DWORD *pid);
BOOL GetClientRect(HWND w, RECT *r);
// The game window: an SDL3 window (Vulkan), shown, borderless filling the screen or windowed at w x h. Its events are
// handled on the calling thread by compat_window_loop, which returns when the window is closed.
HWND compat_window_create(const wchar_t *title, int w, int h, int borderless, int background);
const char *compat_window_error(void);   // why compat_window_create returned 0
void compat_window_borderless(HWND w, int on);
void compat_window_loop(HWND w, void (*toggle_borderless)(void));
#define DBG_PRINTEXCEPTION_C 0x40010006u

// ---- GDI (compat_gdi.c): bitmaps and text are real, paths aren't drawn -------------------------------------------
#pragma pack(push, 2)
typedef struct { WORD bfType; DWORD bfSize; WORD bfReserved1, bfReserved2; DWORD bfOffBits; } BITMAPFILEHEADER;
#pragma pack(pop)
typedef struct { DWORD biSize; LONG biWidth, biHeight; WORD biPlanes, biBitCount; DWORD biCompression, biSizeImage;
                 LONG biXPelsPerMeter, biYPelsPerMeter; DWORD biClrUsed, biClrImportant; } BITMAPINFOHEADER;
typedef struct { BYTE rgbBlue, rgbGreen, rgbRed, rgbReserved; } RGBQUAD;
typedef struct { BITMAPINFOHEADER bmiHeader; RGBQUAD bmiColors[1]; } BITMAPINFO;
typedef struct { FLOAT eM11, eM12, eM21, eM22, eDx, eDy; } XFORM;
typedef struct { UINT lbStyle; COLORREF lbColor; ULONG_PTR lbHatch; } LOGBRUSH;
typedef void *HPEN;
#define BI_RGB 0
#define DIB_RGB_COLORS 0
#define FW_NORMAL 400
#define FW_SEMIBOLD 600
#define FW_BOLD 700
#define DEFAULT_CHARSET 1
#define OUT_TT_ONLY_PRECIS 7
#define ANTIALIASED_QUALITY 4
#define NONANTIALIASED_QUALITY 3
#define TRANSPARENT 1
#define OPAQUE 2
#define TA_LEFT 0
#define TA_RIGHT 2
#define TA_CENTER 6
#define TA_BOTTOM 8
#define TA_BASELINE 24
#define GM_ADVANCED 2
#define DT_CENTER 0x1
#define DT_RIGHT 0x2
#define DT_VCENTER 0x4
#define DT_BOTTOM 0x8
#define DT_WORDBREAK 0x10
#define DT_SINGLELINE 0x20
#define DT_NOCLIP 0x100
#define DT_CALCRECT 0x400
#define DT_NOPREFIX 0x800
#define BLACKNESS 0x00000042
#define WHITENESS 0x00FF0062
#define WHITE_BRUSH 0
#define BLACK_BRUSH 4
#define BS_SOLID 0
#define PS_SOLID 0
#define PS_GEOMETRIC 0x10000
#define PS_ENDCAP_SQUARE 0x100
#define PS_JOIN_MITER 0x2000
#define RT_RCDATA ((const wchar_t *)10)
#define MAKEINTRESOURCEW(i) ((const wchar_t *)(uintptr_t)(i))
#define MAKEINTRESOURCEA(i) ((const char *)(uintptr_t)(i))
HDC CreateCompatibleDC(HDC dc);
BOOL DeleteDC(HDC dc);
HBITMAP CreateDIBSection(HDC dc, const BITMAPINFO *bi, UINT usage, void **bits, HANDLE section, DWORD offset);
HGDIOBJ SelectObject(HDC dc, HGDIOBJ o);
BOOL DeleteObject(HGDIOBJ o);
HGDIOBJ GetStockObject(int i);
HFONT CreateFontW(int h, int w, int esc, int orient, int weight, DWORD italic, DWORD under, DWORD strike, DWORD charset,
                  DWORD outprec, DWORD clipprec, DWORD quality, DWORD pitch, const wchar_t *face);
HFONT CreateFontA(int h, int w, int esc, int orient, int weight, DWORD italic, DWORD under, DWORD strike, DWORD charset,
                  DWORD outprec, DWORD clipprec, DWORD quality, DWORD pitch, const char *face);
int SetBkMode(HDC dc, int mode);
COLORREF SetTextColor(HDC dc, COLORREF c);
UINT SetTextAlign(HDC dc, UINT a);
int SetGraphicsMode(HDC dc, int m);
BOOL SetWorldTransform(HDC dc, const XFORM *x);
BOOL SetMiterLimit(HDC dc, FLOAT l, FLOAT *old);
BOOL TextOutW(HDC dc, int x, int y, const wchar_t *s, int n);
int DrawTextW(HDC dc, const wchar_t *s, int n, RECT *r, UINT f);
int DrawTextA(HDC dc, const char *s, int n, RECT *r, UINT f);
BOOL GdiFlush(void);
BOOL PatBlt(HDC dc, int x, int y, int w, int h, DWORD rop);
BOOL BeginPath(HDC dc);
BOOL EndPath(HDC dc);
BOOL FillPath(HDC dc);
BOOL StrokePath(HDC dc);
BOOL StrokeAndFillPath(HDC dc);
int GetPath(HDC dc, POINT *p, BYTE *types, int n);
BOOL PolyDraw(HDC dc, const POINT *p, const BYTE *types, int n);
HPEN ExtCreatePen(DWORD style, DWORD width, const LOGBRUSH *b, DWORD n, const DWORD *dashes);
HANDLE AddFontMemResourceEx(void *data, DWORD size, void *res, DWORD *count);
HRSRC FindResourceW(HMODULE m, const wchar_t *name, const wchar_t *type);
HGLOBAL LoadResource(HMODULE m, HRSRC r);
void *LockResource(HGLOBAL g);
DWORD SizeofResource(HMODULE m, HRSRC r);
