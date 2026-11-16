// [TL2] On-phone ASTC texture cache builder (docs/tl2/ASTC_CACHE.md; the D3 port's dm/android/astc_convert.cpp, issue #20 there, with
// the TL2 pack reader): the same cache as the PC setup step (tools/tl2_astc_cache.py, dm/gfx/vulkan/engine/xbg_astc_cache.h), made on
// the phone from the game folder, so either path fills Download/Torchlight2Mobile/astc.
//
//   Sources: <data>/romfs/PAKS/DATA*.PAK + .MAN (tl2_pak.h: MAN index, zlib members, XTX textures). No Python on the phone.
//   Per texture (unique by its storage key): the XTX storage block is the key (the game's loader memcpy's exactly it into its pool,
//     xbg_astc_cache_tl2.inc); every level is deswizzled with the engine's nvn_deswizzle (described as the loader describes it to NVN:
//     automatic block height, which equals the XTX's for all 5863 BC textures, tests/tl2_astc_key_test.cpp), decoded with the engine's
//     CPU BC decoder (xbg_decode_block), encoded with ARM astc-encoder 5.7.0 (dm/third_party/astcenc, Apache-2.0, NEON) with the tool's
//     rule (owner 13.11): every texture keeps mip 0 at >= 42 dB against its decoded BC; steps tried in order, the first that reaches it
//     is kept: BC1 6x6 -fast, 5x5 -fast, 4x4 -fast, 4x4 -medium, 4x4 -thorough; BC2 / BC3 4x4 -fast, -medium, -thorough; none reaches
//     it -> left out (the engine decodes its BC, or the lazy cache tries again). All-zero blocks -> void-extent transparent black (TEX-04).
//   Runs only up front (the start dialog or the pause-menu row with the game held, Tl2Textures.java): never alongside the game.
//   Output: <data>/astc/tl2-astc.pack ('D3AC' v1) + manifest.json, the PC tool's format and keys. While it runs, the blocks are
//     appended to tl2-astc.pack.part and a texture counts as done only when its journal line (.journal-phone) is written after the data
//     was synced: an interrupted run truncates the part file to the last journaled byte and skips the journaled textures. At the end the
//     index and header are written, the part file is renamed, manifest.json is written to a temporary file and renamed last.
//   Threads: all big cores (cpufreq max above the smallest cluster's), lowered at run time (thermal) through astc_conv_limit; workers
//     run at nice 10.
// The core is plain C++ (a host build tests it: dm/android/tests/astc_convert_host.cpp); the JNI entry points are in jni_android.c.
#include <stdint.h>
#include <stdarg.h>
#include <math.h>
#include <ctype.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <zlib.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#include "astcenc.h"
#include "astc_convert.h"
extern "C" {
#include "nvn_deswizzle.h"
#include "xbg_texture.h"
}
#include "tl2_pak.h"

namespace {

// ---------------------------------------------------------------- log
void (*g_log)(const char *) = nullptr;
void logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void logf(const char *fmt, ...) {
    char b[1024]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    if (g_log) g_log(b); else fprintf(stderr, "%s\n", b);
}


// ---------------------------------------------------------------- XTX (tl2_pak.h; tools/tl2_astc_cache.py parse_xtx)
struct Tex { uint32_t fmt, w, h, levels, th; const uint8_t *st; uint32_t size; };
bool parse_tex(const std::vector<uint8_t> &d, Tex &t) {
    tl2pak::Xtx x; if (!tl2pak::parse_xtx(d.data(), d.size(), x)) return false;
    if (x.target != 1 || x.depth != 1 || x.size != x.st_bytes || x.st_bytes > UINT32_MAX) return false;   // 2D only; the loader's size check
    t.fmt = x.fmt; t.w = x.w; t.h = x.h; t.levels = x.levels; t.th = x.th; t.st = x.st; t.size = uint32_t(x.st_bytes);
    return true;
}
bool bc_format(uint32_t f, bool *srgb, bool *bc1) { if (f < 0x41 || f > 0x48) return false; *srgb = f >= 0x45; *bc1 = f == 0x41 || f == 0x42 || f == 0x45 || f == 0x46; return true; }
struct Key { uint32_t crc, adler, bytes, fmt, w, h, levels;
    bool operator<(const Key &o) const { return std::tie(crc, adler, bytes, fmt, w, h, levels) < std::tie(o.crc, o.adler, o.bytes, o.fmt, o.w, o.h, o.levels); }
    bool operator==(const Key &o) const { return !(*this < o) && !(o < *this); } };
Key key_of(const Tex &t) {
    return {uint32_t(crc32(0, t.st, t.size)), uint32_t(adler32(1, t.st, t.size)), t.size, t.fmt, t.w, t.h, t.levels};
}

// ---------------------------------------------------------------- encoding (tools/d3_astc_cache.py convert)
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
    bool encode(const uint8_t *rgba, unsigned w, unsigned h, bool srgb, unsigned b, int thorough, std::vector<uint8_t> &out) {
        astcenc_context *c = get(srgb, b, thorough); if (!c) return false;
        const size_t n = size_t((w + b - 1) / b) * ((h + b - 1) / b) * 16; out.resize(n);
        void *slice = (void *)rgba; astcenc_image img{w, h, 1, ASTCENC_TYPE_U8, &slice}; const astcenc_swizzle sw{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
        const bool ok = astcenc_compress_image(c, &img, &sw, out.data(), n, 0) == ASTCENC_SUCCESS; astcenc_compress_reset(c);
        return ok;
    }
    double psnr(const uint8_t *rgba, unsigned w, unsigned h, bool srgb, unsigned b, int thorough, const std::vector<uint8_t> &blocks) {
        astcenc_context *c = get(srgb, b, thorough); if (!c) return 0;
        std::vector<uint8_t> dec(size_t(w) * h * 4); void *slice = dec.data(); astcenc_image img{w, h, 1, ASTCENC_TYPE_U8, &slice};
        const astcenc_swizzle sw{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
        const bool ok = astcenc_decompress_image(c, blocks.data(), blocks.size(), &img, &sw, 0) == ASTCENC_SUCCESS; astcenc_decompress_reset(c);
        if (!ok) return 0;
        double se = 0; for (size_t i = 0; i < dec.size(); i++) { const double e = double(dec[i]) - rgba[i]; se += e * e; }
        const double mse = se / double(dec.size()); return mse == 0 ? 99.0 : 10 * log10(255.0 * 255.0 / mse);
    }
};
unsigned void_fix(std::vector<uint8_t> &b) {
    static const uint8_t vb[16] = {0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0};
    unsigned n = 0; for (size_t o = 0; o + 16 <= b.size(); o += 16) { uint64_t x, y; memcpy(&x, &b[o], 8); memcpy(&y, &b[o + 8], 8); if (!(x | y)) { memcpy(&b[o], vb, 16); ++n; } }
    return n;
}
// Level l of the texture -> tight RGBA8 w x h (the engine's deswizzle and CPU BC decoder, as the tool's numpy port of them).
bool level_rgba(const Tex &t, unsigned l, std::vector<uint8_t> &rgba, unsigned *ow, unsigned *oh) {
    const unsigned w = std::max(1u, t.w >> l), h = std::max(1u, t.h >> l), bx = (w + 3) / 4, by = (h + 3) / 4;
    int bw, bh; const int bpb = nvn_fmt_bpb(int(t.fmt), &bw, &bh); if (bpb != 8 && bpb != 16) return false;
    nvn_texdesc desc{1, int(t.fmt), 0, 0, t.w, t.h, 1, t.levels, 0, 0};   // as the game's loader describes it: automatic block height
    std::vector<uint8_t> blocks(size_t(bx) * by * bpb);
    if (nvn_deswizzle(&desc, l, 0, t.st, blocks.data(), size_t(bx) * bpb)) return false;
    rgba.resize(size_t(w) * h * 4); uint8_t px[64];
    for (unsigned y = 0; y < by; y++) for (unsigned x = 0; x < bx; x++) {
        if (xbg_decode_block(t.fmt, &blocks[(size_t(y) * bx + x) * bpb], size_t(bpb), px, sizeof px)) return false;
        for (unsigned r = 0; r < 4 && y * 4 + r < h; r++) for (unsigned c = 0; c < 4 && x * 4 + c < w; c++) memcpy(&rgba[(size_t(y * 4 + r) * w + x * 4 + c) * 4], &px[(r * 4 + c) * 4], 4);
    }
    *ow = w; *oh = h; return true;
}
struct Result { Key k; unsigned b, preset, steps; double db; std::vector<uint8_t> data; unsigned zeros; uint64_t px; };   // b 0: stays BC
bool convert(Coder &cd, const Tex &t, Result &r) {
    bool srgb, bc1; if (!bc_format(t.fmt, &srgb, &bc1)) return false;
    std::vector<std::vector<uint8_t>> img(t.levels); std::vector<unsigned> W(t.levels), H(t.levels);
    for (unsigned l = 0; l < t.levels; l++) if (!level_rgba(t, l, img[l], &W[l], &H[l])) return false;
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
    r.k = key_of(t); r.b = best_b; r.preset = best_p; r.steps = tried; r.db = best; r.data.clear(); r.zeros = 0; r.px = 0;
    for (unsigned l = 0; best_b && l < t.levels; l++) {
        if (!cd.encode(img[l].data(), W[l], H[l], srgb, best_b, int(best_p), enc)) return false;
        r.zeros += void_fix(enc); r.data.insert(r.data.end(), enc.begin(), enc.end()); r.px += uint64_t(W[l]) * H[l];
    }
    return true;
}

// ---------------------------------------------------------------- the run
struct Item { const tl2pak::Pack *pak; uint32_t off; Key k; uint64_t px; };
struct Run {
    std::string data_dir, out_dir;
    std::vector<std::unique_ptr<tl2pak::Pack>> paks;
    std::atomic<int> state{ASTC_CONV_IDLE}; std::atomic<bool> cancel{false}; std::atomic<int> limit{1}; int threads = 1;
    std::atomic<uint64_t> stayed{0}, retried{0}, plan_done{0}, plan_total{0}, done{0}, total{0}, done_px{0}, total_px{0}, bytes{0}, zeros{0}, failed{0};
    std::chrono::steady_clock::time_point t0; std::atomic<int64_t> elapsed_ms{0};
    std::string error;
};
Run *g_run = nullptr; std::mutex g_mu;

// <romfs>/PAKS/*.PAK with a .MAN, in the game's order: DATA, DATA1 ... DATA5 (shorter name first, then by name; tools/tl2_astc_cache.py packs)
std::vector<std::string> list_paks(const std::string &dir) {
    std::vector<std::string> v; DIR *d = opendir(dir.c_str()); if (!d) return v;
    while (dirent *e = readdir(d)) { std::string n = e->d_name; if (n.size() > 4 && strcasecmp(n.c_str() + n.size() - 4, ".pak") == 0 && !access((dir + "/" + n + ".MAN").c_str(), R_OK)) v.push_back(n); }
    closedir(d);
    std::sort(v.begin(), v.end(), [](const std::string &a, const std::string &b) { return a.size() != b.size() ? a.size() < b.size() : strcasecmp(a.c_str(), b.c_str()) < 0; });
    for (auto &n : v) n = dir + "/" + n; return v;
}

void write_file_atomic(const std::string &p, const std::string &text) {
    const std::string tmp = p + ".tmp"; FILE *f = fopen(tmp.c_str(), "wb"); if (!f) return;
    fwrite(text.data(), 1, text.size(), f); fflush(f); fsync(fileno(f)); fclose(f); rename(tmp.c_str(), p.c_str());
}

void run_main(Run *R) {
    R->t0 = std::chrono::steady_clock::now();
    auto ms = [&] { return int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - R->t0).count()); };
    auto fail = [&](const std::string &e) { R->error = e; logf("[astc] failed: %s", e.c_str()); R->state = ASTC_CONV_FAILED; };
    R->state = ASTC_CONV_PLANNING;
    // 1. packs: every texture member of every data pack (a texture in several packs is one job, by its key)
    const std::string paks_dir = R->data_dir + "/romfs/PAKS";
    std::vector<std::string> names = list_paks(paks_dir);
    if (names.empty()) return fail("no PAKS/*.PAK (+ .MAN) in " + R->data_dir + "/romfs");
    std::vector<std::pair<const tl2pak::Pack *, const tl2pak::Entry *>> list; std::string fingerprint;
    for (auto &p : names) { auto k = std::make_unique<tl2pak::Pack>(); struct stat st{}; stat(p.c_str(), &st);
        fingerprint += p.substr(R->data_dir.size()) + ":" + std::to_string(uint64_t(st.st_size)) + ";";
        if (!k->open(p)) { logf("[astc] %s: cannot read (or its .MAN): skipped", p.c_str()); continue; }
        R->paks.push_back(std::move(k)); }
    for (auto &k : R->paks) for (auto &e : k->entries) if (e.size && tl2pak::is_texture_path(e.path)) list.push_back({k.get(), &e});
    logf("[astc] %zu packs, %zu textures to read", R->paks.size(), list.size());
    // 2. keys (parallel): read, parse, hash every texture; unique BC ones are the jobs
    R->plan_total = list.size();
    std::vector<Item> items(list.size()); std::vector<uint8_t> ok(list.size(), 0); std::atomic<size_t> next{0};
    auto plan_worker = [&] {
        setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10); std::vector<uint8_t> buf;
        for (size_t i; !R->cancel && (i = next++) < list.size(); R->plan_done++) {
            const auto *k = list[i].first; const auto *e = list[i].second; Tex t; bool s, b1;
            if (!k->read(e->off, buf) || !parse_tex(buf, t) || !bc_format(t.fmt, &s, &b1)) continue;
            uint64_t px = 0; for (unsigned l = 0; l < t.levels; l++) px += uint64_t(std::max(1u, t.w >> l)) * std::max(1u, t.h >> l);
            items[i] = {k, e->off, key_of(t), px}; ok[i] = 1;
        }
    };
    { std::vector<std::thread> ts; for (int i = 0; i < R->threads; i++) ts.emplace_back(plan_worker); for (auto &t : ts) t.join(); }
    if (R->cancel) { R->state = ASTC_CONV_CANCELLED; return; }
    std::vector<Item> jobs; { std::map<Key, size_t> seen; for (size_t i = 0; i < items.size(); i++) if (ok[i] && !seen.count(items[i].k)) { seen[items[i].k] = i; } for (auto &kv : seen) jobs.push_back(items[kv.second]); }
    // 3. resume: the journal of an earlier run on the same sources
    mkdir(R->out_dir.c_str(), 0775);
    const std::string part = R->out_dir + "/tl2-astc.pack.part", jpath = R->out_dir + "/.journal-phone", sig = "D3AC-phone 1 " + std::to_string(crc32(0, (const Bytef *)fingerprint.data(), uInt(fingerprint.size())));
    struct Done { Key k; unsigned b; uint32_t db100; uint64_t off; uint32_t len; unsigned preset, steps; };   // b 0: stays BC
    std::vector<Done> finished; uint64_t end = 64;
    if (FILE *j = fopen(jpath.c_str(), "rb")) {
        char ln[256]; if (fgets(ln, sizeof ln, j) && !strncmp(ln, sig.c_str(), sig.size())) {
            while (fgets(ln, sizeof ln, j)) { Done d; unsigned long long off; unsigned len, b, db, pr, st;
                if (!strchr(ln, '\n')) break;   // a torn last line: not committed
                if (sscanf(ln, "%x %x %u %x %u %u %u %u %u %llu %u %u %u", &d.k.crc, &d.k.adler, &d.k.bytes, &d.k.fmt, &d.k.w, &d.k.h, &d.k.levels, &b, &db, &off, &len, &pr, &st) == 13) {
                    d.b = b; d.db100 = db; d.off = off; d.len = len; d.preset = pr; d.steps = st; finished.push_back(d); if (b) end = std::max<uint64_t>(end, off + len); } } }
        fclose(j);
    }
    // A fresh run makes a new part file (a new inode) instead of truncating the old one: a game that opened the old one as a partial
    // cache (xbg_game.cpp astccache, round 4) keeps reading its committed bytes. A resumed run only truncates to the last committed byte.
    if (finished.empty()) { unlink(part.c_str()); unlink(jpath.c_str()); }
    int pf = ::open(part.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0664); if (pf < 0) return fail("cannot write " + part + ": " + strerror(errno));
    if (finished.empty()) { end = 64; uint8_t z[64] = {}; if (pwrite(pf, z, 64, 0) != 64) {} write_file_atomic(jpath, sig + "\n"); }
    else if (ftruncate(pf, off_t(end))) {}
    FILE *jf = fopen(jpath.c_str(), "ab"); if (!jf) { close(pf); return fail("cannot write " + jpath); }
    // "#total <n>": the unique BC textures of this run, for the engine's partial-cache line ("N of M textures committed")
    fprintf(jf, "#total %llu\n", (unsigned long long)jobs.size()); fflush(jf); fdatasync(fileno(jf));
    std::set<Key> have; for (auto &d : finished) have.insert(d.k);
    std::vector<Item> todo; for (auto &j : jobs) if (!have.count(j.k)) todo.push_back(j);
    if (const char *lim = getenv("DM_ASTC_CONV_LIMIT")) if (atol(lim) > 0 && todo.size() > size_t(atol(lim))) {   // tests: a partial cache
        todo.resize(size_t(atol(lim))); std::set<Key> keep; for (auto &j : todo) keep.insert(j.k); for (auto &d : finished) keep.insert(d.k);
        std::vector<Item> j2; for (auto &j : jobs) if (keep.count(j.k)) j2.push_back(j); jobs.swap(j2); }
    R->total = jobs.size(); R->done = jobs.size() - todo.size();
    { uint64_t tp = 0, dp = 0; for (auto &j : jobs) { tp += j.px; if (have.count(j.k)) dp += j.px; } R->total_px = tp; R->done_px = dp; }
    R->bytes = end - 64;
    logf("[astc] %llu unique BC textures, %zu already done (resumed), %zu to encode with %d threads (keys in %.1f s)",
        (unsigned long long)jobs.size(), finished.size(), todo.size(), R->threads, ms() / 1000.0);
    // 4. encode (parallel), append + journal (one writer, a sync every second)
    R->state = ASTC_CONV_ENCODING;
    std::sort(todo.begin(), todo.end(), [](const Item &a, const Item &b) { return a.px > b.px; });   // largest first
    std::mutex wmu; std::vector<Done> pending; uint64_t wend = end; auto last_sync = std::chrono::steady_clock::now();
    auto flush = [&](bool force) {   // caller holds wmu
        if (pending.empty() || (!force && std::chrono::steady_clock::now() - last_sync < std::chrono::seconds(1))) return;
        fdatasync(pf);
        for (auto &d : pending) fprintf(jf, "%08x %08x %u %x %u %u %u %u %u %llu %u %u %u\n", d.k.crc, d.k.adler, d.k.bytes, d.k.fmt, d.k.w, d.k.h, d.k.levels, d.b, d.db100, (unsigned long long)d.off, d.len, d.preset, d.steps);
        fflush(jf); fdatasync(fileno(jf)); finished.insert(finished.end(), pending.begin(), pending.end()); pending.clear(); last_sync = std::chrono::steady_clock::now();
    };
    next = 0;
    auto enc_worker = [&](int index) {
        setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10); Coder cd; std::vector<uint8_t> buf; Result r;
        for (;;) {
            while (index >= R->limit && !R->cancel) std::this_thread::sleep_for(std::chrono::milliseconds(250));   // thermal / game: fewer threads
            size_t i; if (R->cancel || (i = next++) >= todo.size()) break;
            const Item &it = todo[i]; Tex t; const bool got = it.pak->read(it.off, buf);
            if (!got || !parse_tex(buf, t) || !convert(cd, t, r) || !(r.k == it.k)) { R->failed++; R->done++; R->done_px += it.px; continue; }
            std::lock_guard<std::mutex> l(wmu);
            const uint64_t at = wend; if (pwrite(pf, r.data.data(), r.data.size(), off_t(at)) != ssize_t(r.data.size())) { R->failed++; R->done++; continue; }
            wend += r.data.size(); R->bytes += r.data.size(); R->zeros += r.zeros;
            pending.push_back({r.k, r.b, uint32_t(lround(std::max(0.0, r.db) * 100)), r.b ? at : 0, uint32_t(r.data.size()), r.preset, r.steps}); flush(false);
            if (!r.b) R->stayed++; if (r.steps > 1) R->retried++;
            R->done++; R->done_px += it.px; R->elapsed_ms = ms();
        }
    };
    { std::vector<std::thread> ts; for (int i = 0; i < R->threads; i++) ts.emplace_back(enc_worker, i); for (auto &t : ts) t.join(); }
    { std::lock_guard<std::mutex> l(wmu); flush(true); }
    fclose(jf);
    if (R->cancel) { close(pf); R->state = ASTC_CONV_CANCELLED; logf("[astc] cancelled after %llu of %llu textures (kept: the next run resumes)", (unsigned long long)R->done.load(), (unsigned long long)R->total.load()); return; }
    // 5. index + header, rename, manifest last
    R->state = ASTC_CONV_FINISHING;
    std::sort(finished.begin(), finished.end(), [](const Done &a, const Done &b) { return a.k < b.k; });
    finished.erase(std::unique(finished.begin(), finished.end(), [](const Done &a, const Done &b) { return a.k == b.k; }), finished.end());
    uint64_t stayed = 0, retried = 0; for (auto &d : finished) { stayed += !d.b; retried += d.steps > 1; }
    finished.erase(std::remove_if(finished.begin(), finished.end(), [](const Done &d) { return !d.b; }), finished.end());   // stays BC: not in the index
    std::vector<uint8_t> index(finished.size() * 48, 0);
    for (size_t i = 0; i < finished.size(); i++) { const Done &d = finished[i]; uint8_t *e = &index[i * 48];
        const uint16_t fmt = uint16_t(d.k.fmt), lv = uint16_t(d.k.levels), w = uint16_t(d.k.w), h = uint16_t(d.k.h), fl = uint16_t(d.preset); const uint8_t bw = uint8_t(d.b);
        memcpy(e, &d.k.crc, 4); memcpy(e + 4, &d.k.adler, 4); memcpy(e + 8, &d.k.bytes, 4); memcpy(e + 12, &fmt, 2); memcpy(e + 14, &lv, 2); memcpy(e + 16, &w, 2); memcpy(e + 18, &h, 2);
        e[20] = bw; e[21] = bw; memcpy(e + 22, &fl, 2); memcpy(e + 24, &d.off, 8); memcpy(e + 32, &d.len, 4); memcpy(e + 36, &d.db100, 4); }
    if (pwrite(pf, index.data(), index.size(), off_t(wend)) != ssize_t(index.size())) { close(pf); return fail("index write failed"); }
    uint8_t hdr[64] = {}; const uint32_t ver = 1, cnt = uint32_t(finished.size()), flags = 0; const uint64_t io = wend, doff = 64, db = wend - 64;
    memcpy(hdr, "D3AC", 4); memcpy(hdr + 4, &ver, 4); memcpy(hdr + 8, &cnt, 4); memcpy(hdr + 12, &flags, 4); memcpy(hdr + 16, &io, 8); memcpy(hdr + 24, &doff, 8); memcpy(hdr + 32, &db, 8);
    if (pwrite(pf, hdr, 64, 0) != 64) { close(pf); return fail("header write failed"); }
    if (ftruncate(pf, off_t(wend + index.size()))) {}
    fsync(pf); close(pf);
    const std::string pack = R->out_dir + "/tl2-astc.pack"; unlink(pack.c_str());
    if (rename(part.c_str(), pack.c_str())) return fail("rename failed: " + std::string(strerror(errno)));
    unlink(jpath.c_str());
    char man[1024]; snprintf(man, sizeof man,
        "{\n \"format\": \"tl2-astc-cache\",\n \"version\": 1,\n \"made_by\": \"phone (dm/android/astc_convert.cpp)\",\n \"encoders\": {\"astc-encoder\": \"5.7.0\"},\n"
        " \"settings\": {\"formats\": [\"bc1\", \"bc2\", \"bc3\"], \"target_db\": 42.0, \"steps\": {\"bc1\": [\"6x6 fast\", \"5x5 fast\", \"4x4 fast\", \"4x4 medium\", \"4x4 thorough\"], \"bc2\": [\"4x4 fast\", \"4x4 medium\", \"4x4 thorough\"], \"bc3\": [\"4x4 fast\", \"4x4 medium\", \"4x4 thorough\"]}},\n"
        " \"complete\": true,\n \"entries\": %u,\n \"pack_bytes\": %llu,\n \"zero_blocks_fixed\": %llu,\n \"stayed_bc\": %llu,\n \"retried\": %llu,\n \"failed\": %llu,\n \"threads\": %d,\n \"seconds\": %lld\n}\n",
        cnt, (unsigned long long)(wend + index.size()), (unsigned long long)R->zeros.load(), (unsigned long long)stayed, (unsigned long long)retried, (unsigned long long)R->failed.load(), R->threads, (long long)(ms() / 1000));
    write_file_atomic(R->out_dir + "/manifest.json", man);
    R->elapsed_ms = ms(); R->state = ASTC_CONV_DONE;
    logf("[astc] done: %u textures in the cache (%llu needed more than the first step, %llu stay BC), %.2f GB, %llu failed, %.1f min", cnt, (unsigned long long)retried, (unsigned long long)stayed,
        (wend + index.size()) / 1073741824.0, (unsigned long long)R->failed.load(), ms() / 60000.0);
}

}  // namespace


// ---------------------------------------------------------------- API (astc_convert.h)
extern "C" int astc_conv_big_cores(void) {
    long mx[64]; int n = 0; long lo = 0, hi = 0;
    for (int c = 0; c < 64; c++) { char p[96]; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
        FILE *f = fopen(p, "r"); if (!f) break; long v = 0; if (fscanf(f, "%ld", &v) != 1) v = 0; fclose(f); mx[n++] = v;
        if (!lo || v < lo) lo = v; if (v > hi) hi = v; }
    if (!n) { long c = sysconf(_SC_NPROCESSORS_ONLN); return c > 1 ? int(c) : 1; }
    int big = 0; for (int i = 0; i < n; i++) if (mx[i] > lo || lo == hi) big++;
    return big > 0 ? big : 1;
}
// Minutes the whole cache takes here, for the start dialog. The D3 port measured 2.6 Mpx/s of texture levels a PC core (x86-64, SSE4.1,
// the -fast rule with its retries); TL2's BC textures are 1.13 Gpx (5622 unique; the PC tool on 11 workers: 67 s). A phone core is taken as that rate
// x (its cpufreq max / 4.5 GHz) x 0.75 (NEON is as wide as SSE4.1; the in-order / narrower cores make up the rest), summed over the
// big cores; plus ~1 minute for reading the packs. Thermal throttling makes it longer.
extern "C" int astc_conv_estimate_minutes(void) {
    double rate = 0; long mx[64]; int n = 0; long lo = 0, hi = 0;
    for (int c = 0; c < 64; c++) { char p[96]; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
        FILE *f = fopen(p, "r"); if (!f) break; long v = 0; if (fscanf(f, "%ld", &v) != 1) v = 0; fclose(f); mx[n++] = v; if (!lo || v < lo) lo = v; if (v > hi) hi = v; }
    for (int i = 0; i < n; i++) if (mx[i] > lo || lo == hi) rate += 2.6e6 * (mx[i] / 1e6 / 4.5) * 0.75;
    if (rate <= 0) rate = 2.6e6 * 0.5 * 2;
    return int(1.13e9 / rate / 60 + 1.5);
}
extern "C" void astc_conv_set_log(void (*fn)(const char *)) { g_log = fn; }
extern "C" int astc_conv_start(const char *data_dir, int threads) {
    std::lock_guard<std::mutex> l(g_mu);
    if (g_run && (g_run->state == ASTC_CONV_PLANNING || g_run->state == ASTC_CONV_ENCODING || g_run->state == ASTC_CONV_FINISHING)) return 1;
    delete g_run; g_run = new Run; g_run->data_dir = data_dir; g_run->out_dir = std::string(data_dir) + "/astc";
    g_run->threads = threads > 0 ? threads : astc_conv_big_cores(); g_run->limit = g_run->threads; g_run->state = ASTC_CONV_PLANNING;
    logf("[astc] start: %s, %d threads", data_dir, g_run->threads);
    Run *r = g_run; std::thread([r] { run_main(r); }).detach();   // the Run outlives it (replaced only when not running)
    return 0;
}
extern "C" void astc_conv_limit(int n) { std::lock_guard<std::mutex> l(g_mu); if (g_run) g_run->limit = std::max(1, std::min(n, g_run->threads)); }
extern "C" void astc_conv_cancel(void) { std::lock_guard<std::mutex> l(g_mu); if (g_run) g_run->cancel = true; }
extern "C" void astc_conv_progress(int64_t out[ASTC_CONV_FIELDS]) {
    std::lock_guard<std::mutex> l(g_mu); memset(out, 0, sizeof(int64_t) * ASTC_CONV_FIELDS); if (!g_run) return; Run &r = *g_run;
    const int64_t el = int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - r.t0).count());
    const int64_t v[ASTC_CONV_FIELDS] = {r.state.load(), int64_t(r.done.load()), int64_t(r.total.load()), int64_t(r.done_px.load()), int64_t(r.total_px.load()),
        r.state >= ASTC_CONV_DONE ? r.elapsed_ms.load() : el, r.limit.load(), r.threads, int64_t(r.bytes.load()), int64_t(r.plan_done.load()), int64_t(r.plan_total.load()), int64_t(r.failed.load())};
    memcpy(out, v, sizeof v);
}

