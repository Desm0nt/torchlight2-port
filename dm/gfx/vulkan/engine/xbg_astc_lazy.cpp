// [DOS2] On-device lazy ASTC texture cache: background encoder (xbg_astc_lazy.h). The encoding rule, the void-extent fix and the
// journal line format are those of the D3 on-phone builder (d3-port-repo dm/android/astc_convert.cpp, KB TEX-15), so a pack made here
// and one made by a future pak-walking builder share one format and one engine reader.
#include <stdint.h>
#include <stdarg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif
#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include "astcenc.h"
#include "xbg_astc_lazy.h"
#include "xbg_texture.h"
extern "C" {
#include "../../tex/nvn_deswizzle.h"
void xbg_logf(const char *format, ...);
int nvn_fmt_bpb(int format, int *block_w, int *block_h);
}

namespace {

constexpr float TARGET_DB = 42.0f;
struct Coder {   // one per worker thread: astcenc contexts by (sRGB, block, preset)
    astcenc_context *ctx[2][3][3] = {};
    ~Coder() { for (auto &a : ctx) for (auto &b : a) for (auto *c : b) if (c) astcenc_context_free(c); }
    astcenc_context *get(bool srgb, unsigned b, int preset) {   // 0 -fast, 1 -medium, 2 -thorough
        astcenc_context *&c = ctx[srgb][b - 4][preset];
        static const float q[3] = {ASTCENC_PRE_FAST, ASTCENC_PRE_MEDIUM, ASTCENC_PRE_THOROUGH};
        if (!c) { astcenc_config cfg; if (astcenc_config_init(srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR, b, b, 1, q[preset], 0, &cfg) != ASTCENC_SUCCESS ||
                      astcenc_context_alloc(&cfg, 1, &c, nullptr) != ASTCENC_SUCCESS) c = nullptr; }
        return c;
    }
    bool encode(const uint8_t *rgba, unsigned w, unsigned h, bool srgb, unsigned b, int preset, std::vector<uint8_t> &out) {
        astcenc_context *c = get(srgb, b, preset); if (!c) return false;
        const size_t n = size_t((w + b - 1) / b) * ((h + b - 1) / b) * 16; out.resize(n);
        void *slice = (void *)rgba; astcenc_image img{w, h, 1, ASTCENC_TYPE_U8, &slice}; const astcenc_swizzle sw{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
        const bool ok = astcenc_compress_image(c, &img, &sw, out.data(), n, 0) == ASTCENC_SUCCESS; astcenc_compress_reset(c);
        return ok;
    }
    double psnr(const uint8_t *rgba, unsigned w, unsigned h, bool srgb, unsigned b, int preset, const std::vector<uint8_t> &blocks) {
        astcenc_context *c = get(srgb, b, preset); if (!c) return 0;
        std::vector<uint8_t> dec(size_t(w) * h * 4); void *slice = dec.data(); astcenc_image img{w, h, 1, ASTCENC_TYPE_U8, &slice};
        const astcenc_swizzle sw{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
        const bool ok = astcenc_decompress_image(c, blocks.data(), blocks.size(), &img, &sw, 0) == ASTCENC_SUCCESS; astcenc_decompress_reset(c);
        if (!ok) return 0;
        double se = 0; for (size_t i = 0; i < dec.size(); i++) { const double e = double(dec[i]) - rgba[i]; se += e * e; }
        const double mse = se / double(dec.size()); return mse == 0 ? 99.0 : 10 * log10(255.0 * 255.0 / mse);
    }
};
unsigned void_fix(std::vector<uint8_t> &b) {   // KB TEX-04: an all-zero ASTC block is the error block
    static const uint8_t vb[16] = {0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0};
    unsigned n = 0; for (size_t o = 0; o + 16 <= b.size(); o += 16) { uint64_t x, y; memcpy(&x, &b[o], 8); memcpy(&y, &b[o + 8], 8); if (!(x | y)) { memcpy(&b[o], vb, 16); ++n; } }
    return n;
}
// Level l of the storage copy -> tight RGBA8 (the engine's deswizzle and CPU BC decoder, what BcUpload's CPU path uploads).
bool level_rgba(const XbgAlJob &j, unsigned l, std::vector<uint8_t> &rgba, unsigned *ow, unsigned *oh) {
    const unsigned w = std::max(1u, j.d.width >> l), h = std::max(1u, j.d.height >> l), bx = (w + 3) / 4, by = (h + 3) / 4;
    int bw, bh; const int bpb = nvn_fmt_bpb(j.d.format, &bw, &bh); if (bpb != 8 && bpb != 16) return false;
    std::vector<uint8_t> blocks(size_t(bx) * by * bpb);
    if (nvn_deswizzle(&j.d, l, 0, j.storage.data(), blocks.data(), size_t(bx) * bpb)) return false;
    rgba.resize(size_t(w) * h * 4); uint8_t px[64];
    for (unsigned y = 0; y < by; y++) for (unsigned x = 0; x < bx; x++) {
        if (xbg_decode_block(uint32_t(j.d.format), &blocks[(size_t(y) * bx + x) * bpb], size_t(bpb), px, sizeof px)) return false;
        for (unsigned r = 0; r < 4 && y * 4 + r < h; r++) for (unsigned c = 0; c < 4 && x * 4 + c < w; c++) memcpy(&rgba[(size_t(y * 4 + r) * w + x * 4 + c) * 4], &px[(r * 4 + c) * 4], 4);
    }
    *ow = w; *oh = h; return true;
}
struct Result { unsigned b, preset, steps; double db; std::vector<uint8_t> data; unsigned zeros; uint64_t px; };   // b 0: stays BC
bool convert(Coder &cd, const XbgAlJob &j, Result &r) {
    const uint32_t f = uint32_t(j.d.format); if (f < 0x41 || f > 0x48) return false;
    const bool srgb = f >= 0x45, bc1 = f == 0x41 || f == 0x42 || f == 0x45 || f == 0x46;
    const unsigned levels = std::max(1u, j.d.levels);
    std::vector<std::vector<uint8_t>> img(levels); std::vector<unsigned> W(levels), H(levels);
    for (unsigned l = 0; l < levels; l++) if (!level_rgba(j, l, img[l], &W[l], &H[l])) return false;
    static const unsigned steps_bc1[5][2] = {{6, 0}, {5, 0}, {4, 0}, {4, 1}, {4, 2}}, steps_bc23[3][2] = {{4, 0}, {4, 1}, {4, 2}};
    const unsigned (*steps)[2] = bc1 ? steps_bc1 : steps_bc23; const unsigned ns = bc1 ? 5 : 3;
    unsigned best_b = 0, best_p = 0, tried = 0; double best = -1; std::vector<uint8_t> enc;
    for (unsigned i = 0; i < ns && best_b == 0; i++) {
        const unsigned b = steps[i][0], pr = steps[i][1]; tried++;
        if (!cd.encode(img[0].data(), W[0], H[0], srgb, b, int(pr), enc)) return false;
        void_fix(enc);
        const double db = cd.psnr(img[0].data(), W[0], H[0], srgb, b, int(pr), enc);
        best = std::max(best, db);
        if (db >= TARGET_DB) { best_b = b; best_p = pr; best = db; }
    }
    r.b = best_b; r.preset = best_p; r.steps = tried; r.db = best; r.data.clear(); r.zeros = 0; r.px = 0;
    for (unsigned l = 0; best_b && l < levels; l++) {
        if (!cd.encode(img[l].data(), W[l], H[l], srgb, best_b, int(best_p), enc)) return false;
        r.zeros += void_fix(enc); r.data.insert(r.data.end(), enc.begin(), enc.end()); r.px += uint64_t(W[l]) * H[l];
    }
    return true;
}

struct State {
    int fd = -1; uint64_t end = 64, cap = 0, queue_cap = 0; std::string journal; FILE *jf = nullptr; int threads = 0;
    std::mutex mu; std::condition_variable cv; std::deque<XbgAlJob> q; std::atomic<uint64_t> q_bytes{0};
    std::mutex wmu;   // part file end + journal
    std::mutex omu; std::vector<XbgAcEntry> out; std::atomic<uint32_t> out_n{0};
    std::atomic<bool> on{false};
    std::atomic<uint64_t> queued{0}, dropped_queue{0}, dropped_cap{0}, encoded{0}, stayed{0}, failed{0}, retried{0}, zeros{0}, bytes{0}, px{0}, ns{0};
    std::atomic<uint64_t> pack_end{64};
    bool cap_told = false;
};
State &g = *new State;   // never destroyed: detached workers wait on its condition variable until the process ends (a static one hangs exit in pthread_cond_destroy)

void note_progress() {
    const uint64_t n = g.encoded + g.stayed + g.failed;
    if (n < 16 || (n & (n - 1))) return;
    xbg_logf("NVN astc lazy: %llu textures done (cached %llu, stay BC %llu, failed %llu, needed a later step %llu), %.1f Mpx in %.1f s of worker time, pack %.1f MB, queue %.1f MB, dropped %llu (queue) %llu (cap), zero blocks fixed %llu",
        (unsigned long long)n, (unsigned long long)g.encoded.load(), (unsigned long long)g.stayed.load(), (unsigned long long)g.failed.load(), (unsigned long long)g.retried.load(),
        g.px / 1e6, g.ns / 1e9, g.pack_end / 1048576.0, g.q_bytes / 1048576.0, (unsigned long long)g.dropped_queue.load(), (unsigned long long)g.dropped_cap.load(), (unsigned long long)g.zeros.load());
}

void worker() {
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
#if defined(__linux__)
    // [issue #4 review] Started from the translation thread (the first BC texture): a name of its own, so perf_android.c keeps it
    // out of the ADPF session (its CPU is not frame work), and not the translator's inherited one / two core pin (cpu7).
    pthread_setname_np(pthread_self(), "dm-astc");
    {
        cpu_set_t s; CPU_ZERO(&s);
        const long n = sysconf(_SC_NPROCESSORS_CONF);
        if (!sched_getaffinity(0, sizeof s, &s) && CPU_COUNT(&s) >= 1 && CPU_COUNT(&s) <= 2 && n > CPU_COUNT(&s) + 1) {
            cpu_set_t w; CPU_ZERO(&w);
            for (long c = 0; c < n && c < CPU_SETSIZE; c++) if (!CPU_ISSET(c, &s)) CPU_SET(c, &w);
            sched_setaffinity(0, sizeof w, &w);
        }
    }
#endif
    Coder cd; Result r;
    for (;;) {
        XbgAlJob j;
        { std::unique_lock<std::mutex> l(g.mu); g.cv.wait(l, [] { return !g.q.empty(); }); j = std::move(g.q.front()); g.q.pop_front(); g.q_bytes -= j.storage.size(); }
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = convert(cd, j, r);
        g.ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
        if (!ok) {
            ++g.failed; static std::atomic<unsigned> told{0};
            if (told++ < 8) xbg_logf("NVN astc lazy: serial=%llu fmt=%#x %ux%u levels=%u could not be decoded / encoded: stays on the BC decoder",
                                     (unsigned long long)j.serial, j.d.format, j.d.width, j.d.height, j.d.levels);
            note_progress(); continue;
        }
        XbgAcEntry e{}; e.crc = j.k.crc; e.adler = j.k.adler; e.bytes = j.k.bytes; e.format = j.k.format; e.levels = j.k.levels; e.width = j.k.width; e.height = j.k.height;
        e.bw = e.bh = uint8_t(r.b); e.flags = uint16_t(r.preset); e.data_bytes = uint32_t(r.data.size()); e.psnr_x100 = uint32_t(lround(std::max(0.0, r.db) * 100));
        {
            std::lock_guard<std::mutex> l(g.wmu);
            if (r.b) {
                if (g.end + r.data.size() > g.cap) { ++g.dropped_cap; if (!g.cap_told) { g.cap_told = true; xbg_logf("NVN astc lazy: the pack reached its cap (%.0f MB, DM_TEX_ASTC_CACHE_MB): no more textures are added", g.cap / 1048576.0); } continue; }
                e.data_off = g.end;
                if (pwrite(g.fd, r.data.data(), r.data.size(), off_t(g.end)) != ssize_t(r.data.size())) { ++g.failed; xbg_logf("NVN astc lazy: write failed (errno %d)", errno); continue; }
                fdatasync(g.fd); g.end += r.data.size(); g.pack_end = g.end;
            }
            // the journal line commits the texture (a torn last line is not committed: xbg_ac_journal_parse)
            fprintf(g.jf, "%08x %08x %u %x %u %u %u %u %u %llu %u %u %u\n", e.crc, e.adler, e.bytes, unsigned(e.format), unsigned(e.width), unsigned(e.height), unsigned(e.levels),
                    unsigned(r.b), e.psnr_x100, (unsigned long long)(r.b ? e.data_off : 0), r.b ? e.data_bytes : 0u, r.preset, r.steps);
            fflush(g.jf); fdatasync(fileno(g.jf));
        }
        if (r.b) { ++g.encoded; g.bytes += r.data.size(); g.zeros += r.zeros; g.px += r.px; std::lock_guard<std::mutex> l(g.omu); g.out.push_back(e); g.out_n = uint32_t(g.out.size()); }
        else ++g.stayed;
        if (r.steps > 1) ++g.retried;
        { static std::atomic<unsigned> told{0}; if (told++ < 8) xbg_logf("NVN astc lazy: serial=%llu fmt=%#x %ux%u levels=%u -> %s (mip 0 %.1f dB, %u step%s, %u bytes)", (unsigned long long)j.serial, j.d.format, j.d.width, j.d.height, j.d.levels,
                r.b == 6 ? "ASTC 6x6" : r.b == 5 ? "ASTC 5x5" : r.b == 4 ? "ASTC 4x4" : "stays BC", r.db, r.steps, r.steps > 1 ? "s" : "", unsigned(r.data.size())); }
        note_progress();
    }
}

}  // namespace

namespace astclazy {
bool start(int fd, uint64_t end, const std::string &journal, int threads, uint64_t cap, uint64_t queue_cap) {
    if (g.on) return true;
    g.fd = fd; g.end = end; g.pack_end = end; g.cap = cap; g.queue_cap = queue_cap; g.journal = journal; g.threads = std::max(1, std::min(threads, 8));
    g.jf = fopen(journal.c_str(), "ab"); if (!g.jf) { xbg_logf("NVN astc lazy: cannot append to %s (errno %d): no encoder", journal.c_str(), errno); return false; }
    for (int i = 0; i < g.threads; i++) std::thread(worker).detach();   // the workers live as long as the process
    g.on = true; return true;
}
bool running() { return g.on; }
bool submit(XbgAlJob &&j) {
    if (!g.on) return false;
    if (g.pack_end >= g.cap) { ++g.dropped_cap; return false; }
    { std::lock_guard<std::mutex> l(g.mu);
      if (g.q_bytes + j.storage.size() > g.queue_cap) { ++g.dropped_queue; return false; }
      g.q_bytes += j.storage.size(); g.q.push_back(std::move(j)); }
    ++g.queued; g.cv.notify_one(); return true;
}
size_t take(std::vector<XbgAcEntry> &out) {
    if (!g.out_n.load(std::memory_order_relaxed)) return 0;
    std::lock_guard<std::mutex> l(g.omu); const size_t n = g.out.size(); out.insert(out.end(), g.out.begin(), g.out.end()); g.out.clear(); g.out_n = 0; return n;
}
XbgAlStats stats() {
    XbgAlStats s{g.queued, g.dropped_queue, g.dropped_cap, g.encoded, g.stayed, g.failed, g.retried, g.zeros, g.bytes, g.px, g.ns, 0, g.pack_end};
    { std::lock_guard<std::mutex> l(g.mu); s.queue_bytes = g.q_bytes; }
    return s;
}
}
