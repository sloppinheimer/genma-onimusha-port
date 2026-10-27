// The launcher's reader for Xbox disc images (XDVDFS), header-only so tests/selftest.cpp checks the very code the
// install runs (tools/xbe_tool.py has its own, in Python). Reads through a callback: a file for the launcher, memory
// for the tests. Needs <string>, <vector>, <set>, <cstring>, <cstdint> and text.h (disc_name_ok) before it.
#pragma once

struct disc_file { std::string path; uint64_t offset, size; };
// Reads n bytes at off into buf: true only when all of them were read.
typedef bool (*disc_read_fn)(void *ctx, uint64_t off, void *buf, uint32_t n);

// XDVDFS: a volume descriptor at sector 32 of the game partition (at 0 in a trimmed image, 0x18300000 in a full Redump
// image), and directories as entry lists {left, right, sector, size, attr, name}. budget bounds the directory bytes
// read in all (a real disc's are a few hundred KB); seen stops a directory listed inside itself.
static bool disc_walk(disc_read_fn read, void *ctx, uint64_t part, uint32_t sec, uint32_t size, const std::string &prefix,
                      std::vector<disc_file> &out, int depth, uint32_t &budget, std::set<uint32_t> &seen)
{
    if (!seen.insert(sec).second) return true;   // already read
    if (depth > 32 || size > budget || out.size() > 100000) return false;   // no real disc: refused, not half installed
    budget -= size;
    std::vector<uint8_t> d(size);
    // A directory past the image's end (a cut-short download) or an entry cut off fails the install: skipping them
    // installed "successfully" with whole folders missing, and the game failed later, looking like a port bug.
    if (!read(ctx, part + (uint64_t)sec * 2048, d.data(), size)) return false;
    for (size_t i = 0; i + 14 <= d.size();) {
        if (d[i] == 0xff && d[i + 1] == 0xff) { i = (i / 2048 + 1) * 2048; continue; }
        uint32_t start, length; memcpy(&start, &d[i + 4], 4); memcpy(&length, &d[i + 8], 4);
        uint8_t attr = d[i + 12], nlen = d[i + 13];
        if (!nlen) break;
        if (i + 14 + nlen > d.size()) return false;
        std::string name((const char *)&d[i + 14], nlen);
        if (!disc_name_ok(name)) return false;
        if (attr & 0x10) { if (length && !disc_walk(read, ctx, part, start, length, prefix + name + "/", out, depth + 1, budget, seen)) return false; }
        else out.push_back({prefix + name, part + (uint64_t)start * 2048, length});
        i = (i + 14 + nlen + 3) & ~(size_t)3;
    }
    return true;
}

// Every file on the disc into out: 0 when read whole, 1 not an Xbox disc image, 2 one the install must refuse (a
// folder past its end, a name that can't be one, more than a real disc holds).
static int disc_list(disc_read_fn read, void *ctx, std::vector<disc_file> &out)
{
    for (uint64_t p : {0ull, 0x18300000ull}) {
        uint8_t v[28];
        if (!read(ctx, p + 0x10000, v, 28) || memcmp(v, "MICROSOFT*XBOX*MEDIA", 20)) continue;
        uint32_t root_sec, root_size, budget = 16u << 20;
        memcpy(&root_sec, v + 20, 4); memcpy(&root_size, v + 24, 4);
        std::set<uint32_t> seen;
        return disc_walk(read, ctx, p, root_sec, root_size, "", out, 0, budget, seen) ? 0 : 2;
    }
    return 1;
}
