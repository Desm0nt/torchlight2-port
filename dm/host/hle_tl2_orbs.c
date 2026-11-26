// [TL2] Health and mana as numbers ("current / max") beside the HUD orb (GitHub issue #31). The console HUD only fills the orb; PC TL2
// shows the numbers when the mouse is over it. docs/tl2/TL2_PORT.md "Цифры здоровья и маны у шара". Main v5 build a0c6c2d2, offsets =
// main.raw; every site is checked word for word at install.
// Where the game has the numbers (static, main v5):
//   CConsoleUILocalPlayerHud (vt 0x230a100; init vt+0xc8 = 0x11e5d0; UI manager +0x68 -> Hud +0x38 -> it) updates every frame in vt+0xa8 =
//   0x120170, which calls the health orb update 0x120ac0(this, dt) (its only caller, 0x12020c) and tail-calls the mana one 0x120d80. Both
//   take the local player = client (0x6c9270 = *main+0x75d49b0) +0x58 and fill the orb halves with cur / max:
//     health  cur = 0x464ba0(player) = float player+0x704, max = 0x44ccd0(player) (int: base + stat modifiers, player +0x780 / +0x930
//             chain; scvtf in the HUD) -> fill of LocalPlayerHud +0xd8 (HUD_Health_Red_Hit, radial-mask image 0x1a03460, fill 0x1a03f70)
//     mana    cur = 0x497a30(player) = float player+0x728, max = 0x44d250(player) (int) -> fill of +0xe8 (HUD_Mana_Blue_Hit)
//   Both images hang off the LocalPlayerHud root +0x38 (AddChild vt+0x58 at 0x11e9d8 / 0x11e7e4); one orb split down the middle, red half
//   left, blue half right (PC TL2 style), bottom-left of the HUD inside the action-bar ring (LocalPlayerHud +0xb8 = CConsoleUIActionBarWidget,
//   slots +0x40..+0x48 of 0x68, icon +0x18: hle_tl2_icons.c).
// What this does: the health update's prologue (4 plain stores, no PC-relative instruction) jumps to orbs_hook, which runs the original
// and then, on the game thread with the HUD's own player, reads the two current values and calls the game's own two max functions (the
// same calls the original just made, so no new state is touched). With them it publishes the orb halves' laid-out rectangles (element
// +0x60, canvas units) mapped into picture fractions (dm_tl2_ui_canvas_to_frame, the mapping the taps and the icon crop use), the right edge
// of the action-bar ring (the text starts there) and "shown": the HUD root drawn (+0x74 up the parent chain), no loading screen, no
// console menu / panel on the menu stack, no modal dialog, no non-HUD screen over the orb (same tests as hle_tl2_icons.c). Java
// (Tl2OrbNumbers, JNI nativeTl2Orbs) draws the two lines next to the orb; a value older than 400 ms counts as hidden (the HUD update does
// not run on loading screens / the title).
// Code writes: the 4 words at 0x120ac0 only. DM_TL2_ORB_NUMBERS=0 = off (no hook; the gear row "Show HP / MP numbers" only hides the text).
// Log: "[tl2orbs] ..." (first values, then a line when shown / hidden changes and every 10 s while the numbers change; capped).
#ifdef DM_GAME_TL2
#include "hle.h"
#include "dm.h"
#include "pc.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>

enum {
    F_HP_HUD = 0x120ac0, F_HP_HUD_REST = 0x120ad0, F_MAXHP = 0x44ccd0, F_MAXMP = 0x44d250, CLIENT_PTR = 0x75d49b0,
    C_PLAYER = 0x58, P_HP = 0x704, P_MP = 0x728,
    LP_ROOT = 0x38, LP_HP_IMG = 0xd8, LP_MP_IMG = 0xe8, LP_BAR = 0xb8, VT_LOCAL = 0x230a100, VT_LOCAL_UPDATE = 0x120170, VT_BAR = 0x230a548,
    BAR_SLOTS = 0x40, BAR_SLOTS_END = 0x48, SLOT = 0x68, S_ICON = 0x18,
    U_MGR = 0x74cfe70, P_MGR = 0x927a970, M_W = 0x2f8, M_H = 0x2fc, U_LOADING = 0x90, U_STACK = 0x58, U_TOP = 0x60, U_MODAL = 0x50, U_LAYER = 0xa0,
    E_FIRST = 0x40, E_NEXT = 0x58, E_PARENT = 0x38, E_RECT = 0x60, E_DRAWN = 0x74,
};
// prologue of 0x120ac0: str d12,[sp,#-0x60]!; stp d11,d10,[sp,#0x10]; stp d9,d8,[sp,#0x20]; stp x22,x21,[sp,#0x30]
static const uint32_t k_prologue[4] = {0xfc1a0fec, 0x6d012beb, 0x6d0223e9, 0xa90357f6};
static const struct { uint32_t off, w; } k_sites[] = {
    {0x120ae4, 0x9416a1e3 /* bl 0x6c9270 (client) */}, {0x120ae8, 0xf9402c15 /* ldr x21,[x0,#0x58]: player */},
    {0x120b2c, 0x940d101d /* bl 0x464ba0: cur health */}, {0x120b38, 0x940cb066 /* bl 0x44ccd0: max health */},
    {0x120bb4, 0xf9406e60 /* ldr x0,[x19,#0xd8]: red half */}, {0x120e1c, 0x940ddb05 /* bl 0x497a30: cur mana */},
    {0x120e28, 0x940cb10a /* bl 0x44d250: max mana */}, {0x120ec8, 0xf9407660 /* ldr x0,[x19,#0xe8]: blue half */},
    {0x464ba0, 0xbd470400 /* ldr s0,[x0,#0x704] */}, {0x464ba4, 0xd65f03c0}, {0x497a30, 0xbd472800 /* ldr s0,[x0,#0x728] */}, {0x497a34, 0xd65f03c0},
    {0x6c9270, 0xf0037848}, {0x6c9274, 0xf944d900 /* ldr x0,[x8,#0x9b0] */}, {0x12020c, 0x9400022d /* bl 0x120ac0, the only caller */},
    {0x11e96c, 0xf9006e74 /* str x20,[x19,#0xd8] */}, {0x11e770, 0xf9007674 /* str x20,[x19,#0xe8] */}, {0x11f45c, 0xf9005e60 /* str x0,[x19,#0xb8] */},
    {0x44ccd0, 0xd101c3ff}, {0x44d250, 0xd10183ff},
};

int dm_guest_heap_readable(uint64_t a, size_t n);
int dm_tl2_ui_screen_is_hud(uint64_t el);   // hle_tl2_wide.c (-1 = unknown)
void dm_tl2_ui_canvas_to_frame(float cx, float cy, float cw, float ch, float *fx, float *fy);   // hle_tl2_icons.c
static uint8_t *g_base; static size_t g_size;
static int rd(uint64_t a, void *o, size_t n) {
    if (a < 0x10000 || a >= (1ull << 48)) return 0;
    uint64_t b = (uint64_t)(uintptr_t)g_base;
    if (dm_guest_heap_readable(a, n) || (a >= b && a + n <= b + g_size)) { memcpy(o, (const void *)(uintptr_t)a, n); return 1; }
    return 0;
}
static uint64_t rq(uint64_t a) { uint64_t v = 0; return rd(a, &v, 8) ? v : 0; }
static uint64_t img(uint32_t o) { return (uint64_t)(uintptr_t)g_base + o; }
static int drawn_chain(uint64_t e) { uint8_t v; for (int d = 0; e && d < 48; d++, e = rq(e + E_PARENT)) if (!rd(e + E_DRAWN, &v, 1) || !v) return 0; return 1; }
static int rect_of(uint64_t e, float r[4]) { return e && rd(e + E_RECT, r, 16) && r[2] > r[0] && r[3] > r[1] && r[2] - r[0] < 4000 && r[3] - r[1] < 4000; }

// why the orb is not on screen now (0 = it is): the same tests hle_tl2_icons.c makes for the action bar
static int hidden_why(const float orb[4]) {
    uint64_t m = img(U_MGR); uint8_t v = 0;
    uint64_t load = rq(m + U_LOADING); if (load && rd(load + E_DRAWN, &v, 1) && v) return 1;
    int32_t top = -1; rd(m + U_TOP, &top, 4);
    if (top >= 0 && top < 64) { uint64_t st = rq(m + U_STACK); if (st && rq(st + 8ull * (uint32_t)top)) return 2; }
    uint64_t modal = rq(m + U_MODAL); v = 0;
    if (modal && rd(modal + E_DRAWN, &v, 1) && v) return 3;
    uint64_t layer = rq(m + U_LAYER), c = layer ? rq(layer + E_FIRST) : 0;
    for (int n = 0; c && n < 128; n++, c = rq(c + E_NEXT)) {
        float r[4]; v = 0;
        if (!rd(c + E_DRAWN, &v, 1) || !v || !rect_of(c, r)) continue;
        int hud = dm_tl2_ui_screen_is_hud(c); if (hud) continue;   // HUD, or unknown (no hybrid wide UI: the layer is not ours to judge)
        if (r[0] < orb[2] && r[2] > orb[0] && r[1] < orb[3] && r[3] > orb[1]) return 4;
    }
    return 0;
}
static const char *const k_why[] = {"shown", "loading screen", "menu / panel open", "modal dialog", "screen over the orb", "HUD not drawn",
                                    "no orb rectangle", "no player"};

// ---- published (game thread -> any thread)
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { float v[12]; uint64_t t; } g_pub;   // v: shown, hp, hp max, mp, mp max, orb x0 y0 x1 y1, text x, line height (fractions)
static _Atomic int g_on;   // 0 install not run yet, 1 hook installed, -1 off
typedef void (*hud_fn)(uint64_t self, float dt);
typedef int32_t (*max_fn)(uint64_t player);
static hud_fn g_orig;

static void orbs_hook(uint64_t self, float dt) {
    g_orig(self, dt);
    uint64_t now = now_ns();
    float v[12] = {0};
    int why = 0;
    uint64_t client = rq(img(CLIENT_PTR)), player = client ? rq(client + C_PLAYER) : 0;
    float hp = 0, mp = 0; int32_t hpmax = 0, mpmax = 0;
    if (!player || !rd(player + P_HP, &hp, 4) || !rd(player + P_MP, &mp, 4)) why = 7;
    else { hpmax = ((max_fn)(void *)img(F_MAXHP))(player); mpmax = ((max_fn)(void *)img(F_MAXMP))(player); }
    float a[4], b[4], orb[4] = {0}, ring = 0;
    if (!why) {
        int ha = rect_of(rq(self + LP_HP_IMG), a), hb = rect_of(rq(self + LP_MP_IMG), b);
        if (!ha && !hb) why = 6;
        else {
            if (ha && hb) { orb[0] = fminf(a[0], b[0]); orb[1] = fminf(a[1], b[1]); orb[2] = fmaxf(a[2], b[2]); orb[3] = fmaxf(a[3], b[3]); }
            else memcpy(orb, ha ? a : b, 16);
            if (!drawn_chain(rq(self + LP_ROOT))) why = 5;
            else why = hidden_why(orb);
        }
    }
    // the text starts right of the action-bar ring: the right edge of the slot icons near the orb (+ a quarter icon for the button
    // letter the game prints beside A / ZR); without the bar, 1.5 orb widths right of the orb's centre (the ring as on the Switch)
    float ow = orb[2] - orb[0], cx = (orb[0] + orb[2]) / 2, cy = (orb[1] + orb[3]) / 2;
    if (!why) {
        uint64_t bar = rq(self + LP_BAR);
        if (bar && rq(bar) == img(VT_BAR)) {
            uint64_t s0 = rq(bar + BAR_SLOTS), s1 = rq(bar + BAR_SLOTS_END);
            for (uint64_t s = s0; s1 > s0 && s < s1 && s < s0 + 16ull * SLOT; s += SLOT) {
                float r[4];
                if (!rect_of(rq(s + S_ICON), r)) continue;
                if (fabsf((r[0] + r[2]) / 2 - cx) > 4 * ow || fabsf((r[1] + r[3]) / 2 - cy) > 4 * ow) continue;   // not the ring round this orb
                float e = r[2] + (r[2] - r[0]) * 0.25f; if (e > ring) ring = e;
            }
        }
        if (!(ring > orb[2])) ring = cx + 1.5f * ow;
    }
    int32_t cwh[2] = {1920, 1080}; rd(img(P_MGR) + M_W, cwh, 8);
    float cw = cwh[0] > 0 ? (float)cwh[0] : 1920.f, ch = cwh[1] > 0 ? (float)cwh[1] : 1080.f;
    v[0] = why ? 0.f : 1.f; v[1] = hp; v[2] = (float)hpmax; v[3] = mp; v[4] = (float)mpmax;
    if (!why) {
        float x, y, x1, y1;
        dm_tl2_ui_canvas_to_frame(orb[0], orb[1], cw, ch, &v[5], &v[6]); dm_tl2_ui_canvas_to_frame(orb[2], orb[3], cw, ch, &v[7], &v[8]);
        dm_tl2_ui_canvas_to_frame(ring, cy, cw, ch, &v[9], &y);
        dm_tl2_ui_canvas_to_frame(0, 0, cw, ch, &x, &y); dm_tl2_ui_canvas_to_frame(0, 30, cw, ch, &x1, &y1);
        v[10] = y1 - y;   // one text line = 30 canvas units (the HUD's own small text), as a fraction of the picture height
    }
    pthread_mutex_lock(&g_mu); memcpy(g_pub.v, v, sizeof v); g_pub.t = now; pthread_mutex_unlock(&g_mu);
    // log: the first values, every change of shown / hidden, and the numbers every 10 s while they change (capped)
    static int last_why = -1, lines; static uint64_t last_t; static float last_hp = -1, last_mp = -1; static int32_t last_hm = -1, last_mm = -1;
    if (lines < 300 && (why != last_why || ((hp != last_hp || mp != last_mp || hpmax != last_hm || mpmax != last_mm) && now - last_t > 10000000000ull))) {
        lines++; last_t = now; last_why = why; last_hp = hp; last_mp = mp; last_hm = hpmax; last_mm = mpmax;
        if (why == 7) LOG("[tl2orbs] %s", k_why[why]);
        else LOG("[tl2orbs] %s: HP %.1f / %d, MP %.1f / %d; orb canvas %.0f,%.0f..%.0f,%.0f of %.0fx%.0f = picture %.3f,%.3f..%.3f,%.3f, text x %.3f, line %.3f",
                 k_why[why], hp, hpmax, mp, mpmax, orb[0], orb[1], orb[2], orb[3], cw, ch, v[5], v[6], v[7], v[8], v[9], v[10]);
    }
}

// ---- Java (any thread): out[12] as g_pub.v; returns 1 shown, 0 hidden / stale / not decided yet, -1 off (no hook)
int dm_tl2_orbs(float *out, int n);
int dm_tl2_orbs(float *out, int n) {
    int on = atomic_load(&g_on); if (on <= 0) return on;
    float v[12]; uint64_t t;
    pthread_mutex_lock(&g_mu); memcpy(v, g_pub.v, sizeof v); t = g_pub.t; pthread_mutex_unlock(&g_mu);
    if (!t || now_ns() - t > 400000000ull) v[0] = 0;   // the HUD update did not run lately (loading screen, title, paused)
    if (out) memcpy(out, v, sizeof(float) * (size_t)(n < 12 ? n : 12));
    return v[0] > 0 ? 1 : 0;
}

static int poke(uint8_t *base, uint32_t off, const uint32_t *w, int n) {   // code is sealed RX in 16 KiB granules by the loader
    uint32_t *at = (uint32_t *)(void *)(base + off);
    uint8_t *pg = (uint8_t *)((uintptr_t)at & ~(uintptr_t)0x3FFF), *end = (uint8_t *)(at + n);
    size_t len = (size_t)(((uintptr_t)end - (uintptr_t)pg + 0x3FFF) & ~(uintptr_t)0x3FFF);
    if (mprotect(pg, len, PROT_READ | PROT_WRITE)) return -1;
    for (int i = 0; i < n; i++) at[i] = w[i];
    if (mprotect(pg, len, PROT_READ | PROT_EXEC)) die("[tl2orbs] mprotect RX");
    __builtin___clear_cache((char *)at, (char *)end);
    return 0;
}

// ---- install (main.c host_preload, after load_modules, before guest code runs)
void dm_tl2_orbs_install(void);
void dm_tl2_orbs_install(void) {
    atomic_store(&g_on, -1);   // every early return below = off
    const char *e = getenv("DM_TL2_ORB_NUMBERS");
    if (e && *e == '0' && !e[1]) { pc_note("tl2-orbs", 0, "%s", "DM_TL2_ORB_NUMBERS=0"); return; }
    module_t *m = NULL;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    if (!m || m->text_end < F_HP_HUD + 16 || m->size < P_MGR + 0x300) { pc_note("tl2-orbs", 0, "%s", "no main module"); return; }
    g_base = m->base; g_size = m->size;
    if (memcmp(m->base + F_HP_HUD, k_prologue, sizeof k_prologue)) { pc_note("tl2-orbs", 0, "main+%#x differs: not TL2 v5 main (build a0c6c2d2)", F_HP_HUD); return; }
    for (size_t i = 0; i < sizeof k_sites / sizeof *k_sites; i++) {
        uint32_t w; memcpy(&w, m->base + k_sites[i].off, 4);
        if (w != k_sites[i].w) { pc_note("tl2-orbs", 0, "main+%#x differs: not TL2 v5 main (build a0c6c2d2)", k_sites[i].off); return; }
    }
    if (rq(img(VT_LOCAL + 0xa8)) != img(VT_LOCAL_UPDATE) || rq(img(VT_LOCAL + 0xc8)) != img(0x11e5d0)) {
        pc_note("tl2-orbs", 0, "%s", "CConsoleUILocalPlayerHud vtable not as in main v5 a0c6c2d2"); return; }
    // the original's first 4 instructions + a jump to the rest, in a page of their own
    uint32_t *t = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (t == MAP_FAILED) { pc_note("tl2-orbs", 0, "%s", "mmap failed"); return; }
    uint64_t rest = (uint64_t)(uintptr_t)(m->base + F_HP_HUD_REST);
    memcpy(t, k_prologue, 16); t[4] = 0x58000050 /* ldr x16,#8 */; t[5] = 0xd61f0200 /* br x16 */; t[6] = (uint32_t)rest; t[7] = (uint32_t)(rest >> 32);
    if (mprotect(t, 0x4000, PROT_READ | PROT_EXEC)) { munmap(t, 0x4000); pc_note("tl2-orbs", 0, "%s", "mprotect RX failed"); return; }
    __builtin___clear_cache((char *)t, (char *)(t + 8));
    g_orig = (hud_fn)(void *)t;
    uint64_t to = (uint64_t)(uintptr_t)orbs_hook;
    const uint32_t jump[4] = {0x58000050 /* ldr x16,#8 */, 0xd61f0200 /* br x16 */, (uint32_t)to, (uint32_t)(to >> 32)};
    if (poke(m->base, F_HP_HUD, jump, 4)) { munmap(t, 0x4000); pc_note("tl2-orbs", 0, "%s", "mprotect RW of the health orb update refused"); return; }
    atomic_store(&g_on, 1);
    pc_note("tl2-orbs", 1, "HP / MP numbers: health orb update main+%#x wrapped (cur player+0x704 / +0x728, max main+%#x / main+%#x)", F_HP_HUD, F_MAXHP, F_MAXMP);
}
#endif
