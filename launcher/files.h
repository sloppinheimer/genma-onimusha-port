// The launcher's file helpers, and the update's file swap: header-only so tests/selftest.cpp runs the swap and its
// putting back in a temporary folder. Needs <windows.h>, <shlobj.h>, <string>, <vector>, <cstdio>, mods_list.h (is_file),
// layout.h (LAYOUT_TITLE_SAVES) and text.h (utf8, xsave_sign) before it.
#pragma once
#pragma comment(lib, "shell32.lib")

static bool file_exists(const std::wstring &p) { return is_file(p.c_str()); }
static void make_dirs(const std::wstring &dir) { SHCreateDirectoryExW(0, dir.c_str(), 0); }

// A save.dat from a real Xbox (copied off its hard disk, E:\UDATA\43430001\<save>\save.dat) re-signed for the port
// (xsave_sign) and put where the port keeps its save under root: the folder it already has, else the one it makes for
// its save ("SAVE DATA"). The save it replaces is kept beside it as save.dat.bak-<time>; nothing is changed when that
// copy can't be made.
enum { SAVE_IMPORTED, SAVE_SIGNED_ALREADY, SAVE_NOT_ONE, SAVE_NO_BACKUP, SAVE_NOT_WRITTEN };
static int save_import(const std::wstring &root, const std::wstring &src)
{
    std::string d;
    {
        HANDLE f = CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
        LARGE_INTEGER n = {};
        if (f != INVALID_HANDLE_VALUE && GetFileSizeEx(f, &n) && n.QuadPart == (LONGLONG)XSAVE_SIZE) {
            d.resize(XSAVE_SIZE);
            DWORD got = 0;
            if (!ReadFile(f, &d[0], (DWORD)d.size(), &got, 0) || got != d.size()) d.clear();
        }
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    }
    std::string was = d.substr(0, 20);
    if (!xsave_sign(d)) return SAVE_NOT_ONE;
    std::wstring udata = root + L"\\saves\\" LAYOUT_TITLE_SAVES, dir;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((udata + L"\\*").c_str(), &fd);
    for (bool more = h != INVALID_HANDLE_VALUE; more && dir.empty(); more = FindNextFileW(h, &fd))
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.' && file_exists(udata + L"\\" + fd.cFileName + L"\\save.dat"))
            dir = udata + L"\\" + fd.cFileName;
    if (h != INVALID_HANDLE_VALUE) FindClose(h);
    if (dir.empty()) {
        dir = udata + L"\\78C67B91A23F";
        make_dirs(dir);
        static const char meta[] = "\xff\xfeN\0a\0m\0e\0=\0S\0A\0V\0E\0 \0D\0A\0T\0A\0\r\0\n\0";   // UTF-16 "Name=SAVE DATA"
        write_file_atomic((dir + L"\\SaveMeta.xbx").c_str(), meta, sizeof meta - 1);
    }
    std::wstring dst = dir + L"\\save.dat";
    if (file_exists(dst)) {
        wchar_t stamp[32];
        SYSTEMTIME t;
        GetLocalTime(&t);
        swprintf(stamp, 32, L".bak-%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        if (!CopyFileW(dst.c_str(), (dst + stamp).c_str(), TRUE)) return SAVE_NO_BACKUP;
    }
    if (!write_file_atomic(dst.c_str(), d.data(), d.size())) return SAVE_NOT_WRITTEN;
    return was == d.substr(0, 20) ? SAVE_SIGNED_ALREADY : SAVE_IMPORTED;
}

// Every file under dir, in subfolders too: f(its path relative to dir, its find data).
template <class F> static void each_file(const std::wstring &dir, const F &f, const std::wstring &rel = L"")
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\" + rel + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) each_file(dir, f, rel + fd.cFileName + L"\\");
        else f(rel + fd.cFileName, fd);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Move every file of stage (subfolders too) into root. Each one it replaces is kept as <name>.old and listed in list
// (one relative path a line, appended) as it's made, for the next start to delete. moved: the targets put in place so
// far. False at the first file that can't be moved (or when stage is empty): swap_back then undoes it.
static bool swap_in(const std::wstring &stage, const std::wstring &root, const std::wstring &list_path, std::vector<std::wstring> &moved)
{
    std::vector<std::wstring> files;
    each_file(stage, [&](const std::wstring &rel, const WIN32_FIND_DATAW &) { files.push_back(rel); });
    bool ok = !files.empty();
    FILE *list = _wfopen(list_path.c_str(), L"a");
    for (auto &rel : files) {
        std::wstring target = root + L"\\" + rel;
        make_dirs(target.substr(0, target.find_last_of(L'\\')));
        DeleteFileW((target + L".old").c_str());
        if (file_exists(target)) {
            if (!MoveFileExW(target.c_str(), (target + L".old").c_str(), 0)) { ok = false; break; }
            if (list) { fprintf(list, "%s\n", utf8(rel).c_str()); fflush(list); }
        }
        moved.push_back(target);
        if (!MoveFileExW((stage + L"\\" + rel).c_str(), target.c_str(), 0)) { ok = false; break; }
    }
    if (list) fclose(list);
    return ok;
}
// Put back what swap_in moved: each new file out, its .old (if it had one) back.
static void swap_back(const std::vector<std::wstring> &moved)
{
    for (auto &t : moved) { DeleteFileW(t.c_str()); MoveFileExW((t + L".old").c_str(), t.c_str(), 0); }
}
