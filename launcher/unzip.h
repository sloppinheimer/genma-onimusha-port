// Unpacking a .zip in the launcher itself (an Install .zip, the update, the HD pack's parts) instead of starting
// Windows' tar.exe for it: antivirus machine learning counts a hidden program started with a command line against an
// unsigned launcher. Stored and deflated entries (stb_image's inflate), zip64 sizes and offsets, each file's CRC
// checked; a name that would land outside dir (.., a drive, a leading \) fails the whole zip. Header-only so
// tests/selftest.cpp unpacks zips with it. Needs <windows.h>, <string> and <vector> before it.
#pragma once
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_ZLIB
#define STBI_SUPPORT_ZLIB
#define STBI_NO_STDIO
#include "../src/linux/stb/stb_image.h"

static uint32_t zip_crc(uint32_t crc, const uint8_t *p, size_t n)
{
    static uint32_t t[256];
    if (!t[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >> 1) : c >> 1; t[i] = c; }
    crc = ~crc;
    while (n--) crc = t[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}
static bool zip_read(HANDLE f, uint64_t off, void *buf, size_t n)
{
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)off;
    if (!SetFilePointerEx(f, li, 0, FILE_BEGIN)) return false;
    for (uint8_t *p = (uint8_t *)buf; n; ) {
        DWORD want = n > (1u << 30) ? (1u << 30) : (DWORD)n, got = 0;
        if (!ReadFile(f, p, want, &got, 0) || !got) return false;
        p += got; n -= got;
    }
    return true;
}
static bool zip_write(HANDLE f, const void *p, size_t n)
{
    DWORD put = 0;
    return WriteFile(f, p, (DWORD)n, &put, 0) && put == n;
}
static uint16_t zip_u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t zip_u32(const uint8_t *p) { return (uint32_t)zip_u16(p) | (uint32_t)zip_u16(p + 2) << 16; }
static uint64_t zip_u64(const uint8_t *p) { return zip_u32(p) | (uint64_t)zip_u32(p + 4) << 32; }

// Each folder of rel (backslashes) under dir, made. \\?\ so a deep path past MAX_PATH still works.
static std::wstring zip_long(const std::wstring &p) { return p.size() > 2 && p[1] == L':' ? L"\\\\?\\" + p : p; }
static void zip_dirs(const std::wstring &dir, const std::wstring &rel)
{
    for (size_t i = rel.find(L'\\'); i != std::wstring::npos; i = rel.find(L'\\', i + 1))
        CreateDirectoryW(zip_long(dir + L"\\" + rel.substr(0, i)).c_str(), 0);
}

static bool unzip(const std::wstring &zip, const std::wstring &dir)
{
    HANDLE f = CreateFileW(zip_long(zip).c_str(), GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, 0);
    if (f == INVALID_HANDLE_VALUE) return false;
    struct closer { HANDLE h; ~closer() { CloseHandle(h); } } fc{f};
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart < 22) return false;
    uint64_t size = (uint64_t)sz.QuadPart;
    // the end of central directory record: the last 22 bytes, or earlier by a comment of up to 64 KB
    std::vector<uint8_t> tail((size_t)(size < 22 + 65535 ? size : 22 + 65535));
    uint64_t tail_at = size - tail.size();
    if (!zip_read(f, tail_at, tail.data(), tail.size())) return false;
    size_t e = tail.size() - 22;
    while (zip_u32(&tail[e]) != 0x06054b50 || e + 22 + zip_u16(&tail[e + 20]) != tail.size()) if (!e--) return false;
    uint64_t count = zip_u16(&tail[e + 10]), cd_size = zip_u32(&tail[e + 12]), cd_at = zip_u32(&tail[e + 16]);
    if (count == 0xffff || cd_size == 0xffffffff || cd_at == 0xffffffff) {   // zip64: its own record, found by a locator
        uint8_t loc[20], z[56];
        if (tail_at + e < 20 || !zip_read(f, tail_at + e - 20, loc, 20) || zip_u32(loc) != 0x07064b50) return false;
        if (!zip_read(f, zip_u64(loc + 8), z, 56) || zip_u32(z) != 0x06064b50) return false;
        count = zip_u64(z + 32); cd_size = zip_u64(z + 40); cd_at = zip_u64(z + 48);
    }
    if (cd_size > (256u << 20) || cd_at + cd_size > size) return false;
    std::vector<uint8_t> cd((size_t)cd_size);
    if (!zip_read(f, cd_at, cd.data(), cd.size())) return false;
    std::vector<uint8_t> buf(1 << 20);
    size_t p = 0;
    for (uint64_t i = 0; i < count; i++) {
        if (p + 46 > cd.size() || zip_u32(&cd[p]) != 0x02014b50) return false;
        const uint8_t *c = &cd[p];
        uint16_t flags = zip_u16(c + 8), method = zip_u16(c + 10), nlen = zip_u16(c + 28), xlen = zip_u16(c + 30), clen = zip_u16(c + 32);
        uint32_t crc = zip_u32(c + 16);
        uint64_t csize = zip_u32(c + 20), usize = zip_u32(c + 24), at = zip_u32(c + 42);
        if (p + 46 + nlen + xlen + clen > cd.size()) return false;
        for (size_t x = p + 46 + nlen; x + 4 <= p + 46 + nlen + xlen; x += 4 + zip_u16(&cd[x + 2])) {
            if (zip_u16(&cd[x]) != 1) continue;   // the zip64 field: the 8-byte values of those that didn't fit, in order
            size_t v = x + 4, end = v + zip_u16(&cd[x + 2]);
            if (end > p + 46 + nlen + xlen) return false;
            if (usize == 0xffffffff) { if (v + 8 > end) return false; usize = zip_u64(&cd[v]); v += 8; }
            if (csize == 0xffffffff) { if (v + 8 > end) return false; csize = zip_u64(&cd[v]); v += 8; }
            if (at == 0xffffffff) { if (v + 8 > end) return false; at = zip_u64(&cd[v]); }
        }
        std::string raw((const char *)c + 46, nlen);
        p += 46 + nlen + xlen + clen;
        // the name: UTF-8 when flagged, else the DOS code page zips started with
        std::wstring name(raw.size(), 0);
        if (!raw.empty()) name.resize(MultiByteToWideChar(flags & 0x800 ? CP_UTF8 : 437, 0, raw.data(), (int)raw.size(), &name[0], (int)name.size()));
        for (auto &ch : name) if (ch == L'/') ch = L'\\';
        if (name.empty() || name[0] == L'\\' || name.find(L':') != std::wstring::npos) return false;
        for (size_t s = 0; s < name.size(); ) {   // no ".." part
            size_t n = name.find(L'\\', s);
            if (n == std::wstring::npos) n = name.size();
            if (n - s == 2 && name.compare(s, 2, L"..") == 0) return false;
            s = n + 1;
        }
        if (name.back() == L'\\') { zip_dirs(dir, name); continue; }   // a folder
        if (flags & 1) return false;   // encrypted
        uint8_t lh[30];
        if (!zip_read(f, at, lh, 30) || zip_u32(lh) != 0x04034b50) return false;
        uint64_t data = at + 30 + zip_u16(lh + 26) + zip_u16(lh + 28);
        if (data + csize > size) return false;
        zip_dirs(dir, name);
        HANDLE o = CreateFileW(zip_long(dir + L"\\" + name).c_str(), GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
        if (o == INVALID_HANDLE_VALUE) return false;
        closer oc{o};
        uint32_t got = 0;
        if (method == 0) {
            if (csize != usize) return false;
            for (uint64_t done = 0; done < csize; ) {
                size_t n = csize - done < buf.size() ? (size_t)(csize - done) : buf.size();
                if (!zip_read(f, data + done, buf.data(), n) || !zip_write(o, buf.data(), n)) return false;
                got = zip_crc(got, buf.data(), n);
                done += n;
            }
        } else if (method == 8) {   // deflate, decoded whole: an Install .zip's or the update's files are small
            if (csize > (1u << 30) || usize > (1u << 30)) return false;
            std::vector<uint8_t> in((size_t)csize + 1), out((size_t)usize + 1);
            if (!zip_read(f, data, in.data(), (size_t)csize)) return false;
            int n = stbi_zlib_decode_noheader_buffer((char *)out.data(), (int)out.size(), (const char *)in.data(), (int)csize);
            if (n < 0 || (uint64_t)n != usize || !zip_write(o, out.data(), (size_t)n)) return false;
            got = zip_crc(0, out.data(), (size_t)n);
        } else return false;
        if (got != crc) return false;
    }
    return true;
}
