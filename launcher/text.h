// The launcher's pure helpers: text, JSON, version and name checks, the default settings. Header-only, so
// tests/selftest.cpp tests the very code the launcher runs. Needs <windows.h>, <string>, <vector>, settings_default.h
// and mods_list.h before it, and links bcrypt.
#pragma once

// ---- small helpers ----------------------------------------------------------------------------------
static std::string utf8(const std::wstring &w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), 0, 0, 0, 0);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, 0, 0);
    return s;
}

static std::wstring wide(const std::string &s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), 0, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::string jstr(const std::string &s)   // JSON string literal
{
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}

// A number from onimusha.ini as a JSON number, or def when it isn't one. JSON has no ".4", "4." or "02", and one such
// value (a hand edit) made the whole state message unreadable, so the page showed nothing at all.
static std::string json_number(const std::string &v, const std::string &def)
{
    char *end = 0;
    double d = v.empty() ? 0 : strtod(v.c_str(), &end);
    if (v.empty() || *end || d - d != 0 || d > 1e15 || d < -1e15) return def;   // d - d: not inf or nan
    char b[32];
    snprintf(b, sizeof b, "%.15g", d);
    return b;
}

// The value of the first field called name in JSON (strings and numbers only; at any depth, but only keys match).
static std::string field(const std::string &j, const char *name)
{
    auto ws = [&](size_t &p) { while (p < j.size() && strchr(" \t\r\n", j[p]) && j[p]) p++; };
    auto str = [&](size_t &p) {   // the string whose opening quote is at p, as UTF-8; p ends past its closing quote
        std::string out;
        std::wstring u16;   // \uXXXX escapes, decoded together so surrogate pairs join up
        for (p++; p < j.size() && j[p] != '"'; p++) {
            // \uXXXX with all four hex digits: a cut-off "\u12" used to take the closing quote as a digit and read on
            if (j[p] == '\\' && p + 1 < j.size() && j[p + 1] == 'u' && p + 5 < j.size() && isxdigit((unsigned char)j[p + 2]) && isxdigit((unsigned char)j[p + 3])
                && isxdigit((unsigned char)j[p + 4]) && isxdigit((unsigned char)j[p + 5])) { u16 += (wchar_t)strtol(j.substr(p + 2, 4).c_str(), 0, 16); p += 5; continue; }
            out += utf8(u16); u16.clear();
            if (j[p] == '\\' && p + 1 < j.size()) {
                static const char from[] = "bfnrt", to[] = "\b\f\n\r\t";
                char e = j[++p];
                const char *k = e ? strchr(from, e) : 0;
                out += k ? to[k - from] : e;   // \" \\ \/ are the character itself
            } else out += j[p];
        }
        p++;
        return out + utf8(u16);
    };
    for (size_t p = 0; p < j.size();) {
        if (j[p] != '"') { p++; continue; }
        std::string k = str(p);
        ws(p);
        if (p >= j.size() || j[p] != ':' || k != name) continue;
        p++; ws(p);
        if (p < j.size() && j[p] == '"') return str(p);
        std::string out;
        while (p < j.size() && !strchr(",}] \t\r\n", j[p])) out += j[p++];
        return out;
    }
    return {};
}

static std::string trim(std::string s)
{
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");   // fgets keeps the line's \n
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

static bool line_key(const std::string &line, std::string *k, std::string *v)   // onimusha.ini's rules (ini_line, mods_list.h)
{
    std::vector<char> b(line.begin(), line.end());
    b.push_back(0);
    char *key, *val;
    if (!ini_line(b.data(), &key, &val, 1, 0, 0)) return false;
    *k = key; *v = val;
    return true;
}

// Every onimusha.ini key the game knows, with its section and default: read from the file the game writes
// (settings_default.h), so a new setting needs no list here.
struct ini_default { std::string key, section, value; };

static const std::vector<ini_default> &ini_defaults()
{
    static std::vector<ini_default> d;
    if (d.empty()) {
        std::string all = settings_default_file, sec, k, v;
        for (size_t a = 0, b; a < all.size(); a = b + 1) {
            if ((b = all.find('\n', a)) == std::string::npos) b = all.size();
            std::string l = trim(all.substr(a, b - a));
            if (l.size() > 2 && l[0] == '[' && l.back() == ']') sec = l.substr(1, l.size() - 2);
            else if (line_key(l, &k, &v)) d.push_back({k, sec, v});
        }
    }
    return d;
}

// A name a disc image gives must be a plain file name (no path parts), or the install is refused: a crafted one could
// write outside game\.
static bool plain_name(const std::wstring &n);
static bool disc_name_ok(const std::string &n)
{
    if (n.empty() || n == "." || n == "..") return false;
    for (unsigned char c : n) if (c < 0x20 || strchr("\\/:*?\"<>|", c)) return false;
    return plain_name(wide(n));   // and no device name (COM1, NUL.txt: CreateFileW would open the device) or trailing dot / space
}

// A folder or file name from the page or pack.ini, one that stays one: no path parts, no control characters, not ending in a dot or space (Windows
// drops those, so "..." would name the folder it's in) and no device name (CON, NUL, COM1...).
static bool plain_name(const std::wstring &n)
{
    if (n.empty() || n.back() == L'.' || n.back() == L' ' || n.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) return false;
    for (wchar_t c : n) if (c < 32) return false;
    std::wstring stem = n.substr(0, n.find(L'.'));
    for (auto &c : stem) c = towupper(c);
    static const wchar_t *const dev[] = {L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8",
                                         L"COM9", L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9"};
    for (auto d : dev) if (stem == d) return false;
    return true;
}

// ---- onimusha.ini editing: values change in place, comments stay ----------------------------------------
// The section a key goes in: its own in settings_default.h ([launcher] included), else [keys].
static const char *ini_section_of(const std::string &key)
{
    for (auto &d : ini_defaults()) if (key == d.key) return d.section.c_str();
    return "keys";
}
// Only keys settings_default.h knows, and a value that stays one value on one line: the page's messages reach
// ini_set, and a newline (or a value past the game's 256-byte lines) would add settings of its own.
static bool ini_set_ok(const std::string &key, const std::string &value)
{
    bool known = false;
    for (auto &d : ini_defaults()) known |= key == d.key;
    return known && value.size() <= 200 && value.find_first_of(std::string("\r\n;\0", 4)) == std::string::npos;
}
// key = value into the file's lines: its line replaced where it has one, else added after the last setting of its
// section, or in a new section at the end.
static void ini_set_lines(std::vector<std::string> &lines, const std::string &key, const std::string &value)
{
    std::string k, v, sec = std::string("[") + ini_section_of(key) + "]";
    for (auto &l : lines) if (line_key(l, &k, &v) && !_stricmp(k.c_str(), key.c_str())) { l = key + " = " + value; return; }
    size_t s = 0;
    for (; s < lines.size() && _stricmp(trim(lines[s]).c_str(), sec.c_str()); s++) {}
    if (s == lines.size()) { lines.push_back(""); lines.push_back(sec); lines.push_back(key + " = " + value); return; }
    size_t end = s + 1;   // after the section's last setting
    for (size_t i = s + 1; i < lines.size() && trim(lines[i]).rfind("[", 0) != 0; i++) if (!trim(lines[i]).empty()) end = i + 1;
    lines.insert(lines.begin() + end, key + " = " + value);
}

// ---- the HD pack's pack.ini (tools/hdpack/pack_release.py): version, then [folder] name, description, size, parts --
struct pack_part { std::string name, sha; uint64_t size; };
struct pack_mod { std::string folder, name, about; uint64_t size = 0; std::vector<pack_part> parts; };
// False when it has no version or no usable folder. Folders and parts need plain names (a crafted release can't
// reach outside mods\) and a folder needs a part.
static bool pack_ini_parse(const std::string &ini, std::string &version, std::vector<pack_mod> &mods)
{
    version.clear(); mods.clear();
    size_t p = 0;
    while (p < ini.size()) {
        size_t e = ini.find('\n', p);
        std::string l = trim(ini.substr(p, e == std::string::npos ? std::string::npos : e - p));
        p = e == std::string::npos ? ini.size() : e + 1;
        if (l.empty() || l[0] == ';') continue;
        if (l[0] == '[' && l.back() == ']') { mods.push_back({}); mods.back().folder = l.substr(1, l.size() - 2); continue; }
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(l.substr(0, eq)), v = trim(l.substr(eq + 1));
        if (mods.empty()) { if (k == "version") version = v; continue; }
        auto &m = mods.back();
        if (k == "name") m.name = v;
        else if (k == "description") m.about = v;
        else if (k == "size") m.size = _strtoui64(v.c_str(), 0, 10);
        else if (k == "part") {
            char name[256] = "", sha[80] = "";
            unsigned long long size = 0;
            if (sscanf(v.c_str(), "%255s %llu %79s", name, &size, sha) == 3 && size) m.parts.push_back({name, sha, size});
        }
    }
    mods.erase(std::remove_if(mods.begin(), mods.end(), [](const pack_mod &m) {
        if (!plain_name(wide(m.folder)) || m.parts.empty()) return true;
        for (auto &pt : m.parts) if (!plain_name(wide(pt.name))) return true;
        return false;
    }), mods.end());
    return !version.empty() && !mods.empty();
}
// A mod.ini's text with version = <version>: the first version line replaced, or one added at the end.
static std::string stamp_version_text(const std::string &text, const std::string &version)
{
    std::string out;
    bool done = false;
    for (size_t p = 0; p < text.size();) {
        size_t e = text.find('\n', p);
        std::string l = text.substr(p, e == std::string::npos ? std::string::npos : e + 1 - p);
        p = e == std::string::npos ? text.size() : e + 1;
        std::string k = trim(l.substr(0, l.find('=')));
        if (!done && l.find('=') != std::string::npos && !_stricmp(k.c_str(), "version")) { l = "version = " + version + "\r\n"; done = true; }
        out += l;
    }
    if (!out.empty() && out.back() != '\n') out += "\r\n";
    if (!done) out += "version = " + version + "\r\n";
    return out;
}

// Genma's save.dat (one file, all slots): a 20-byte signature, then the game's data. The game signs it with the SDK's
// XCalculateSignature, an HMAC-SHA1 of everything after the signature whose key comes from the console's keys, so a
// save from a real Xbox fails the check here. Under the port those keys are all zero, and the key the game ends up
// using is 16 zero bytes (checked against a save the port wrote). Re-signing with it is the whole conversion: the data
// is the same game's. Returns false for a file that isn't a Genma save.
static const size_t XSAVE_SIZE = 0x4600;
static bool xsave_sign(std::string &d)
{
    if (d.size() != XSAVE_SIZE) return false;
    static const UCHAR key[16] = {};
    return BCRYPT_SUCCESS(BCryptHash(BCRYPT_HMAC_SHA1_ALG_HANDLE, (PUCHAR)key, sizeof key, (PUCHAR)&d[20],
                                     (ULONG)(d.size() - 20), (PUCHAR)&d[0], 20));
}

// Version strings like "v0.8.1", "0.7", "0.9-beta". Numbers first; with the same numbers, a release is newer than its
// pre-release ("0.9" than "0.9-beta": a beta tester was never offered the final, the two read as equal).
static bool newer_than(const std::string &a, const std::string &b)
{
    int x[3] = {}, y[3] = {}, na = 0, nb = 0;
    sscanf(a.c_str() + (!a.empty() && a[0] == 'v'), "%d.%d.%d%n", &x[0], &x[1], &x[2], &na);
    sscanf(b.c_str() + (!b.empty() && b[0] == 'v'), "%d.%d.%d%n", &y[0], &y[1], &y[2], &nb);
    for (int i = 0; i < 3; i++) if (x[i] != y[i]) return x[i] > y[i];
    bool pa = a.find('-') != std::string::npos, pb = b.find('-') != std::string::npos;
    return !pa && pb;
}
