// [TL2] Torchlight II data packs and Switch textures, read natively (the device port of tools/tl2_mod.py man_read / pak_get /
// xtx_parse; formats in docs/tl2/TL2_PORT.md). Used by the on-phone ASTC cache builder (astc_convert.cpp) and its host test.
//   PAK.MAN  u16 version (>= 2: + u32), wstr root, u32 capacity, u32 directories; per directory: wstr name, u32 entries; per entry:
//            u32 crc, u8 type (7 = directory), wstr name, u32 offset, u32 size, u64 file time. wstr = u16 count + UTF-16LE.
//   PAK      at an entry's offset: u32 size, u32 packed size, then the data (zlib when packed != 0 and != size, else stored).
//   XTX      'DFvN' (u32 at 4: first block), 'HBvN' blocks (36-byte header: magic, u32, u64 payload bytes, u64 payload offset from the
//            block, u32 type, u32 index, u32 0): type 2 = info (u64 storage bytes, u32 alignment, width, height, depth, target, NVN
//            format, levels, u32, 17 x u32 level offsets at 0x28, u32 block height log2 at 0x6c), type 3 = the storage.
// The game's loader (main+0x1995f40) builds the NVN texture from the info block (format 0x41 is given as 0x42), refuses it unless
// nvnTextureBuilderGetStorageSize equals the info's storage bytes, and memcpy's the type-3 block into its pool: that block is the
// guest storage the engine keys the ASTC cache by.
#pragma once
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <zlib.h>
#include <string>
#include <vector>

namespace tl2pak {

struct Entry { std::string path; uint32_t off, size; };   // path: directory + name, ASCII upper case ('/' separators)
struct Pack {
    std::string path; int fd = -1; uint64_t bytes = 0; std::vector<Entry> entries;
    ~Pack() { if (fd >= 0) close(fd); }
    // Opens <path> and reads <path>.MAN. false when either is missing or the MAN does not parse.
    bool open(const std::string &p) {
        path = p; fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC); if (fd < 0) return false;
        struct stat st{}; if (fstat(fd, &st)) return false; bytes = uint64_t(st.st_size);
        const std::string mp = p + ".MAN"; const int mf = ::open(mp.c_str(), O_RDONLY | O_CLOEXEC); if (mf < 0) return false;
        std::vector<uint8_t> m; { struct stat ms{}; if (fstat(mf, &ms) || ms.st_size < 8) { close(mf); return false; } m.resize(size_t(ms.st_size));
            size_t got = 0; while (got < m.size()) { const ssize_t r = pread(mf, m.data() + got, m.size() - got, off_t(got)); if (r <= 0) break; got += size_t(r); }
            close(mf); if (got != m.size()) return false; }
        return parse_man(m);
    }
    bool parse_man(const std::vector<uint8_t> &b) {
        size_t o = 0; bool ok = true;
        auto u16 = [&]() -> uint32_t { if (o + 2 > b.size()) { ok = false; return 0; } uint16_t v; memcpy(&v, &b[o], 2); o += 2; return v; };
        auto u32 = [&]() -> uint32_t { if (o + 4 > b.size()) { ok = false; return 0; } uint32_t v; memcpy(&v, &b[o], 4); o += 4; return v; };
        auto ws = [&]() -> std::string { const uint32_t n = u16(); std::string s; if (o + size_t(n) * 2 > b.size()) { ok = false; return s; }
            for (uint32_t i = 0; i < n; i++) { uint16_t c; memcpy(&c, &b[o + 2 * i], 2); s.push_back(c == '\\' ? '/' : c < 128 ? char(c >= 'a' && c <= 'z' ? c - 32 : c) : '?'); }
            o += size_t(n) * 2; return s; };
        const uint32_t ver = u16(); if (ver >= 2) o += 4;
        ws(); u32(); const uint32_t nd = u32();
        for (uint32_t d = 0; ok && d < nd; d++) {
            const std::string dir = ws(); const uint32_t n = u32();
            for (uint32_t i = 0; ok && i < n; i++) {
                u32(); if (o + 1 > b.size()) { ok = false; break; } const uint8_t typ = b[o++];
                const std::string name = ws(); const uint32_t off = u32(), size = u32(); o += 8; if (o > b.size()) ok = false;
                if (ok && typ != 7) entries.push_back({dir + name, off, size});
            }
        }
        return ok;
    }
    // The file stored at off (decompressed). false when it reaches past the pack or does not inflate.
    bool read(uint32_t off, std::vector<uint8_t> &out) const {
        uint32_t h[2]; if (pread(fd, h, 8, off_t(off)) != 8) return false;
        const uint32_t dl = h[0], cl = h[1], n = cl ? cl : dl;
        if (uint64_t(off) + 8 + n > bytes || dl > (1u << 30)) return false;
        std::vector<uint8_t> raw(n); size_t got = 0;
        while (got < n) { const ssize_t r = pread(fd, raw.data() + got, n - got, off_t(off + 8 + got)); if (r <= 0) return false; got += size_t(r); }
        if (!cl || cl == dl) { out.swap(raw); return true; }
        out.resize(dl); uLongf ol = dl;
        return uncompress(out.data(), &ol, raw.data(), uLong(n)) == Z_OK && ol == dl;
    }
};

// A 2D XTX texture's description and storage (pointers into the file's bytes).
struct Xtx { uint32_t fmt, w, h, depth, target, levels, th, align; uint64_t size; uint32_t offsets[17]; const uint8_t *st; uint64_t st_bytes; };
inline bool parse_xtx(const uint8_t *d, size_t n, Xtx &x) {
    if (n < 16 || memcmp(d, "DFvN", 4)) return false;
    uint32_t o32; memcpy(&o32, d + 4, 4); uint64_t o = o32; const uint8_t *info = nullptr; uint64_t info_n = 0; x.st = nullptr; x.st_bytes = 0;
    while (o + 0x24 <= n && !memcmp(d + o, "HBvN", 4)) {
        uint64_t dsz, doff; uint32_t typ; memcpy(&dsz, d + o + 8, 8); memcpy(&doff, d + o + 16, 8); memcpy(&typ, d + o + 24, 4);
        if (doff < 0x24 || o + doff + dsz > n) return false;
        if (typ == 2 && !info) { info = d + o + doff; info_n = dsz; }
        if (typ == 3 && !x.st) { x.st = d + o + doff; x.st_bytes = dsz; }
        o += doff + dsz;
    }
    if (!info || info_n < 0x70 || !x.st) return false;
    uint32_t v[9]; memcpy(&x.size, info, 8); memcpy(v, info + 8, 32);
    x.align = v[0]; x.w = v[1]; x.h = v[2]; x.depth = v[3]; x.target = v[4]; x.fmt = v[5]; x.levels = v[6];
    memcpy(x.offsets, info + 0x28, sizeof x.offsets); memcpy(&x.th, info + 0x6c, 4);
    if (x.fmt == 0x41) x.fmt = 0x42;   // the loader's own remap (main+0x19960bc)
    return x.w && x.h && x.levels >= 1 && x.levels <= 16;
}
inline bool is_texture_path(const std::string &p) {
    const size_t n = p.size(); if (n < 4) return false; const char *e = p.c_str() + n - 4;
    return !strcmp(e, ".DDS") || !strcmp(e, ".PNG") || !strcmp(e, ".JPG") || !strcmp(e, ".TGA");
}

}  // namespace tl2pak
