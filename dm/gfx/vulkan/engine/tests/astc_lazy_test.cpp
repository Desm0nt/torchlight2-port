// [DOS2] Host test of the lazy ASTC cache encoder (xbg_astc_lazy.cpp, production code): NVN packaged textures (DFvN, as DOS2's .dds
// members of the LSPK paks: tools/lspk.py extract) go through astclazy::submit like the engine's offer(); the test waits for the
// commits, reads the journal back with the engine's parser (xbg_ac_journal_parse), checks every entry against its pack bytes and
// measures mip 0 of the cached ASTC against the decoded BC (the engine's deswizzle + CPU decoder).
// Build (Linux / WSL; the Vulkan headers only for the VkFormat enum, e.g. a copy of the NDK's sysroot/usr/include/vulkan + vk_video):
//   E=dm/gfx/vulkan/engine; A=dm/third_party/astcenc
//   gcc -O2 -c dm/gfx/tex/nvn_deswizzle.c dm/gfx/nvn/nvn_tex.c $E/xbg_texture.c -fsigned-char -I<vk>
//   g++ -O2 -std=c++17 -I$A -I$E -I<vk> $E/tests/astc_lazy_test.cpp $E/xbg_astc_lazy.cpp $A/*.cpp *.o -lpthread -o astc_lazy_test
//   ./astc_lazy_test <out dir> <tex.dds>...
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <chrono>
#include <string>
#include <thread>
#include <vector>
#include "astcenc.h"
#include "../xbg_astc_lazy.h"
#include "../xbg_texture.h"
extern "C" {
#include "../../../tex/nvn_deswizzle.h"
void xbg_logf(const char *f, ...) { va_list a; va_start(a, f); vprintf(f, a); va_end(a); printf("\n"); fflush(stdout); }
}

#define CHECK(e) do { if (!(e)) { printf("FAIL line %d: %s\n", __LINE__, #e); return 1; } } while (0)

static bool read_file(const char *p, std::vector<uint8_t> &d) {
    FILE *f = fopen(p, "rb"); if (!f) return false; fseek(f, 0, SEEK_END); d.resize(size_t(ftell(f))); fseek(f, 0, SEEK_SET);
    const bool ok = fread(d.data(), 1, d.size(), f) == d.size(); fclose(f); return ok;
}
// DFvN -> descriptor + storage (the D3 converter's parse_tex, without the .tex wrapper: DOS2's .dds members start with DFvN)
static bool parse(const std::vector<uint8_t> &d, nvn_texdesc &t, const uint8_t **st, uint32_t *size) {
    if (d.size() < 0x34 || memcmp(d.data(), "DFvN", 4)) return false;
    int64_t info = -1, data = -1; uint32_t dn = 0; uint64_t o = 0x10;
    while (o + 36 <= d.size() && !memcmp(&d[o], "HBvN", 4)) {
        uint32_t f[8]; memcpy(f, &d[o + 4], 32); const uint32_t n = f[1], rel = f[3], typ = f[5];
        if (typ == 2 && info < 0) info = int64_t(o + rel);
        if (typ == 3 && data < 0) { data = int64_t(o + rel); dn = n; }
        o += uint64_t(rel) + n;
    }
    if (info < 0 || data < 0 || uint64_t(info) + 112 > d.size() || uint64_t(data) + dn > d.size()) return false;
    uint32_t v[28]; memcpy(v, &d[size_t(info)], sizeof v);
    if (v[0] != dn || v[5] != 1 || v[6] != 1) return false;
    t = nvn_texdesc{1, int(v[7]), 0, 0, v[3], v[4], 1, v[8], 0, int(v[27]) + 1};
    *st = &d[size_t(data)]; *size = dn; return true;
}
static double psnr_mip0(const nvn_texdesc &t, const uint8_t *st, const XbgAcEntry &e, const uint8_t *astc) {
    const unsigned w = t.width, h = t.height, bx = (w + 3) / 4, by = (h + 3) / 4; int bw, bh; const int bpb = nvn_fmt_bpb(t.format, &bw, &bh);
    std::vector<uint8_t> blocks(size_t(bx) * by * bpb), ref(size_t(w) * h * 4), dec(size_t(w) * h * 4); uint8_t px[64];
    if (nvn_deswizzle(&t, 0, 0, st, blocks.data(), size_t(bx) * bpb)) return -1;
    for (unsigned y = 0; y < by; y++) for (unsigned x = 0; x < bx; x++) {
        if (xbg_decode_block(uint32_t(t.format), &blocks[(size_t(y) * bx + x) * bpb], size_t(bpb), px, sizeof px)) return -1;
        for (unsigned r = 0; r < 4 && y * 4 + r < h; r++) for (unsigned c = 0; c < 4 && x * 4 + c < w; c++) memcpy(&ref[(size_t(y * 4 + r) * w + x * 4 + c) * 4], &px[(r * 4 + c) * 4], 4);
    }
    astcenc_config cfg; astcenc_context *ctx = nullptr; const bool srgb = t.format >= 0x45;
    if (astcenc_config_init(srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR, e.bw, e.bh, 1, ASTCENC_PRE_FAST, ASTCENC_FLG_DECOMPRESS_ONLY, &cfg) != ASTCENC_SUCCESS ||
        astcenc_context_alloc(&cfg, 1, &ctx, nullptr) != ASTCENC_SUCCESS) return -1;
    void *slice = dec.data(); astcenc_image img{w, h, 1, ASTCENC_TYPE_U8, &slice}; const astcenc_swizzle sw{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    const size_t n0 = size_t((w + e.bw - 1) / e.bw) * ((h + e.bh - 1) / e.bh) * 16;
    const bool ok = astcenc_decompress_image(ctx, astc, n0, &img, &sw, 0) == ASTCENC_SUCCESS; astcenc_context_free(ctx); if (!ok) return -1;
    double se = 0; for (size_t i = 0; i < dec.size(); i++) { const double d = double(dec[i]) - ref[i]; se += d * d; }
    const double mse = se / double(dec.size()); return mse == 0 ? 99.0 : 10 * log10(255.0 * 255.0 / mse);
}

int main(int argc, char **argv) {
    CHECK(argc >= 3);
    const std::string dir = argv[1], part = dir + "/test.pack.part", journal = dir + "/.journal-test";
    mkdir(dir.c_str(), 0775); unlink(part.c_str());
    const int fd = open(part.c_str(), O_RDWR | O_CREAT, 0664); CHECK(fd >= 0);
    uint8_t z[64] = {}; CHECK(pwrite(fd, z, 64, 0) == 64);
    { FILE *j = fopen(journal.c_str(), "wb"); CHECK(j); fprintf(j, "D3AC-phone 1 lazy-dos2 1\n"); fclose(j); }
    CHECK(astclazy::start(fd, 64, journal, 2, 1ull << 30, 64ull << 20));
    std::vector<std::vector<uint8_t>> files(size_t(argc - 2)); std::vector<nvn_texdesc> desc(files.size()); std::vector<const uint8_t *> st(files.size()); std::vector<uint32_t> sz(files.size());
    unsigned submitted = 0;
    for (size_t i = 0; i < files.size(); i++) {
        if (!read_file(argv[i + 2], files[i]) || !parse(files[i], desc[i], &st[i], &sz[i])) { printf("skip %s (not a 2D DFvN)\n", argv[i + 2]); continue; }
        if (desc[i].format < 0x41 || desc[i].format > 0x48) { printf("skip %s (fmt %#x)\n", argv[i + 2], desc[i].format); continue; }
        XbgAlJob j; j.k = {uint32_t(i) * 2654435761u, uint32_t(i), sz[i], uint16_t(desc[i].format), uint16_t(desc[i].levels), uint16_t(desc[i].width), uint16_t(desc[i].height)};
        j.d = desc[i]; j.storage.assign(st[i], st[i] + sz[i]); j.serial = i;
        CHECK(astclazy::submit(std::move(j))); submitted++;
        printf("submitted %s fmt=%#x %ux%u levels=%u pkg_block_h=%d storage=%u\n", argv[i + 2], desc[i].format, desc[i].width, desc[i].height, desc[i].levels, desc[i].pkg_block_h, sz[i]);
    }
    std::vector<XbgAcEntry> got; const auto t0 = std::chrono::steady_clock::now();
    for (;;) { astclazy::take(got); const XbgAlStats s = astclazy::stats();
        if (s.encoded + s.stayed_bc + s.failed >= submitted) break;
        if (std::chrono::steady_clock::now() - t0 > std::chrono::minutes(5)) { printf("timeout\n"); return 1; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    astclazy::take(got);
    const XbgAlStats s = astclazy::stats();
    printf("encoded %llu stayed %llu failed %llu, %.2f Mpx in %.2f s worker time (%.2f Mpx/s per thread-second)\n", (unsigned long long)s.encoded, (unsigned long long)s.stayed_bc, (unsigned long long)s.failed,
        s.px_encoded / 1e6, s.encode_ns / 1e9, s.px_encoded / 1e6 / (s.encode_ns / 1e9));
    CHECK(s.failed == 0);
    // the journal as the engine reads it at the next start
    struct stat pst{}; CHECK(fstat(fd, &pst) == 0);
    std::vector<uint8_t> jt; CHECK(read_file(journal.c_str(), jt));
    std::vector<XbgAcEntry> parsed; uint64_t total = 0;
    CHECK(xbg_ac_journal_parse((const char *)jt.data(), jt.size(), uint64_t(pst.st_size), parsed, &total) == 0);
    CHECK(parsed.size() == got.size() && parsed.size() == s.encoded);
    for (const XbgAcEntry &e : parsed) {
        const size_t i = e.adler; CHECK(i < files.size() && e.bytes == sz[i]);
        std::vector<uint8_t> blk(e.data_bytes); CHECK(pread(fd, blk.data(), blk.size(), off_t(e.data_off)) == ssize_t(blk.size()));
        const double db = psnr_mip0(desc[i], st[i], e, blk.data());
        printf("  %s: ASTC %ux%u %u bytes (BC storage %u) mip0 %.2f dB (journal %.2f)\n", argv[i + 2], e.bw, e.bh, e.data_bytes, sz[i], db, e.psnr_x100 / 100.0);
        CHECK(fabs(db - e.psnr_x100 / 100.0) < 0.05 && db >= 42.0);
    }
    printf("OK\n"); return 0;
}
