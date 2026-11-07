// [TL2] Icons of the skills bound to the buttons, for the touch buttons (Tl2Touch draws them inside the fan / quick cells).
// Where the binding lives (main v5 build a0c6c2d2, offsets = main.raw; checked word for word at the first poll):
//   UI manager (static main+0x74cfe70) +0x68 = CConsoleUIHud (vt 0x230a470, made at 0x143154), +0x38 = CConsoleUILocalPlayerHud (vt
//   0x230a100, stored 0x124c60), +0xb8 = CConsoleUIActionBarWidget (vt 0x230a548, stored 0x11f45c): slots vector +0x40..+0x48 of 0x68 bytes
//   (built by 0x1265b0, one per button code 10 L, 8 R, 9 ZR, 1 A, 3 B, 0 Y, 2 X, 11 ZL in that order, 0x126300; the code names the
//   button glyph BUTTON_SWITCH_*_new, table 0x230d8b0): +0 code, +8 the bound skill / item hash (0x47c100(player, code); -1 / 0x499602d2
//   = nothing), +0x18 the icon PUI::Image (54 x 54 canvas units, its image set by name 0x19f8c80 in the update 0x127540), +0x30 the
//   cooldown image over it (fill +0x134, 0x1a03f70).
// The pixels: what the game drew. While the HUD is on screen the presenter (vulkan_present.cpp, translation thread) copies the icon
// rectangles of the slots that changed (or are due a refresh: every 5 s, never while their cooldown runs) out of the presented frame into a
// host buffer (vkCmdCopyImageToBuffer inside its own command buffer, read back after that frame's fence: no extra GPU sync), at most
// 4 times a second; they are scaled to 64 x 64 ARGB and published with a serial for Java (JNI nativeTl2IconSerial / nativeTl2Icons).
//   DM_TL2_ICONS=0 turns it off (default on: it reads only; a wrong rectangle can only give a wrong picture on a touch button).
#ifdef DM_GAME_TL2
#include "hle.h"
#include "dm.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

enum {
    U_MGR = 0x74cfe70, U_GOT_MGR = 0x23a3590, P_MGR = 0x927a970, M_W = 0x2f8, M_H = 0x2fc, U_LOADING = 0x90,
    VT_HUD = 0x230a470, VT_LOCAL = 0x230a100, VT_BAR = 0x230a548, HUD_OF_MGR = 0x68, LOCAL_OF_HUD = 0x38, BAR_OF_LOCAL = 0xb8,
    BAR_SLOTS = 0x40, BAR_SLOTS_END = 0x48, SLOT = 0x68, S_CODE = 0, S_HASH = 8, S_ICON = 0x18, S_COOL = 0x30, COOL_FILL = 0x134,
    E_PARENT = 0x38, E_RECT = 0x60, E_DRAWN = 0x74,
    NSLOT = 8, ICON = 64, MAXPX = 160,
};
static const struct { uint32_t off, w; } k_sites[] = {
    {0x143154, 0xf9003693 /* str x19,[x20,#0x68]: UI manager -> Hud */}, {0x124c60, 0xf9001e74 /* str x20,[x19,#0x38]: Hud -> LocalPlayerHud */},
    {0x11f45c, 0xf9005e60 /* str x0,[x19,#0xb8]: LocalPlayerHud -> ActionBarWidget */}, {0x11e5fc, 0xf9001801 /* LocalPlayerHud init +0x30 */},
    {0x1a03f70, 0xbd013400 /* str s0,[x0,#0x134]: cooldown fill */},
};

int dm_guest_heap_readable(uint64_t a, size_t n);
static uint8_t *g_base; static size_t g_size; static int g_ok = -1;
static int rd(uint64_t a, void *o, size_t n) {
    if (a < 0x10000 || a >= (1ull << 48)) return 0;
    uint64_t b = (uint64_t)(uintptr_t)g_base;
    if (dm_guest_heap_readable(a, n) || (a >= b && a + n <= b + g_size)) { memcpy(o, (const void *)(uintptr_t)a, n); return 1; }
    return 0;
}
static uint64_t rq(uint64_t a) { uint64_t v = 0; return rd(a, &v, 8) ? v : 0; }
static uint64_t img(uint32_t o) { return (uint64_t)(uintptr_t)g_base + o; }
static int drawn_chain(uint64_t e) { uint8_t v; for (int d = 0; e && d < 48; d++, e = rq(e + E_PARENT)) if (!rd(e + E_DRAWN, &v, 1) || !v) return 0; return 1; }

// ---- shared state (game thread <-> translation thread <-> Java)
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { int code; int64_t hash; float r[4]; int vis, cool; } g_slot[NSLOT];   // game thread's latest look
static struct { int n, idx[NSLOT]; float r[NSLOT][4]; float cw, ch; int64_t hash[NSLOT]; uint64_t t; int taken, used[NSLOT], nused; } g_req;   // under g_mu
static int64_t g_have[NSLOT]; static uint64_t g_got_t[NSLOT];                             // game thread: hash captured, when
static uint32_t g_px[NSLOT][ICON * ICON]; static int g_meta[NSLOT][4]; static _Atomic int g_serial;   // published (under g_mu)

static int setup(void) {
    if (g_ok >= 0) return g_ok;
    g_ok = 0;
    const char *e = getenv("DM_TL2_ICONS");
    if (e && *e == '0') { LOG("[tl2icons] DM_TL2_ICONS=0: off"); return 0; }
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) { g_base = g_mod[i].base; g_size = g_mod[i].size; }
    if (!g_base) return 0;
    for (size_t i = 0; i < sizeof k_sites / sizeof *k_sites; i++) {
        uint32_t w; memcpy(&w, g_base + k_sites[i].off, 4);
        if (w != k_sites[i].w) { LOG("【红】[tl2icons] main+%#x differs: not TL2 v5 main, button icons off", k_sites[i].off); return 0; }
    }
    if (rq(img(U_GOT_MGR)) != img(U_MGR) || rq(img(VT_HUD + 0x20)) != img(0x124900) || rq(img(VT_LOCAL + 0xc8)) != img(0x11e5d0) || rq(img(VT_BAR + 0x10)) != img(0x1260a0)) {
        LOG("【红】[tl2icons] HUD vtables do not match: button icons off"); return 0; }
    for (int i = 0; i < NSLOT; i++) { g_have[i] = 0x7fffffffffffffffll; g_meta[i][0] = -1; }
    LOG("[tl2icons] on: HUD action bar at UI manager +0x68 -> +0x38 -> +0xb8 (slots +0x40 x 0x68), icons cropped from the presented frame");
    return g_ok = 1;
}
static uint64_t bar_now(void) {
    uint64_t hud = rq(img(U_MGR) + HUD_OF_MGR);
    if (!hud || rq(hud) != img(VT_HUD)) return 0;
    uint64_t lp = rq(hud + LOCAL_OF_HUD); if (!lp || rq(lp) != img(VT_LOCAL)) return 0;
    uint64_t bar = rq(lp + BAR_OF_LOCAL); return bar && rq(bar) == img(VT_BAR) ? bar : 0;
}

// ---- game thread, every npad read
void dm_tl2_icons_poll(void);
void dm_tl2_icons_poll(void) {
    if (g_ok < 0) setup();
    if (g_ok <= 0) return;
    static uint64_t next; uint64_t now = now_ns();
    if (now < next) return;
    next = now + 50000000ull;   // 20 Hz is plenty
    uint64_t bar = bar_now(), s0 = bar ? rq(bar + BAR_SLOTS) : 0, s1 = bar ? rq(bar + BAR_SLOTS_END) : 0;
    int n = s1 > s0 ? (int)((s1 - s0) / SLOT) : 0; if (n > NSLOT) n = NSLOT;
    uint64_t load = rq(img(U_MGR) + U_LOADING); uint8_t lv = 0; int loading = load && rd(load + E_DRAWN, &lv, 1) && lv;
    int32_t cwh[2] = {1920, 1080}; rd(img(P_MGR) + M_W, cwh, 8);
    int want[NSLOT], nw = 0, published_change = 0;
    for (int i = 0; i < NSLOT; i++) {
        uint64_t s = s0 + (uint64_t)i * SLOT; int code = -1; int64_t hash = -1; float r[4] = {0}; int vis = 0, cool = 0;
        if (i < n) {
            rd(s + S_CODE, &code, 4); rd(s + S_HASH, &hash, 8);
            uint64_t ic = rq(s + S_ICON), cl = rq(s + S_COOL); float fill = 0;
            vis = !loading && ic && drawn_chain(ic) && rd(ic + E_RECT, r, 16) && r[2] > r[0] && r[3] > r[1];
            // +0x30 is the same icon drawn through the radial mask HUD_Action_Icon_Mask_02 with fill +0x134: 0 = no sweep (ready), between
            // = cooldown sweep, 1 = fully covered (cannot be used now: 0x127540 sets 1.0 then). Capture a ready one; a fully covered one only
            // when nothing better came for 2 s (it shows dimmed, still the right skill).
            if (cl && drawn_chain(cl) && rd(cl + COOL_FILL, &fill, 4)) {
                static uint64_t covered_since[NSLOT];
                if (fill <= 0.001f) covered_since[i] = 0;
                else if (fill >= 0.999f) { if (!covered_since[i]) covered_since[i] = now; cool = now - covered_since[i] < 2000000000ull || g_meta[i][1] == 1; }
                else { covered_since[i] = 0; cool = 1; }
            }
            // the HUD slides / scales in (0x1295e0): only a rectangle that stayed put for 3 polls (150 ms)
            static float last_r[NSLOT][4]; static int same[NSLOT];
            if (memcmp(last_r[i], r, 16)) { memcpy(last_r[i], r, 16); same[i] = 0; } else if (same[i] < 100) same[i]++;
            if (same[i] < 3) cool = 1;
        }
        g_slot[i].code = code; g_slot[i].hash = hash; memcpy(g_slot[i].r, r, 16); g_slot[i].vis = vis; g_slot[i].cool = cool;
        int empty = i < n && (hash == -1 || hash == 0x499602d2ll);
        pthread_mutex_lock(&g_mu);
        if (g_meta[i][0] != code) { g_meta[i][0] = code; g_meta[i][1] = 0; published_change = 1; }   // slot code known (button)
        if (empty && g_meta[i][1] != 2) { g_meta[i][1] = 2; g_have[i] = hash; published_change = 1; }   // became empty: no pixels
        pthread_mutex_unlock(&g_mu);
        if (empty || !vis || cool) continue;
        if (hash != g_have[i] || now - g_got_t[i] > 5000000000ull) want[nw++] = i;
    }
    if (published_change) atomic_fetch_add(&g_serial, 1);
    if (!nw) return;
    pthread_mutex_lock(&g_mu);
    if (!g_req.n || (g_req.taken && now - g_req.t > 1000000000ull) || (!g_req.taken && now - g_req.t > 250000000ull)) {
        g_req.n = nw; g_req.cw = (float)(cwh[0] > 0 ? cwh[0] : 1920); g_req.ch = (float)(cwh[1] > 0 ? cwh[1] : 1080); g_req.t = now; g_req.taken = 0;
        for (int k = 0; k < nw; k++) { g_req.idx[k] = want[k]; memcpy(g_req.r[k], g_slot[want[k]].r, 16); g_req.hash[k] = g_slot[want[k]].hash; }
    }
    pthread_mutex_unlock(&g_mu);
}

// ---- canvas <-> game picture, the one mapping for the pointer (hle_tl2_ui.c) and the icon crop. The PUI canvas (manager +0x2f8 / +0x2fc,
// 1920 x 1080) is stretched over the whole picture today: fx = cx / cw. Code that puts the UI into a band (widescreen 16:9 UI) calls
// dm_tl2_ui_set_canvas_map(scale x, scale y, offset x, offset y) in picture fractions: fx = ox + sx * cx / cw, fy = oy + sy * cy / ch.
static _Atomic uint64_t g_map_s = 0x3f8000003f800000ull, g_map_o = 0;   // two floats each: (sx, sy), (ox, oy)
static void map_get(float *sx, float *sy, float *ox, float *oy) {
    uint64_t s = atomic_load(&g_map_s), o = atomic_load(&g_map_o); uint32_t v;
    v = (uint32_t)s; memcpy(sx, &v, 4); v = (uint32_t)(s >> 32); memcpy(sy, &v, 4); v = (uint32_t)o; memcpy(ox, &v, 4); v = (uint32_t)(o >> 32); memcpy(oy, &v, 4);
}
void dm_tl2_ui_set_canvas_map(float sx, float sy, float ox, float oy);
void dm_tl2_ui_set_canvas_map(float sx, float sy, float ox, float oy) {
    if (!(sx > 0 && sy > 0)) { sx = sy = 1; ox = oy = 0; }
    uint32_t a, b; memcpy(&a, &sx, 4); memcpy(&b, &sy, 4); atomic_store(&g_map_s, (uint64_t)b << 32 | a);
    memcpy(&a, &ox, 4); memcpy(&b, &oy, 4); atomic_store(&g_map_o, (uint64_t)b << 32 | a);
    LOG("[tl2icons] UI canvas -> picture: x %.4f + %.4f * cx/cw, y %.4f + %.4f * cy/ch", ox, sx, oy, sy);
}
void dm_tl2_ui_canvas_to_frame(float cx, float cy, float cw, float ch, float *fx, float *fy);
void dm_tl2_ui_canvas_to_frame(float cx, float cy, float cw, float ch, float *fx, float *fy) {
    float sx, sy, ox, oy; map_get(&sx, &sy, &ox, &oy);
    *fx = ox + sx * cx / (cw > 0 ? cw : 1920.f); *fy = oy + sy * cy / (ch > 0 ? ch : 1080.f);
}
void dm_tl2_ui_frame_to_canvas(float fx, float fy, float cw, float ch, float *cx, float *cy);
void dm_tl2_ui_frame_to_canvas(float fx, float fy, float cw, float ch, float *cx, float *cy) {
    float sx, sy, ox, oy; map_get(&sx, &sy, &ox, &oy);
    *cx = (fx - ox) / sx * (cw > 0 ? cw : 1920.f); *cy = (fy - oy) / sy * (ch > 0 ? ch : 1080.f);
}

// ---- translation thread (vulkan_present.cpp present())
// Rectangles to copy, in source pixels of the picture (x, y, w, h); 0 = nothing this frame. Rate: at most one batch per 250 ms.
static int icon_want(int32_t px, int32_t py, int32_t pw, int32_t ph, int32_t out[][4], int max) {
    if (g_ok <= 0 || pw <= 0 || ph <= 0) return 0;
    static uint64_t last; uint64_t now = now_ns(); int n = 0;
    pthread_mutex_lock(&g_mu);
    if (g_req.n && !g_req.taken && now - last >= 250000000ull) {
        for (int k = 0; k < g_req.n && n < max; k++) {
            const float *r = g_req.r[k]; float a, b, c, d;
            dm_tl2_ui_canvas_to_frame(r[0], r[1], g_req.cw, g_req.ch, &a, &b); dm_tl2_ui_canvas_to_frame(r[2], r[3], g_req.cw, g_req.ch, &c, &d);
            int x0 = px + (int)(a * pw + .5f), y0 = py + (int)(b * ph + .5f), x1 = px + (int)(c * pw + .5f), y1 = py + (int)(d * ph + .5f);
            if (x0 < px) x0 = px; if (y0 < py) y0 = py; if (x1 > px + pw) x1 = px + pw; if (y1 > py + ph) y1 = py + ph;
            int w = x1 - x0, h = y1 - y0;
            if (w < 4 || h < 4 || w > MAXPX || h > MAXPX) continue;
            out[n][0] = x0; out[n][1] = y0; out[n][2] = w; out[n][3] = h; g_req.used[n] = k; n++;
        }
        g_req.taken = 1; g_req.nused = n; last = now; if (!n) g_req.n = 0;
    }
    pthread_mutex_unlock(&g_mu);
    return n;
}
// The copied pixels (tightly packed rows, 4 bytes, RGBA or BGRA), in the order icon_want gave the rectangles
static void icon_done(int n, const int32_t rect[][4], const uint8_t *pix, int bgra) {
    if (g_ok <= 0) return;
    pthread_mutex_lock(&g_mu);
    int m = g_req.taken ? g_req.nused : 0, k = 0;
    for (; k < m && k < n; k++) {
        int j = g_req.used[k], i = g_req.idx[j], w = rect[k][2], h = rect[k][3];
        uint32_t *o = g_px[i];
        for (int y = 0; y < ICON; y++) for (int x = 0; x < ICON; x++) {   // box-ish: nearest of the centre (icons are 50..120 px)
            int sx = (x * w + w / 2) / ICON, sy = (y * h + h / 2) / ICON; const uint8_t *p = pix + ((size_t)sy * w + sx) * 4;
            uint8_t R = bgra ? p[2] : p[0], G = p[1], B = bgra ? p[0] : p[2];
            o[y * ICON + x] = 0xff000000u | (uint32_t)R << 16 | (uint32_t)G << 8 | B;
        }
        pix += (size_t)w * h * 4;
        g_meta[i][1] = 1; g_have[i] = g_req.hash[j]; g_got_t[i] = now_ns();
    }
    g_req.n = 0; g_req.taken = 0;
    pthread_mutex_unlock(&g_mu);
    if (k) atomic_fetch_add(&g_serial, 1);
}
int (*dm_present_icon_want)(int32_t, int32_t, int32_t, int32_t, int32_t[][4], int) = icon_want;
void (*dm_present_icon_done)(int, const int32_t[][4], const uint8_t *, int) = icon_done;

// ---- Java (any thread)
int dm_tl2_icon_serial(void);
int dm_tl2_icon_serial(void) { return g_ok > 0 ? atomic_load(&g_serial) : -1; }
// px: NSLOT * 64 * 64 ARGB, meta: NSLOT * 4 (button code, state 0 unknown / 1 icon / 2 empty, 0, 0); returns the serial
int dm_tl2_icons(uint32_t *px, int npx, int *meta, int nmeta);
int dm_tl2_icons(uint32_t *px, int npx, int *meta, int nmeta) {
    if (g_ok <= 0) return -1;
    pthread_mutex_lock(&g_mu);
    if (px && npx >= NSLOT * ICON * ICON) memcpy(px, g_px, sizeof g_px);
    if (meta && nmeta >= NSLOT * 4) memcpy(meta, g_meta, sizeof g_meta);
    int s = atomic_load(&g_serial);
    pthread_mutex_unlock(&g_mu);
    return s;
}
#endif
