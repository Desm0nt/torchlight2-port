// [TL2] Host test of the ASTC cache key against the game's own data (docs/tl2/ASTC_CACHE.md "Ключ"): every texture of the RomFS packs
// is read with the native reader (dm/android/tl2_pak.h, as the on-phone builder reads them) and described to the engine's NVN layout
// code exactly as the game's loader does (main+0x1995f40: width, height, depth, levels, format, target; no packaged layout, no flags).
// Checks, per BC1-BC3 2D texture:
//   1. nvn_tex_layout(desc).size == the XTX info's storage bytes == the storage block's size (the loader refuses the texture otherwise,
//      so this is what the engine's t->L.size is) -> the engine hashes exactly the bytes the loader copied;
//   2. the automatic block height == the XTX's block height, and every level deswizzles to the same blocks either way;
//   3. prints the key of every texture ("crc adler bytes fmt w h levels path") for tools/tl2_astc_cache.py --keys to compare.
// Build and run (WSL, from the repository root; Vulkan headers only for VkFormat in xbg_texture.h):
//   gcc -O2 -c dm/gfx/tex/nvn_deswizzle.c dm/gfx/nvn/nvn_tex.c -DDM_GAME_D3 -Idm/gfx/nvn
//   g++ -O2 -std=c++17 -DDM_GAME_D3 -Idm/android -Idm/gfx/tex -Idm/gfx/nvn dm/android/tests/tl2_astc_key_test.cpp nvn_deswizzle.o nvn_tex.o -lz -o tl2_astc_key_test
//   ./tl2_astc_key_test <romfs>/PAKS [keys.tsv]
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <map>
#include <set>
#include <tuple>
#include <algorithm>
#include "tl2_pak.h"
extern "C" {
#include "nvn_tex.h"
#include "nvn_deswizzle.h"
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <romfs>/PAKS [keys.tsv]\n", argv[0]); return 2; }
    const std::string dir = argv[1]; FILE *keys = argc > 2 ? fopen(argv[2], "w") : nullptr;
    std::vector<std::string> paks; if (DIR *d = opendir(dir.c_str())) { while (dirent *e = readdir(d)) { std::string n = e->d_name; if (n.size() > 4 && n.substr(n.size() - 4) == ".PAK") paks.push_back(n); } closedir(d); }
    std::sort(paks.begin(), paks.end());
    uint64_t tex = 0, bc = 0, size_ok = 0, size_bad = 0, block_bad = 0, th_diff = 0, th_diff_same_blocks = 0, desw_bad = 0, other = 0, cube = 0, rgba = 0, unread = 0;
    std::map<uint32_t, uint64_t> by_fmt; std::set<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>> uniq; uint64_t bc_bytes = 0;
    std::vector<uint8_t> buf;
    for (const auto &p : paks) {
        tl2pak::Pack pk; if (!pk.open(dir + "/" + p)) { printf("%s: cannot open (or its .MAN)\n", p.c_str()); return 1; }
        uint64_t n_here = 0;
        for (const auto &e : pk.entries) {
            if (!tl2pak::is_texture_path(e.path) || !e.size) continue;
            if (!pk.read(e.off, buf)) { ++unread; continue; }
            tl2pak::Xtx x; if (!tl2pak::parse_xtx(buf.data(), buf.size(), x)) { ++other; continue; }
            ++tex; ++n_here;
            if (x.fmt == 0x25) { ++rgba; continue; }
            if (x.fmt < 0x41 || x.fmt > 0x48) { ++other; continue; }
            if (x.target != 1 || x.depth != 1) { ++cube; continue; }
            ++bc; ++by_fmt[x.fmt];
            nvn_texdesc d{}; d.target = int(x.target); d.format = int(x.fmt); d.width = x.w; d.height = x.h; d.depth = x.depth; d.levels = x.levels;
            nvn_texlayout L; nvn_tex_layout(&d, &L);
            if (L.size == x.size && x.size == x.st_bytes) ++size_ok;
            else { if (size_bad++ < 10) printf("SIZE %s fmt=%#x %ux%u levels=%u: layout %llu, info %llu, block %llu\n", e.path.c_str(), x.fmt, x.w, x.h, x.levels, (unsigned long long)L.size, (unsigned long long)x.size, (unsigned long long)x.st_bytes); continue; }
            if (L.tileH != x.th) {
                ++th_diff; nvn_texdesc pd = d; pd.pkg_block_h = int(x.th) + 1; bool same = true;
                for (unsigned l = 0; l < x.levels && same; l++) {
                    const unsigned w = std::max(1u, x.w >> l), h = std::max(1u, x.h >> l), bx = (w + 3) / 4, by = (h + 3) / 4; const unsigned bpb = (x.fmt == 0x42 || x.fmt == 0x46) ? 8 : 16;
                    std::vector<uint8_t> a(size_t(bx) * by * bpb), b2(a.size());
                    if (nvn_deswizzle(&d, l, 0, x.st, a.data(), size_t(bx) * bpb) || nvn_deswizzle(&pd, l, 0, x.st, b2.data(), size_t(bx) * bpb) || a != b2) same = false;
                }
                if (same) ++th_diff_same_blocks; else if (desw_bad++ < 10) printf("TILE %s fmt=%#x %ux%u: auto tileH %u, XTX %u, levels differ\n", e.path.c_str(), x.fmt, x.w, x.h, L.tileH, x.th);
            }
            const uint32_t crc = uint32_t(crc32(0, x.st, uInt(x.st_bytes))), ad = uint32_t(adler32(1, x.st, uInt(x.st_bytes)));
            if (uniq.insert({crc, ad, uint32_t(x.st_bytes), x.fmt, x.w, x.h, x.levels}).second) bc_bytes += x.st_bytes;
            if (keys) fprintf(keys, "%08x %08x %llu %x %u %u %u %s %s\n", crc, ad, (unsigned long long)x.st_bytes, x.fmt, x.w, x.h, x.levels, p.c_str(), e.path.c_str());
        }
        printf("%s: %llu textures\n", p.c_str(), (unsigned long long)n_here);
    }
    if (keys) fclose(keys);
    (void)block_bad;
    printf("textures %llu: BC 2D %llu (", (unsigned long long)tex, (unsigned long long)bc); for (auto &f : by_fmt) printf(" %#x:%llu", f.first, (unsigned long long)f.second);
    printf(" ), RGBA8 %llu, cube / volume BC %llu, other %llu, unreadable %llu\n", (unsigned long long)rgba, (unsigned long long)cube, (unsigned long long)other, (unsigned long long)unread);
    printf("storage size = engine layout = loader check: %llu ok, %llu differ\n", (unsigned long long)size_ok, (unsigned long long)size_bad);
    printf("block height: %llu differ from the XTX (%llu of them deswizzle to the same blocks, %llu not)\n", (unsigned long long)th_diff, (unsigned long long)th_diff_same_blocks, (unsigned long long)desw_bad);
    printf("unique BC keys %zu, %.1f MB of storage\n", uniq.size(), bc_bytes / 1048576.0);
    return size_bad || desw_bad ? 1 : 0;
}
