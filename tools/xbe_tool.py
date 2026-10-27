"""Genma Onimusha port: Xbox disc + XBE inspector.

  python -I xbe_tool.py ls      ISO              list every file on the disc
  python -I xbe_tool.py extract ISO NAME OUT     copy one file off the disc
  python -I xbe_tool.py prepare ISO OUTDIR       copy the whole disc (the port's game/ folder)
  python -I xbe_tool.py info    XBE              header, sections, kernel imports, XDK libs
  python -I xbe_tool.py image   XBE OUT          flat memory image, sections at their Xbox addresses
  python -I xbe_tool.py symbols XBE XBSYMS OUT   XBE + XbSymbolDatabaseCLI output -> one flat symbol list
  python -I xbe_tool.py selftest
"""
import os, struct, sys, tempfile

SECTOR = 2048
XDVDFS_MAGIC = b"MICROSOFT*XBOX*MEDIA"

# ---------- XDVDFS (Xbox disc filesystem) ----------

def _find_volume(f):
    # Trimmed XISO has the volume at 0x10000; full Redump images of XGD1 discs put the game partition at 0x18300000.
    # Other disc layouts (XGD2/XGD3) aren't handled.
    for part in (0, 0x18300000):
        f.seek(part + 0x10000)
        if f.read(20) == XDVDFS_MAGIC:
            root_sec, root_size = struct.unpack("<II", f.read(8))
            return part, root_sec, root_size
    raise SystemExit("not an Xbox disc image (no XDVDFS volume descriptor)")

def _walk(f, part, sec, size, prefix="", seen=None):
    # Names must be plain file names (a crafted image's "..", "\" or "C:" would write outside OUT); a directory listed
    # inside itself is read once.
    seen = set() if seen is None else seen
    if sec in seen:
        return
    if len(seen) > 10000:   # no real disc has that many folders: refused, as the launcher does, not half prepared
        raise SystemExit("disc image has more folders than a real disc: it may be damaged")
    seen.add(sec)
    f.seek(part + sec * SECTOR)
    data = f.read(size)
    i = 0
    while i + 14 <= len(data):
        if data[i:i + 2] == b"\xff\xff":           # padding to the next sector
            i = (i // SECTOR + 1) * SECTOR
            continue
        _, _, start, length, attr, nlen = struct.unpack_from("<HHIIBB", data, i)
        if nlen == 0:
            break
        name = data[i + 14:i + 14 + nlen].decode("ascii", "replace")
        if name in (".", "..") or any(c in name for c in '\\/:*?"<>|') or any(ord(c) < 32 for c in name):
            raise SystemExit(f"disc image has a file name that can't be right: {prefix + name!r}")
        path = prefix + name
        if attr & 0x10:
            if length:
                yield from _walk(f, part, start, length, path + "/", seen)
        else:
            yield path, part + start * SECTOR, length
        i = (i + 14 + nlen + 3) & ~3

def disc_files(iso):
    with open(iso, "rb") as f:
        part, sec, size = _find_volume(f)
        return list(_walk(f, part, sec, size))

def extract(iso, name, out):
    for path, off, length in disc_files(iso):
        if path.lower() == name.lower():
            with open(iso, "rb") as f, open(out, "wb") as o:
                f.seek(off)
                o.write(f.read(length))
            return length
    raise SystemExit(f"{name} not found on disc")

# ---------- XBE ----------

# Entry point / kernel thunk addresses are XOR-obfuscated with a retail or debug key.
ENTRY_KEYS = {"retail": 0xA8FC57AB, "debug": 0x94859D4B}
THUNK_KEYS = {"retail": 0x5B6D40B6, "debug": 0xEFB1F152}
SECTION_FLAGS = [(1, "W"), (2, "preload"), (4, "X"), (8, "insert"), (16, "headRO"), (32, "tailRO")]

def load_kernel_names():
    names = {}
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "xboxkrnl_exports.txt")
    with open(p) as fh:
        for line in fh:
            if line[:1].isdigit():
                n, s = line.split()
                names[int(n)] = s
    return names

class Xbe:
    def __init__(self, raw):
        if raw[:4] != b"XBEH":
            raise ValueError("not an XBE")
        self.raw = raw
        (self.base, self.headers_size, self.image_size, _, self.timestamp, cert, nsec, sechdr,
         self.init_flags, entry_x, self.tls) = struct.unpack_from("<11I", raw, 0x104)
        thunk_x, = struct.unpack_from("<I", raw, 0x158)
        nlib, libs, klib, xlib = struct.unpack_from("<4I", raw, 0x160)
        self.kind, self.entry = self._unxor(entry_x, ENTRY_KEYS)
        self.thunk = thunk_x ^ THUNK_KEYS[self.kind]
        self.title = raw[self.h(cert) + 0xC:self.h(cert) + 0xC + 80].decode("utf-16le").rstrip("\0")
        self.title_id, = struct.unpack_from("<I", raw, self.h(cert) + 8)
        self.sections = []
        for k in range(nsec):
            flags, va, vsize, raddr, rsize, name_addr = struct.unpack_from("<6I", raw, self.h(sechdr) + k * 56)
            self.sections.append(dict(name=self.cstr(name_addr), flags=flags, va=va, vsize=vsize, raddr=raddr, rsize=rsize))
        self.libs = [self._lib(libs + 16 * k) for k in range(nlib)]

    def _unxor(self, v, keys):
        for kind, key in keys.items():
            a = v ^ key
            if self.base <= a < self.base + self.image_size:
                return kind, a
        raise ValueError("entry point doesn't decode with retail or debug key")

    def h(self, va):                 # VA inside the header block -> file offset
        return va - self.base

    def cstr(self, va):
        o = self.h(va)
        return self.raw[o:self.raw.index(b"\0", o)].decode("ascii", "replace")

    def _lib(self, va):
        o = self.h(va)
        name = self.raw[o:o + 8].rstrip(b"\0").decode("ascii")
        major, minor, build, flags = struct.unpack_from("<4H", self.raw, o + 8)
        return dict(name=name, version=f"{major}.{minor}.{build}", qfe=flags & 0x1FFF, debug=bool(flags & 0x8000))

    def read_va(self, va, n):
        for s in self.sections:
            if s["va"] <= va < s["va"] + s["rsize"]:
                o = s["raddr"] + va - s["va"]
                return self.raw[o:o + n]
        raise ValueError(f"VA {va:#x} not in any section")

    def flat_image(self):
        """Memory as the Xbox loader lays it out from `base`: headers, then each section at its VA (bss zeroed)."""
        img = bytearray(self.image_size)
        img[:self.headers_size] = self.raw[:self.headers_size]
        for s in self.sections:
            o = s["va"] - self.base
            img[o:o + s["rsize"]] = self.raw[s["raddr"]:s["raddr"] + s["rsize"]]
        return bytes(img)

    def kernel_imports(self):
        out, va = [], self.thunk
        while True:
            v, = struct.unpack("<I", self.read_va(va, 4))
            if v == 0:
                return out
            out.append((va, v & 0x7FFFFFFF))
            va += 4

def info(path):
    x = Xbe(open(path, "rb").read())
    names = load_kernel_names()
    print(f"# {os.path.basename(path)}\n")
    print(f"title      {x.title}  (title id {x.title_id:08X})")
    print(f"build      {x.kind}")
    print(f"base       {x.base:#010x}   image size {x.image_size:#x}")
    print(f"entry      {x.entry:#010x}")
    print(f"kernel thunk table {x.thunk:#010x}")
    print(f"TLS dir    {x.tls:#010x}   init flags {x.init_flags:#x}\n")
    print(f"## Sections ({len(x.sections)})\n")
    print(f"{'name':<10} {'va':>10} {'vsize':>9} {'file off':>9} {'fsize':>9}  flags")
    for s in x.sections:
        fl = ",".join(n for b, n in SECTION_FLAGS if s["flags"] & b)
        print(f"{s['name']:<10} {s['va']:#010x} {s['vsize']:#9x} {s['raddr']:#9x} {s['rsize']:#9x}  {fl}")
    print(f"\n## XDK libraries linked in ({len(x.libs)})\n")
    for l in x.libs:
        print(f"{l['name']:<9} {l['version']}  qfe={l['qfe']}{'  DEBUG' if l['debug'] else ''}")
    imps = x.kernel_imports()
    print(f"\n## Kernel imports ({len(imps)})\n")
    for slot, ordn in imps:
        print(f"{slot:#010x}  {ordn:3d}  {names.get(ordn, f'ord_{ordn}')}")

def selftest():
    # Minimal synthetic XBE: one section holding a 2-entry thunk table.
    base = 0x10000
    raw = bytearray(0x1000)
    raw[0:4] = b"XBEH"
    cert, sechdr, secname, libs = base + 0x200, base + 0x300, base + 0x380, base + 0x3A0
    struct.pack_into("<11I", raw, 0x104, base, 0x1000, 0x2000, 0x178, 0, cert, 1, sechdr, 0, (base + 0x800) ^ ENTRY_KEYS["retail"], 0)
    struct.pack_into("<I", raw, 0x158, (base + 0x800) ^ THUNK_KEYS["retail"])
    struct.pack_into("<4I", raw, 0x160, 1, libs, 0, 0)
    raw[0x20C:0x20C + 8] = "Test".encode("utf-16le")
    struct.pack_into("<6I", raw, 0x300, 4, base + 0x800, 0x100, 0x800, 0x100, secname)
    raw[0x380:0x386] = b".text\0"
    raw[0x3A0:0x3A8] = b"D3D8\0\0\0\0"
    struct.pack_into("<4H", raw, 0x3A8, 1, 0, 5849, 0x8001)
    struct.pack_into("<3I", raw, 0x800, 0x80000000 | 49, 0x80000000 | 187, 0)
    x = Xbe(bytes(raw))
    assert x.kind == "retail" and x.entry == base + 0x800 and x.title == "Test"
    assert x.sections[0]["name"] == ".text"
    assert x.libs == [dict(name="D3D8", version="1.0.5849", qfe=1, debug=True)]
    assert x.kernel_imports() == [(base + 0x800, 49), (base + 0x804, 187)]
    img = x.flat_image()
    assert len(img) == 0x2000 and img[:4] == b"XBEH" and img[0x800:0x804] == raw[0x800:0x804]
    names = load_kernel_names()
    assert names[49] == "HalReturnToFirmware" and names[187] == "NtClose", (names[49], names[187])
    # Synthetic XDVDFS image: root (sector 40) = default.xbe + dir "image" (41), which lists a file and itself.
    def disc(root):
        img = bytearray(60 * SECTOR)
        img[0x10000:0x10000 + 28] = XDVDFS_MAGIC + struct.pack("<II", 40, SECTOR)
        def put(sec, entries):
            d = bytearray(b"\xff" * SECTOR); i = 0
            for start, length, attr, name in entries:
                struct.pack_into("<HHIIBB", d, i, 0, 0, start, length, attr, len(name)); d[i + 14:i + 14 + len(name)] = name
                i = (i + 14 + len(name) + 3) & ~3
            img[sec * SECTOR:(sec + 1) * SECTOR] = d
        put(40, root)
        put(41, [(50, 4, 0, b"a.bin"), (41, SECTOR, 0x10, b"loop")])
        img[50 * SECTOR:50 * SECTOR + 4] = b"DATA"
        f = tempfile.NamedTemporaryFile(suffix=".iso", delete=False); f.write(img); f.close()
        try: return disc_files(f.name)
        finally: os.unlink(f.name)
    got = disc(([(50, 4, 0, b"default.xbe"), (41, SECTOR, 0x10, b"image")]))
    assert [p for p, _, _ in got] == ["default.xbe", "image/a.bin"], got
    assert got[0][1:] == (50 * SECTOR, 4)
    for bad in (b"..", b"..\\x", b"C:x", b"a/b"):
        try: disc([(50, 4, 0, bad)]); raise AssertionError(bad)
        except SystemExit: pass
    print("selftest ok")

if __name__ == "__main__":
    a = sys.argv[1:]
    if a[:1] == ["ls"]:
        for p, off, n in disc_files(a[1]):
            print(f"{n:>12}  {p}")
    elif a[:1] == ["extract"]:
        print(extract(a[1], a[2], a[3]), "bytes")
    elif a[:1] == ["prepare"]:      # copy the whole disc into OUT (the port's game/ folder)
        files = disc_files(a[1])
        with open(a[1], "rb") as f:
            for i, (path, off, length) in enumerate(files):
                dst = os.path.join(a[2], *path.split("/"))
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                if os.path.exists(dst) and os.path.getsize(dst) == length:
                    continue
                f.seek(off)
                with open(dst, "wb") as o:
                    left = length
                    while left:
                        chunk = f.read(min(left, 1 << 24))
                        if not chunk:
                            raise SystemExit(f"{path}: disc image ends {left} bytes early (truncated ISO?)")
                        o.write(chunk)
                        left -= len(chunk)
                if i % 100 == 0:
                    print(f"{i}/{len(files)} {path}", flush=True)
        print("done:", len(files), "files")
    elif a[:1] == ["info"]:
        info(a[1])
    elif a[:1] == ["symbols"]:      # XBE + XbSymbolDatabaseCLI output -> one flat list for Ghidra / the loader
        x = Xbe(open(a[1], "rb").read())
        names = load_kernel_names()
        out = [f"ENTRY {x.entry:#010x} xbe_entry"]
        out += [f"SECT {s['va']:#010x} {s['name']} {s['vsize']:#x} {s['flags']:#x}" for s in x.sections]
        out += [f"KTHUNK {slot:#010x} krnl_{names.get(o, f'ord_{o}')} {o}" for slot, o in x.kernel_imports()]
        for line in open(a[2], encoding="utf-8-sig"):
            lib, kind, rest = line.strip().split("__", 2)
            name, addr = rest.rsplit(" = ", 1)
            if kind == "FUN":
                name = name.split("__", 1)[1].split("(")[0]     # drop calling convention + params
            out.append(f"{kind} {int(addr, 16):#010x} {name} {lib}")
        open(a[3], "w").write("\n".join(out) + "\n")
        print(len(out), "lines")
    elif a[:1] == ["image"]:
        x = Xbe(open(a[1], "rb").read())
        open(a[2], "wb").write(x.flat_image())
        print(f"{x.image_size:#x} bytes at base {x.base:#x}")
    elif a[:1] == ["selftest"]:
        selftest()
    else:
        print(__doc__)
