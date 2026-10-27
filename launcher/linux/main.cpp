// Genma Onimusha launcher, Linux: the Windows launcher's page (launcher/ui/index.html, built in) in Qt WebEngine, and
// the same native side as launcher/main.cpp, ported: onimusha.ini, installing the game from the player's disc image,
// art taken from that disc, controller and display detection, the mods/ list (mods/mods.txt), the HD pack and the
// randomizer, and starting the game (bin/onimusha). The shared code (text.h, disc.h, files.h, randomizer.h and the
// game's mods_list.h, layout.h) is the Windows launcher's own, through launcher/linux/windows.h.
//
// Qt WebEngine, not WebKitGTK: SteamOS ships Qt 6 WebEngine and no WebKitGTK. The page is served from oni://app/ and
// talks to this side through QWebChannel, behind the window.chrome.webview object it uses under WebView2 (the
// messages are main.cpp's, listed at its top). What Linux has no counterpart for: the self-update (the release page
// opens instead), the Recycle Bin (the desktop's trash), the desktop shortcut (a .desktop file).
#include <QApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QIcon>
#include <QImage>
#include <QMessageAuthenticationCode>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QScreen>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QWebChannel>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlScheme>
#include <QWebEngineUrlSchemeHandler>
#include <QWebEngineView>
#include <QWindow>
#include <QCloseEvent>

#include "windows.h"   // launcher/linux/windows.h
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <functional>
#include <filesystem>
#include <map>
#include <set>
#include <cstdio>
#include <cstdint>
#include <spawn.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <linux/input.h>
#include "../../src/host/settings_default.h"
#include "../../src/host/mods_list.h"
#include "../../src/host/layout.h"
#include "../../src/host/achievements_list.h"
#include "../text.h"
#include "../disc.h"
#include "../files.h"
#include "../randomizer.h"

extern char **environ;

static const char *const VERSION = ONI_VERSION;
static const char *const USA_XBE_SHA1 = "08bfe432eb55877b5af2dd401b713743c5b751cd";

static QWebEngineView *win;
static QWebEnginePage *pg;
static std::wstring exe_dir, data_dir;   // Windows form (windows.h): px() for the real path
static std::atomic<bool> installing{false}, running{false};
static bool pad_connected;
static std::wstring pending_iso;   // a disc image given on the command line, installed once the page is up

bool oni_hash(int kind, const void *key, size_t key_n, const void *data, size_t n, uint8_t *out)
{
    QByteArray d = QByteArray::fromRawData((const char *)data, (int)n), h;
    h = kind == 2 ? QMessageAuthenticationCode::hash(d, QByteArray((const char *)key, (int)key_n), QCryptographicHash::Sha1)
                  : QCryptographicHash::hash(d, QCryptographicHash::Sha1);
    if (h.size() != 20) return false;
    memcpy(out, h.constData(), 20);
    return true;
}
static std::wstring winpath(const std::string &posix) { std::wstring w = wide(posix); for (auto &c : w) if (c == L'/') c = L'\\'; return w; }
static QString qs(const std::string &s) { return QString::fromStdString(s); }
static bool on_gui_thread() { return QThread::currentThread() == qApp->thread(); }
static void on_gui(std::function<void()> f) { QMetaObject::invokeMethod(qApp, std::move(f), Qt::QueuedConnection); }

static void mark(const char *what);
// Send JSON to the page. Safe from any thread: other threads hand it to the window's.
static void post(const std::string &json)
{
    if (!on_gui_thread()) return on_gui([json] { post(json); });
    if (pg) pg->runJavaScript("window.__oniRecv && window.__oniRecv(" + qs(json) + ")");   // dropped before the page is up, as on Windows
}
static void toast(const std::string &text) { post("{\"type\":\"toast\",\"text\":" + jstr(text) + "}"); }
// A yes/no question in a box, from any thread (a worker waits for the answer).
static bool ask(const std::string &text, bool warn = false)
{
    bool yes = false;
    auto f = [&] {
        yes = (warn ? QMessageBox::warning(win, "Genma Onimusha", qs(text), QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
                    : QMessageBox::question(win, "Genma Onimusha", qs(text), QMessageBox::Yes | QMessageBox::No, QMessageBox::No)) == QMessageBox::Yes;
    };
    if (on_gui_thread()) f(); else QMetaObject::invokeMethod(qApp, f, Qt::BlockingQueuedConnection);
    return yes;
}

// Startup timings in <data>/startup.log, rewritten each start.
static ULONGLONG t_start = GetTickCount64();
static void mark(const char *what)
{
    static FILE *f;
    if (!f && !data_dir.empty()) f = _wfopen((data_dir + L"\\startup.log").c_str(), L"w");
    if (f) { fprintf(f, "%6llu ms  %s\n", GetTickCount64() - t_start, what); fflush(f); }
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
        write_text_atomic(ini_path().c_str(), settings_default_file);
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
static void ini_set(const std::string &key, const std::string &value)   // the rules and the line editing: text.h
{
    if (!ini_set_ok(key, value)) { mark(("ini_set refused: " + key.substr(0, 40)).c_str()); return; }
    bool ok;
    auto lines = ini_load(&ok);
    if (!ok) { toast("Couldn't read onimusha.ini, so the setting wasn't saved."); return; }
    ini_set_lines(lines, key, value);
    std::string out;
    for (auto &l : lines) out += l + "\r\n";   // as the Windows launcher and the game write it
    if (!write_file_atomic(ini_path().c_str(), out.data(), out.size())) toast("Couldn't save onimusha.ini next to the launcher.");
}

// ---- files -------------------------------------------------------------------------------------------------------
// Hex digest of a file: SHA-1 (game check) or SHA-256 (download check).
static std::string hash_file(const std::wstring &path, bool sha256 = false)
{
    QFile f(qs(px(path)));
    if (!f.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash h(sha256 ? QCryptographicHash::Sha256 : QCryptographicHash::Sha1);
    if (!h.addData(&f)) return {};
    return h.result().toHex().toStdString();
}
// Delete a file or folder tree. RECYCLE: to the desktop's trash; ASK: where it can't go there, ask before deleting it
// for good (as Windows does for a drive without a Recycle Bin). Links are removed, never followed.
enum { RECYCLE = 1, ASK = 2 };
static bool sh_delete(const std::wstring &path, int flags = 0)
{
    std::string p = px(path);
    std::error_code ec;
    if (!std::filesystem::exists(std::filesystem::symlink_status(p, ec))) return true;
    if ((flags & RECYCLE) && QFile::moveToTrash(qs(p))) return true;
    if ((flags & ASK) && !ask("It can't be moved to the trash. Delete it for good?\n\n" + p)) return false;
    std::filesystem::remove_all(p, ec);
    return !ec;
}
// A program on PATH (or "" when there is none), looked up before fork: the child may only exec.
static std::string which(const char *name)
{
    const char *path = getenv("PATH");
    std::string all = path && *path ? path : "/usr/local/bin:/usr/bin:/bin";
    for (size_t a = 0, b; a <= all.size(); a = b + 1) {
        if ((b = all.find(':', a)) == std::string::npos) b = all.size();
        std::string p = all.substr(a, b - a) + "/" + name;
        if (b > a && !access(p.c_str(), X_OK)) return p;
    }
    return {};
}
// Run a program and wait for it: its exit code, -1 when it couldn't start. It ends with the launcher (closing the
// launcher mid-unpack used to leave tar writing into a folder the next start deletes, on Windows).
static int run_wait(const std::vector<std::string> &args)
{
    std::vector<char *> argv;
    for (auto &a : args) argv.push_back((char *)a.c_str());
    argv.push_back(nullptr);
    pid_t parent = getpid(), pid = fork();
    if (pid < 0) return -1;
    if (!pid) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parent) _exit(127);
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, 0); dup2(null, 1); }
        execv(argv[0], argv.data());
        _exit(127);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
// Unpack a zip: bsdtar (libarchive, as Windows' tar.exe), else unzip, else Python's zipfile.
static bool untar(const std::wstring &zip, const std::wstring &dir)
{
    std::string z = px(zip), d = px(dir), t;
    if (!(t = which("bsdtar")).empty()) return run_wait({t, "-xf", z, "-C", d}) == 0;
    if (!(t = which("unzip")).empty()) return run_wait({t, "-q", "-o", z, "-d", d}) == 0;
    if (!(t = which("python3")).empty()) return run_wait({t, "-I", "-m", "zipfile", "-e", z, d}) == 0;
    return false;
}

// ---- game files: install from the disc image ----------------------------------------------------------
static std::wstring game_dir() { return exe_dir + L"\\game"; }
// game/.installing is there from an install's start until it finishes: an install that stopped (closed, disk full)
// isn't ready to play, and the next one copies only what it didn't get to.
static bool game_ready() { return file_exists(game_dir() + L"\\default.xbe") && !file_exists(game_dir() + L"\\.installing"); }

// The disc image's file list: disc.h, read through read_fd.
static bool read_fd(void *f, uint64_t off, void *buf, uint32_t n)
{
    for (uint32_t got = 0; got < n;) {
        ssize_t r = pread((int)(intptr_t)f, (char *)buf + got, n - got, (off_t)(off + got));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        got += (uint32_t)r;
    }
    return true;
}

// Whether the installed default.xbe is the USA release, and the game folder's size: worked out on a worker thread,
// at start and after each install, then sent to the page.
static std::mutex game_lock;
static std::string game_verified, game_size;   // "" until known
static void send_state();
static void game_facts()
{
    std::string v = hash_file(game_dir() + L"\\default.xbe") == USA_XBE_SHA1 ? "true" : "false";
    uint64_t total = 0;
    each_file(game_dir(), [&](const std::wstring &, const WIN32_FIND_DATAW &fd) { total += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow; });
    char b[32];
    snprintf(b, sizeof b, "%.2f GB", total / 1073741824.0);
    { std::lock_guard<std::mutex> g(game_lock); game_verified = v; game_size = b; }
    on_gui(send_state);
}

static void art_refresh();
static void install_(const std::wstring &iso, std::string &error);
static void setup_done(const std::string &err);
static void install(std::wstring iso)
{
    std::string error;
    try { install_(iso, error); } catch (...) { error = "Installing stopped: out of memory (is the disc image damaged?)."; }
    if (error.empty()) art_refresh();
    if (game_ready()) game_facts();
    on_gui([error] { setup_done(error); });
}
static void install_(const std::wstring &iso, std::string &error)
{
    int f = open(px(iso).c_str(), O_RDONLY | O_CLOEXEC);
    struct closer { int fd; ~closer() { if (fd >= 0) close(fd); } } close_f{f};   // a throw too
    void *ctx = (void *)(intptr_t)f;
    std::vector<disc_file> files;
    if (f < 0) error = "That file couldn't be opened.";
    else {
        posix_fadvise(f, 0, 0, POSIX_FADV_SEQUENTIAL);
        int listed = disc_list(read_fd, ctx, files);
        if (listed == 1) error = "That isn't an Xbox disc image. Choose a full ISO of Genma Onimusha.";
        else {
            if (listed == 2) error = "That disc image doesn't read right (a folder past its end, or a name that can't be): it may be damaged or cut short.";
            const disc_file *xbe = 0;
            for (auto &d : files) if (!_stricmp(d.path.c_str(), "default.xbe")) xbe = &d;
            if (!xbe && error.empty()) error = "This Xbox disc has no game on it (no default.xbe).";
            // Installing again over a working game with another disc (another region, another game): ask first.
            if (xbe && error.empty() && game_ready() && xbe->size < (64u << 20)) {
                std::vector<uint8_t> b((size_t)xbe->size);
                uint8_t d[20];
                std::string hex;
                if (read_fd(ctx, xbe->offset, b.data(), (uint32_t)b.size()) && oni_hash(1, 0, 0, b.data(), b.size(), d))
                    for (uint8_t c : d) { char x[3]; snprintf(x, sizeof x, "%02x", c); hex += x; }
                if (hex != USA_XBE_SHA1 && !ask("This disc image isn't the USA release of Genma Onimusha, which the port is made for.\n\n"
                                                "Install it over the game you have anyway?", true))
                    error = "Not installed: that disc isn't the USA release. Your game is as it was.";
            }
        }
    }
    if (error.empty()) {
        uint64_t total = 0, done = 0;
        for (auto &d : files) total += d.size;
        std::vector<char> buf(8 << 20);
        ULONGLONG last = 0;
        // Files already there at their size are skipped only when finishing a stopped install: installing again copies
        // everything, so it repairs, and one image's files never mix with another's.
        make_dirs(game_dir());
        std::wstring marker = game_dir() + L"\\.installing";
        bool resume = file_exists(marker);
        if (FILE *m = _wfopen(marker.c_str(), L"w")) fclose(m);
        for (auto &d : files) {
            std::wstring dst = game_dir() + L"\\" + wide(d.path);
            for (auto &c : dst) if (c == L'/') c = L'\\';
            make_dirs(dst.substr(0, dst.find_last_of(L'\\')));
            std::string out = px(dst);
            struct stat st;
            bool same = resume && !stat(out.c_str(), &st) && S_ISREG(st.st_mode) && (uint64_t)st.st_size == d.size;
            if (!same) {
                int o = open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
                if (o < 0) { error = "Couldn't write into the game folder: " + out; break; }
                uint64_t left = d.size, off = d.offset;
                while (left) {
                    uint32_t n = (uint32_t)std::min<uint64_t>(left, buf.size());
                    bool wrote = read_fd(ctx, off, buf.data(), n);
                    for (uint32_t put = 0; wrote && put < n;) {
                        ssize_t w = write(o, buf.data() + put, n - put);
                        if (w < 0 && errno == EINTR) continue;
                        if (w <= 0) wrote = false; else put += (uint32_t)w;
                    }
                    if (!wrote) { error = "Copying stopped: the disc image may be damaged, or the drive is full."; break; }
                    left -= n; off += n; done += n;
                    if (GetTickCount64() - last > 100) {
                        last = GetTickCount64();
                        char m[512];
                        snprintf(m, sizeof m, "{\"type\":\"progress\",\"pct\":%.2f,\"done\":%llu,\"total\":%llu,\"file\":%s}", total ? 100.0 * done / total : 0.0,
                                 (unsigned long long)done, (unsigned long long)total, jstr(d.path).c_str());
                        post(m);
                    }
                }
                // on disk before the next file: after a power cut a file can keep its size with zeros inside, and a
                // resumed install skips files of the right size
                if (error.empty() && fdatasync(o)) error = "Copying stopped: the drive is full, or it was removed.";
                close(o);
                if (!error.empty()) break;
            } else done += d.size;
        }
        if (error.empty()) DeleteFileW(marker.c_str());
    }
}

// ---- art from the player's disc: .BG files are a 2 KB XPR header + a 1024x512 DXT1 image ------------------
// Written as 1024x768 bitmaps (the game shows them stretched to 4:3) into the launcher's data folder.
static std::wstring art_dir() { return data_dir + L"\\art"; }
static bool decode_bg(const std::wstring &src, const std::wstring &dst)
{
    FILE *f = _wfopen(src.c_str(), L"rb");
    if (!f) return false;
    std::vector<uint8_t> d(2048 + 1024 * 512 / 2);
    size_t n = fread(d.data(), 1, d.size(), f);
    fclose(f);
    if (n != d.size() || memcmp(d.data(), "XPR0", 4)) return false;
    const int W = 1024, H = 512, OH = 768;
    std::vector<uint32_t> px_(W * H);
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
            for (int j = 0; j < 16; j++) px_[(by + (j >> 2)) * W + bx + (j & 3)] = pal[(bits >> (2 * j)) & 3];
        }
    QImage out(W, OH, QImage::Format_RGB32);   // stretch 512 -> 768 rows, linear
    for (int y = 0; y < OH; y++) {
        float sy = (y + 0.5f) * H / OH - 0.5f;
        int y0 = sy < 0 ? 0 : (int)sy, y1 = y0 + 1 < H ? y0 + 1 : H - 1;
        float t = sy - y0 < 0 ? 0 : sy - y0;
        uint8_t *o = out.scanLine(y);
        for (int x = 0; x < W; x++, o += 4) {
            uint32_t a = px_[y0 * W + x], b = px_[y1 * W + x];
            for (int c = 0; c < 3; c++) o[c] = (uint8_t)(((a >> (8 * c)) & 255) * (1 - t) + ((b >> (8 * c)) & 255) * t);
            o[3] = 255;
        }
    }
    return out.save(qs(px(dst)), "BMP");
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

// ---- mods/: the list and mods/mods.txt are in src/host/mods_list.h, shared with the game ----------------------
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
            if (!t.compare(0, 3, "\xEF\xBB\xBF")) t.erase(0, 3);
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
// A mod folder's size and the files it would put in place of the game's, in one walk, kept while the folder's own
// time stamps stay the same (the state is rebuilt at every window activation).
struct mod_facts { uint64_t bytes = 0; int files = 0; FILETIME made = {}, written = {}; };
static bool same_time(const FILETIME &a, const FILETIME &b) { return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime; }
static mod_facts mod_facts_of(const std::wstring &dir)
{
    static std::map<std::wstring, mod_facts> cache;
    WIN32_FILE_ATTRIBUTE_DATA a = {};
    GetFileAttributesExW(dir.c_str(), GetFileExInfoStandard, &a);
    auto it = cache.find(dir);
    if (it != cache.end() && same_time(it->second.made, a.ftCreationTime) && same_time(it->second.written, a.ftLastWriteTime)) return it->second;
    mod_facts f;
    f.made = a.ftCreationTime; f.written = a.ftLastWriteTime;
    each_file(dir, [&](const std::wstring &rel, const WIN32_FIND_DATAW &fd) {
        f.bytes += (uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow;
        f.files += rel.find(L'\\') != std::wstring::npos || (_wcsicmp(rel.c_str(), L"mod.ini") && _wcsicmp(rel.c_str(), L"patch.txt") && _wcsnicmp(rel.c_str(), L"preview.", 8));
    });
    return cache[dir] = f;
}
static std::string mods_json()
{
    std::string j = "[";
    for (auto &x : mods_get()) {
        char title[256] = "", about[1024] = "", builtin[64] = "", author[128] = "", version[64] = "", modern[8] = "", pack[64] = "";
        mod_ini(exe_dir.c_str(), x.name, "", "name", title, sizeof title);
        mod_ini(exe_dir.c_str(), x.name, "", "description", about, sizeof about);
        mod_ini(exe_dir.c_str(), x.name, "", "builtin", builtin, sizeof builtin);
        mod_ini(exe_dir.c_str(), x.name, "", "author", author, sizeof author);
        mod_ini(exe_dir.c_str(), x.name, "", "version", version, sizeof version);
        mod_ini(exe_dir.c_str(), x.name, "", "modern", modern, sizeof modern);
        mod_ini(exe_dir.c_str(), x.name, "", "pack", pack, sizeof pack);
        std::wstring dir = exe_dir + L"\\mods\\" + x.name;
        bool patch = file_exists(dir + L"\\patch.txt");
        if (patch && !about[0]) snprintf(about, sizeof about, "%s", patch_about(dir + L"\\patch.txt").c_str());
        const char *preview = file_exists(dir + L"\\preview.png") ? "preview.png" : file_exists(dir + L"\\preview.jpg") ? "preview.jpg" : "";
        mod_facts facts = mod_facts_of(dir);
        std::string extra = ",\"author\":" + jstr(author) + ",\"version\":" + jstr(version) + ",\"pack\":" + jstr(pack) + ",\"bytes\":" + std::to_string(facts.bytes) + ",\"modern\":" + (atoi(modern) ? "true" : "false") + ",\"preview\":" + jstr(preview)
                          + ",\"files\":" + std::to_string(facts.files) + ",\"patch\":" + (patch ? "true" : "false")
                          + ",\"textures\":" + ((GetFileAttributesW((dir + L"\\textures").c_str()) & (FILE_ATTRIBUTE_DIRECTORY | 0x80000000u)) == FILE_ATTRIBUTE_DIRECTORY ? "true" : "false");
        j += (j.size() > 1 ? "," : "") + std::string("{\"name\":") + jstr(utf8(x.name)) + ",\"on\":" + (x.on ? "true" : "false")
           + ",\"title\":" + jstr(title) + ",\"about\":" + jstr(about) + ",\"builtin\":" + jstr(builtin) + extra + "}";
    }
    return j + "]";
}

// Install a mod from a .zip: unpacked into a temporary folder inside mods/, then renamed to the folder the zip holds
// when it holds just one, or else to the zip's name. Installs wait for the start-up clean-up of what a launcher
// closed mid-way left (leftovers_cleared).
static std::atomic<bool> leftovers_cleared{false};
static void wait_leftovers() { while (!leftovers_cleared) Sleep(50); }
static void mod_install(std::wstring zip)
{
    wait_leftovers();
    std::wstring mods = exe_dir + L"\\mods", stage = mods + L"\\.installing-" + std::to_wstring(GetTickCount64());
    make_dirs(stage);
    bool unpacked = untar(zip, stage);
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
    if (!unpacked || files + dirs == 0) msg = which("bsdtar").empty() && which("unzip").empty() && which("python3").empty() ? "Unpacking a zip needs bsdtar, unzip or python3." : "That zip couldn't be unpacked.";
    else {
        if (dirs == 1 && !files) { from = stage + L"\\" + only; name = only; }
        std::wstring to = mods + L"\\" + name;
        for (int k = 2; GetFileAttributesW(to.c_str()) != INVALID_FILE_ATTRIBUTES; k++) to = mods + L"\\" + name + L" (" + std::to_wstring(k) + L")";
        if (MoveFileExW(from.c_str(), to.c_str(), 0)) {
            char en[8] = "";
            std::wstring folder = to.substr(mods.size() + 1);
            mod_ini(exe_dir.c_str(), folder.c_str(), "", "enabled", en, sizeof en);
            msg = "Installed " + utf8(folder) + (!strcmp(en, "0") ? ". It starts off: switch it on in the list." : ". It's on from the next game start.");
        }
        else msg = "Couldn't move the mod into the mods folder.";
    }
    sh_delete(stage);
    toast(msg);
    on_gui(send_state);
}

// ---- achievements (src/host/achievements_list.h; the game writes saves/achievements.dat, sealed: layout.h) ----------
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
        j += (j.size() > 1 ? "," : "") + std::string("{\"id\":") + jstr(a.id) + ",\"name\":" + jstr(a.name) + ",\"desc\":" + jstr(a.desc) + ",\"t\":" + t + "}";
    }
    return j + "]";
}
// ---- play time: the game keeps saves/playtime.txt (seconds = N) -------------------------------------------------
static std::string playtime_seconds(const std::wstring &saves = exe_dir + L"\\saves")
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
    std::string s = "{", k = "{";
    for (auto &d : ini_defaults()) {
        const char *key = d.key.c_str(), *def = d.value.c_str();
        if (d.section == "keys") { k += (k.size() > 1 ? "," : "") + jstr(d.key) + ":" + str(key, def); continue; }
        std::string v;
        if (d.key == "window") { std::string w = ini_get(ini, "window", def); v = jstr(!_stricmp(w.c_str(), "borderless") || !_stricmp(w.c_str(), "fullscreen") ? "borderless" : "windowed"); }
        else if (d.key == "modern_controls") v = num(key, num("stick_move", def).c_str());
        else if (d.key == "weapon_wheel") v = num(key, num("swap_weapon", def).c_str());
        else v = !d.value.empty() && d.value.find_first_not_of("0123456789.-") == std::string::npos ? num(key, def) : str(key, def);
        s += (s.size() > 1 ? "," : "") + jstr(d.key) + ":" + v;
    }
    s += "}";
    k += "}";
    // display: the screen the launcher is on, in pixels. The window's scale, not the screen's: under a fractional
    // desktop scale (a Steam Deck's KDE at 1.35) Wayland gives the screen a whole one (2), and 1280x800 read 1896x1186.
    QScreen *sc = win && win->screen() ? win->screen() : QGuiApplication::primaryScreen();
    // The screen's size in desktop units was rounded (800 / 1.35 = 592.6 came as 593), so under a fractional scale the
    // pixels are rounded to an even number, as every display mode's are.
    double scale = win ? win->devicePixelRatioF() : sc ? sc->devicePixelRatio() : 1;
    auto pixels = [scale](int v) { double d = v * scale; return scale == (int)scale ? (int)(d + 0.5) : 2 * (int)(d / 2 + 0.5); };
    char monitor[96] = "{\"w\":0,\"h\":0,\"hz\":60}";
    if (sc) snprintf(monitor, sizeof monitor, "{\"w\":%d,\"h\":%d,\"hz\":%d}", pixels(sc->size().width()), pixels(sc->size().height()), (int)(sc->refreshRate() + 0.5));
    std::string setup;
    if (installing) setup = "{\"busy\":true}";
    else if (game_ready()) {
        std::lock_guard<std::mutex> g(game_lock);
        setup = "{\"ready\":true," + (game_verified.empty() ? "" : "\"verified\":" + game_verified + ",") + "\"title\":\"Genma Onimusha\",\"path\":"
              + jstr(px(game_dir())) + ",\"size\":" + jstr(game_size) + "}";
    } else setup = "{\"ready\":false}";
    std::string art = "null";
    if (game_ready() && file_exists(art_dir() + L"\\logo.bmp")) art = "{\"logo\":\"oni://art/logo.bmp\",\"hero\":\"oni://art/hero.bmp\",\"hero2\":\"oni://art/hero2.bmp\"}";
    return std::string("{\"type\":\"state\",\"version\":") + jstr(VERSION) + ",\"monitor\":" + monitor + ",\"controller\":" + (pad_connected ? "true" : "false")
        + ",\"running\":" + (running ? "true" : "false") + ",\"setup\":" + setup + ",\"art\":" + art + ",\"s\":" + s + ",\"keys\":" + k
        + ",\"mods\":" + mods_json() + ",\"update\":" + update_json() + ",\"pack\":" + pack_json() + ",\"ach\":" + achievements_json() + ",\"playtime\":" + playtime_seconds() + ",\"rando\":" + rando_json() + "}";
}
static void send_state() { post(state_json()); }

// ---- downloads: GET a URL into a string or a file (Qt's network, on the calling thread) --------------------------
// Follows redirects, never from https to http. from > 0: carry on a file from that byte (Range); a server that sends
// the whole file anyway starts it over. progress gets bytes so far and in all. stop: set from another thread to give up.
static bool http_get(const std::string &url, std::string *body, int out, void (*progress)(uint64_t, uint64_t) = 0, uint64_t from = 0,
                     const std::atomic<bool> *stop = 0)
{
    QUrl u(qs(url));
#ifndef ONI_DEBUG   // a release downloads over HTTPS only (a development build may test against a local HTTP server)
    if (u.scheme() != "https") return false;
#endif
    if (!u.isValid() || u.isEmpty()) return false;
    QNetworkAccessManager nam;
    QNetworkRequest rq(u);
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    rq.setHeader(QNetworkRequest::UserAgentHeader, QByteArray("GenmaOnimusha/") + VERSION);
    rq.setRawHeader("Accept", "application/vnd.github+json");
    if (from) rq.setRawHeader("Range", "bytes=" + QByteArray::number((qulonglong)from) + "-");
    rq.setTransferTimeout(60000);   // a dead link fails, not hangs
    QNetworkReply *r = nam.get(rq);
    QEventLoop loop;
    bool ok = true, started = false;
    uint64_t total = 0, done = 0;
    auto take = [&] {
        if (!ok) return;
        if (!started) {
            int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status >= 300 && status < 400) return;   // a redirect's own body
            started = true;
            if (!(status == 200 || (from && status == 206))) { ok = false; r->abort(); return; }
            total = r->header(QNetworkRequest::ContentLengthHeader).toULongLong();
            if (status == 206) done = from, total += from;   // carrying on
            else if (from && out >= 0) { if (ftruncate(out, 0) || lseek(out, 0, SEEK_SET) < 0) { ok = false; r->abort(); return; } }
        }
        QByteArray b = r->readAll();
        if (body) body->append(b.constData(), (size_t)b.size());
        for (qsizetype put = 0; out >= 0 && put < b.size();) {
            ssize_t w = write(out, b.constData() + put, (size_t)(b.size() - put));
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) { ok = false; r->abort(); return; }
            put += w;
        }
        done += (uint64_t)b.size();
        if (progress && b.size()) progress(done, total);
    };
    QObject::connect(r, &QNetworkReply::readyRead, take);
    QObject::connect(r, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer poll;   // Cancel, while nothing arrives too
    QObject::connect(&poll, &QTimer::timeout, [&] { if (stop && *stop) { ok = false; r->abort(); } });
    poll.start(250);
    if (!r->isFinished()) loop.exec();
    take();
    if (r->error() != QNetworkReply::NoError || !started) ok = false;
    if (total && done != total) ok = false;
    delete r;
    return ok;
}

// ---- updates: the latest GitHub release of ONI_UPDATE_REPO ------------------------------------------------------
// Checked once per start, as on Windows. The Linux launcher doesn't swap its own files: Update opens the release
// page, where the new archive is (unpacked over this folder, it keeps game/, saves/, onimusha.ini and mods/mods.txt).
static std::mutex upd_lock;
static std::string upd_state = "none", upd_version;   // none (not checked)|current|available
static std::string update_url()
{
#ifdef ONI_DEBUG
    if (const char *over = getenv("ONI_UPDATE_URL")) return over;
#endif
    if (!*ONI_UPDATE_REPO) return {};
    return std::string("https://api.github.com/repos/") + ONI_UPDATE_REPO + "/releases/latest";
}
static std::string update_json()
{
    std::lock_guard<std::mutex> g(upd_lock);
    return std::string("{\"enabled\":") + (update_url().empty() ? "false" : "true") + ",\"state\":" + jstr(upd_state) + ",\"version\":" + jstr(upd_version) + "}";
}
static void set_update(const std::string &state)
{
    std::string v;
    { std::lock_guard<std::mutex> g(upd_lock); upd_state = state; v = upd_version; }
    post("{\"type\":\"update\",\"state\":" + jstr(state) + ",\"version\":" + jstr(v) + "}");
}
static void update_check()
{
    std::string j;
    if (!http_get(update_url(), &j, -1)) return;   // offline or rate-limited: say nothing
    std::string tag = field(j, "tag_name");
    if (tag.empty()) return;
    if (!newer_than(tag, VERSION)) return set_update("current");
    { std::lock_guard<std::mutex> g(upd_lock); upd_version = tag[0] == 'v' ? tag.substr(1) : tag; }
    set_update("available");
}

// ---- the HD pack: mod folders to download from the latest release of ONI_PACK_REPO --------------------------------
// As on Windows (main.cpp has the whole story): pack.ini lists per folder its parts, each fetched into mods/.download/,
// checked, unpacked into the folder's stage and deleted; a download cut off carries on; the new folder then replaces
// the installed one, which goes to the trash. One folder at a time.
static std::mutex pk_lock;
static std::string pk_state = "none", pk_version, pk_job, pk_text;   // none (not read)|ready|busy|error; job: the folder
static std::vector<pack_mod> pk_mods;
static std::vector<std::string> pk_queue;
static int pk_pct;
static std::atomic<bool> pk_cancel{false};
static std::string pack_url()
{
#ifdef ONI_DEBUG
    if (const char *over = getenv("ONI_PACK_URL")) return over;
#endif
    if (!*ONI_PACK_REPO) return {};
    return std::string("https://github.com/") + ONI_PACK_REPO + "/releases/latest/download/pack.ini";
}
static std::string mod_version(const std::string &folder)
{
    char v[64] = "";
    mod_ini(exe_dir.c_str(), wide(folder).c_str(), "", "version", v, sizeof v);
    return v;
}
static bool is_dir(const std::wstring &p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
static std::string pack_json()
{
    std::lock_guard<std::mutex> g(pk_lock);
    std::string j = std::string("{\"enabled\":") + (pack_url().empty() ? "false" : "true") + ",\"state\":" + jstr(pk_state)
                  + ",\"version\":" + jstr(pk_version) + ",\"job\":" + jstr(pk_job) + ",\"pct\":" + std::to_string(pk_pct) + ",\"queue\":[";
    for (size_t i = 0; i < pk_queue.size(); i++) j += (i ? "," : "") + jstr(pk_queue[i]);
    j += std::string("],\"text\":") + jstr(pk_text) + ",\"mods\":[";
    for (size_t i = 0; i < pk_mods.size(); i++) {
        auto &m = pk_mods[i];
        const char *have = !is_dir(exe_dir + L"\\mods\\" + wide(m.folder)) ? "none" : mod_version(m.folder) == pk_version ? "current" : "old";
        j += (i ? "," : "") + std::string("{\"folder\":") + jstr(m.folder) + ",\"name\":" + jstr(m.name) + ",\"about\":" + jstr(m.about)
           + ",\"size\":" + std::to_string(m.size) + ",\"have\":" + jstr(have) + "}";
    }
    return j + "]}";
}
static void pack_check()
{
    std::string ini;
    if (!http_get(pack_url(), &ini, -1)) return;
    std::vector<pack_mod> mods;
    std::string version;
    if (!pack_ini_parse(ini, version, mods)) return;
    { std::lock_guard<std::mutex> g(pk_lock); pk_mods = mods; pk_version = version; if (pk_state == "none") pk_state = "ready"; }
    on_gui(send_state);
}
static void stamp_version(const std::wstring &dir, const std::string &version)
{
    std::string text;
    if (FILE *f = _wfopen((dir + L"\\mod.ini").c_str(), L"rb")) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f))) text.append(b, n); fclose(f); }
    std::string out = stamp_version_text(text, version);
    write_file_atomic((dir + L"\\mod.ini").c_str(), out.data(), out.size());
}
static uint64_t pk_total, pk_before;
static void pack_worker()
{
    wait_leftovers();
    std::string base = pack_url();
    base.resize(base.find_last_of('/') + 1);   // parts sit next to pack.ini
    std::string version;
    { std::lock_guard<std::mutex> g(pk_lock); version = pk_version; }
    for (;;) {
        pack_mod m;
        {
            std::lock_guard<std::mutex> g(pk_lock);
            pk_cancel = false;
            if (pk_queue.empty()) { pk_state = pk_text.empty() ? "ready" : "error"; pk_job.clear(); pk_cancel = false; break; }
            std::string f = pk_queue.front();
            pk_queue.erase(pk_queue.begin());
            for (auto &x : pk_mods) if (x.folder == f) m = x;
            pk_job = f; pk_pct = 0;
        }
        on_gui(send_state);
        if (m.folder.empty()) continue;
        std::string vid;
        for (char ch : version) if (isalnum((unsigned char)ch) || ch == '.' || ch == '-') vid += ch;
        std::wstring mods = exe_dir + L"\\mods", parts = mods + L"\\.download", stage = parts + L"\\" + wide(m.folder) + L"-" + wide(vid);
        make_dirs(stage);
        pk_total = 0; pk_before = 0;
        for (auto &pt : m.parts) pk_total += pt.size;
        std::string err;
        bool stopped = false;
        for (auto &pt : m.parts) {
            std::string id;
            for (char ch : pt.sha) if (isxdigit((unsigned char)ch) && id.size() < 16) id += ch;
            std::wstring zip = parts + L"\\" + wide(pt.name) + L"." + wide(id) + L".part", done = stage + L"\\.unpacked-" + wide(id);
            if (file_exists(done)) { pk_before += pt.size; continue; }
            bool got = false;
            for (int attempt = 0; attempt < 4 && !got && !pk_cancel; attempt++) {
                if (attempt) for (DWORD t = 0; t < (2000u << attempt) && !pk_cancel; t += 250) Sleep(250);
                if (pk_cancel) break;
                int f = open(px(zip).c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
                if (f < 0) break;
                off_t have = lseek(f, 0, SEEK_END);
                got = (uint64_t)have >= pt.size || http_get(base + pt.name, 0, f, [](uint64_t done, uint64_t) {
                    static ULONGLONG last;
                    if (GetTickCount64() - last < 250) return;
                    last = GetTickCount64();
                    int pct;
                    { std::lock_guard<std::mutex> g(pk_lock); pct = pk_pct = pk_total ? (int)(100 * (pk_before + done) / pk_total) : 0; }
                    post("{\"type\":\"packpct\",\"pct\":" + std::to_string(pct) + "}");
                }, (uint64_t)have, &pk_cancel);
                close(f);
            }
            if (pk_cancel) err = "cancelled";
            else if (!got) err = "The download stopped. Try again.", stopped = true;
            else if (hash_file(zip, true) != pt.sha) err = "The download was damaged (its checksum doesn't match). Try again.";
            else if (!untar(zip, stage)) err = "It couldn't be unpacked. Is the disk full?";
            if (got || pk_cancel) DeleteFileW(zip.c_str());
            if (!err.empty()) break;
            if (FILE *mk = _wfopen(done.c_str(), L"w")) fclose(mk);
            pk_before += pt.size;
        }
        std::wstring target = mods + L"\\" + wide(m.folder), from = stage + L"\\" + wide(m.folder), aside = stage + L"\\.old-copy";
        bool had = GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (err.empty() && GetFileAttributesW(from.c_str()) == INVALID_FILE_ATTRIBUTES) err = "The download didn't hold that mod.";
        if (err.empty() && had && !MoveFileExW(target.c_str(), aside.c_str(), 0)) err = "The old copy couldn't be moved. Close the game and try again.";
        if (err.empty() && !MoveFileExW(from.c_str(), target.c_str(), 0)) {
            err = "Couldn't move it into the mods folder.";
            if (had) MoveFileExW(aside.c_str(), target.c_str(), 0);
        }
        if (err.empty() && had) sh_delete(aside, RECYCLE);
        if (had && GetFileAttributesW(aside.c_str()) != INVALID_FILE_ATTRIBUTES) {
            std::wstring keep = target + L" (old)";
            for (int k = 2; GetFileAttributesW(keep.c_str()) != INVALID_FILE_ATTRIBUTES; k++) keep = target + L" (old " + std::to_wstring(k) + L")";
            if (MoveFileExW(aside.c_str(), keep.c_str(), 0)) toast("The earlier copy of " + m.folder + " is kept as " + utf8(keep.substr(mods.size() + 1)) + ": delete it when you like.");
            else { toast("The earlier copy of " + m.folder + " couldn't be moved; it's left in " + px(stage) + "."); stage.clear(); }
        }
        if (!stage.empty() && !stopped) sh_delete(stage);
        if (err == "cancelled") { toast("Download cancelled."); continue; }
        if (!err.empty()) {
            toast((m.name.empty() ? m.folder : m.name) + ": " + err);
            std::lock_guard<std::mutex> g(pk_lock);
            pk_text += (pk_text.empty() ? "" : " ") + (m.name.empty() ? m.folder : m.name) + ": " + err;
            continue;
        }
        stamp_version(target, version);
        toast("Installed " + (m.name.empty() ? m.folder : m.name) + ".");
    }
    on_gui(send_state);
}
static void pack_get(const std::string &folder)
{
    std::lock_guard<std::mutex> g(pk_lock);
    bool idle = pk_state != "busy";
    for (auto &m : pk_mods) {
        if (folder != "all" ? m.folder != folder : mod_version(m.folder) == pk_version) continue;
        if (std::find(pk_queue.begin(), pk_queue.end(), m.folder) == pk_queue.end() && (m.folder != pk_job || pk_cancel)) pk_queue.push_back(m.folder);
    }
    if (idle && !pk_queue.empty()) { pk_state = "busy"; pk_text.clear(); std::thread(pack_worker).detach(); }
}

// ---- controllers: every gamepad under /dev/input (evdev), the Steam Deck's own and Steam Input's virtual one too ---
// The page can be driven with one: new presses go to it as {type: button, b: up|down|left|right|a|b|lb|rb|start|x|y}.
enum { P_UP = 1, P_DOWN = 2, P_LEFT = 4, P_RIGHT = 8, P_A = 16, P_B = 32, P_X = 64, P_Y = 128, P_LB = 256, P_RB = 512, P_START = 1024 };
struct pad { int fd; std::string path; unsigned keys = 0; int x = 0, y = 0, hx = 0, hy = 0, xmin = 0, xmax = 0, ymin = 0, ymax = 0; };
static std::vector<pad> pads;
static bool test_bit(const unsigned long *bits, int n) { return bits[n / (8 * sizeof(long))] >> (n % (8 * sizeof(long))) & 1; }
static void pads_scan()   // every 2 s: new devices opened, gone ones dropped
{
    pads.erase(std::remove_if(pads.begin(), pads.end(), [](pad &p) { struct stat st; if (!stat(p.path.c_str(), &st)) return false; close(p.fd); return true; }), pads.end());
    DIR *d = opendir("/dev/input");
    if (!d) return;
    for (struct dirent *e; (e = readdir(d));) {
        if (strncmp(e->d_name, "event", 5)) continue;
        std::string p = std::string("/dev/input/") + e->d_name;
        if (std::any_of(pads.begin(), pads.end(), [&](const pad &x) { return x.path == p; })) continue;
        static std::set<std::string> not_pads;   // keyboards, mice, unreadable ones: looked at once
        if (not_pads.count(p)) continue;
        int fd = open(p.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        unsigned long keys[(KEY_MAX + 1) / (8 * sizeof(long)) + 1] = {};
        if (fd < 0 || ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) < 0 || !test_bit(keys, BTN_GAMEPAD)) { if (fd >= 0) close(fd); not_pads.insert(p); continue; }
        pad np{fd, p};
        struct input_absinfo ax;
        if (!ioctl(fd, EVIOCGABS(ABS_X), &ax)) { np.xmin = ax.minimum; np.xmax = ax.maximum; np.x = (ax.minimum + ax.maximum) / 2; }
        if (!ioctl(fd, EVIOCGABS(ABS_Y), &ax)) { np.ymin = ax.minimum; np.ymax = ax.maximum; np.y = (ax.minimum + ax.maximum) / 2; }
        pads.push_back(np);
    }
    closedir(d);
}
static unsigned pad_read(pad &p)   // the device's events so far, as P_ bits
{
    struct input_event ev[64];
    for (ssize_t n; (n = read(p.fd, ev, sizeof ev)) > 0;)
        for (ssize_t i = 0; i < n / (ssize_t)sizeof *ev; i++) {
            auto &e = ev[i];
            if (e.type == EV_KEY) {
                static const struct { int code; unsigned bit; } km[] = {{BTN_SOUTH, P_A}, {BTN_EAST, P_B}, {BTN_X, P_X}, {BTN_Y, P_Y}, {BTN_TL, P_LB}, {BTN_TR, P_RB},
                    {BTN_START, P_START}, {BTN_DPAD_UP, P_UP}, {BTN_DPAD_DOWN, P_DOWN}, {BTN_DPAD_LEFT, P_LEFT}, {BTN_DPAD_RIGHT, P_RIGHT}};
                for (auto &k : km) if (e.code == k.code) p.keys = e.value ? p.keys | k.bit : p.keys & ~k.bit;
            } else if (e.type == EV_ABS) {
                if (e.code == ABS_X) p.x = e.value;
                else if (e.code == ABS_Y) p.y = e.value;
                else if (e.code == ABS_HAT0X) p.hx = e.value;
                else if (e.code == ABS_HAT0Y) p.hy = e.value;
            }
        }
    unsigned now = p.keys;
    auto past = [](int v, int lo, int hi) { int mid = (lo + hi) / 2, half = (hi - lo) / 2; return half > 0 ? (v - mid) * 2 / half : 0; };   // -2..2 at a full push, past half: +-1
    int sx = past(p.x, p.xmin, p.xmax), sy = past(p.y, p.ymin, p.ymax);
    if (p.hy < 0 || sy < 0) now |= P_UP;
    if (p.hy > 0 || sy > 0) now |= P_DOWN;
    if (p.hx < 0 || sx < 0) now |= P_LEFT;
    if (p.hx > 0 || sx > 0) now |= P_RIGHT;
    return now;
}
static void poll_buttons()
{
    static unsigned held;
    unsigned now = 0;
    for (auto &p : pads) now |= pad_read(p);   // read always, so a press made elsewhere isn't seen as new here
    if (!win->isActiveWindow()) now = 0;
    static const struct { unsigned bit; const char *name; } map[] = {{P_UP, "up"}, {P_DOWN, "down"}, {P_LEFT, "left"}, {P_RIGHT, "right"}, {P_A, "a"},
        {P_B, "b"}, {P_LB, "lb"}, {P_RB, "rb"}, {P_START, "start"}, {P_X, "x"}, {P_Y, "y"}};
    for (auto &m : map) if ((now & m.bit) && !(held & m.bit)) post(std::string("{\"type\":\"button\",\"b\":\"") + m.name + "\"}");
    held = now;
}

// ---- actions -----------------------------------------------------------------------------------------
// The game is running: one the launcher started (running), or one started some other way (its lock, layout.h).
static bool game_running() { return running || layout_game_running(exe_dir.c_str()); }
static void game_exited();
static void play()
{
    if (running || !game_ready()) return;
    if (game_running()) return toast("The game is already running from this folder.");
    { std::lock_guard<std::mutex> g(pk_lock); if (pk_state == "busy") return toast("Wait for the HD pack download to finish, or cancel it on the Mods page."); }
    std::string exe = px(exe_dir + L"\\bin\\onimusha"), root = px(exe_dir);
    if (access(exe.c_str(), X_OK)) return toast("Couldn't start the game: bin/onimusha is missing.");
    make_dirs(exe_dir + L"\\saves");   // the game opens saves/ (its E: drive) as it starts, and quits when a fresh install has none
    char *argv[] = {(char *)exe.c_str(), nullptr};
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addchdir_np(&fa, root.c_str());
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    sigset_t none;
    sigemptyset(&none);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK);
    pid_t pid;
    int e = posix_spawn(&pid, exe.c_str(), &fa, &at, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    if (e) { toast(std::string("Couldn't start the game: ") + strerror(e)); return; }
    running = true;
    post("{\"type\":\"running\",\"running\":true}");
    if (ini_get(ini_load(), "hide_while_playing", "1") != "0") win->hide();
    std::thread([pid] { int st; while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {} on_gui(game_exited); }).detach();
}
static void game_exited()
{
    running = false;
    win->show();
    win->raise();
    win->activateWindow();
    post("{\"type\":\"running\",\"running\":false}");
    send_state();   // achievements unlocked while playing
}

static void start_install(const std::wstring &iso)
{
    if (game_running()) return toast("Close the game first: it's using the game files.");
    if (installing) return;
    installing = true;
    send_state();
    std::thread(install, iso).detach();
}
static void setup_done(const std::string &err)
{
    installing = false;
    send_state();
    if (err.empty()) toast("Installed and ready. Press Play to start.");
    else post("{\"type\":\"setupError\",\"text\":" + jstr(err) + "}");
}

// An Open dialog; the chosen file (Windows form), or "" when cancelled.
static std::wstring pick_file(const char *filter, const char *title)
{
    QString f = QFileDialog::getOpenFileName(win, title, QDir::homePath(), filter);
    return f.isEmpty() ? std::wstring() : winpath(f.toStdString());
}
static void pick_disc()
{
    if (installing) return;
    std::wstring iso = pick_file("Xbox disc image (*.iso *.ISO);;All files (*)", "Choose your Genma Onimusha disc image");
    if (!iso.empty()) start_install(iso);
}
// Settings > Import save: a save.dat from a real Xbox, put in as the port's save (files.h save_import).
static void import_save()
{
    if (game_running()) { toast("Close the game first: it keeps the save open."); return; }
    std::wstring src = pick_file("Xbox save (save.dat);;All files (*)", "Choose a Genma Onimusha save.dat from an Xbox");
    if (src.empty()) return;
    switch (save_import(exe_dir, src)) {
    case SAVE_NOT_ONE: toast("That isn't a Genma Onimusha save: the game's save.dat is 17,920 bytes."); break;
    case SAVE_NO_BACKUP: toast("Couldn't back up your current save, so nothing was changed."); break;
    case SAVE_NOT_WRITTEN: toast("Couldn't write the save next to the launcher."); break;
    case SAVE_SIGNED_ALREADY: toast("Save imported (it was already signed for the port)."); break;
    default: toast("Save imported: load it from the game's title screen. Your previous save was backed up beside it.");
    }
}

static std::string m_last;   // the message being handled, for helpers that need more of it
static void open_path(const std::wstring &p) { QDesktopServices::openUrl(QUrl::fromLocalFile(qs(px(p)))); }
static void open_place(const std::string &what)
{
    std::wstring p;
    if (what == "saves") { p = exe_dir + L"\\saves\\UDATA"; make_dirs(p); }
    else if (what == "shots") { p = exe_dir + L"\\captures\\screenshots"; make_dirs(p); }
    else if (what == "game") p = game_dir();
    else if (what == "mods") { p = exe_dir + L"\\mods"; make_dirs(p); }
    else if (what == "mod") {
        std::wstring n = wide(field(m_last, "name"));
        if (!plain_name(n)) return;
        p = exe_dir + L"\\mods\\" + n;
    }
    else if (what == "run" || what == "spoiler") {
        std::string n = field(m_last, "run");
        if (!rando_name_ok(n)) return;
        p = rando_dir() + L"\\" + wide(n);
        if (what == "spoiler") p += L"\\spoiler.txt";
    }
    else if (what == "log") {
        p = exe_dir + L"\\logs\\onimusha.log";
        if (!file_exists(p)) { toast("No log yet: it's written when the game runs."); return; }
    }
    else return;
    open_path(p);
}

// Desktop shortcut: a .desktop file on the desktop and in the applications menu (where Steam's "Add a Non-Steam Game"
// lists it too).
static void make_shortcut()
{
    std::string self = px(exe_dir) + "/GenmaOnimusha", icon = px(data_dir) + "/icon.png";
    char real[MAX_PATH];
    ssize_t l = readlink("/proc/self/exe", real, sizeof real - 1);
    if (l > 0) { real[l] = 0; self = real; }
    QIcon(":/app.ico").pixmap(256, 256).save(qs(icon), "PNG");
    auto quote = [](const std::string &s) { std::string o = "\""; for (char c : s) { if (strchr("\"`$\\", c)) o += '\\'; o += c; } return o + "\""; };
    std::string text = "[Desktop Entry]\nType=Application\nName=Genma Onimusha\nComment=Genma Onimusha (native port)\nExec=" + quote(self)
                     + "\nPath=" + px(exe_dir) + "\nIcon=" + icon + "\nTerminal=false\nCategories=Game;\n";
    const char *home = getenv("HOME");
    const char *xdh = getenv("XDG_DATA_HOME");
    std::string apps = (xdh && *xdh ? std::string(xdh) : std::string(home ? home : "") + "/.local/share") + "/applications";
    QString desk = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    bool ok = false;
    for (std::string dir : {apps, desk.toStdString()}) {
        if (dir.empty() || !QDir().mkpath(qs(dir))) continue;
        std::wstring f = winpath(dir + "/genma-onimusha.desktop");
        if (write_file_atomic(f.c_str(), text.data(), text.size())) { chmod(px(f).c_str(), 0755); ok = true; }   // a desktop file there runs only when executable
    }
    toast(ok ? "Shortcut created on your desktop and in the applications menu" : "Couldn't create the shortcut");
}

// ---- Randomizer: runs in randomizer/<name>/ (randomizer.h makes them; the game plays the one named by onimusha.ini
// randomizer_run, with its own saves) ---------------------------------------------------------------------------
static std::atomic<bool> rando_busy{false};
static std::mutex rando_lock;
static std::string rando_making;
static std::wstring rando_dir() { return exe_dir + L"\\" LAYOUT_RUNS; }
static std::string rando_dat()   // randomizer.dat, built into the launcher
{
    QFile f(":/randomizer.dat");
    return f.open(QIODevice::ReadOnly) ? f.readAll().toStdString() : std::string();
}
// How far a run has got: of its item locations, how many the furthest save slot has picked up (main.cpp).
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
            if (!save_slot_used((const uint32_t *)&d[at])) continue;
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
        std::string seed, label, fs = "0", lc = "0", dw = "0", vs = "0", kc = "0";
        std::wstring ini = rando_dir() + L"\\" + n + L"\\run.ini";
        FILE *f = _wfopen(ini.c_str(), L"r");
        char line[256];
        while (f && fgets(line, sizeof line, f)) {
            std::string k, v;
            if (!line_key(line, &k, &v)) continue;
            if (k == "seed") seed = v.substr(0, 95);
            else if (k == "name") label = v.substr(0, 95);
            else if (k == "fluorite_sanity") fs = v;
            else if (k == "long_challenges") lc = v;
            else if (k == "door_warp") dw = v;
            else if (k == "vision_staff") vs = v;
            else if (k == "keep_consumables") kc = v;
        }
        if (f) fclose(f);
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
        auto one = [](const std::string &v) { return v[0] == '1' ? "1" : "0"; };
        out += (out.size() > 1 ? "," : "") + std::string("{\"name\":") + jstr(utf8(n)) + ",\"label\":" + jstr(label.empty() ? seed : label) + ",\"seed\":" + jstr(seed)
             + ",\"found\":" + std::to_string(rando_found(saves)) + ",\"total\":" + std::to_string(rando_total())
             + ",\"fs\":" + one(fs) + ",\"lc\":" + one(lc) + ",\"dw\":" + one(dw) + ",\"vs\":" + one(vs) + ",\"kc\":" + one(kc)
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
static void rando_made(const std::string &name)   // on the window's thread: the run just made becomes the one Play starts
{
    ini_set("randomizer_on", "1"); ini_set("randomizer_run", name); ini_set("randomizer_pick", name);
    post("{\"type\":\"randoMade\"}");
    send_state();
}
static const int RANDO_SLOTS = 3;
static void rando_create(std::string label, std::string seed, rando::Options o)
{
    int runs = 0;
    for (size_t p = 0; (p = rando_runs_json().find("\"name\":", p)) != std::string::npos; p++) runs++;
    if (runs >= RANDO_SLOTS) { toast("All three run slots are full: delete a run to make a new one."); return; }
    if (rando_busy.exchange(true)) return;
    bool random_seed = seed.empty(), named = !label.empty();
    auto roll = [] { char b[16]; snprintf(b, sizeof b, "%06u", (unsigned)((GetTickCount64() * 2654435761u + GetCurrentThreadId()) % 1000000)); return std::string(b); };
    if (random_seed) seed = roll();
    for (auto *s : {&label, &seed}) {
        s->erase(std::remove_if(s->begin(), s->end(), [](char c) { return c == '\r' || c == '\n'; }), s->end());
        size_t k = s->size() > 40 ? 40 : s->size();
        while (k < s->size() && k && ((unsigned char)(*s)[k] & 0xC0) == 0x80) k--;
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
            seed = roll() + std::to_string(tries);
            seed = seed.substr(seed.size() - 6);
            if (!named) { label = seed; std::lock_guard<std::mutex> l(rando_lock); rando_making = label; }
        }
        if (err.empty()) toast("Run \"" + label + "\" is ready");
        else { sh_delete(rando_dir() + L"\\" + wide(name)); toast(err); }
        rando_busy = false;
        if (err.empty()) on_gui([name] { rando_made(name); });
        else on_gui(send_state);
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
        rando::Options o;
        o.fluorite_sanity = field(m, "fs") == "1"; o.long_challenges = field(m, "lc") == "1"; o.doors = field(m, "dw") == "1";
        o.keep_consumables = field(m, "kc") == "1";
        rando_create(field(m, "name"), field(m, "seed"), o);
        send_state();
    }
    else if (type == "randoDelete") {   // {run}: to the trash; no longer the one played
        std::string n = field(m, "run");
        if (game_running()) toast("Close the game first: it may be playing that run.");
        else if (rando_name_ok(n)) {
            if (sh_delete(rando_dir() + L"\\" + wide(n), RECYCLE | ASK)) {
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
    else if (type == "update") {   // no self-update on Linux: the release page, in the browser
        if (*ONI_UPDATE_REPO) QDesktopServices::openUrl(QUrl(QString("https://github.com/") + ONI_UPDATE_REPO + "/releases/latest"));
        toast("The release page is open in your browser: unpack the new archive over this folder. Your game files, saves and settings stay.");
    }
    else if (type == "reset") {   // everything but [launcher], which would otherwise bring back first-run setup
        std::vector<std::pair<std::string, std::string>> keep;
        std::string k, v;
        for (auto &l : ini_load()) if (line_key(l, &k, &v) && !strcmp(ini_section_of(k), "launcher")) keep.push_back({k, v});
        DeleteFileW(ini_path().c_str());
        ini_load();   // writes the defaults
        for (auto &kv : keep) ini_set(kv.first, kv.second);
        send_state();
        toast("All settings are back to their defaults");
    } else if (type == "window") {
        std::string a = field(m, "action");
        if (a == "min") win->showMinimized();
        else if (a == "close") win->close();
    } else if (type == "mod" || type == "modMove") {   // {name, on: 0|1} / {name, d: -1|1}
        auto v = mods_get();
        std::wstring name = wide(field(m, "name"));
        size_t i = 0;
        while (i < v.size() && _wcsicmp(v[i].name, name.c_str())) i++;
        if (i == v.size()) { send_state(); return; }
        if (type == "mod") v[i].on = field(m, "on") == "1";
        else {
            size_t j = i + atoi(field(m, "d").c_str());
            if (j < v.size()) std::swap(v[i], v[j]);
        }
        mods_save(v);
        send_state();
    } else if (type == "packCancel") {
        std::lock_guard<std::mutex> g(pk_lock);
        if (pk_state == "busy") { pk_cancel = true; pk_queue.clear(); }
    } else if (type == "packGet") {
        if (game_running()) toast("Close the game first: it's using the mods.");
        else pack_get(field(m, "folder"));
        send_state();
    } else if (type == "modInstall") {
        std::wstring zip = pick_file("Mod (*.zip *.ZIP)", "Choose a mod to install");
        if (zip.empty()) return;
        toast("Installing...");
        std::thread(mod_install, zip).detach();
    } else if (type == "modRemove") {   // {name}: the mod's folder goes to the trash (asked first when it can't)
        std::wstring n = wide(field(m, "name"));
        if (!plain_name(n)) return;
        toast("Removing " + utf8(n) + "...");
        std::thread([n] {
            toast(sh_delete(exe_dir + L"\\mods\\" + n, RECYCLE | ASK) ? "Removed " + utf8(n) : utf8(n) + " wasn't removed (cancelled, or the game is using it)");
            on_gui(send_state);
        }).detach();
    } else if (type == "drag") {   // the page's title bar: the window manager moves the window
        if (win->windowHandle()) win->windowHandle()->startSystemMove();
    }
}

// ---- the window ----------------------------------------------------------------------------------------
// The page's messages arrive here (QWebChannel's "host" object, behind window.chrome.webview).
class Bridge : public QObject {
    Q_OBJECT
public:
    Q_INVOKABLE void send(const QString &json) { on_message(json.toStdString()); }
};

// oni://app/index.html (the page, built in), oni://art/<file> (the disc's art), oni://mods/<folder>/<file> (mods'
// preview pictures). Only plain names: nothing outside those folders.
class Files : public QWebEngineUrlSchemeHandler {
public:
    void requestStarted(QWebEngineUrlRequestJob *job) override
    {
        QUrl u = job->requestUrl();
        QString host = u.host(), path = u.path(QUrl::FullyDecoded);
        QStringList parts = path.split('/', Qt::SkipEmptyParts);
        for (auto &p : parts) if (!plain_name(wide(p.toStdString())) || p == "..") return job->fail(QWebEngineUrlRequestJob::UrlInvalid);
        QString file;
        if (host == "app" && parts == QStringList{"index.html"}) {
            file = ":/ui/index.html";
#ifdef ONI_DEBUG   // development builds only: ONI_LAUNCHER_UI=<folder> serves the page from a source folder
            if (const char *dev = getenv("ONI_LAUNCHER_UI")) file = QString(dev) + "/index.html";
#endif
        }
        else if (host == "art" && parts.size() == 1) file = qs(px(art_dir() + L"\\" + wide(parts[0].toStdString())));
        else if (host == "mods" && parts.size() == 2) file = qs(px(exe_dir + L"\\mods\\" + wide(parts[0].toStdString()) + L"\\" + wide(parts[1].toStdString())));
        auto *f = new QFile(file, job);
        if (file.isEmpty() || !f->open(QIODevice::ReadOnly)) return job->fail(QWebEngineUrlRequestJob::UrlNotFound);
        QString ext = file.section('.', -1).toLower();
        QByteArray type = ext == "html" ? "text/html" : ext == "png" ? "image/png" : ext == "jpg" || ext == "jpeg" ? "image/jpeg" : ext == "bmp" ? "image/bmp" : "application/octet-stream";
        job->reply(type, f);
    }
};
// Nothing leaves the launcher: the page's https://mods.example/ (WebView2's folder host) goes to oni://mods/, and any
// other request that isn't the launcher's own is refused.
class Requests : public QWebEngineUrlRequestInterceptor {
public:
    void interceptRequest(QWebEngineUrlRequestInfo &info) override
    {
        QUrl u = info.requestUrl();
        if (u.scheme() == "https" && u.host() == "mods.example") { u.setScheme("oni"); u.setHost("mods"); info.redirect(u); }
        else if (u.scheme() != "oni" && u.scheme() != "data" && u.scheme() != "blob") info.block(true);
    }
};
// The view never leaves the launcher's page; web links open in the browser.
class Page : public QWebEnginePage {
public:
    using QWebEnginePage::QWebEnginePage;
protected:
    bool acceptNavigationRequest(const QUrl &url, NavigationType, bool main) override
    {
        if (url.scheme() == "oni" && url.host() == "app") return true;
        if (main && url.scheme() == "https") QDesktopServices::openUrl(url);
        return false;
    }
    QWebEnginePage *createWindow(WebWindowType) override   // target=_blank, window.open: caught on its first navigation
    {
        auto *p = new Page(profile(), this);
        connect(p, &QWebEnginePage::urlChanged, p, [p](const QUrl &url) { if (url.scheme() == "https") QDesktopServices::openUrl(url); p->deleteLater(); });
        return p;
    }
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel level, const QString &msg, int line, const QString &) override
    {
        if (level == ErrorMessageLevel) mark(("page: line " + std::to_string(line) + ": " + msg.toStdString()).substr(0, 300).c_str());
    }
};
class Window : public QWebEngineView {
public:
    using QWebEngineView::QWebEngineView;
protected:
    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::WindowActivate) send_state();   // coming back (from the file manager, say): mods/ may have changed
        return QWebEngineView::event(e);
    }
    void closeEvent(QCloseEvent *e) override
    {
        bool pack_busy;
        { std::lock_guard<std::mutex> g(pk_lock); pack_busy = pk_state == "busy"; }
        if ((installing && !ask("The game is still being copied from your disc. Stop and close?"))
            || (pack_busy && !ask("The HD pack is still downloading. Closing stops it; next time it carries on where it stopped. Close anyway?"))
            || (rando_busy && !ask("A randomizer run is still being made. Close anyway? It won't be kept."))) { e->ignore(); return; }
        e->accept();
        qApp->quit();
    }
};

// The window is 1120x720 (Qt scales it for the screen); where that doesn't fit the screen (a Steam Deck's 1280x800
// does), the window and the page shrink together, so the title bar and Play stay on screen.
static void fit_window()
{
    QScreen *sc = QGuiApplication::primaryScreen();
    QRect wa = sc ? sc->availableGeometry() : QRect(0, 0, 1120, 720);
    double f = std::min({1.0, (double)wa.width() / 1120, (double)wa.height() / 720});
    win->setZoomFactor(f);
    int W = (int)(1120 * f), H = (int)(720 * f);
    win->setGeometry(wa.x() + (wa.width() - W) / 2, wa.y() + (wa.height() - H) / 2, W, H);
}

// window.chrome.webview, as WebView2 gives it to the page: postMessage(object) and "message" events, here over
// QWebChannel (its qwebchannel.js, from Qt, inlined).
static QString bridge_script()
{
    QFile f(":/qtwebchannel/qwebchannel.js");
    QString channel = f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
    return "(function () {\n"
           "const listeners = [], queue = [];\n"
           "let host = null;\n"
           "window.__oniRecv = d => { for (const f of listeners) f({ data: d }); };\n"
           "window.chrome = window.chrome || {};\n"
           "window.chrome.webview = {\n"
           "  postMessage: m => { const j = JSON.stringify(m); if (host) host.send(j); else queue.push(j); },\n"
           "  addEventListener: (t, f) => { if (t === 'message') listeners.push(f); },\n"
           "};\n" + channel + "\n"
           "const connect = () => {\n"
           "  if (!(window.qt && qt.webChannelTransport)) return setTimeout(connect, 5);\n"
           "  new QWebChannel(qt.webChannelTransport, ch => { host = ch.objects.host; for (const j of queue.splice(0)) host.send(j); });\n"
           "};\n"
           "connect();\n"
           "})();\n";
}

int main(int argc, char **argv)
{
    QWebEngineUrlScheme scheme("oni");   // before the application: the page's own scheme
    scheme.setSyntax(QWebEngineUrlScheme::Syntax::Host);
    scheme.setFlags(QWebEngineUrlScheme::SecureScheme | QWebEngineUrlScheme::LocalAccessAllowed | QWebEngineUrlScheme::CorsEnabled);
    QWebEngineUrlScheme::registerScheme(scheme);
    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv);
    app.setApplicationName("Genma Onimusha");
    app.setDesktopFileName("genma-onimusha");
    app.setQuitOnLastWindowClosed(false);   // the window hides while the game runs; closeEvent quits

    char self[MAX_PATH];
    ssize_t l = readlink("/proc/self/exe", self, sizeof self - 1);
    if (l <= 0) return 1;
    self[l] = 0;
    exe_dir = winpath(std::string(self).substr(0, std::string(self).find_last_of('/')));
    const char *home = getenv("HOME"), *xdh = getenv("XDG_DATA_HOME");
    data_dir = winpath((xdh && *xdh ? std::string(xdh) : std::string(home ? home : "/tmp") + "/.local/share") + "/genma-onimusha/launcher");
    make_dirs(data_dir);
    layout_migrate(exe_dir.c_str());   // an install from before the folders (layout.h); the game does it too
    // One launcher per install: a second would delete the first one's downloads in progress and write the same files.
    {
        uint32_t hash = 2166136261u;
        for (wchar_t c : exe_dir) hash = (hash ^ (uint32_t)towlower(c)) * 16777619u;
        char name[64];
        snprintf(name, sizeof name, "/one-%08x.lock", hash);
        int one = open((px(data_dir) + name).c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);   // held until the process ends
        if (one >= 0 && flock(one, LOCK_EX | LOCK_NB)) { QMessageBox::information(0, "Genma Onimusha", "The launcher is already open."); return 0; }
    }
    mark("start");
    if (game_ready()) art_refresh();
    mark("art checked");
    pads_scan();
    pad_connected = !pads.empty();
    std::thread([] {
        // What a launcher closed mid-way left: mods/.installing-<n> (an Install .zip), and pack parts and half-unpacked
        // folders in mods/.download/ untouched for a month. This is the only launcher for this folder (above), and
        // installs wait for leftovers_cleared, so nothing is in progress yet.
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((exe_dir + L"\\mods\\.installing-*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) sh_delete(exe_dir + L"\\mods\\" + fd.cFileName, RECYCLE);
            while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if ((h = FindFirstFileW((exe_dir + L"\\mods\\.download\\*").c_str(), &fd)) != INVALID_HANDLE_VALUE) {
            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            ULONGLONG t = (ULONGLONG)now.dwHighDateTime << 32 | now.dwLowDateTime;
            do {
                if (fd.cFileName[0] == L'.') continue;
                ULONGLONG w = (ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32 | fd.ftLastWriteTime.dwLowDateTime;
                if (t - w <= 30ull * 24 * 3600 * 10000000) continue;
                sh_delete(exe_dir + L"\\mods\\.download\\" + fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? RECYCLE : 0);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        leftovers_cleared = true;
    }).detach();
    QStringList args = app.arguments();
    if (args.size() > 1) pending_iso = winpath(QFileInfo(args[1]).absoluteFilePath().toStdString());

    // The page: in memory only (it keeps nothing of its own), its scheme served by Files, nothing else reachable.
    auto *profile = new QWebEngineProfile(&app);
    profile->installUrlSchemeHandler("oni", new Files);
    profile->setUrlRequestInterceptor(new Requests);
    win = new Window;
    pg = new Page(profile, win);
    pg->setBackgroundColor(QColor(12, 10, 9));   // no white flash
    auto *st = pg->settings();
    st->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
    st->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
    st->setAttribute(QWebEngineSettings::ErrorPageEnabled, false);
    st->setAttribute(QWebEngineSettings::PluginsEnabled, false);
    QWebEngineScript bridge;
    bridge.setName("bridge");
    bridge.setInjectionPoint(QWebEngineScript::DocumentCreation);
    bridge.setWorldId(QWebEngineScript::MainWorld);
    bridge.setRunsOnSubFrames(false);
    bridge.setSourceCode(bridge_script());
    pg->scripts().insert(bridge);
    auto *channel = new QWebChannel(pg);
    channel->registerObject("host", new Bridge);
    pg->setWebChannel(channel);
    win->setPage(pg);
    win->setContextMenuPolicy(Qt::NoContextMenu);
    win->setAcceptDrops(false);   // a file dropped on the window would otherwise replace the page
    win->setWindowFlag(Qt::FramelessWindowHint);
    win->setWindowTitle("Genma Onimusha");
    win->setWindowIcon(QIcon(":/app.ico"));
    fit_window();
    win->show();
    mark("window shown");
    QTimer pads_timer, buttons;
    QObject::connect(&pads_timer, &QTimer::timeout, [] {
        pads_scan();
        bool p = !pads.empty();
        if (p != pad_connected) { pad_connected = p; post(std::string("{\"type\":\"pad\",\"connected\":") + (p ? "true}" : "false}")); }
    });
    QObject::connect(&buttons, &QTimer::timeout, [] { if (!running) poll_buttons(); });
    pads_timer.start(2000);
    buttons.start(33);
    if (game_ready()) std::thread(game_facts).detach();
    // a new query each run, so nothing cached of an older page is shown
    pg->load(QUrl("oni://app/index.html?run=" + QString::number(GetTickCount64())));

    int r = app.exec();
    // Not a return: a worker thread (an HD pack download, a run being made) may still be running, and returning would
    // destroy the globals it uses under it. Every file the launcher keeps is already written (write_file_atomic syncs).
    delete win;
    fflush(0);
    _exit(r);
}

#include "main.moc"
