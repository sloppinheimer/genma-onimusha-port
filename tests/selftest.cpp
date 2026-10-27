// Checks of the pure code the game and the launcher share or keep apart from Windows: ini lines, the default
// settings, mods.txt and mod.ini, whole-file writes, sealed files and moving an old install into the folders, the launcher's JSON reading, version and name checks. Built with
// the rest (CMake target selftest); run it after a change to any of them, and the release workflow runs it before
// packaging. Prints what failed and exits 1, or "selftest ok". The launcher's disc reader (disc.h) is here too;
// tools/xbe_tool.py selftest covers its Python one.
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#include <string>
#include <vector>
#include <cstdio>
#include <cstdint>
#include "../src/host/settings_default.h"
#include "../src/host/mods_list.h"
#include "../src/host/layout.h"
#include "../src/host/overlay.h"
#include "../src/host/game.h"
#include "../src/host/redact.h"
#include <filesystem>
#include <algorithm>
#include <shlobj.h>
#include "../launcher/text.h"
#include <set>
#include "../launcher/disc.h"
#include "../launcher/files.h"
#include "../launcher/randomizer.h"

static std::string slurp(const std::wstring &path)   // a whole file, "" when missing
{
    std::string s;
    if (FILE *f = _wfopen(path.c_str(), L"rb")) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f))) s.append(b, n); fclose(f); }
    return s;
}

// A synthetic XDVDFS image in memory, as tools/xbe_tool.py's selftest builds them: the game partition at part (0, or a
// full image's 0x18300000, kept sparse: nothing before it is stored), sector 40 the root, 41 a folder that lists itself.
struct mem_disc { uint64_t part = 0; std::vector<uint8_t> b; };
static bool mem_read(void *ctx, uint64_t off, void *buf, uint32_t n)
{
    auto *d = (mem_disc *)ctx;
    if (off < d->part || off - d->part + n > d->b.size()) return false;
    memcpy(buf, d->b.data() + (size_t)(off - d->part), n);
    return true;
}
struct dentry { uint32_t start, length; uint8_t attr; std::string name; };
static void disc_dir(mem_disc &d, uint32_t sec, const std::vector<dentry> &entries)
{
    uint8_t *s = &d.b[sec * 2048];
    memset(s, 0xff, 2048);
    size_t i = 0;
    for (auto &e : entries) {
        uint8_t *p = s + i;
        memset(p, 0, 4); memcpy(p + 4, &e.start, 4); memcpy(p + 8, &e.length, 4); p[12] = e.attr; p[13] = (uint8_t)e.name.size();
        memcpy(p + 14, e.name.data(), e.name.size());
        i = (i + 14 + e.name.size() + 3) & ~(size_t)3;
    }
}
static mem_disc disc_image(const std::vector<dentry> &root, uint64_t part = 0, uint32_t root_size = 2048)
{
    mem_disc d;
    d.part = part;
    d.b.assign(60 * 2048, 0);
    uint32_t v[2] = {40, root_size};
    memcpy(&d.b[0x10000], "MICROSOFT*XBOX*MEDIA", 20); memcpy(&d.b[0x10000 + 20], v, 8);
    disc_dir(d, 40, root);
    disc_dir(d, 41, {{50, 4, 0, "a.bin"}, {41, 2048, 0x10, "loop"}});
    return d;
}

static int failed;
#define CHECK(c) do { if (!(c)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); failed++; } } while (0)

static void put(const std::wstring &path, const char *text) { CHECK(write_file_atomic(path.c_str(), text, strlen(text))); }

int main()
{
    // ini_line: onimusha.ini's rules (';' ends a value) and mod.ini's (it doesn't)
    {
        char a[] = "\xEF\xBB\xBF  scale = 3 ; comment\r\n", b[] = "name = a;b", c[] = "[settings]", d[] = "; x = 1", e[] = "k =", f[] = "no equals";
        char *k, *v, sec[16] = "";
        CHECK(ini_line(a, &k, &v, 1, 0, 0) && !strcmp(k, "scale") && !strcmp(v, "3"));
        CHECK(ini_line(b, &k, &v, 0, 0, 0) && !strcmp(k, "name") && !strcmp(v, "a;b"));
        CHECK(!ini_line(c, &k, &v, 0, sec, sizeof sec) && !strcmp(sec, "settings"));
        CHECK(!ini_line(d, &k, &v, 1, 0, 0) && !ini_line(f, &k, &v, 1, 0, 0));
        CHECK(ini_line(e, &k, &v, 1, 0, 0) && !strcmp(k, "k") && !*v);
    }
    // the launcher's view of settings_default.h: every key with its section and default
    {
        auto has = [](const char *k, const char *s, const char *v) {
            for (auto &d : ini_defaults()) if (d.key == k) return d.section == s && d.value == v;
            return false;
        };
        CHECK(has("scale", "display", "2") && has("movie_subtitles", "game", "text") && has("god_mode", "cheats", "0"));
        CHECK(has("up", "keys", "Up") && has("background_denoise", "display", "0.4"));
        CHECK(has("hide_while_playing", "launcher", "1") && has("console", "launcher", "0") && has("setup_style", "launcher", ""));
    }
    // write_file_atomic, mods.txt and mod.ini in a throwaway folder
    {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        std::wstring root = std::wstring(tmp) + L"oni_selftest_" + std::to_wstring(GetCurrentProcessId());
        CreateDirectoryW(root.c_str(), 0);
        CreateDirectoryW((root + L"\\mods").c_str(), 0);
        for (auto m : {L"A", L"B", L"C", L".installing-1"}) CreateDirectoryW((root + L"\\mods\\" + m).c_str(), 0);
        put(root + L"\\mods\\mods.txt", "; order\nmod_off = B\nmod = A\nmod = ..\nmod = Missing\n");
        put(root + L"\\mods\\A\\mod.ini", "name = Test; one\n[settings]\nname = other\n");
        put(root + L"\\mods\\C\\mod.ini", "enabled = 0\n");
        CHECK(GetFileAttributesW((root + L"\\mods\\mods.txt.tmp").c_str()) == INVALID_FILE_ATTRIBUTES);   // the .tmp was renamed
        static mod_item mods[MOD_MAX];
        int n = mod_list(root.c_str(), mods);
        CHECK(n == 3);   // B, A as listed; C new (off: its mod.ini); "..", Missing and the hidden staging folder left out
        CHECK(n == 3 && !wcscmp(mods[0].name, L"B") && !mods[0].on && !wcscmp(mods[1].name, L"A") && mods[1].on && !wcscmp(mods[2].name, L"C") && !mods[2].on);
        char v[64];
        CHECK(mod_ini(root.c_str(), L"A", "", "name", v, sizeof v) && !strcmp(v, "Test; one"));
        CHECK(mod_ini(root.c_str(), L"A", "settings", "name", v, sizeof v) && !strcmp(v, "other"));
        for (auto f : {L"\\mods\\A\\mod.ini", L"\\mods\\C\\mod.ini", L"\\mods\\mods.txt"}) DeleteFileW((root + f).c_str());
        for (auto m : {L"A", L"B", L"C", L".installing-1"}) RemoveDirectoryW((root + L"\\mods\\" + m).c_str());
        RemoveDirectoryW((root + L"\\mods").c_str());
        RemoveDirectoryW(root.c_str());
    }
    // layout.h: sealed files, and an old (flat) install moved into the folders
    {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        std::wstring root = std::wstring(tmp) + L"oni_layout_" + std::to_wstring(GetCurrentProcessId());
        CreateDirectoryW(root.c_str(), 0);
        char text[256];
        std::wstring s = root + L"\\sealed.dat";
        CHECK(sealed_read(s.c_str(), text, sizeof text) == -1);
        CHECK(sealed_write(s.c_str(), "a = 1\n", 6) && sealed_read(s.c_str(), text, sizeof text) == 6 && !strcmp(text, "a = 1\n"));
        { FILE *f = _wfopen(s.c_str(), L"rb"); char raw[32] = {0}; CHECK(f); if (f) { fread(raw, 1, sizeof raw, f); fclose(f); } CHECK(!strstr(raw + 8, "a = 1")); }
        { FILE *f = _wfopen(s.c_str(), L"r+b"); CHECK(f); if (f) { fseek(f, 9, SEEK_SET); fputc(0x55, f); fclose(f); } }   // an edit
        CHECK(sealed_read(s.c_str(), text, sizeof text) == -2);
        put(s, "a = 1\n");   // a plain text file is not a sealed one
        CHECK(sealed_read(s.c_str(), text, sizeof text) == -2);
        // 0.7's flat install
        for (auto d : {L"\\hdd", L"\\hdd\\E", L"\\hdd\\E\\UDATA", L"\\hdd\\Z", L"\\bin"}) CreateDirectoryW((root + d).c_str(), 0);
        put(root + L"\\hdd\\E\\UDATA\\save.dat", "save");
        put(root + L"\\hdd\\partition0.bin", "p0");
        CreateDirectoryW((root + L"\\hdd\\Z\\IMAGE").c_str(), 0);
        put(root + L"\\hdd\\Z\\IMAGE\\ROOM000.BGS", "copy");
        CreateDirectoryW((root + L"\\keep").c_str(), 0);
        put(root + L"\\keep\\precious.txt", "x");
        CreateDirectoryW((root + L"\\hdd\\Z\\IMAGE\\BMP").c_str(), 0);
        _wsystem((L"mklink /J \"" + root + L"\\hdd\\Z\\IMAGE\\link\" \"" + root + L"\\keep\" >nul").c_str());
        CHECK(layout_exists((root + L"\\hdd\\Z\\IMAGE\\link\\precious.txt").c_str()));   // the junction is there to test
        put(root + L"\\achievements.txt", "first_blood = 5\n");
        put(root + L"\\playtime.txt", "seconds = 9\n");
        put(root + L"\\onimusha.log", "log");
        put(root + L"\\onimusha.exe", "old");
        put(root + L"\\SDL3.dll", "old");
        put(root + L"\\bin\\onimusha.exe", "new");
        for (auto d : {L"\\fonts", L"\\licenses"}) CreateDirectoryW((root + d).c_str(), 0);   // 0.8's first layout
        put(root + L"\\fonts\\KabutoSans.ttf", "old");
        // mods.txt on top and patches\*.txt (the first 0.8 builds)
        for (auto d : {L"\\mods", L"\\mods\\Taken", L"\\patches"}) CreateDirectoryW((root + d).c_str(), 0);
        put(root + L"\\mods.txt", "mod = Taken\npatch_off = Off one.txt\n");
        put(root + L"\\patches\\Skip logos.txt", "\n; Skips the logos\n11bf9 = 66 90\n");
        put(root + L"\\patches\\Off one.txt", "11bf9 = 66 90\n");
        put(root + L"\\patches\\Taken.txt", "11bf9 = 66 90\n");
        put(root + L"\\licenses\\SDL3-LICENSE.txt", "old");
        layout_migrate(root.c_str());
        layout_migrate(root.c_str());   // a second start changes nothing
        auto has = [&](const wchar_t *p) { return layout_exists((root + p).c_str()) != 0; };
        CHECK(has(L"\\saves\\UDATA\\save.dat") && !has(L"\\cache") && !has(L"\\hdd"));   // the old cache is gone
        CHECK(sealed_read((root + L"\\saves\\achievements.dat").c_str(), text, sizeof text) > 0 && !strcmp(text, "first_blood = 5\n"));
        CHECK(!has(L"\\achievements.txt") && has(L"\\saves\\playtime.txt") && has(L"\\logs\\onimusha.log") && !has(L"\\onimusha.log"));
        CHECK(!has(L"\\onimusha.exe") && !has(L"\\SDL3.dll") && has(L"\\bin\\onimusha.exe"));
        CHECK(!has(L"\\fonts") && !has(L"\\licenses"));
        CHECK(has(L"\\mods\\mods.txt") && !has(L"\\mods.txt") && !has(L"\\patches"));
        CHECK(has(L"\\mods\\Skip logos\\patch.txt") && has(L"\\mods\\Off one\\patch.txt") && has(L"\\mods\\Taken (patch)\\patch.txt"));
        {
            char v[64];
            CHECK(mod_ini(root.c_str(), L"Skip logos", "", "description", v, sizeof v) && !strcmp(v, "Skips the logos"));
            CHECK(!mod_ini(root.c_str(), L"Skip logos", "", "enabled", v, sizeof v));
            CHECK(mod_ini(root.c_str(), L"Off one", "", "enabled", v, sizeof v) && !strcmp(v, "0"));
            static mod_item m[MOD_MAX];
            int n = mod_list(root.c_str(), m);   // Taken as listed, then the new ones alphabetically
            CHECK(n == 4 && !wcscmp(m[0].name, L"Taken") && !wcscmp(m[1].name, L"Off one") && !m[1].on && !wcscmp(m[2].name, L"Skip logos") && m[2].on);
        }
        CHECK(has(L"\\keep\\precious.txt"));   // a junction inside the old cache was removed, not followed
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    // JSON: the release JSON and the page's messages
    {
        std::string j = R"({"type":"set","nested":{"key":"scale","value":3},"text":"a\"b\\cé😀\n"})";
        CHECK(field(j, "type") == "set" && field(j, "key") == "scale" && field(j, "value") == "3");
        CHECK(field(j, "text") == "a\"b\\c\xC3\xA9\xF0\x9F\x98\x80\n");
        CHECK(field(j, "missing").empty());
        CHECK(jstr("a\"b\\\x01") == "\"a\\\"b\\\\\\u0001\"");
        CHECK(field(R"({"t":"é!"})", "t") == "\xC3\xA9!" && field(R"({"t":"😀"})", "t") == "\xF0\x9F\x98\x80");   // escapes, a pair joined
        CHECK(field(R"({"t":"a\u12","u":"x"})", "t") == "au12" && field(R"({"t":"a\u12","u":"x"})", "u") == "x");   // cut off: stays in its string
        // ini numbers: whatever a hand edit holds, the state JSON gets a valid number or the default
        CHECK(json_number("0.4", "1") == "0.4" && json_number(".4", "1") == "0.4" && json_number("4.", "1") == "4" && json_number("02", "1") == "2");
        CHECK(json_number("-1", "1") == "-1" && json_number("", "1") == "1" && json_number("-", "1") == "1" && json_number("1-2", "1") == "1");
        CHECK(json_number("abc", "1") == "1" && json_number("inf", "1") == "1" && json_number("nan", "1") == "1" && json_number("1e999", "1") == "1");
    }
    // versions and names
    {
        CHECK(newer_than("v0.10", "0.9") && newer_than("0.8.1", "v0.8") && !newer_than("0.8", "0.8") && !newer_than("0.7", "0.8"));
        CHECK(newer_than("v0.9", "0.9-beta") && !newer_than("0.9-beta", "0.9") && newer_than("0.9-rc1", "0.8") && !newer_than("0.9-b", "0.9-a"));
        CHECK(!newer_than("", "0.1") && newer_than("0.1", ""));
        CHECK(plain_name(L"HD Textures") && plain_name(L"a.b"));
        CHECK(!plain_name(L"") && !plain_name(L"...") && !plain_name(L"a.") && !plain_name(L"a ") && !plain_name(L"a/b") && !plain_name(L"a\\b"));
        CHECK(!plain_name(L"CON") && !plain_name(L"con.txt") && !plain_name(L"COM1") && plain_name(L"CONSOLE"));
        CHECK(disc_name_ok("default.xbe") && !disc_name_ok("..") && !disc_name_ok(".") && !disc_name_ok("a\\b") && !disc_name_ok("C:x") && !disc_name_ok(""));
        CHECK(!disc_name_ok("COM1") && !disc_name_ok("nul.txt") && !disc_name_ok("a.") && !disc_name_ok("a ") && disc_name_ok("CONSOLE.BIN"));
        CHECK(utf8(wide("\xC3\xA9t\xC3\xA9")) == "\xC3\xA9t\xC3\xA9");
    }
    // onimusha.ini editing (ini_set's rules and where a line goes), the HD pack's pack.ini, a mod.ini's version stamp
    {
        CHECK(ini_set_ok("scale", "3") && ini_set_ok("up", "Up") && ini_set_ok("setup_style", "modern") && ini_set_ok("randomizer_run", ""));
        CHECK(!ini_set_ok("nonsense", "1") && !ini_set_ok("up\ngod_mode", "1") && !ini_set_ok("scale", "3\nscale = 8") && !ini_set_ok("up", "a;b")
              && !ini_set_ok("scale", std::string(201, '1')) && !ini_set_ok("scale", std::string("1\0", 2)));
        std::vector<std::string> l = {"[display]", "scale = 2", "", "[game]", "skip_intro = 0"};
        ini_set_lines(l, "SCALE", "4");
        CHECK(l.size() == 5 && l[1] == "SCALE = 4");   // keys match whatever their case
        ini_set_lines(l, "msaa", "8");
        CHECK(l.size() == 6 && l[2] == "msaa = 8" && l[3].empty());   // after the section's last setting, not after the blank
        ini_set_lines(l, "setup_style", "classic");
        CHECK(l.size() == 9 && l[7] == "[launcher]" && l[8] == "setup_style = classic");   // a section the file lacks: at the end
        std::string version;
        std::vector<pack_mod> pm;
        CHECK(pack_ini_parse("; pack\nversion = 1.1\n[HD Textures]\nname = HD Textures\nsize = 10\npart = t1.zip 5 abc\r\npart = t2.zip 5 def\n"
                             "[..]\npart = x.zip 1 a\n[Empty]\nname = none\n[Bad part]\npart = a/b.zip 1 a\n[COM1]\npart = c.zip 1 a\n", version, pm)
              && version == "1.1" && pm.size() == 1 && pm[0].folder == "HD Textures" && pm[0].name == "HD Textures" && pm[0].size == 10
              && pm[0].parts.size() == 2 && pm[0].parts[0].sha == "abc" && pm[0].parts[1].name == "t2.zip" && pm[0].parts[1].size == 5);
        CHECK(!pack_ini_parse("[HD Textures]\npart = t.zip 1 a\n", version, pm));   // no version
        CHECK(!pack_ini_parse("version = 1\n[A]\npart = t.zip 0 a\n", version, pm));   // a part of no size is no part
        CHECK(stamp_version_text("name = X\r\nversion = 1.0\r\nauthor = Y\r\n", "1.1") == "name = X\r\nversion = 1.1\r\nauthor = Y\r\n");
        CHECK(stamp_version_text("name = X", "1.1") == "name = X\r\nversion = 1.1\r\n");
        CHECK(stamp_version_text("", "1.1") == "version = 1.1\r\n");
    }
    // The randomizer from randomizer.dat alone (rando::place: the placement and its own playthrough check, without the
    // game's files): every seed made with the plain options (rando_gen batch makes 20 of 20), Door-sanity's that it
    // makes are made too (an odd seed can fail; the launcher rolls another), and a seed always gives the same run.
    {
        std::vector<uint8_t> raw;
        CHECK(rando::read_file(wide(ONI_SOURCE_DIR "/launcher/randomizer.dat"), raw) && raw.size() > 1000);
        std::string dat(raw.begin(), raw.end());
        auto run = [&](const std::string &seed, bool fs, bool lc, bool dw, bool kc, std::string *layout) {
            rando::Options o;
            o.seed = seed; o.fluorite_sanity = fs; o.long_challenges = lc; o.doors = dw; o.keep_consumables = kc;
            rando::Data d = rando::parse(dat);
            std::string spheres, err = rando::place(d, o, 0, spheres);
            if (layout) { *layout = spheres; for (auto &l : d.locs) *layout += std::to_string(l.placed) + "x" + std::to_string(l.placed_count) + ","; for (auto &dr : d.doors) *layout += std::to_string(dr.paired) + ";"; }
            return err;
        };
        int plain = 0, doors = 0;
        for (int s = 1; s <= 8; s++) {
            plain += run(std::to_string(s), false, false, false, false, 0).empty();
            plain += run(std::to_string(s), true, true, false, true, 0).empty();
        }
        for (int s = 1; s <= 3; s++) doors += run(std::to_string(s), false, false, true, false, 0).empty();
        CHECK(plain == 16);
        CHECK(doors >= 2);
        std::string a, b, c;
        run("race", true, false, true, false, &a);
        run("race", true, false, true, false, &b);
        run("Race", true, false, true, false, &c);
        CHECK(a == b && a.size() > 100 && a != c);   // the same seed and options: the same run; another seed: another
    }
    // Which copy of a disc file the game reads (overlay.h): the run's, then the first mod with it, else the disc's
    {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        std::wstring root = std::wstring(tmp) + L"oni_overlay_" + std::to_wstring(GetCurrentProcessId());
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        make_dirs(root + L"\\randomizer\\run1\\files\\image"); make_dirs(root + L"\\mods\\M1\\image\\c.bin"); make_dirs(root + L"\\mods\\M2\\image");
        put(root + L"\\randomizer\\run1\\files\\image\\a.bin", "run"); put(root + L"\\mods\\M1\\image\\a.bin", "m1");
        put(root + L"\\mods\\M2\\image\\b.bin", "m2"); put(root + L"\\mods\\M2\\image\\c.bin", "m2");
        static mod_item mods[2] = {{L"M1", 1}, {L"M2", 1}};
        wchar_t out[MAX_PATH * 2];
        int which = -1;
        CHECK(overlay_path(root.c_str(), "run1", mods, 2, L"image\\a.bin", out, MAX_PATH * 2, &which) == 2 && slurp(out) == "run");
        CHECK(overlay_path(root.c_str(), 0, mods, 2, L"image\\a.bin", out, MAX_PATH * 2, &which) == 1 && which == 0 && slurp(out) == "m1");
        CHECK(overlay_path(root.c_str(), "", mods, 2, L"image\\b.bin", out, MAX_PATH * 2, &which) == 1 && which == 1);   // "" = no run
        CHECK(overlay_path(root.c_str(), "run1", mods, 2, L"image\\c.bin", out, MAX_PATH * 2, &which) == 1 && which == 1);   // M1's is a folder
        CHECK(overlay_path(root.c_str(), "run1", mods, 2, L"image\\none.bin", out, MAX_PATH * 2, &which) == 0);
        CHECK(overlay_path(root.c_str(), "run1", mods, 1, L"image\\b.bin", out, MAX_PATH * 2, &which) == 0);   // only mods that are on count
        std::filesystem::remove_all(root, ec);
    }
    // The update's swap into the install and its putting back (files.h), in a throwaway folder
    {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        std::wstring root = std::wstring(tmp) + L"oni_swap_" + std::to_wstring(GetCurrentProcessId());
        std::wstring stage = root + L"\\.update", inst = root + L"\\inst", list = root + L"\\replaced.txt";
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        make_dirs(stage + L"\\bin"); make_dirs(inst);
        put(inst + L"\\a.txt", "old a"); put(stage + L"\\a.txt", "new a"); put(stage + L"\\bin\\b.dll", "new b");
        std::vector<std::wstring> moved;
        CHECK(swap_in(stage, inst, list, moved) && moved.size() == 2);
        CHECK(slurp(inst + L"\\a.txt") == "new a" && slurp(inst + L"\\a.txt.old") == "old a" && slurp(inst + L"\\bin\\b.dll") == "new b");
        CHECK(slurp(list) == "a.txt\r\n" && !file_exists(stage + L"\\a.txt"));   // only replaced files are listed (text mode: CRLF)
        // a file in use (open without delete sharing) can't be moved: the swap stops there, and swap_back takes out
        // what went in before it (c.txt) and leaves the held one as it was
        put(inst + L"\\z.txt", "old z"); put(stage + L"\\c.txt", "new c"); put(stage + L"\\z.txt", "new z");
        HANDLE held = CreateFileW((inst + L"\\z.txt").c_str(), GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
        moved.clear();
        bool ok = swap_in(stage, inst, list, moved);
        CloseHandle(held);
        CHECK(!ok && moved.size() == 1 && moved[0] == inst + L"\\c.txt");
        swap_back(moved);
        CHECK(!file_exists(inst + L"\\c.txt") && slurp(inst + L"\\z.txt") == "old z");
        moved.clear();
        CHECK(!swap_in(root + L"\\nothing", inst, list, moved));   // an empty stage is no update
        std::filesystem::remove_all(root, ec);
    }
    // The launcher's disc reader: a good image (trimmed and full), a folder listed inside itself, names that would
    // leave game\, a folder past the image's end, a cut-off entry, not an Xbox image at all
    {
        std::vector<disc_file> f;
        mem_disc d = disc_image({{50, 4, 0, "default.xbe"}, {41, 2048, 0x10, "image"}});
        CHECK(disc_list(mem_read, &d, f) == 0 && f.size() == 2 && f[0].path == "default.xbe" && f[0].offset == 50 * 2048 && f[0].size == 4
              && f[1].path == "image/a.bin");
        f.clear();
        mem_disc full = disc_image({{50, 4, 0, "default.xbe"}}, 0x18300000ull);
        CHECK(disc_list(mem_read, &full, f) == 0 && f.size() == 1 && f[0].offset == 0x18300000ull + 50 * 2048);
        for (const char *bad : {"..", "..\\x", "C:x", "a/b"}) {
            f.clear();
            mem_disc b = disc_image({{50, 4, 0, bad}});
            CHECK(disc_list(mem_read, &b, f) == 2);
        }
        f.clear();
        mem_disc past = disc_image({{50, 4, 0, "default.xbe"}, {900, 2048, 0x10, "gone"}});   // its sectors aren't in the image
        CHECK(disc_list(mem_read, &past, f) == 2);
        f.clear();
        mem_disc cut = disc_image({{50, 4, 0, "longname1"}}, 0, 20);   // the root is 20 bytes: its one entry's name runs past it
        CHECK(disc_list(mem_read, &cut, f) == 2);
        f.clear();
        mem_disc whole = disc_image({{50, 4, 0, "longname1"}}, 0, 24);   // the same, 23 bytes long, fits
        CHECK(disc_list(mem_read, &whole, f) == 0 && f.size() == 1 && f[0].path == "longname1");
        f.clear();
        mem_disc none; none.b.assign(0x20000, 0);
        CHECK(disc_list(mem_read, &none, f) == 1);
    }
    // A save slot as the game checks it (game.h): word 0 the sum of words 1..0x21e, word 0x21f their xor
    {
        std::vector<uint32_t> w(SAVE_SLOT_SIZE / 4, 0);
        CHECK(!save_slot_used(w.data()));   // never used: zeros
        for (int i = 1; i < 0x21f; i++) w[i] = i * 2654435761u;
        uint32_t sum, x;
        save_slot_sums(w.data(), &sum, &x);
        w[0] = sum; w[0x21f] = x;
        CHECK(save_slot_used(w.data()));
        w[0x21f] ^= 1;
        CHECK(!save_slot_used(w.data()));   // the sum alone isn't enough
        CHECK(G_ITEMGOT - G_SAVE_STATE == 0x598);
    }
    // write_text_atomic: Windows line ends, as the first-run onimusha.ini always had
    {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        std::wstring p = std::wstring(tmp) + L"oni_selftest_text.ini";
        CHECK(write_text_atomic(p.c_str(), "a\n\nb = 1\n"));
        uint32_t n = 0;
        std::vector<uint8_t> got;
        if (FILE *f = _wfopen(p.c_str(), L"rb")) { uint8_t b[64]; n = (uint32_t)fread(b, 1, sizeof b, f); got.assign(b, b + n); fclose(f); }
        CHECK(std::string(got.begin(), got.end()) == "a\r\n\r\nb = 1\r\n");
        CHECK(!file_missing(p.c_str()));   // there: never replaced by defaults, read or not
        DeleteFileW(p.c_str());
        CHECK(file_missing(p.c_str()) && file_missing((std::wstring(tmp) + L"oni_no_such_dir\\x.ini").c_str()));
    }
    // The log's profile redaction (redact.h): the folder and its 8.3 form, any case, not a longer name that starts alike
    {
        const char *forms[2] = {"C:\\Users\\Al", "C:\\Users\\AL~1"};
        char out[256];
        const char *in = "open '\\??\\c:\\users\\al\\Desktop\\x.iso' (C:\\Users\\AL~1\\a) C:\\Users\\Alex\\b C:\\Users\\Al\n";
        size_t n = redact_profile(in, strlen(in), out, sizeof out, forms, 2);
        CHECK(!strcmp(out, "open '\\??\\%USERPROFILE%\\Desktop\\x.iso' (%USERPROFILE%\\a) C:\\Users\\Alex\\b %USERPROFILE%\n"));
        CHECK(n == strlen(out));
        CHECK(redact_profile("abc", 3, out, 3, forms, 2) == 2 && !strcmp(out, "ab"));   // cut to fit, still terminated
        CHECK(redact_profile("abc", 3, out, sizeof out, forms, 0) == 3);
        const char *ends = "c:\\users\\al, C:\\Users\\Al: C:\\Users\\Al;C:\\Users\\Al.\tC:\\Users\\Al]C:\\Users\\Al_2";
        redact_profile(ends, strlen(ends), out, sizeof out, forms, 2);
        CHECK(!strcmp(out, "%USERPROFILE%, %USERPROFILE%: %USERPROFILE%;%USERPROFILE%.\t%USERPROFILE%]C:\\Users\\Al_2"));
    }
    // The game's one-per-install mutex (layout.h): one name for a folder however it's spelled, seen while it's held
    {
        // A folder of its own (not this exe's, where a game being played would hold the real mutex), named long enough
        // to have a short (8.3) spelling where the drive makes them
        wchar_t here[MAX_PATH], tmp[MAX_PATH * 2], shrt[MAX_PATH], up[MAX_PATH * 2], a[64], b[64], c[64];
        GetTempPathW(MAX_PATH, here);
        wcscat(here, L"oni selftest mutex folder");
        CreateDirectoryW(here, 0);
        if (!GetLongPathNameW(here, tmp, MAX_PATH * 2)) wcscpy(tmp, here);
        if (!GetShortPathNameW(here, shrt, MAX_PATH)) wcscpy(shrt, here);
        wcscpy(up, tmp); CharUpperW(up);

        layout_game_mutex(tmp, a, 64); layout_game_mutex(shrt, b, 64); layout_game_mutex(up, c, 64);
        CHECK(!wcscmp(a, b) && !wcscmp(a, c));
        CHECK(!layout_game_running(tmp));
        HANDLE m = CreateMutexW(0, TRUE, a);
        CHECK(m && layout_game_running(shrt));
        if (m) CloseHandle(m);
        CHECK(!layout_game_running(tmp));
        RemoveDirectoryW(here);
    }
    // Xbox save re-signing: HMAC-SHA1 with a zero key over everything after the signature (value from Python's hmac)
    {
        std::string d(XSAVE_SIZE, 0);
        for (size_t i = 0; i < d.size(); i++) d[i] = (char)(i * 7);
        CHECK(xsave_sign(d));
        static const unsigned char want[20] = {0x7e, 0xeb, 0x88, 0x4b, 0xcc, 0xd1, 0x65, 0x47, 0x0d, 0xf8,
                                               0xc5, 0xbd, 0xd7, 0xf0, 0x68, 0xea, 0x09, 0x72, 0xb3, 0xc4};
        CHECK(!memcmp(d.data(), want, 20));
        std::string short_one(100, 0);
        CHECK(!xsave_sign(short_one));
    }
    // save_import (files.h): signed into a new save folder, the old save backed up, an already signed one noticed,
    // a file of the wrong size refused
    {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        std::wstring root = std::wstring(tmp) + L"oni_selftest_import", src = root + L"\\in.dat", bad = root + L"\\short.dat";
        std::filesystem::remove_all(root);
        make_dirs(root);
        std::string d(XSAVE_SIZE, 0);
        for (size_t i = 0; i < d.size(); i++) d[i] = (char)(i * 13);
        CHECK(write_file_atomic(src.c_str(), d.data(), d.size()));
        CHECK(write_file_atomic(bad.c_str(), "x", 1));
        std::wstring dir = root + L"\\saves\\" LAYOUT_TITLE_SAVES L"\\78C67B91A23F";
        CHECK(save_import(root, src) == SAVE_IMPORTED);
        CHECK(file_exists(dir + L"\\save.dat") && file_exists(dir + L"\\SaveMeta.xbx"));
        std::string signed_one = d;
        xsave_sign(signed_one);
        std::vector<uint8_t> got;
        if (FILE *f = _wfopen((dir + L"\\save.dat").c_str(), L"rb")) { got.resize(XSAVE_SIZE); got.resize(fread(got.data(), 1, got.size(), f)); fclose(f); }
        CHECK(std::string(got.begin(), got.end()) == signed_one);
        CHECK(write_file_atomic(src.c_str(), signed_one.data(), signed_one.size()));
        CHECK(save_import(root, src) == SAVE_SIGNED_ALREADY);
        int baks = 0;
        for (auto &e : std::filesystem::directory_iterator(dir)) baks += e.path().filename().wstring().rfind(L"save.dat.bak-", 0) == 0;
        CHECK(baks == 1);
        CHECK(save_import(root, bad) == SAVE_NOT_ONE);
        std::filesystem::remove_all(root);
    }
    if (failed) { printf("selftest: %d failed\n", failed); return 1; }
    printf("selftest ok\n");
    return 0;
}
