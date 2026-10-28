// Genma Onimusha launcher: a borderless window hosting the HTML interface (ui/index.html, embedded) in WebView2,
// plus the native side it talks to: onimusha.ini, installing the game from the player's disc image, art taken from
// that disc, controller and display detection, the mods\ list (mods\mods.txt), and starting the game.
//
// Messages, page -> host (JSON): {type: ready|set|key|play|pickDisc|open|shortcut|update|reset|window|drag|mod|modMove|
//                                       modInstall|modRemove|packGet|packCancel|randoCreate|randoDelete|importSave, ...}
//                host -> page: {type: state|progress|setupError|toast|running|pad|button|update|packpct|randoMade, ...}
// on_message is the one place the page's messages are handled: every type above, and nothing else.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <xinput.h>
#include <bcrypt.h>
#include <winhttp.h>
#include <wrl.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <set>
#include <cstdio>
#include <cstdint>
#include "WebView2.h"
#include "../src/host/settings_default.h"
#include "../src/host/mods_list.h"
#include "../src/host/layout.h"
#include "../src/host/achievements_list.h"
#include "text.h"
#include "disc.h"
#include "files.h"
#include "unzip.h"
#include "resource.h"
#include "randomizer.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "xinput.lib")
#pragma comment(lib, "bcrypt.lib")

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

static const char *const VERSION = ONI_VERSION;
static const char *const USA_XBE_SHA1 = "08bfe432eb55877b5af2dd401b713743c5b751cd";
enum { WM_APP_JSON = WM_APP + 1, WM_APP_GAME_EXITED, WM_APP_SETUP_DONE, WM_APP_UPDATED, WM_APP_STATE, WM_APP_RUN_MADE };

static HWND hwnd;
static ComPtr<ICoreWebView2Controller> controller;
static double fit_zoom = 1;   // the page's zoom: below 1 where the window had to shrink to fit the screen (fit_window)
static ComPtr<ICoreWebView2> webview;
static std::wstring exe_dir, data_dir;
static std::atomic<bool> installing{false}, running{false};
static bool pad_connected;
static std::wstring pending_iso;   // a disc image given on the command line (or dropped on the exe), installed once the page is up

static void mark(const char *what);
// Send JSON to the page. Safe from any thread: other threads hand it to the window's thread.
static void post(const std::string &json)
{
    if (GetCurrentThreadId() != GetWindowThreadProcessId(hwnd, 0)) { PostMessageW(hwnd, WM_APP_JSON, 0, (LPARAM)new std::string(json)); return; }
    if (webview && FAILED(webview->PostWebMessageAsJson(wide(json).c_str()))) mark(("page refused a message: " + json.substr(0, 80)).c_str());
}
static void toast(const std::string &text) { post("{\"type\":\"toast\",\"text\":" + jstr(text) + "}"); }

// Startup timings in <data>\startup.log, rewritten each start: shows what keeps the window empty, if anything does.
static DWORD t_start = GetTickCount();
static void mark(const char *what)
{
    static FILE *f;
    if (!f && !data_dir.empty()) f = _wfopen((data_dir + L"\\startup.log").c_str(), L"w");
    if (f) { fprintf(f, "%6lu ms  %s\n", GetTickCount() - t_start, what); fflush(f); }
}

// ---- onimusha.ini: values change in place, comments stay -------------------------------------------
static std::wstring ini_path() { return exe_dir + L"\\onimusha.ini"; }
// ok (if given): false when the file is there but couldn't be read, so nothing may be written over it
static std::vector<std::string> ini_load(bool *ok = nullptr)
{
    std::vector<std::string> lines;
    if (ok) *ok = true;
    FILE *f = _wfopen(ini_path().c_str(), L"r");
    if (!f && file_missing(ini_path().c_str())) {   // first run: the same commented file the game writes
        write_text_atomic(ini_path().c_str(), settings_default_file);   // whole or not at all (mods_list.h)
        f = _wfopen(ini_path().c_str(), L"r");
    }
    if (!f) { if (ok) *ok = false; return lines; }
    char buf[1024];
    while (fgets(buf, sizeof buf, f)) { std::string l = buf; while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back(); lines.push_back(l); }
    fclose(f);
    return lines;
}
static std::string ini_get(const std::vector<std::string> &lines, const char *key, const char *def)
{
    std::string k, v;
    for (auto &l : lines) if (line_key(l, &k, &v) && !_stricmp(k.c_str(), key)) return v;
    return def;
}
static const char *section_of(const std::string &key) { return ini_section_of(key); }   // text.h
static void ini_set(const std::string &key, const std::string &value)   // the rules and the line editing: text.h (tested)
{
    if (!ini_set_ok(key, value)) { mark(("ini_set refused: " + key.substr(0, 40)).c_str()); return; }
    bool ok;
    auto lines = ini_load(&ok);
    if (!ok) { toast("Couldn't read onimusha.ini, so the setting wasn't saved. Is it open in another program?"); return; }
    ini_set_lines(lines, key, value);
    std::string out;
    for (auto &l : lines) out += l + "\r\n";
    if (!write_file_atomic(ini_path().c_str(), out.data(), out.size())) toast("Couldn't save onimusha.ini next to the launcher.");
}

// ---- game files: install from the disc image ----------------------------------------------------------
static std::wstring game_dir() { return exe_dir + L"\\game"; }
// game\.installing is there from an install's start until it finishes: an install that stopped (closed, disk full) isn't
// ready to play, and the next one copies only what it didn't get to.
static bool game_ready() { return file_exists(game_dir() + L"\\default.xbe") && !file_exists(game_dir() + L"\\.installing"); }

// Hex digest of a file: SHA-1 (game check) or SHA-256 (update check).
static std::string hash_file(const std::wstring &path, bool sha256 = false)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (f == INVALID_HANDLE_VALUE) return {};
    BCRYPT_HASH_HANDLE h = 0;
    BCryptCreateHash(sha256 ? BCRYPT_SHA256_ALG_HANDLE : BCRYPT_SHA1_ALG_HANDLE, &h, 0, 0, 0, 0, 0);
    std::vector<uint8_t> buf(1 << 20);
    DWORD n;
    while (ReadFile(f, buf.data(), (DWORD)buf.size(), &n, 0) && n) BCryptHashData(h, buf.data(), n, 0);
    CloseHandle(f);
    uint8_t d[32];
    ULONG len = sha256 ? 32 : 20;
    BCryptFinishHash(h, d, len, 0);
    BCryptDestroyHash(h);
    std::string s;
    char b[3];
    for (ULONG i = 0; i < len; i++) { snprintf(b, sizeof b, "%02x", d[i]); s += b; }
    return s;
}


// Delete a file or folder tree (path must be a whole path). FOF_NO_UI and flags: FOF_ALLOWUNDO to recycle.
static bool sh_delete(const std::wstring &path, FILEOP_FLAGS flags = 0)
{
    std::wstring dz = path + L'\0';
    SHFILEOPSTRUCTW op = {hwnd, FO_DELETE, dz.c_str(), 0, (FILEOP_FLAGS)(FOF_NO_UI | flags)};
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}
// The disc image's file list: disc.h (tested by selftest), read through read_handle.
static bool read_at(HANDLE f, uint64_t off, void *buf, DWORD n)
{
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)off;
    DWORD got = 0;
    return SetFilePointerEx(f, li, 0, FILE_BEGIN) && ReadFile(f, buf, n, &got, 0) && got == n;
}
static bool read_handle(void *f, uint64_t off, void *buf, uint32_t n) { return read_at((HANDLE)f, off, buf, n); }

// Whether the installed default.xbe is the USA release, and the game folder's size: worked out on a worker thread
// (hashing and walking 5 GB), at start and after each install, then sent to the page.
static std::mutex game_lock;
static std::string game_verified, game_size;   // "" until known
static void game_facts()
{
    std::string v = hash_file(game_dir() + L"\\default.xbe") == USA_XBE_SHA1 ? "true" : "false";
    uint64_t total = 0;
    each_file(game_dir(), [&](const std::wstring &, const WIN32_FIND_DATAW &fd) { total += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow; });
    char b[32];
    snprintf(b, sizeof b, "%.2f GB", total / 1073741824.0);
    { std::lock_guard<std::mutex> g(game_lock); game_verified = v; game_size = b; }
    PostMessageW(hwnd, WM_APP_STATE, 0, 0);
}

static void art_refresh();
static void install_(const std::wstring &iso, std::string &error);
static void install(std::wstring iso)
{
    std::string error;
    try { install_(iso, error); } catch (...) { error = "Installing stopped: out of memory (is the disc image damaged?)."; }
    if (error.empty()) art_refresh();
    if (game_ready()) game_facts();
    PostMessageW(hwnd, WM_APP_SETUP_DONE, 0, (LPARAM)new std::string(error));
}
static void install_(const std::wstring &iso, std::string &error)
{
    HANDLE f = CreateFileW(iso.c_str(), GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, 0);
    struct closer { HANDLE h; ~closer() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); } } close_f{f};   // a throw too
    std::vector<disc_file> files;
    if (f == INVALID_HANDLE_VALUE) error = "That file couldn't be opened.";
    else {
        int listed = disc_list(read_handle, f, files);
        if (listed == 1) error = "That isn't an Xbox disc image. Choose a full ISO of Genma Onimusha.";
        else {
            if (listed == 2) error = "That disc image doesn't read right (a folder past its end, or a name that can't be): it may be damaged or cut short.";
            const disc_file *xbe = 0;
            for (auto &d : files) if (!_stricmp(d.path.c_str(), "default.xbe")) xbe = &d;
            if (!xbe && error.empty()) error = "This Xbox disc has no game on it (no default.xbe).";
            // Installing again over a working game with another disc (another region, another game) copied it in at once
            // and only warned afterwards: ask first.
            if (xbe && error.empty() && game_ready() && xbe->size < (64u << 20)) {
                std::vector<uint8_t> b((size_t)xbe->size);
                uint8_t d[20];
                std::string hex;
                if (read_at(f, xbe->offset, b.data(), (DWORD)b.size()) && BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA1_ALG_HANDLE, 0, 0, b.data(), (ULONG)b.size(), d, 20)))
                    for (uint8_t c : d) { char x[3]; snprintf(x, sizeof x, "%02x", c); hex += x; }
                if (hex != USA_XBE_SHA1 && MessageBoxW(hwnd, L"This disc image isn't the USA release of Genma Onimusha, which the port is made for.\n\n"
                                                            L"Install it over the game you have anyway?", L"Genma Onimusha", MB_YESNO | MB_ICONWARNING) != IDYES)
                    error = "Not installed: that disc isn't the USA release. Your game is as it was.";
            }
        }
    }
    if (error.empty()) {
        uint64_t total = 0, done = 0;
        for (auto &d : files) total += d.size;
        std::vector<char> buf(8 << 20);
        DWORD last = 0;
        // Files already there at their size are skipped only when finishing a stopped install: installing again copies
        // everything, so it repairs, and one image's files never mix with another's.
        make_dirs(game_dir());
        std::wstring marker = game_dir() + L"\\.installing";
        bool resume = file_exists(marker);
        if (HANDLE m = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, 0); m != INVALID_HANDLE_VALUE) CloseHandle(m);
        for (auto &d : files) {
            std::wstring dst = game_dir() + L"\\" + wide(d.path);
            for (auto &c : dst) if (c == L'/') c = L'\\';
            make_dirs(dst.substr(0, dst.find_last_of(L'\\')));
            WIN32_FILE_ATTRIBUTE_DATA a;
            bool same = resume && GetFileAttributesExW(dst.c_str(), GetFileExInfoStandard, &a) && (((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow) == d.size;
            if (!same) {
                HANDLE o = CreateFileW(dst.c_str(), GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
                if (o == INVALID_HANDLE_VALUE) { error = "Couldn't write into the game folder: " + utf8(dst); break; }
                uint64_t left = d.size, off = d.offset;
                while (left) {
                    DWORD n = (DWORD)std::min<uint64_t>(left, buf.size()), w = 0;
                    if (!read_at(f, off, buf.data(), n) || !WriteFile(o, buf.data(), n, &w, 0) || w != n) { error = "Copying stopped: the disc image may be damaged, or the drive is full."; break; }
                    left -= n; off += n; done += n;
                    if (GetTickCount() - last > 100) {
                        last = GetTickCount();
                        char m[512];
                        snprintf(m, sizeof m, "{\"type\":\"progress\",\"pct\":%.2f,\"done\":%llu,\"total\":%llu,\"file\":%s}", total ? 100.0 * done / total : 0.0,
                                 (unsigned long long)done, (unsigned long long)total, jstr(d.path).c_str());
                        post(m);
                    }
                }
                // flushed before the next file: after a power cut a file can keep its size with zeros inside, and a
                // resumed install skips files of the right size
                if (error.empty()) FlushFileBuffers(o);
                CloseHandle(o);
                if (!error.empty()) break;
            } else done += d.size;
        }
        if (error.empty()) DeleteFileW(marker.c_str());
    }
}

// ---- art from the player's disc: .BG files are a 2 KB XPR header + a 1024x512 DXT1 image ------------------
// Written as 1024x768 bitmaps (the game shows them stretched to 4:3) into the page's folder.
static std::wstring art_dir() { return data_dir + L"\\ui\\art"; }
static bool decode_bg(const std::wstring &src, const std::wstring &dst)
{
    HANDLE f = CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (f == INVALID_HANDLE_VALUE) return false;
    std::vector<uint8_t> d(2048 + 1024 * 512 / 2);
    DWORD n = 0;
    BOOL ok = ReadFile(f, d.data(), (DWORD)d.size(), &n, 0);
    CloseHandle(f);
    if (!ok || n != d.size() || memcmp(d.data(), "XPR0", 4)) return false;
    const int W = 1024, H = 512, OH = 768;
    std::vector<uint32_t> px(W * H);
    auto c565 = [](uint16_t v) { uint32_t r = (v >> 11) * 255 / 31, g = ((v >> 5) & 63) * 255 / 63, b = (v & 31) * 255 / 31; return r << 16 | g << 8 | b; };
    const uint8_t *p = d.data() + 2048;
    for (int by = 0; by < H; by += 4)
        for (int bx = 0; bx < W; bx += 4, p += 8) {
            uint16_t c0 = p[0] | p[1] << 8, c1 = p[2] | p[3] << 8;
            uint32_t bits = p[4] | p[5] << 8 | p[6] << 16 | (uint32_t)p[7] << 24, pal[4] = {c565(c0), c565(c1)};
            auto mix = [](uint32_t a, uint32_t b, int wa, int wb, int div) {
                uint32_t o = 0;
                for (int s = 0; s < 24; s += 8) o |= ((((a >> s) & 255) * wa + ((b >> s) & 255) * wb) / div) << s;
                return o;
            };
            if (c0 > c1) { pal[2] = mix(pal[0], pal[1], 2, 1, 3); pal[3] = mix(pal[0], pal[1], 1, 2, 3); }
            else { pal[2] = mix(pal[0], pal[1], 1, 1, 2); pal[3] = 0; }
            for (int j = 0; j < 16; j++) px[(by + (j >> 2)) * W + bx + (j & 3)] = pal[(bits >> (2 * j)) & 3];
        }
    std::vector<uint8_t> out((size_t)W * OH * 4);   // stretch 512 -> 768 rows, linear
    for (int y = 0; y < OH; y++) {
        float sy = (y + 0.5f) * H / OH - 0.5f;
        int y0 = sy < 0 ? 0 : (int)sy, y1 = y0 + 1 < H ? y0 + 1 : H - 1;
        float t = sy - y0 < 0 ? 0 : sy - y0;
        for (int x = 0; x < W; x++) {
            uint32_t a = px[y0 * W + x], b = px[y1 * W + x];
            uint8_t *o = &out[((size_t)y * W + x) * 4];
            for (int c = 0; c < 3; c++) o[c] = (uint8_t)(((a >> (8 * c)) & 255) * (1 - t) + ((b >> (8 * c)) & 255) * t);
            o[3] = 255;
        }
    }
    BITMAPFILEHEADER fh = {0x4D42, (DWORD)(sizeof fh + sizeof(BITMAPINFOHEADER) + out.size()), 0, 0, sizeof fh + sizeof(BITMAPINFOHEADER)};
    BITMAPINFOHEADER ih = {sizeof ih, W, -OH, 1, 32};
    FILE *o = _wfopen(dst.c_str(), L"wb");
    if (!o) return false;
    fwrite(&fh, sizeof fh, 1, o); fwrite(&ih, sizeof ih, 1, o); fwrite(out.data(), out.size(), 1, o);
    fclose(o);
    return true;
}
static const struct { const wchar_t *src, *name; } ART[] = {{L"x_oni.bg", L"logo"}, {L"saimyo0.bg", L"hero"}, {L"sougen0.bg", L"hero2"}};
static void art_refresh()
{
    make_dirs(art_dir());
    for (auto &a : ART) {
        std::wstring dst = art_dir() + L"\\" + a.name + L".bmp";
        if (!file_exists(dst)) decode_bg(game_dir() + L"\\image\\bmp\\etc\\" + a.src, dst);
    }
}

// ---- mods\: the list and mods\mods.txt are in src/host/mods_list.h, shared with the game ----------------------
static std::vector<mod_item> mods_get()
{
    std::vector<mod_item> v(MOD_MAX);
    v.resize(mod_list(exe_dir.c_str(), v.data()));
    static bool warned;
    if (v.size() == MOD_MAX && !warned) { warned = true; toast("There are " + std::to_string(MOD_MAX) + " or more mod folders: only the first " + std::to_string(MOD_MAX) + " are listed and used."); }
    return v;
}
static void mods_save(const std::vector<mod_item> &mods)
{
    std::string out = "; Mods, kept by the launcher. Where two mods change the same file (or the same code), the higher one wins.\n"
                      "; mod = on, mod_off = off. New ones are on, unless a mod's mod.ini says enabled = 0.\n";
    for (auto &x : mods) out += std::string(x.on ? "mod" : "mod_off") + " = " + utf8(x.name) + "\n";
    make_dirs(exe_dir + L"\\mods");
    if (!write_file_atomic((exe_dir + L"\\mods\\mods.txt").c_str(), out.data(), out.size())) toast("Couldn't save mods.txt in the mods folder.");
}
// A patch's description: its first line, when that's a comment.
static std::string patch_about(const std::wstring &path)
{
    std::string about;
    if (FILE *f = _wfopen(path.c_str(), L"r")) {
        char buf[512];
        while (fgets(buf, sizeof buf, f)) {
            std::string t = buf;
            if (!t.compare(0, 3, "\xEF\xBB\xBF")) t.erase(0, 3);   // UTF-8 BOM, as the game skips it
            while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
            t = trim(t);
            if (t.empty()) continue;
            if (t[0] == ';') about = trim(t.substr(1));
            break;
        }
        fclose(f);
    }
    return about;
}
// A mod folder's size and the files it would put in place of the game's (everything but its mod.ini, patch.txt and
// preview picture), in one walk. Kept per folder while the folder's own time stamps stay the same: the state is
// rebuilt at every window activation, and walking the HD pack's thousands of files each time froze the window.
// ponytail: a file changed deep inside a folder without touching the folder itself keeps the old figures until restart.
struct mod_facts { uint64_t bytes = 0; int files = 0; FILETIME made = {}, written = {}; };
static mod_facts mod_facts_of(const std::wstring &dir)
{
    static std::map<std::wstring, mod_facts> cache;
    WIN32_FILE_ATTRIBUTE_DATA a = {};
    GetFileAttributesExW(dir.c_str(), GetFileExInfoStandard, &a);
    auto it = cache.find(dir);
    if (it != cache.end() && !CompareFileTime(&it->second.made, &a.ftCreationTime) && !CompareFileTime(&it->second.written, &a.ftLastWriteTime)) return it->second;
    mod_facts f;
    f.made = a.ftCreationTime; f.written = a.ftLastWriteTime;
    each_file(dir, [&](const std::wstring &rel, const WIN32_FIND_DATAW &fd) {
        f.bytes += (uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow;
        f.files += rel.find(L'\\') != std::wstring::npos || (_wcsicmp(rel.c_str(), L"mod.ini") && _wcsicmp(rel.c_str(), L"patch.txt") && _wcsnicmp(rel.c_str(), L"preview.", 8));
    });
    return cache[dir] = f;
}
// name: the folder (what messages use). From mod.ini: title, about, author, version and builtin (the id of a change
// built into the port); preview: preview.png/.jpg in the folder, if any; files: how many it replaces; patch: it has a
// patch.txt (whose first ; line is the about when mod.ini has no description).
static std::string mods_json()
{
    std::string j = "[";
    for (auto &x : mods_get()) {
        char title[256] = "", about[1024] = "", builtin[64] = "", author[128] = "", version[64] = "", modern[8] = "", pack[64] = "";
        std::string extra;
        {
            mod_ini(exe_dir.c_str(), x.name, "", "name", title, sizeof title);
            mod_ini(exe_dir.c_str(), x.name, "", "description", about, sizeof about);
            mod_ini(exe_dir.c_str(), x.name, "", "builtin", builtin, sizeof builtin);
            mod_ini(exe_dir.c_str(), x.name, "", "author", author, sizeof author);
            mod_ini(exe_dir.c_str(), x.name, "", "version", version, sizeof version);
            mod_ini(exe_dir.c_str(), x.name, "", "modern", modern, sizeof modern);   // part of the Modern preset
            mod_ini(exe_dir.c_str(), x.name, "", "pack", pack, sizeof pack);   // from a pack the launcher downloads (HD pack)
            std::wstring dir = exe_dir + L"\\mods\\" + x.name;
            bool patch = file_exists(dir + L"\\patch.txt");
            if (patch && !about[0]) snprintf(about, sizeof about, "%s", patch_about(dir + L"\\patch.txt").c_str());
            const char *preview = file_exists(dir + L"\\preview.png") ? "preview.png" : file_exists(dir + L"\\preview.jpg") ? "preview.jpg" : "";
            mod_facts facts = mod_facts_of(dir);
            extra = ",\"author\":" + jstr(author) + ",\"version\":" + jstr(version) + ",\"pack\":" + jstr(pack) + ",\"bytes\":" + std::to_string(facts.bytes) + ",\"modern\":" + (atoi(modern) ? "true" : "false") + ",\"preview\":" + jstr(preview)
                  + ",\"files\":" + std::to_string(facts.files) + ",\"patch\":" + (patch ? "true" : "false")
                  + ",\"textures\":" + ((GetFileAttributesW((dir + L"\\textures").c_str()) & (FILE_ATTRIBUTE_DIRECTORY | 0x80000000u)) == FILE_ATTRIBUTE_DIRECTORY ? "true" : "false");   // an HD texture pack
        }
        j += (j.size() > 1 ? "," : "") + std::string("{\"name\":") + jstr(utf8(x.name)) + ",\"on\":" + (x.on ? "true" : "false")
           + ",\"title\":" + jstr(title) + ",\"about\":" + jstr(about) + ",\"builtin\":" + jstr(builtin) + extra + "}";
    }
    return j + "]";
}


// Install a mod from a .zip: unpacked (unzip.h) into a temporary folder inside mods\, then renamed to the
// folder the zip holds when it holds just one, or else to the zip's name.
// Set once the start-up thread has cleared what a launcher closed mid-way left (mods\.installing-*, old pack
// downloads): an install waits for it, so the clean-up never takes the folder of one that has just begun.
static std::atomic<bool> leftovers_cleared{false};
static void wait_leftovers() { while (!leftovers_cleared) Sleep(50); }

static void mod_install(std::wstring zip)
{
    wait_leftovers();
    std::wstring mods = exe_dir + L"\\mods", stage = mods + L"\\.installing-" + std::to_wstring(GetTickCount64());
    make_dirs(stage);
    bool unpacked = unzip(zip, stage);
    std::wstring from = stage, name = zip.substr(zip.find_last_of(L'\\') + 1);
    if (name.size() > 4 && !_wcsicmp(name.c_str() + name.size() - 4, L".zip")) name.resize(name.size() - 4);
    int files = 0, dirs = 0;
    std::wstring only;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((stage + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { dirs++; only = fd.cFileName; } else files++;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::string msg;
    if (!unpacked || files + dirs == 0) msg = "That zip couldn't be unpacked.";
    else {
        if (dirs == 1 && !files) { from = stage + L"\\" + only; name = only; }
        std::wstring to = mods + L"\\" + name;
        for (int k = 2; GetFileAttributesW(to.c_str()) != INVALID_FILE_ATTRIBUTES; k++) to = mods + L"\\" + name + L" (" + std::to_wstring(k) + L")";
        if (MoveFileExW(from.c_str(), to.c_str(), 0)) {
            char en[8] = "";
            std::wstring folder = to.substr(mods.size() + 1);
            mod_ini(exe_dir.c_str(), folder.c_str(), "", "enabled", en, sizeof en);   // enabled = 0: it starts off (mods_list.h)
            msg = "Installed " + utf8(folder) + (!strcmp(en, "0") ? ". It starts off: switch it on in the list." : ". It's on from the next game start.");
        }
        else msg = "Couldn't move the mod into the mods folder.";
    }
    if (GetFileAttributesW(stage.c_str()) != INVALID_FILE_ATTRIBUTES) sh_delete(stage);
    toast(msg);
    PostMessageW(hwnd, WM_APP_STATE, 0, 0);
}

// ---- achievements: the list is in src/host/achievements_list.h; the game writes saves\achievements.dat (sealed, layout.h)
static std::string achievements_json()
{
    struct ach { const char *id, *name, *desc; };
#define X(id, name, desc) {#id, name, desc},
    static const ach all[] = {ACHIEVEMENTS(X)};
#undef X
    std::vector<std::string> lines;
    static char text[16384];
    if (sealed_read((exe_dir + L"\\saves\\achievements.dat").c_str(), text, sizeof text) > 0)
        for (char *line = text, *next; line; line = next) { if ((next = strchr(line, '\n'))) *next++ = 0; lines.push_back(line); }
    std::string j = "[";
    for (auto &a : all) {
        std::string t = ini_get(lines, a.id, "0");
        if (t.empty() || t.find_first_not_of("0123456789") != std::string::npos) t = "0";
        j += (j.size() > 1 ? "," : "") + std::string("{\"id\":") + jstr(a.id) + ",\"name\":" + jstr(a.name) + ",\"desc\":" + jstr(a.desc)
           + ",\"t\":" + t + "}";
    }
    return j + "]";
}

// ---- play time: the game keeps saves\playtime.txt (seconds = N) -------------------------------------------------
static std::string playtime_seconds(const std::wstring &saves = exe_dir + L"\\saves")   // a run keeps its own, in its saves
{
    std::vector<std::string> lines;
    if (FILE *f = _wfopen((saves + L"\\playtime.txt").c_str(), L"r")) {
        char b[128];
        while (fgets(b, sizeof b, f)) lines.push_back(b);
        fclose(f);
    }
    std::string t = ini_get(lines, "seconds", "0");
    return t.empty() || t.find_first_not_of("0123456789") != std::string::npos ? "0" : t;
}

// ---- state for the page ------------------------------------------------------------------------------
static std::string update_json();
static std::string pack_json();
static std::string rando_json();
static std::wstring rando_dir();
static bool rando_name_ok(const std::string &n);
static std::string state_json()
{
    auto ini = ini_load();
    auto num = [&](const char *k, const char *def) { return json_number(ini_get(ini, k, def), def); };
    auto str = [&](const char *k, const char *def) { return jstr(ini_get(ini, k, def)); };
    // every setting at its value or default (numbers as numbers), [launcher] too; keys apart
    std::string s = "{", k = "{";
    for (auto &d : ini_defaults()) {
        const char *key = d.key.c_str(), *def = d.value.c_str();
        if (d.section == "keys") { k += (k.size() > 1 ? "," : "") + jstr(d.key) + ":" + str(key, def); continue; }
        std::string v;
        if (d.key == "window") { std::string w = ini_get(ini, "window", def); v = jstr(!_stricmp(w.c_str(), "borderless") || !_stricmp(w.c_str(), "fullscreen") ? "borderless" : "windowed"); }
        else if (d.key == "modern_controls") v = num(key, num("stick_move", def).c_str());   // earlier names as fallback
        else if (d.key == "weapon_wheel") v = num(key, num("swap_weapon", def).c_str());
        else v = !d.value.empty() && d.value.find_first_not_of("0123456789.-") == std::string::npos ? num(key, def) : str(key, def);
        s += (s.size() > 1 ? "," : "") + jstr(d.key) + ":" + v;
    }
    s += "}";
    k += "}";
    // display: the monitor the launcher is on
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi = {}; mi.cbSize = sizeof mi;
    GetMonitorInfoW(mon, &mi);
    DEVMODEW dm = {}; dm.dmSize = sizeof dm;
    EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm);
    char monitor[96];
    snprintf(monitor, sizeof monitor, "{\"w\":%lu,\"h\":%lu,\"hz\":%lu}", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency);
    // game files
    std::string setup;
    if (installing) setup = "{\"busy\":true}";
    else if (game_ready()) {
        std::lock_guard<std::mutex> g(game_lock);   // verified is left out until game_facts knows
        setup = "{\"ready\":true," + (game_verified.empty() ? "" : "\"verified\":" + game_verified + ",") + "\"title\":\"Genma Onimusha\",\"path\":"
              + jstr(utf8(game_dir())) + ",\"size\":" + jstr(game_size) + "}";
    } else setup = "{\"ready\":false}";
    std::string art = "null";
    if (game_ready() && file_exists(art_dir() + L"\\logo.bmp")) art = "{\"logo\":\"https://art.example/logo.bmp\",\"hero\":\"https://art.example/hero.bmp\",\"hero2\":\"https://art.example/hero2.bmp\"}";
    return std::string("{\"type\":\"state\",\"version\":") + jstr(VERSION) + ",\"monitor\":" + monitor + ",\"controller\":" + (pad_connected ? "true" : "false")
        + ",\"running\":" + (running ? "true" : "false") + ",\"setup\":" + setup + ",\"art\":" + art + ",\"s\":" + s + ",\"keys\":" + k
        + ",\"mods\":" + mods_json() + ",\"update\":" + update_json() + ",\"pack\":" + pack_json() + ",\"ach\":" + achievements_json() + ",\"playtime\":" + playtime_seconds() + ",\"rando\":" + rando_json() + "}";
}
static void send_state() { post(state_json()); }

// ---- updates: the latest GitHub release of ONI_UPDATE_REPO ------------------------------------------------------
// The release workflow attaches GenmaOnimusha-<version>.zip and its .sha256. The launcher checks once per start and,
// when the player asks, downloads both, checks the hash, unpacks it (unzip.h) and swaps the files in.
// A running exe can be renamed but not overwritten, so each replaced file is kept as <name>.old until a later start,
// and listed in <data>\update\replaced.txt so only those are deleted. Only files in the zip are replaced: game\, saves\,
// onimusha.ini and mods\mods.txt are never in it, and in mods\ only the port's own files (README.txt and any built-in mod folders) are,
// so the player's mod folders are never touched.
// Debug: ONI_UPDATE_URL=<url of a release JSON> checks that instead of GitHub.
static std::mutex upd_lock;
static std::string upd_state = "none", upd_version, upd_zip, upd_sha;   // none (not checked)|current|available|downloading|installing|error
static std::wstring update_url()
{
#ifdef ONI_DEBUG   // development builds only (CMake: ONI_UPDATE_REPO empty): a release never lets the environment pick where its code comes from
    wchar_t over[1024] = L"";
    if (GetEnvironmentVariableW(L"ONI_UPDATE_URL", over, 1024)) return over;
#endif
    if (!*ONI_UPDATE_REPO) return {};
    return L"https://api.github.com/repos/" + wide(ONI_UPDATE_REPO) + L"/releases/latest";
}
static std::string update_json()   // {enabled, state, version}: whether this build can update, and how far it got
{
    std::lock_guard<std::mutex> g(upd_lock);
    return std::string("{\"enabled\":") + (update_url().empty() ? "false" : "true") + ",\"state\":" + jstr(upd_state) + ",\"version\":" + jstr(upd_version) + "}";
}
static bool updating()   // an update is downloading or swapping files: it ends by restarting the launcher
{
    std::lock_guard<std::mutex> g(upd_lock);
    return upd_state == "downloading" || upd_state == "installing";
}
static void set_update(const std::string &state, const std::string &extra = "")
{
    std::string v;
    { std::lock_guard<std::mutex> g(upd_lock); upd_state = state; v = upd_version; }
    post("{\"type\":\"update\",\"state\":" + jstr(state) + ",\"version\":" + jstr(v) + extra + "}");
}

// GET a URL into a string or a file. Follows redirects (GitHub sends downloads to its storage host), never from https
// to http. from > 0: carry on a file from that byte (Range); a server that sends the whole file anyway starts it over.
// progress gets bytes so far and in all, counting from 0 either way. stop: set from another thread to give up (false).
static bool http_get(const std::wstring &url, std::string *body, HANDLE out, void (*progress)(uint64_t, uint64_t) = 0, uint64_t from = 0,
                     const std::atomic<bool> *stop = 0)
{
    wchar_t host[256], path[2048], extra[1024];
    URL_COMPONENTS uc = {sizeof uc};
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 1024;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
#ifndef ONI_DEBUG   // a release downloads its code over HTTPS only (a development build may test against a local HTTP server)
    if (uc.nScheme != INTERNET_SCHEME_HTTPS) return false;
#endif
    std::wstring target = std::wstring(path) + extra, agent = L"GenmaOnimusha/" + wide(VERSION);
    bool ok = false;
    HINTERNET s = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, 0, 0, 0), c = 0, r = 0;
    if (s) WinHttpSetTimeouts(s, 30000, 30000, 30000, 60000);   // resolve, connect, send, each read: a dead link fails, not hangs
    if (s) c = WinHttpConnect(s, host, uc.nPort, 0);
    if (c) r = WinHttpOpenRequest(c, L"GET", target.c_str(), 0, 0, 0, uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    if (r) WinHttpSetOption(r, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);
    std::wstring headers = L"Accept: application/vnd.github+json\r\n";
    if (from) headers += L"Range: bytes=" + std::to_wstring(from) + L"-\r\n";
    DWORD status = 0, len = sizeof status;
    if (r && WinHttpSendRequest(r, headers.c_str(), (DWORD)-1, 0, 0, 0, 0) && WinHttpReceiveResponse(r, 0)
        && WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, 0, &status, &len, 0) && (status == 200 || (from && status == 206))) {
        uint64_t total = 0, done = 0;
        wchar_t cl[32];
        DWORD cll = sizeof cl;
        if (WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, 0, cl, &cll, 0)) total = _wcstoui64(cl, 0, 10);
        if (status == 206) done = from, total += from;   // carrying on
        else if (from && out) {   // the whole file again: start it over
            LARGE_INTEGER zero = {};
            SetFilePointerEx(out, zero, 0, FILE_BEGIN);
            SetEndOfFile(out);
        }
        std::vector<char> buf(1 << 16);
        DWORD n = 0;
        ok = true;
        for (;;) {
            if (stop && *stop) { ok = false; break; }
            if (!WinHttpReadData(r, buf.data(), (DWORD)buf.size(), &n)) { ok = false; break; }   // a dropped link: not done
            if (!n || !ok) break;
            if (body) body->append(buf.data(), n);
            DWORD w = 0;
            if (out != INVALID_HANDLE_VALUE && out && (!WriteFile(out, buf.data(), n, &w, 0) || w != n)) ok = false;
            done += n;
            if (progress) progress(done, total);
        }
        if (total && done != total) ok = false;
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return ok;
}


static void update_check()
{
    std::string j;
    if (!http_get(update_url(), &j, 0)) return;   // offline or rate-limited: say nothing
    std::string tag = field(j, "tag_name"), zip, sha;
    for (size_t p = 0; (p = j.find("\"browser_download_url\":", p)) != std::string::npos; p++) {
        std::string u = field(j.substr(p), "browser_download_url");
        if (u.size() > 7 && !u.compare(u.size() - 7, 7, ".sha256")) sha = u;
        else if (u.size() > 4 && !u.compare(u.size() - 4, 4, ".zip")) zip = u;
    }
    if (tag.empty() || zip.empty() || sha.empty()) return;
#ifndef ONI_DEBUG   // the files of a GitHub release are on github.com (it redirects to its storage host itself)
    if (zip.rfind("https://github.com/", 0) || sha.rfind("https://github.com/", 0)) return;
#endif
    if (!newer_than(tag, VERSION)) return set_update("current");
    { std::lock_guard<std::mutex> g(upd_lock); upd_version = tag[0] == 'v' ? tag.substr(1) : tag; upd_zip = zip; upd_sha = sha; }
    set_update("available");
}

// The update's swap into the install (files.h, tested), its replaced files listed in replaced.txt for remove_old_files.
static bool swap_in(const std::wstring &stage, std::vector<std::wstring> &moved)
{
    return swap_in(stage, exe_dir, data_dir + L"\\update\\replaced.txt", moved);
}

static void update_install()
{
    std::string zip_url, sha_url;
    { std::lock_guard<std::mutex> g(upd_lock); zip_url = upd_zip; sha_url = upd_sha; }
    // unpacked next to the exe, so each swap below is a rename on the same drive, never a copy a crash could cut short
    std::wstring dir = data_dir + L"\\update", zip = dir + L"\\update.zip", stage = exe_dir + L"\\.update";
    sh_delete(stage);   // leftovers of an earlier attempt
    make_dirs(dir);
    make_dirs(stage);
    auto fail = [](const char *why) { set_update("error", std::string(",\"text\":") + jstr(why)); };
    std::string sha;
    if (!http_get(wide(sha_url), &sha, 0) || sha.size() < 64) return fail("Couldn't reach GitHub. Check your connection and try again.");
    HANDLE f = CreateFileW(zip.c_str(), GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
    bool got = f != INVALID_HANDLE_VALUE && http_get(wide(zip_url), 0, f, [](uint64_t done, uint64_t total) {
        static DWORD last;
        if (total && GetTickCount() - last > 150) { last = GetTickCount(); char b[48]; snprintf(b, sizeof b, ",\"pct\":%.0f", 100.0 * done / total); set_update("downloading", b); }
    });
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (!got) return fail("The download stopped. Try again.");
    if (hash_file(zip, true) != sha.substr(0, 64)) return fail("The download was damaged (its checksum doesn't match). Try again.");
    set_update("installing");
    if (!unzip(zip, stage)) return fail("The update couldn't be unpacked.");
    // Swap file by file; on a failure put back what was already moved. The "swapping" marker stays while files move:
    // a launcher killed in the middle (power cut, Task Manager) finishes the swap at its next start from the checked,
    // unpacked files still in .update, instead of keeping half the old version and half the new.
    std::wstring swapping = dir + L"\\swapping";
    if (FILE *m = _wfopen(swapping.c_str(), L"w")) fclose(m);
    std::vector<std::wstring> moved;
    if (!swap_in(stage, moved)) {
        swap_back(moved);
        DeleteFileW(swapping.c_str());
        sh_delete(stage);
        return fail("Some files couldn't be replaced. Close the game and try again.");
    }
    DeleteFileW(swapping.c_str());
    sh_delete(stage);
    DeleteFileW(zip.c_str());
    PostMessageW(hwnd, WM_APP_UPDATED, 0, 0);
}

// Files replaced by the last update (the <name>.old of each line of replaced.txt), deleted once the old launcher has
// exited. A *.old the updater didn't make is left alone.
static void remove_old_files()
{
    std::wstring list = data_dir + L"\\update\\replaced.txt";
    std::vector<std::string> rels;
    if (FILE *f = _wfopen(list.c_str(), L"r")) {
        char b[1024];
        while (fgets(b, sizeof b, f)) { std::string t = trim(b); if (!t.empty()) rels.push_back(t); }
        fclose(f);
    }
    if (rels.empty()) return;
    for (int tries = 0; tries < 10; tries++) {
        bool left = false;
        for (auto &r : rels) {
            std::wstring old = exe_dir + L"\\" + wide(r) + L".old";
            // a swap cut off between its two renames left only the .old: that's the file, put it back
            if (!file_exists(exe_dir + L"\\" + wide(r))) { MoveFileExW(old.c_str(), (exe_dir + L"\\" + wide(r)).c_str(), 0); continue; }
            left |= !DeleteFileW(old.c_str()) && GetFileAttributesW(old.c_str()) != INVALID_FILE_ATTRIBUTES;
        }
        if (!left) { DeleteFileW(list.c_str()); return; }
        Sleep(1000);
    }
}

// ---- the HD pack: mod folders to download from the latest release of ONI_PACK_REPO --------------------------------
// The release holds pack.ini (tools/hdpack/pack_release.py) and the zips it lists: per mod folder one or more parts,
// each with its size and SHA-256. A folder is fetched part by part into mods\.download\, each part checked, unpacked
// (unzip.h) into mods\.download\<folder>-<version>\ as soon as it's in and then deleted, so the disk needs one part to
// spare. Both are kept across launches: a download cut off (the launcher closed, the connection lost) carries on with
// the part it was on, and the parts already unpacked aren't fetched again. Then
// the new folder replaces the installed one of that name, which goes to the Recycle Bin (or is deleted when it's too
// big for it). Installed folders say which release they came from in their mod.ini (version). One folder at a time.
// Debug: ONI_PACK_URL=<url of a pack.ini> reads that instead (its parts are next to it).
static std::mutex pk_lock;   // pack_part, pack_mod and pack.ini's reading: text.h
static std::string pk_state = "none", pk_version, pk_job, pk_text;   // none (not read)|ready|busy|error; job: the folder
static std::vector<pack_mod> pk_mods;
static std::vector<std::string> pk_queue;
static int pk_pct;
static std::atomic<bool> pk_cancel{false};   // the page's Cancel: the queue is dropped and the current folder given up
static std::wstring pack_url()
{
#ifdef ONI_DEBUG   // development builds only, as ONI_UPDATE_URL
    wchar_t over[1024] = L"";
    if (GetEnvironmentVariableW(L"ONI_PACK_URL", over, 1024)) return over;
#endif
    if (!*ONI_PACK_REPO) return {};
    return L"https://github.com/" + wide(ONI_PACK_REPO) + L"/releases/latest/download/pack.ini";
}
static std::string mod_version(const std::string &folder)   // the version an installed folder's mod.ini gives, or ""
{
    char v[64] = "";
    mod_ini(exe_dir.c_str(), wide(folder).c_str(), "", "version", v, sizeof v);
    return v;
}
static std::string pack_json()   // {enabled, state, version, job, pct, queue: [folders waiting], text, mods: [{folder, name, about, size, have: none|old|current}]}
{
    std::lock_guard<std::mutex> g(pk_lock);
    std::string j = std::string("{\"enabled\":") + (pack_url().empty() ? "false" : "true") + ",\"state\":" + jstr(pk_state)
                  + ",\"version\":" + jstr(pk_version) + ",\"job\":" + jstr(pk_job) + ",\"pct\":" + std::to_string(pk_pct)
                  + ",\"queue\":[";
    for (size_t i = 0; i < pk_queue.size(); i++) j += (i ? "," : "") + jstr(pk_queue[i]);
    j += std::string("],\"text\":") + jstr(pk_text) + ",\"mods\":[";
    for (size_t i = 0; i < pk_mods.size(); i++) {
        auto &m = pk_mods[i];
        DWORD a = GetFileAttributesW((exe_dir + L"\\mods\\" + wide(m.folder)).c_str());
        const char *have = a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY) ? "none" : mod_version(m.folder) == pk_version ? "current" : "old";
        j += (i ? "," : "") + std::string("{\"folder\":") + jstr(m.folder) + ",\"name\":" + jstr(m.name) + ",\"about\":" + jstr(m.about)
           + ",\"size\":" + std::to_string(m.size) + ",\"have\":" + jstr(have) + "}";
    }
    return j + "]}";
}
static void pack_check()
{
    std::string ini;
    if (!http_get(pack_url(), &ini, 0)) return;   // offline, or no release yet: the page just offers nothing
    std::vector<pack_mod> mods;
    std::string version;
    if (!pack_ini_parse(ini, version, mods)) return;
    { std::lock_guard<std::mutex> g(pk_lock); pk_mods = mods; pk_version = version; if (pk_state == "none") pk_state = "ready"; }
    PostMessageW(hwnd, WM_APP_STATE, 0, 0);
}
// Write version = <the release's> into an installed folder's mod.ini, so it counts as up to date even if the zips
// carried another (the release builder stamps it before zipping; this keeps the two from ever disagreeing).
static void stamp_version(const std::wstring &dir, const std::string &version)
{
    std::string text;
    if (FILE *f = _wfopen((dir + L"\\mod.ini").c_str(), L"rb")) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f))) text.append(b, n); fclose(f); }
    std::string out = stamp_version_text(text, version);   // text.h
    write_file_atomic((dir + L"\\mod.ini").c_str(), out.data(), out.size());
}
// Fetch and install the queued folders one after another.
static uint64_t pk_total, pk_before;   // the current folder's bytes, and those of its parts already in: one progress figure
static void pack_worker()
{
    wait_leftovers();
    std::wstring base = pack_url();
    base.resize(base.find_last_of(L'/') + 1);   // parts sit next to pack.ini
    std::string version;
    { std::lock_guard<std::mutex> g(pk_lock); version = pk_version; }
    for (;;) {
        pack_mod m;
        {
            std::lock_guard<std::mutex> g(pk_lock);
            pk_cancel = false;   // Cancel emptied the queue when it was pressed: anything in it now was asked for after
            if (pk_queue.empty()) { pk_state = pk_text.empty() ? "ready" : "error"; pk_job.clear(); pk_cancel = false; break; }
            std::string f = pk_queue.front();
            pk_queue.erase(pk_queue.begin());
            for (auto &x : pk_mods) if (x.folder == f) m = x;
            pk_job = f; pk_pct = 0;
        }
        PostMessageW(hwnd, WM_APP_STATE, 0, 0);
        if (m.folder.empty()) continue;
        // The folder's own stage, for this release: what is unpacked there stays for a download that carries on
        std::string vid;
        for (char ch : version) if (isalnum((unsigned char)ch) || ch == '.' || ch == '-') vid += ch;
        std::wstring mods = exe_dir + L"\\mods", parts = mods + L"\\.download", stage = parts + L"\\" + wide(m.folder) + L"-" + wide(vid);
        make_dirs(stage);
        pk_total = 0; pk_before = 0;
        for (auto &pt : m.parts) pk_total += pt.size;
        std::string err;
        bool stopped = false;   // the connection gave out: the stage is kept for the next try
        for (auto &pt : m.parts) {
            // named by its checksum too, so a part a later release changed isn't carried on from the old one's bytes
            std::string id;
            for (char ch : pt.sha) if (isxdigit((unsigned char)ch) && id.size() < 16) id += ch;
            std::wstring zip = parts + L"\\" + wide(pt.name) + L"." + wide(id) + L".part", done = stage + L"\\.unpacked-" + wide(id);
            if (file_exists(done)) { pk_before += pt.size; continue; }   // unpacked by an earlier try
            // A dropped connection is tried again up to 4 times, 4, 8 and 16 s apart, carrying on from what arrived;
            // what arrived stays in mods\.download\ for the next try (Get again, or after the launcher is reopened).
            bool got = false;
            for (int attempt = 0; attempt < 4 && !got && !pk_cancel; attempt++) {
                if (attempt) for (DWORD t = 0; t < (2000u << attempt) && !pk_cancel; t += 250) Sleep(250);
                if (pk_cancel) break;
                HANDLE f = CreateFileW(zip.c_str(), GENERIC_WRITE, 0, 0, OPEN_ALWAYS, 0, 0);
                if (f == INVALID_HANDLE_VALUE) break;
                LARGE_INTEGER have = {}, zero = {};
                GetFileSizeEx(f, &have);
                SetFilePointerEx(f, zero, 0, FILE_END);
                got = (uint64_t)have.QuadPart >= pt.size || http_get(base + wide(pt.name), 0, f, [](uint64_t done, uint64_t) {
                    static DWORD last;
                    if (GetTickCount() - last < 250) return;
                    last = GetTickCount();
                    int pct;
                    { std::lock_guard<std::mutex> g(pk_lock); pct = pk_pct = pk_total ? (int)(100 * (pk_before + done) / pk_total) : 0; }
                    post("{\"type\":\"packpct\",\"pct\":" + std::to_string(pct) + "}");   // the page updates the bar alone
                }, (uint64_t)have.QuadPart, &pk_cancel);
                CloseHandle(f);
            }
            if (pk_cancel) err = "cancelled";
            else if (!got) err = "The download stopped. Try again.", stopped = true;
            else if (hash_file(zip, true) != pt.sha) err = "The download was damaged (its checksum doesn't match). Try again.";
            else if (!unzip(zip, stage)) err = "It couldn't be unpacked. Is the disk full?";
            if (got || pk_cancel) DeleteFileW(zip.c_str());   // kept only when the connection gave out: carried on next time
            if (!err.empty()) break;
            if (HANDLE mk = CreateFileW(done.c_str(), GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0); mk != INVALID_HANDLE_VALUE) CloseHandle(mk);
            pk_before += pt.size;
        }
        // The installed copy is moved aside (into the stage, same drive) before the new one goes in, and put back if
        // that fails; only then is it sent to the Recycle Bin.
        std::wstring target = mods + L"\\" + wide(m.folder), from = stage + L"\\" + wide(m.folder), aside = stage + L"\\.old-copy";
        bool had = GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (err.empty() && GetFileAttributesW(from.c_str()) == INVALID_FILE_ATTRIBUTES) err = "The download didn't hold that mod.";
        if (err.empty() && had && !MoveFileExW(target.c_str(), aside.c_str(), 0)) err = "The old copy couldn't be moved. Close the game and try again.";
        if (err.empty() && !MoveFileExW(from.c_str(), target.c_str(), 0)) {
            err = "Couldn't move it into the mods folder.";
            if (had) MoveFileExW(aside.c_str(), target.c_str(), 0);
        }
        if (err.empty() && had) sh_delete(aside, FOF_ALLOWUNDO | FOF_NOCONFIRMATION);
        // Still there: putting it back or recycling it failed (a file in use, say). Deleting the stage below would
        // delete it for good, so it's kept in mods\ instead, as "<folder> (old)".
        if (had && GetFileAttributesW(aside.c_str()) != INVALID_FILE_ATTRIBUTES) {
            std::wstring keep = target + L" (old)";
            for (int k = 2; GetFileAttributesW(keep.c_str()) != INVALID_FILE_ATTRIBUTES; k++) keep = target + L" (old " + std::to_wstring(k) + L")";
            if (MoveFileExW(aside.c_str(), keep.c_str(), 0)) toast("The earlier copy of " + m.folder + " is kept as " + utf8(keep.substr(mods.size() + 1)) + ": delete it when you like.");
            else { toast("The earlier copy of " + m.folder + " couldn't be moved; it's left in " + utf8(stage) + "."); stage.clear(); }   // so not deleted
        }
        if (!stage.empty() && !stopped) sh_delete(stage);
        if (err == "cancelled") { toast("Download cancelled."); continue; }   // the loop's top drops the queue
        if (!err.empty()) {   // the rest of the queue still goes
            toast((m.name.empty() ? m.folder : m.name) + ": " + err);   // also kept in the HD pack header (pk_text)
            std::lock_guard<std::mutex> g(pk_lock);
            pk_text += (pk_text.empty() ? "" : " ") + (m.name.empty() ? m.folder : m.name) + ": " + err;
            continue;
        }
        stamp_version(target, version);
        toast("Installed " + (m.name.empty() ? m.folder : m.name) + ".");
    }
    PostMessageW(hwnd, WM_APP_STATE, 0, 0);
}
static void pack_get(const std::string &folder)   // one folder, or "all": every one not installed or out of date
{
    std::lock_guard<std::mutex> g(pk_lock);
    bool idle = pk_state != "busy";
    for (auto &m : pk_mods) {
        if (folder != "all" ? m.folder != folder : mod_version(m.folder) == pk_version) continue;
        // the folder being fetched is queued again only when it is being cancelled (Get right after Cancel)
        if (std::find(pk_queue.begin(), pk_queue.end(), m.folder) == pk_queue.end() && (m.folder != pk_job || pk_cancel)) pk_queue.push_back(m.folder);
    }
    if (idle && !pk_queue.empty()) { pk_state = "busy"; pk_text.clear(); std::thread(pack_worker).detach(); }
}

static bool poll_pad()
{
    for (DWORD i = 0; i < 4; i++) { XINPUT_STATE st; if (XInputGetState(i, &st) == ERROR_SUCCESS) return true; }
    return false;
}
// The page can be driven with a controller: new presses go to it as {type: button, b: up|down|left|right|a|b|lb|rb|start|x|y}.
static void poll_buttons()
{
    static WORD held;
    WORD now = 0;
    if (GetForegroundWindow() == hwnd)
        for (DWORD i = 0; i < 4; i++) {
            XINPUT_STATE st;
            if (XInputGetState(i, &st) != ERROR_SUCCESS) continue;
            const XINPUT_GAMEPAD &g = st.Gamepad;
            now |= g.wButtons;
            if (g.sThumbLY > 16000) now |= XINPUT_GAMEPAD_DPAD_UP;
            if (g.sThumbLY < -16000) now |= XINPUT_GAMEPAD_DPAD_DOWN;
            if (g.sThumbLX < -16000) now |= XINPUT_GAMEPAD_DPAD_LEFT;
            if (g.sThumbLX > 16000) now |= XINPUT_GAMEPAD_DPAD_RIGHT;
        }
    static const struct { WORD bit; const char *name; } map[] = {{XINPUT_GAMEPAD_DPAD_UP, "up"}, {XINPUT_GAMEPAD_DPAD_DOWN, "down"}, {XINPUT_GAMEPAD_DPAD_LEFT, "left"},
        {XINPUT_GAMEPAD_DPAD_RIGHT, "right"}, {XINPUT_GAMEPAD_A, "a"}, {XINPUT_GAMEPAD_B, "b"}, {XINPUT_GAMEPAD_LEFT_SHOULDER, "lb"},
        {XINPUT_GAMEPAD_RIGHT_SHOULDER, "rb"}, {XINPUT_GAMEPAD_START, "start"}, {XINPUT_GAMEPAD_X, "x"}, {XINPUT_GAMEPAD_Y, "y"}};
    for (auto &m : map) if ((now & m.bit) && !(held & m.bit)) post(std::string("{\"type\":\"button\",\"b\":\"") + m.name + "\"}");
    held = now;
}

// ---- actions -----------------------------------------------------------------------------------------
// The game is running: one the launcher started (running), or one started some other way (its mutex, layout.h).
static bool game_running() { return running || layout_game_running(exe_dir.c_str()); }

static void play()
{
    if (running || !game_ready()) return;
    if (game_running()) return toast("The game is already running from this folder.");
    if (updating()) return toast("The launcher is updating: it restarts when it's done.");
    { std::lock_guard<std::mutex> g(pk_lock); if (pk_state == "busy") return toast("Wait for the HD pack download to finish, or cancel it on the Mods page."); }   // the page blocks Play too
    std::wstring exe = exe_dir + L"\\bin\\onimusha.exe";
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe.c_str(), 0, 0, 0, FALSE, 0, 0, exe_dir.c_str(), &si, &pi)) { toast("Couldn't start the game: bin\\onimusha.exe is missing."); return; }
    CloseHandle(pi.hThread);
    running = true;
    post("{\"type\":\"running\",\"running\":true}");
    if (ini_get(ini_load(), "hide_while_playing", "1") != "0") ShowWindow(hwnd, SW_HIDE);
    std::thread([p = pi.hProcess] { WaitForSingleObject(p, INFINITE); CloseHandle(p); PostMessageW(hwnd, WM_APP_GAME_EXITED, 0, 0); }).detach();
}

static void start_install(const std::wstring &iso)
{
    if (game_running()) return toast("Close the game first: it's using the game files.");
    if (installing) return;
    installing = true;
    send_state();
    std::thread(install, iso).detach();
}

// An Open dialog; the chosen file, or "" when cancelled.
static std::wstring pick_file(const COMDLG_FILTERSPEC *types, UINT n, const wchar_t *title)
{
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, 0, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return {};
    dlg->SetFileTypes(n, types);
    dlg->SetTitle(title);
    ComPtr<IShellItem> item;
    PWSTR path = 0;
    if (FAILED(dlg->Show(hwnd)) || FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return {};
    std::wstring p = path;
    CoTaskMemFree(path);
    return p;
}

static void pick_disc()
{
    if (installing) return;
    COMDLG_FILTERSPEC types[] = {{L"Xbox disc image (*.iso)", L"*.iso"}, {L"All files", L"*.*"}};
    std::wstring iso = pick_file(types, 2, L"Choose your Genma Onimusha disc image");
    if (!iso.empty()) start_install(iso);
}

// Settings > Import save: a save.dat from a real Xbox, put in as the port's save (files.h save_import).
static void import_save()
{
    if (game_running()) { toast("Close the game first: it keeps the save open."); return; }
    COMDLG_FILTERSPEC types[] = {{L"Xbox save (save.dat)", L"save.dat"}, {L"All files", L"*.*"}};
    std::wstring src = pick_file(types, 2, L"Choose a Genma Onimusha save.dat from an Xbox");
    if (src.empty()) return;
    switch (save_import(exe_dir, src)) {   // files.h
    case SAVE_NOT_ONE: toast("That isn't a Genma Onimusha save: the game's save.dat is 17,920 bytes."); break;
    case SAVE_NO_BACKUP: toast("Couldn't back up your current save, so nothing was changed."); break;
    case SAVE_NOT_WRITTEN: toast("Couldn't write the save next to the launcher."); break;
    case SAVE_SIGNED_ALREADY: toast("Save imported (it was already signed for the port)."); break;
    default: toast("Save imported: load it from the game's title screen. Your previous save was backed up beside it.");
    }
}

static std::string m_last;   // the message being handled, for helpers that need more of it
static void open_place(const std::string &what)
{
    std::wstring p;
    if (what == "saves") { p = exe_dir + L"\\saves\\UDATA"; make_dirs(p); }
    else if (what == "shots") { p = exe_dir + L"\\captures\\screenshots"; make_dirs(p); }
    else if (what == "game") p = game_dir();
    else if (what == "mods") { p = exe_dir + L"\\mods"; make_dirs(p); }
    else if (what == "mod") {   // one mod's folder: {what: mod, name}
        std::wstring n = wide(field(m_last, "name"));
        if (!plain_name(n)) return;
        p = exe_dir + L"\\mods\\" + n;
    }
    else if (what == "run" || what == "spoiler") {   // a randomizer run's folder or its spoiler log: {what, run}
        std::string n = field(m_last, "run");
        if (!rando_name_ok(n)) return;
        p = rando_dir() + L"\\" + wide(n);
        if (what == "spoiler") { ShellExecuteW(hwnd, L"open", (p + L"\\spoiler.txt").c_str(), 0, 0, SW_SHOWNORMAL); return; }
    }
    else if (what == "log") {
        p = exe_dir + L"\\logs\\onimusha.log";
        if (!file_exists(p)) { toast("No log yet: it's written when the game runs."); return; }
        ShellExecuteW(hwnd, L"open", p.c_str(), 0, 0, SW_SHOWNORMAL);
        return;
    }
    else return;
    ShellExecuteW(hwnd, L"explore", p.c_str(), 0, 0, SW_SHOWNORMAL);
}

static void make_shortcut()
{
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(0, self, MAX_PATH);
    PWSTR desk = 0;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, 0, &desk))) return;
    std::wstring lnk = std::wstring(desk) + L"\\Genma Onimusha.lnk";
    CoTaskMemFree(desk);
    ComPtr<IShellLinkW> sl;
    ComPtr<IPersistFile> pf;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_ShellLink, 0, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))
        && SUCCEEDED(sl->SetPath(self)) && SUCCEEDED(sl->SetWorkingDirectory(exe_dir.c_str())) && SUCCEEDED(sl->SetIconLocation(self, 0))
        && SUCCEEDED(sl->SetDescription(L"Genma Onimusha (native port)")) && SUCCEEDED(sl.As(&pf)) && SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
    toast(ok ? "Shortcut created on your desktop" : "Couldn't create the shortcut");
}


// ---- Randomizer: runs in randomizer\<name>\ (randomizer.h makes them; the game plays the one named by
// onimusha.ini randomizer_run, with its own saves) ---------------------------------------------------------------
static std::atomic<bool> rando_busy{false};
static std::mutex rando_lock;
static std::string rando_making;   // the name of the run being made, while rando_busy
static std::wstring rando_dir() { return exe_dir + L"\\" LAYOUT_RUNS; }
static std::string rando_dat()   // randomizer.dat, built into the launcher
{
    HRSRC r = FindResourceW(0, MAKEINTRESOURCEW(IDR_RANDO_DAT), RT_RCDATA);
    HGLOBAL g = r ? LoadResource(0, r) : 0;
    return g ? std::string((const char *)LockResource(g), SizeofResource(0, r)) : "";
}
// How far a run has got: of its item locations (randomizer.dat L lines), how many the furthest save slot has picked up.
// A save.dat's slots (game.h, SAVE_SLOT0 / SAVE_SLOT_SIZE) are the game state from G_SAVE_STATE, so the pickup bits
// (G_ITEMGOT, most significant bit first) are at +0x598. A slot counts as the game would load it (save_slot_used).
static int rando_found(const std::wstring &saves)
{
    static std::vector<int> flags;
    if (flags.empty()) for (auto &l : rando::parse(rando_dat()).locs) flags.push_back(l.flag);
    std::wstring udata = saves + L"\\" LAYOUT_TITLE_SAVES L"\\";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((udata + L"*").c_str(), &fd);
    int best = 0;
    for (bool more = h != INVALID_HANDLE_VALUE; more; more = FindNextFileW(h, &fd)) {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        std::vector<uint8_t> d;
        if (!rando::read_file(udata + fd.cFileName + L"\\save.dat", d)) continue;
        for (size_t at = SAVE_SLOT0; at + SAVE_SLOT_SIZE <= d.size(); at += SAVE_SLOT_SIZE) {
            if (!save_slot_used((const uint32_t *)&d[at])) continue;   // the sum and, as the game, the xor (it checked the sum only)
            int n = 0;
            for (int f : flags) if (d[at + (G_ITEMGOT - G_SAVE_STATE) + f / 8] & (0x80 >> (f % 8))) n++;
            best = std::max(best, n);
        }
    }
    if (h != INVALID_HANDLE_VALUE) FindClose(h);
    return best;
}
static int rando_total() { static int n = (int)rando::parse(rando_dat()).locs.size(); return n; }
static std::string rando_runs_json()
{
    std::string out = "[";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((rando_dir() + L"\\*").c_str(), &fd);
    for (bool more = h != INVALID_HANDLE_VALUE; more; more = FindNextFileW(h, &fd)) {
        std::wstring n = fd.cFileName;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || n[0] == L'.' || !file_exists(rando_dir() + L"\\" + n + L"\\run.ini")) continue;
        wchar_t seed[96] = L"", label[96] = L"", fs[8] = L"0", lc[8] = L"0", dw[8] = L"0", vs[8] = L"0", kc[8] = L"0";
        std::wstring ini = rando_dir() + L"\\" + n + L"\\run.ini";
        // run.ini is "key = value" lines with no section: read them by hand
        FILE *f = _wfopen(ini.c_str(), L"r");
        char line[256];
        while (f && fgets(line, sizeof line, f)) {
            std::string k, v;
            if (!line_key(line, &k, &v)) continue;
            if (k == "seed") wcsncpy(seed, wide(v).c_str(), 95);
            else if (k == "name") wcsncpy(label, wide(v).c_str(), 95);
            else if (k == "fluorite_sanity") wcsncpy(fs, wide(v).c_str(), 7);
            else if (k == "long_challenges") wcsncpy(lc, wide(v).c_str(), 7);
            else if (k == "door_warp") wcsncpy(dw, wide(v).c_str(), 7);
            else if (k == "vision_staff") wcsncpy(vs, wide(v).c_str(), 7);   // a run made while the option existed (its tag)
            else if (k == "keep_consumables") wcsncpy(kc, wide(v).c_str(), 7);
        }
        if (f) fclose(f);
        // when it was made (run.ini) and last saved (the newest save.dat), as Unix times; 0: never saved
        auto unix_time = [](FILETIME t) { return std::to_string((long long)((((ULONGLONG)t.dwHighDateTime << 32 | t.dwLowDateTime) - 116444736000000000ull) / 10000000)); };
        WIN32_FILE_ATTRIBUTE_DATA a;
        std::string made = GetFileAttributesExW(ini.c_str(), GetFileExInfoStandard, &a) ? unix_time(a.ftCreationTime) : "0", saved = "0";
        std::wstring saves = rando_dir() + L"\\" + n + L"\\saves", udata = saves + L"\\" LAYOUT_TITLE_SAVES L"\\";
        WIN32_FIND_DATAW sd;
        HANDLE sh = FindFirstFileW((udata + L"*").c_str(), &sd);
        ULONGLONG newest = 0;
        for (bool m = sh != INVALID_HANDLE_VALUE; m; m = FindNextFileW(sh, &sd))
            if ((sd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && sd.cFileName[0] != L'.'
                && GetFileAttributesExW((udata + sd.cFileName + L"\\save.dat").c_str(), GetFileExInfoStandard, &a)) {
                ULONGLONG t = (ULONGLONG)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime;
                if (t > newest) { newest = t; saved = unix_time(a.ftLastWriteTime); }
            }
        if (sh != INVALID_HANDLE_VALUE) FindClose(sh);
        out += (out.size() > 1 ? "," : "") + std::string("{\"name\":") + jstr(utf8(n)) + ",\"label\":" + jstr(utf8(label[0] ? label : seed)) + ",\"seed\":" + jstr(utf8(seed))
             + ",\"found\":" + std::to_string(rando_found(saves)) + ",\"total\":" + std::to_string(rando_total())
             + ",\"fs\":" + (fs[0] == L'1' ? "1" : "0") + ",\"lc\":" + (lc[0] == L'1' ? "1" : "0") + ",\"dw\":" + (dw[0] == L'1' ? "1" : "0")
             + ",\"vs\":" + (vs[0] == L'1' ? "1" : "0") + ",\"kc\":" + (kc[0] == L'1' ? "1" : "0")
             + ",\"made\":" + made + ",\"saved\":" + saved + ",\"time\":" + playtime_seconds(saves) + "}";
    }
    if (h != INVALID_HANDLE_VALUE) FindClose(h);
    return out + "]";
}
static std::string rando_json()
{
    std::lock_guard<std::mutex> l(rando_lock);
    return std::string("{\"busy\":") + (rando_busy ? "true" : "false") + ",\"making\":" + jstr(rando_busy ? rando_making : "") + ",\"runs\":" + rando_runs_json() + "}";
}
// A new run from {name, seed, fs, lc, dw, vs, kc}; an empty seed gets a random one, an empty name the seed. Its folder
// is the name's letters, digits, - and _ (with -2, -3... if taken); made, it's the run being played. Three at most:
// the tab has three slots.
static const int RANDO_SLOTS = 3;
static void rando_create(std::string label, std::string seed, rando::Options o)
{
    int runs = 0;
    for (size_t p = 0; (p = rando_runs_json().find("\"name\":", p)) != std::string::npos; p++) runs++;
    if (runs >= RANDO_SLOTS) { toast("All three run slots are full: delete a run to make a new one."); return; }
    if (rando_busy.exchange(true)) return;
    // a random seed that can't make a run (every door layout tried had a trap) is swapped for another, up to 8 times;
    // a seed the player typed is theirs, so that one fails with the reason
    bool random_seed = seed.empty(), named = !label.empty();
    auto roll = [] { char b[16]; snprintf(b, sizeof b, "%06u", (unsigned)((GetTickCount64() * 2654435761u + GetCurrentThreadId()) % 1000000)); return std::string(b); };
    if (random_seed) seed = roll();
    for (auto *s : {&label, &seed}) {
        s->erase(std::remove_if(s->begin(), s->end(), [](char c) { return c == '\r' || c == '\n'; }), s->end());
        size_t k = s->size() > 40 ? 40 : s->size();
        while (k < s->size() && k && ((unsigned char)(*s)[k] & 0xC0) == 0x80) k--;   // 40 bytes, but never inside a UTF-8 letter (it showed as "?")
        s->resize(k);
    }
    if (label.empty()) label = seed;
    { std::lock_guard<std::mutex> l(rando_lock); rando_making = label; }
    std::thread([label, seed, o, random_seed, named, roll]() mutable {
        std::string dat = rando_dat(), name, err;
        make_dirs(rando_dir());
        for (int tries = 0;; tries++) {
            name.clear();
            for (char c : label) if (isalnum((unsigned char)c) || c == '-' || c == '_') name += c;
            if (name.empty()) name = "run";
            name = name.substr(0, 40);
            std::string base = name;
            for (int k = 2; GetFileAttributesW((rando_dir() + L"\\" + wide(name)).c_str()) != INVALID_FILE_ATTRIBUTES; k++) name = base + "-" + std::to_string(k);
            o.seed = seed; o.name = label;
            err = dat.empty() ? "The randomizer's data is missing from the launcher." : rando::generate(dat, game_dir(), rando_dir() + L"\\" + wide(name), o);
            if (err.empty() || dat.empty() || !random_seed || tries >= 7) break;
            sh_delete(rando_dir() + L"\\" + wide(name));
            seed = roll() + std::to_string(tries);   // another random seed
            seed = seed.substr(seed.size() - 6);
            if (!named) { label = seed; std::lock_guard<std::mutex> l(rando_lock); rando_making = label; }
        }
        // onimusha.ini and the page's state are the window thread's: a write here could race one from the page (one
        // of the two lost) and state_json shares buffers, so the run is handed over (WM_APP_RUN_MADE) to choose it
        if (err.empty()) toast("Run \"" + label + "\" is ready");
        else { sh_delete(rando_dir() + L"\\" + wide(name)); toast(err); }
        rando_busy = false;
        if (err.empty()) PostMessageW(hwnd, WM_APP_RUN_MADE, 0, (LPARAM)new std::string(name));
        else PostMessageW(hwnd, WM_APP_STATE, 0, 0);
    }).detach();
}
static bool rando_name_ok(const std::string &n) { return !n.empty() && plain_name(wide(n)) && file_exists(rando_dir() + L"\\" + wide(n) + L"\\run.ini"); }

static void on_message(const std::string &m)
{
    m_last = m;
    std::string type = field(m, "type");
    if (type == "ready") {
        mark("page loaded");
        send_state();
        mark("state sent");
        static bool checked;
        if (!checked && !update_url().empty() && ini_get(ini_load(), "check_updates", "1") != "0") { checked = true; std::thread(update_check).detach(); }
        static bool pack_read;
        if (!pack_read && !pack_url().empty()) { pack_read = true; std::thread(pack_check).detach(); }
        if (!pending_iso.empty()) { start_install(pending_iso); pending_iso.clear(); }
    }
    else if (type == "set") {
        std::string k = field(m, "key"), v = field(m, "value");
        if (k.empty() || k.find_first_of("=[]\n") != std::string::npos || v.find_first_of("\n;") != std::string::npos) return;
        size_t x = v.find('x');
        if (k == "window_size" && x != std::string::npos) { ini_set("window_width", v.substr(0, x)); ini_set("window_height", v.substr(x + 1)); }
        else ini_set(k, v);
    } else if (type == "key") {
        std::string a = field(m, "action"), k = field(m, "key");
        if (!a.empty() && !k.empty() && k.find_first_of("=\n;") == std::string::npos) ini_set(a, k);
    } else if (type == "play") play();
    else if (type == "randoCreate") {
        if (updating()) { toast("The launcher is updating: make the run once it has restarted."); send_state(); return; }
        rando::Options o;
        o.fluorite_sanity = field(m, "fs") == "1"; o.long_challenges = field(m, "lc") == "1"; o.doors = field(m, "dw") == "1";
        o.keep_consumables = field(m, "kc") == "1";
        rando_create(field(m, "name"), field(m, "seed"), o);
        send_state();
    }
    else if (type == "randoDelete") {   // {run}: to the Recycle Bin; no longer the one played
        std::string n = field(m, "run");
        // Not while the game runs: its E: drive is that run's saves folder. Where the drive has no Recycle Bin (a USB
        // stick, a network folder) Windows asks first (FOF_WANTNUKEWARNING), as for a mod: the run's saves would be gone.
        if (game_running()) toast("Close the game first: it may be playing that run.");
        else if (rando_name_ok(n)) {
            if (sh_delete(rando_dir() + L"\\" + wide(n), FOF_ALLOWUNDO | FOF_WANTNUKEWARNING)) {
                if (ini_get(ini_load(), "randomizer_run", "") == n) ini_set("randomizer_run", "");
                if (ini_get(ini_load(), "randomizer_pick", "") == n) ini_set("randomizer_pick", "");
                toast("Deleted the run " + n + ".");
            } else toast("The run " + n + " wasn't deleted (cancelled, or a file is in use).");
        }
        send_state();
    }
    else if (type == "pickDisc") pick_disc();
    else if (type == "open") open_place(field(m, "what"));
    else if (type == "importSave") import_save();
    else if (type == "shortcut") make_shortcut();
    else if (type == "update") {
        // An update ends by restarting the launcher, which would cut off an HD pack download (its parts are deleted at
        // the next start) or a run being made: those finish first. Neither starts while an update runs (below).
        bool pack_busy;
        { std::lock_guard<std::mutex> g(pk_lock); pack_busy = pk_state == "busy"; }
        if (pack_busy) toast("Let the HD pack download finish first: updating restarts the launcher.");
        else if (rando_busy) toast("Let the run being made finish first: updating restarts the launcher.");
        else {
            std::lock_guard<std::mutex> g(upd_lock);
            if (game_running()) toast("Close the game first: the update replaces its files.");
            else if ((upd_state == "available" || upd_state == "error") && !installing) { upd_state = "downloading"; std::thread(update_install).detach(); }
        }
    }
    else if (type == "reset") {   // everything but [launcher], which would otherwise bring back first-run setup
        std::vector<std::pair<std::string, std::string>> keep;
        std::string k, v;
        for (auto &l : ini_load()) if (line_key(l, &k, &v) && !strcmp(section_of(k), "launcher")) keep.push_back({k, v});
        DeleteFileW(ini_path().c_str());
        ini_load();   // writes the defaults
        for (auto &kv : keep) ini_set(kv.first, kv.second);
        send_state();
        toast("All settings are back to their defaults");
    } else if (type == "window") {
        std::string a = field(m, "action");
        if (a == "min") ShowWindow(hwnd, SW_MINIMIZE);
        else if (a == "close") PostMessageW(hwnd, WM_CLOSE, 0, 0);
    } else if (type == "mod" || type == "modMove") {   // {name, on: 0|1} / {name, d: -1|1}
        auto v = mods_get();
        std::wstring name = wide(field(m, "name"));
        size_t i = 0;
        while (i < v.size() && _wcsicmp(v[i].name, name.c_str())) i++;
        if (i == v.size()) { send_state(); return; }   // removed from the folder meanwhile
        if (type == "mod") v[i].on = field(m, "on") == "1";
        else {
            size_t j = i + atoi(field(m, "d").c_str());
            if (j < v.size()) std::swap(v[i], v[j]);
        }
        mods_save(v);
        send_state();
    } else if (type == "packCancel") {   // stop the HD pack download: the queue and the folder being fetched
        std::lock_guard<std::mutex> g(pk_lock);
        if (pk_state == "busy") { pk_cancel = true; pk_queue.clear(); }
    } else if (type == "packGet") {   // {folder}: download and install that pack folder, or "all" of them
        if (game_running()) toast("Close the game first: it's using the mods.");
        else if (updating()) toast("The launcher is updating: download the HD pack once it has restarted.");
        else pack_get(field(m, "folder"));
        send_state();
    } else if (type == "modInstall") {   // pick a .zip and unpack it as a new mod folder
        COMDLG_FILTERSPEC types[] = {{L"Mod (*.zip)", L"*.zip"}};
        std::wstring zip = pick_file(types, 1, L"Choose a mod to install");
        if (zip.empty()) return;
        toast("Installing...");
        std::thread(mod_install, zip).detach();
    } else if (type == "modRemove") {   // {name}: the mod's folder goes to the Recycle Bin; Windows asks first if it's too big for it
        std::wstring n = wide(field(m, "name"));
        if (!plain_name(n)) return;
        toast("Removing " + utf8(n) + "...");
        std::thread([n] {   // off the window's thread: recycling a pack folder of thousands of files takes seconds
            CoInitializeEx(0, COINIT_APARTMENTTHREADED);
            toast(sh_delete(exe_dir + L"\\mods\\" + n, FOF_ALLOWUNDO | FOF_WANTNUKEWARNING) ? "Removed " + utf8(n) : utf8(n) + " wasn't removed (cancelled, or the game is using it)");
            CoUninitialize();
            PostMessageW(hwnd, WM_APP_STATE, 0, 0);
        }).detach();
    } else if (type == "drag") {   // the page's title bar: move the window as Windows does for a caption
        ReleaseCapture();
        PostMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    }
}

// ---- the window ----------------------------------------------------------------------------------------
static std::wstring extract_ui(const std::wstring &dir)
{
    make_dirs(dir);
    HRSRC r = FindResourceW(0, MAKEINTRESOURCEW(IDR_INDEX_HTML), RT_RCDATA);
    HGLOBAL g = r ? LoadResource(0, r) : 0;
    if (g) {
        if (FILE *f = _wfopen((dir + L"\\index.html").c_str(), L"wb")) { fwrite(LockResource(g), SizeofResource(0, r), 1, f); fclose(f); }
    }
    return dir;
}

static void no_webview2()
{
    MessageBoxW(hwnd, L"The launcher needs the Microsoft Edge WebView2 Runtime, which comes with Windows 11.\n\n"
                      L"You can get it free from microsoft.com/edge/webview2. Once the game is installed, bin\\onimusha.exe also starts it without the launcher.",
                L"Genma Onimusha", MB_ICONWARNING);
    PostQuitMessage(1);
}

static void start_webview()
{
    std::wstring ui = extract_ui(data_dir + L"\\ui");
    // Debug: ONI_LAUNCHER_UI=<folder> serves the page from a source folder instead (art is still taken from data_dir).
#ifdef ONI_DEBUG   // development builds only: the page served from here can move and delete files
    wchar_t dev[MAX_PATH] = L"";
    if (GetEnvironmentVariableW(L"ONI_LAUNCHER_UI", dev, MAX_PATH)) ui = dev;
#endif
    std::wstring udf = data_dir + L"\\WebView2";
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(0, udf.c_str(), 0,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([ui](HRESULT r, ICoreWebView2Environment *env) -> HRESULT {
            if (FAILED(r) || !env) { no_webview2(); return S_OK; }
            mark("WebView2 environment");
            return env->CreateCoreWebView2Controller(hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([ui](HRESULT r2, ICoreWebView2Controller *c) -> HRESULT {
                if (FAILED(r2) || !c) {   // it used to close without a word
                    wchar_t m[200];
                    swprintf(m, 200, L"The launcher's window couldn't start WebView2 (error %08lx).\n\nRestarting Windows, or repairing the Microsoft Edge WebView2 Runtime in Settings > Apps, usually fixes it.", (unsigned long)r2);
                    MessageBoxW(hwnd, m, L"Genma Onimusha", MB_ICONWARNING);
                    PostQuitMessage(1);
                    return S_OK;
                }
                controller = c;
                mark("WebView2 controller");
                controller->get_CoreWebView2(&webview);
                ComPtr<ICoreWebView2Controller2> c2;
                if (SUCCEEDED(controller.As(&c2))) c2->put_DefaultBackgroundColor({255, 12, 10, 9});   // no white flash
                ComPtr<ICoreWebView2Settings> st;
                webview->get_Settings(&st);
#ifdef ONI_DEBUG
                wchar_t devtools[4] = L"";
                st->put_AreDevToolsEnabled(GetEnvironmentVariableW(L"ONI_LAUNCHER_DEVTOOLS", devtools, 4) && devtools[0] == L'1');
#else
                st->put_AreDevToolsEnabled(FALSE);
#endif
                st->put_AreDefaultContextMenusEnabled(FALSE);
                st->put_IsStatusBarEnabled(FALSE);
                st->put_IsZoomControlEnabled(FALSE);
                ComPtr<ICoreWebView2Controller4> c4;   // a file dropped on the window would otherwise replace the page
                if (SUCCEEDED(controller.As(&c4))) c4->put_AllowExternalDrop(FALSE);
                ComPtr<ICoreWebView2Settings3> st3;
                if (SUCCEEDED(st.As(&st3))) st3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
                ComPtr<ICoreWebView2_3> w3;
                if (SUCCEEDED(webview.As(&w3))) {
                    w3->SetVirtualHostNameToFolderMapping(L"app.example", ui.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                    w3->SetVirtualHostNameToFolderMapping(L"art.example", art_dir().c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                    make_dirs(exe_dir + L"\\mods");   // mods' preview pictures
                    w3->SetVirtualHostNameToFolderMapping(L"mods.example", (exe_dir + L"\\mods").c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                }
                // Only the launcher's own page talks to it, and the view never leaves that page: anything served from
                // mods.example (a mod's files) or the web can't reach the messages that move and delete files.
                auto from_app = [](PCWSTR uri) { return uri && !wcsncmp(uri, L"https://app.example/", 20); };
                webview->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>([from_app](ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *a) -> HRESULT {
                    PWSTR src = 0, j = 0;
                    if (SUCCEEDED(a->get_Source(&src)) && from_app(src) && SUCCEEDED(a->get_WebMessageAsJson(&j))) { on_message(utf8(j)); CoTaskMemFree(j); }
                    CoTaskMemFree(src);
                    return S_OK;
                }).Get(), 0);
                webview->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>([from_app](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *a) -> HRESULT {
                    PWSTR uri = 0;
                    if (SUCCEEDED(a->get_Uri(&uri)) && !from_app(uri)) a->put_Cancel(TRUE);
                    CoTaskMemFree(uri);
                    return S_OK;
                }).Get(), 0);
                // Web links in the page open in the browser, not in the launcher; nothing else is opened.
                webview->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>([](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *a) -> HRESULT {
                    PWSTR uri = 0;
                    if (SUCCEEDED(a->get_Uri(&uri)) && !wcsncmp(uri, L"https://", 8)) ShellExecuteW(0, L"open", uri, 0, 0, SW_SHOWNORMAL);
                    CoTaskMemFree(uri);
                    a->put_Handled(TRUE);
                    return S_OK;
                }).Get(), 0);
                // If the page can't be read (seen once: the ui folder had kept stale permissions and WebView2 answered
                // ERR_ACCESS_DENIED), write it to a new folder and try once more.
                webview->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>([](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *a) -> HRESULT {
                    static bool retried;
                    BOOL ok = TRUE;
                    a->get_IsSuccess(&ok);
                    ComPtr<ICoreWebView2_3> w;
                    if (ok || retried || FAILED(webview.As(&w))) return S_OK;
                    retried = true;
                    mark("page failed: retrying from a new folder");
                    std::wstring fresh = extract_ui(data_dir + L"\\ui-" + std::to_wstring(GetTickCount64()));
                    w->ClearVirtualHostNameToFolderMapping(L"app.example");
                    w->SetVirtualHostNameToFolderMapping(L"app.example", fresh.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                    webview->Navigate((L"https://app.example/index.html?run=" + std::to_wstring(GetTickCount64())).c_str());
                    return S_OK;
                }).Get(), 0);
                RECT rc; GetClientRect(hwnd, &rc);
                controller->SetBoundsAndZoomFactor(rc, fit_zoom);
                // a new query each run, so WebView2 never shows a cached copy of an older page
                webview->Navigate((L"https://app.example/index.html?run=" + std::to_wstring(GetTickCount64())).c_str());
                return S_OK;
            }).Get());
        }).Get());
    if (FAILED(hr)) no_webview2();
}

// The window is 1120x720 at 100 %, scaled for the monitor's DPI. Where that doesn't fit the work area (a 1080p laptop at
// 150 % has about 1008 px for 1080), the window and the page shrink together (WebView2 zoom), so the title bar and Play
// stay on screen. at: the rectangle Windows suggests on a DPI change (kept in place, pulled inside the work area), or 0
// to centre it.
static void fit_window(HWND h, UINT dpi, const RECT *at)
{
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoW(at ? MonitorFromRect(at, MONITOR_DEFAULTTONEAREST) : MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT wa = mi.rcWork;
    int aw = wa.right - wa.left, ah = wa.bottom - wa.top;
    int W = MulDiv(1120, dpi, 96), H = MulDiv(720, dpi, 96);
    double f = (double)aw / W < (double)ah / H ? (double)aw / W : (double)ah / H;
    fit_zoom = f < 1 ? f : 1;
    W = (int)(W * fit_zoom); H = (int)(H * fit_zoom);
    int x = at ? at->left : wa.left + (aw - W) / 2, y = at ? at->top : wa.top + (ah - H) / 2;
    if (x > wa.right - W) x = wa.right - W;
    if (x < wa.left) x = wa.left;
    if (y > wa.bottom - H) y = wa.bottom - H;
    if (y < wa.top) y = wa.top;
    SetWindowPos(h, 0, x, y, W, H, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_NCCALCSIZE: if (w) return 0; break;   // no frame: the page draws everything, DWM keeps the shadow
    case WM_NCHITTEST: { LRESULT r = DefWindowProcW(h, m, w, l); return r == HTCLIENT || r == HTCAPTION ? r : HTCLIENT; }
    case WM_SIZE: if (controller) { RECT rc; GetClientRect(h, &rc); controller->SetBoundsAndZoomFactor(rc, fit_zoom); } return 0;
    case WM_DPICHANGED: fit_window(h, HIWORD(w), (RECT *)l); return 0;
    case WM_ACTIVATE:   // coming back (from Explorer, say): the mods\ folder may have changed
        if (controller && LOWORD(w) != WA_INACTIVE) { controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC); send_state(); }
        break;
    case WM_TIMER: if (w == 2) { if (!running) poll_buttons(); return 0; } { bool p = poll_pad(); if (p != pad_connected) { pad_connected = p; post(std::string("{\"type\":\"pad\",\"connected\":") + (p ? "true}" : "false}")); } return 0; }
    case WM_APP_JSON: { auto *s = (std::string *)l; post(*s); delete s; return 0; }
    case WM_APP_GAME_EXITED:
        running = false;
        ShowWindow(h, SW_SHOW);
        SetForegroundWindow(h);
        post("{\"type\":\"running\",\"running\":false}");
        send_state();   // achievements unlocked while playing
        return 0;
    case WM_APP_STATE: send_state(); return 0;   // from a worker thread: something the page shows changed
    case WM_APP_RUN_MADE: {   // from the randomizer worker: the run it made becomes the one Play starts
        auto *name = (std::string *)l;
        ini_set("randomizer_on", "1"); ini_set("randomizer_run", *name); ini_set("randomizer_pick", *name);
        delete name;
        post("{\"type\":\"randoMade\"}");   // the page clears the form's name and seed (kept until now, in case it failed)
        send_state();
        return 0;
    }
    case WM_APP_UPDATED: {   // start the new launcher and leave; it deletes the *.old files once this one is gone
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(0, self, MAX_PATH);
        STARTUPINFOW si = {sizeof si};
        PROCESS_INFORMATION pi;
        if (CreateProcessW(self, 0, 0, 0, FALSE, 0, 0, exe_dir.c_str(), &si, &pi)) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); DestroyWindow(h); }
        else set_update("error", ",\"text\":\"Updated. Start the launcher again to finish.\"");
        return 0;
    }
    case WM_APP_SETUP_DONE: {
        auto *err = (std::string *)l;
        installing = false;
        send_state();
        if (err->empty()) toast("Installed and ready. Press Play to start.");
        else post("{\"type\":\"setupError\",\"text\":" + jstr(*err) + "}");
        delete err;
        return 0;
    }
    case WM_CLOSE:
        { std::lock_guard<std::mutex> g(upd_lock); if (upd_state == "installing") return 0; }   // mid-swap: a few seconds at most
        if (installing &&MessageBoxW(h, L"The game is still being copied from your disc. Stop and close?", L"Genma Onimusha", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
        {
            bool pack_busy;
            { std::lock_guard<std::mutex> g(pk_lock); pack_busy = pk_state == "busy"; }
            if (pack_busy && MessageBoxW(h, L"The HD pack is still downloading. Closing stops it; next time it carries on where it stopped. Close anyway?",
                                         L"Genma Onimusha", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
            if (rando_busy && MessageBoxW(h, L"A randomizer run is still being made. Close anyway? It won't be kept.", L"Genma Onimusha", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
        }
        break;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(0, COINIT_APARTMENTTHREADED);
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(0, buf, MAX_PATH);
    exe_dir = buf;
    exe_dir.resize(exe_dir.find_last_of(L'\\'));
    layout_migrate(exe_dir.c_str());   // an install from before the folders (layout.h); the game does it too
    PWSTR local = 0;
    SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, 0, &local);
    data_dir = std::wstring(local ? local : exe_dir.c_str()) + L"\\Genma Onimusha\\Launcher";
    CoTaskMemFree(local);
    make_dirs(data_dir);
    // One launcher per install: a second one would delete the first one's downloads in progress (below) and write the
    // same files. It brings the first forward instead. The updater starts the new launcher before the old one has
    // quite gone, so it waits a few seconds first.
    std::wstring folder = exe_dir;
    for (auto &c : folder) c = towlower(c);
    HANDLE one = CreateMutexW(0, TRUE, (L"GenmaOnimushaLauncher-" + std::to_wstring(std::hash<std::wstring>()(folder))).c_str());
    if (one && GetLastError() == ERROR_ALREADY_EXISTS && WaitForSingleObject(one, 5000) == WAIT_TIMEOUT) {
        if (HWND other = FindWindowW(L"GenmaOnimushaLauncher", 0)) { ShowWindow(other, SW_RESTORE); SetForegroundWindow(other); }
        return 0;
    }
    mark("start");
    if (game_ready()) art_refresh();
    mark("art checked");
    pad_connected = poll_pad();
    std::thread([] {
        // What a launcher closed mid-way left: mods\.installing-<n> (an Install .zip) and .update. This is the only
        // launcher for this folder (above), and installs wait for leftovers_cleared, so nothing is in progress yet.
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((exe_dir + L"\\mods\\.installing-*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            // to the Recycle Bin: one may hold an HD pack folder's earlier copy that couldn't be put back (pack_worker)
            do if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) sh_delete(exe_dir + L"\\mods\\" + fd.cFileName, FOF_ALLOWUNDO);
            while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        // pack parts and half-unpacked folders left in mods\.download\ by a download that never came back for them
        // (a month untouched; a folder's stage from an older release is one)
        if ((h = FindFirstFileW((exe_dir + L"\\mods\\.download\\*").c_str(), &fd)) != INVALID_HANDLE_VALUE) {
            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            ULONGLONG t = (ULONGLONG)now.dwHighDateTime << 32 | now.dwLowDateTime;
            do {
                if (fd.cFileName[0] == L'.') continue;
                ULONGLONG w = (ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32 | fd.ftLastWriteTime.dwLowDateTime;
                if (t - w <= 30ull * 24 * 3600 * 10000000) continue;
                std::wstring p = exe_dir + L"\\mods\\.download\\" + fd.cFileName;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) sh_delete(p, FOF_ALLOWUNDO); else DeleteFileW(p.c_str());
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        leftovers_cleared = true;
        // an update swap cut off: finish it (the files in .update were checked and unpacked before it began)
        std::wstring swapping = data_dir + L"\\update\\swapping";
        if (file_exists(swapping)) {
            std::vector<std::wstring> moved;
            if (swap_in(exe_dir + L"\\.update", moved)) DeleteFileW(swapping.c_str());   // else: tried again next start
        }
        if (!file_exists(swapping) && GetFileAttributesW((exe_dir + L"\\.update").c_str()) != INVALID_FILE_ATTRIBUTES) sh_delete(exe_dir + L"\\.update");
        remove_old_files();
    }).detach();
    int argc = 0;
    if (wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        if (argc > 1) pending_iso = argv[1];
        LocalFree(argv);
    }

    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(0, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hbrBackground = CreateSolidBrush(RGB(12, 10, 9));
    wc.lpszClassName = L"GenmaOnimushaLauncher";
    RegisterClassExW(&wc);
    // 1120x720 at 100%, centred on the primary monitor's work area, scaled for its DPI.
    POINT origin = {0, 0};
    HMONITOR mon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoW(mon, &mi);
    RECT wa = mi.rcWork;
    hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"Genma Onimusha", WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                           wa.left, wa.top, 1120, 720, 0, 0, inst, 0);
    UINT dpi = GetDpiForWindow(hwnd);   // the window is on that monitor now: size and centre it (fit_window below)
    MARGINS mg = {0, 0, 0, 1};
    DwmExtendFrameIntoClientArea(hwnd, &mg);   // keeps the drop shadow without a frame
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    int round = 2 /* DWMWCP_ROUND */;
    DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof round);
    fit_window(hwnd, dpi, 0);
    ShowWindow(hwnd, show);
    mark("window shown");
    SetTimer(hwnd, 1, 2000, 0);
    SetTimer(hwnd, 2, 33, 0);
    if (game_ready()) std::thread(game_facts).detach();
    start_webview();

    MSG msg;
    while (GetMessageW(&msg, 0, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    controller.Reset();
    webview.Reset();
    CoUninitialize();
    // Not return: a worker thread (an HD pack download, a run being made) may still be running, and returning would
    // destroy the globals it uses under it. ExitProcess ends the threads first; every file the launcher keeps is
    // already written (write_file_atomic flushes), and the tar job ends with the process.
    ExitProcess(0);
}
