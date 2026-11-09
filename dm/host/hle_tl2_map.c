// [TL2] The automap's own modes and a tap on the map (docs/tl2/TL2_PORT.md "Карта: мини-карта / большая карта").
// How the Switch build shows the map (main v5 build a0c6c2d2, offsets = main.raw):
//   * The console UI manager's HandleInput 0x143800, with its menu stack empty (+0x60 = -1) and the gameplay HUD receiver (+0x80) not
//     taking the event, turns a press of button code 5 = D-pad Right (glyph table 0x230d8b0: 0 Y, 1 A, 2 X, 3 B, 4 / 5 / 6 / 7 D-pad left /
//     right / up / down, ...; the prompt table 0x230d968 names it TOGGLE_MAP) into (0x143978..0x1439e4):
//         mode = (mode + 1) % 3; on = mode > 0      (0x1a0f30 on the settings object 0x26caa0, ids *main+0x753f60c / *main+0x753f610)
//     D-pad Up / Down (codes 6 / 7, 0x143aec / 0x143b2c) zoom it (0x143c20: automap +0xf0, clamped 10..70).
//   * CConsoleAutomap (vt main+0x230a628, ctor 0x129630, owned by the level: +0x3a8, 0x3e6394; the level of the local player =
//     0x6c9fe0(client), client = *main+0x75d49b0, as 0x143c34..0x143c50 reach it) reads the mode in its update 0x12dc90: when it changed,
//     1 = the minimap (+0x139 = 0, the "ig_automap" frame +0xd0, 230 x 230 canvas units, top right), 2 = the big map (+0x139 = 1,
//     "map_background" +0xd8 and a 400 x 670 map, 0x12c088..), 0 = hidden; then SetVisible 0x12bf80 (+0x105) behind its own gates
//     (mode and "on" set, player +0x26b8 = 0). The pause / radial menu has no world map panel (panel 0x20 MapSelector is the map-item portal).
// So the big map is the game's own second mode, reached on the Switch by pressing D-pad Right once (twice = map off). A tap on the map
// here switches minimap <-> big map with the same two setting writes the D-pad code makes (1 <-> 2; never 0, so a tap does not hide it),
// on the game's input thread (dm_tl2_ui_pump, before the game dispatches its own input events); the automap's update applies it itself.
//   Hit rect: the drawn map element (+0xd8 big / +0xd0 minimap, else the container +0x328), its laid-out rect +0x60 in canvas units
//   (the automap is a HUD screen: whole width in the hybrid wide UI) -> picture fractions via dm_tl2_ui_canvas_to_frame, published every
//   few npad reads while no console menu / dialog / loading screen is up and the map is shown.
//   DM_TL2_MAP=0 off. Test without the front end: file dm/map-now ("" / "t" = toggle, "0" / "1" / "2" = that mode), checked every 30 polls.
// Java: DmActivity.nativeTl2MapHit(x, y) (is the point on the map now) / nativeTl2ToggleMap(); nativeTl2UiTap(x, y) also toggles when
// no menu is open and the point is on the map (Tl2Touch lets a finger that lands on the map wait like a menu tap).
#ifdef DM_GAME_TL2
#include "hle.h"
#include "dm.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

enum {
    M_CLIENT = 0x75d49b0, F_LEVEL = 0x6c9fe0, L_AUTOMAP = 0x3a8, VT_AUTOMAP = 0x230a628, GOT_VT_AUTOMAP = 0x23a3d70,
    F_SETTINGS = 0x26caa0, F_GETI = 0x1a0f10, F_SETI = 0x1a0f30, G_MODE_ID = 0x753f60c, G_ON_ID = 0x753f610, GOT_MODE_ID = 0x23a3de0, GOT_ON_ID = 0x23a3de8,
    A_SHOWN = 0x105, A_BIG = 0x139, A_FRAME = 0xd0, A_BG = 0xd8, A_ROOT = 0x328,
    U_MGR = 0x74cfe70, U_STACK = 0x58, U_TOP = 0x60, U_MODAL = 0x50, U_LOADING = 0x90,
    P_MGR = 0x927a970, M_W = 0x2f8, E_PARENT = 0x38, E_RECT = 0x60, E_DRAWN = 0x74,
};
static const struct { uint32_t off, n, w[20]; } k_sites[] = {
    {0x26caa0, 3, {0xf0039688, 0xf9439d00, 0xd65f03c0}},                                       // settings object getter (ldr [#0x738] of main+0x753f000)
    {0x1a0f10, 6, {0xb9407008, 0x6b01011f, 0x54000089, 0xf9403008, 0xb8615900, 0xd65f03c0}},   // settings GetInt(id): +0x60 array, +0x70 count
    {0x1a0f30, 6, {0xb9407008, 0x6b01011f, 0x540000a9, 0xf9403008, 0x320003e0, 0xb8215902}},   // settings SetInt(id, v)
    {0x6c9270, 3, {0xf0037848, 0xf944d900, 0xd65f03c0}},                                       // client getter: *main+0x75d49b0
    {0x6c9fe0, 8, {0xa9bd57f6, 0xa9014ff4, 0xa9027bfd, 0x910083fd, 0xf9402c09, 0xb966c808, 0xaa0003f3, 0xb40003a9}},   // level of the local player
    // UI manager HandleInput: code 5 -> mode = (mode + 1) % 3 (the D-pad Right map toggle)
    {0x143978, 20, {0xb9400688, 0x7100151f, 0x54000a81, 0x9404a447, 0x90011317, 0xf946f2f7, 0xb94002e1, 0x9401755f, 0x11000408, 0x528aaac9,
                    0x72aaaaa9, 0x9b297d09, 0xd37ffd2a, 0xd360fd29, 0x0b0a0129, 0x0b090529, 0x4b090116, 0x9404a439, 0xb94002e1, 0x2a1603e2}},
    {0x12dd5c, 7, {0x71000abf, 0x540003e0, 0x710006bf, 0x540006a1, 0x320003e8, 0x39041268, 0x79027268}},   // automap update: mode 2 / 1
    {0x12dddc, 2, {0x52802029, 0x79027269}},                                                   // mode 2: +0x138 = 0x101 (+0x139 big)
    {0x3e6390, 2, {0x97f50ca8, 0xf901d675}},                                                   // level: new CConsoleAutomap -> +0x3a8
    {0x12bf9c, 1, {0x39441408}},                                                               // SetVisible: +0x105 shown
    {0x12c034, 6, {0xf9406e68, 0x3901d11f, 0xf9406a68, 0x3901d11f, 0xf9419668, 0x3901d11f}},   // its elements +0xd8 / +0xd0 / +0x328
};

static uint8_t *g_base; static size_t g_size; static int g_ok = -1;
static char g_dir[1100];
int dm_guest_heap_readable(uint64_t a, size_t n);
static int rd(uint64_t a, void *out, size_t n) {   // a stale pointer reads as absent, never faults
    if (a < 0x10000 || a >= (1ull << 48) || n > 4096) return 0;
    if (dm_guest_heap_readable(a, n)) { memcpy(out, (const void *)(uintptr_t)a, n); return 1; }
    uint64_t b = (uint64_t)(uintptr_t)g_base;
    if (a >= b && n <= g_size && a - b <= g_size - n) { memcpy(out, (const void *)(uintptr_t)a, n); return 1; }
    struct iovec l = {out, n}, r = {(void *)(uintptr_t)a, n};
    return syscall(SYS_process_vm_readv, getpid(), &l, 1UL, &r, 1UL, 0UL) == (ssize_t)n;
}
static uint64_t rq(uint64_t a) { uint64_t v = 0; return rd(a, &v, 8) ? v : 0; }
static uint8_t r8(uint64_t a) { uint8_t v = 0; rd(a, &v, 1); return v; }
static int32_t r32(uint64_t a) { int32_t v = 0; rd(a, &v, 4); return v; }
static uint64_t img(uint32_t off) { return (uint64_t)(uintptr_t)g_base + off; }

static int setup(void) {
    if (g_ok >= 0) return g_ok;
    g_ok = 0;
    const char *e = getenv("DM_TL2_MAP"); if (e && *e && atoi(e) <= 0) { LOG("[tl2map] DM_TL2_MAP=0: map tap off"); return 0; }
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) { g_base = g_mod[i].base; g_size = g_mod[i].size; }
    if (!g_base || g_size < P_MGR + 0x340) { LOG("【红】[tl2map] no main module: map tap off"); return 0; }
    for (size_t i = 0; i < sizeof k_sites / sizeof *k_sites; i++)
        if (memcmp(g_base + k_sites[i].off, k_sites[i].w, 4 * k_sites[i].n)) {
            LOG("【红】[tl2map] main+%#x differs: not TL2 v5 main (build a0c6c2d2), map tap off", k_sites[i].off); return 0; }
    if (rq(img(GOT_VT_AUTOMAP)) + 0x10 != img(VT_AUTOMAP) || rq(img(GOT_MODE_ID)) != img(G_MODE_ID) || rq(img(GOT_ON_ID)) != img(G_ON_ID)) {
        LOG("【红】[tl2map] automap vtable / setting ids do not match: map tap off"); return 0; }
    e = getenv("DM_RUNTIME_DIR"); snprintf(g_dir, sizeof g_dir, "%s", e && *e ? e : "dm");
    LOG("[tl2map] on: tap on the minimap = the game's big map (its D-pad Right mode 2), again = minimap; file %s/map-now", g_dir);
    return g_ok = 1;
}

// ---- the game's own setting writes (what the D-pad Right code does), game input thread only
typedef uint64_t (*settings_fn)(void);
static int mode_get(void) { uint64_t s = ((settings_fn)img(F_SETTINGS))(); return s ? ((int (*)(uint64_t, uint32_t))img(F_GETI))(s, (uint32_t)r32(img(G_MODE_ID))) : -1; }
static void mode_set(int m) {
    uint64_t s = ((settings_fn)img(F_SETTINGS))(); if (!s) return;
    ((int (*)(uint64_t, uint32_t, int))img(F_SETI))(s, (uint32_t)r32(img(G_MODE_ID)), m);
    ((int (*)(uint64_t, uint32_t, int))img(F_SETI))(s, (uint32_t)r32(img(G_ON_ID)), m > 0);
}
static int menu_up(void) {   // the D-pad code only runs with the menu stack empty; no dialog, no loading screen
    uint64_t m = img(U_MGR), modal = rq(m + U_MODAL), load = rq(m + U_LOADING);
    if (r32(m + U_TOP) != -1) return 1;
    return (modal && r8(modal + E_DRAWN)) || (load && r8(load + E_DRAWN));
}
static uint64_t automap(void) {
    uint64_t client = rq(img(M_CLIENT)); if (!client) return 0;
    uint64_t level = ((uint64_t (*)(uint64_t))img(F_LEVEL))(client); if (!level) return 0;
    uint64_t am = rq(level + L_AUTOMAP);
    return am && rq(am) == img(VT_AUTOMAP) ? am : 0;
}
static int drawn_chain(uint64_t e) { for (int d = 0; e && d < 48; d++, e = rq(e + E_PARENT)) if (!r8(e + E_DRAWN)) return 0; return 1; }

// ---- published hit rect (picture fractions), written by the input thread, read by any
static _Atomic uint32_t g_hr[4]; static _Atomic int g_hvalid, g_req = -1;   // g_req: -1 none, 3 toggle, 0..2 that mode
void dm_tl2_ui_canvas_to_frame(float cx, float cy, float cw, float ch, float *fx, float *fy);   // hle_tl2_icons.c
static void publish(uint64_t am) {
    static int last_key = -1, said; static float last_r[4];
    int valid = 0, big = am ? r8(am + A_BIG) : 0, shown = am ? r8(am + A_SHOWN) : 0; float r[4] = {0}; const char *which = "-";
    if (am && shown && !menu_up()) {
        static const struct { uint32_t off; const char *n; } k[] = {{A_BG, "map_background +0xd8"}, {A_FRAME, "ig_automap +0xd0"}, {A_ROOT, "container +0x328"}};
        for (int i = big ? 0 : 1; i < 3 && !valid; i++) {
            uint64_t e = rq(am + k[i].off);
            if (e && drawn_chain(e) && rd(e + E_RECT, r, 16) && r[2] - r[0] > 8 && r[3] - r[1] > 8) { valid = 1; which = k[i].n; }
        }
    }
    if (valid) {
        int32_t wh[2] = {0, 0}; rd(img(P_MGR) + M_W, wh, 8);
        float cw = wh[0] > 0 ? (float)wh[0] : 1920.f, ch = wh[1] > 0 ? (float)wh[1] : 1080.f, f[4];
        dm_tl2_ui_canvas_to_frame(r[0] - 12, r[1] - 12, cw, ch, &f[0], &f[1]); dm_tl2_ui_canvas_to_frame(r[2] + 12, r[3] + 12, cw, ch, &f[2], &f[3]);
        for (int i = 0; i < 4; i++) { uint32_t v; memcpy(&v, &f[i], 4); atomic_store_explicit(&g_hr[i], v, memory_order_relaxed); }
        int key = big * 2 + 1;
        if ((key != last_key || fabsf(r[0] - last_r[0]) + fabsf(r[1] - last_r[1]) + fabsf(r[2] - last_r[2]) + fabsf(r[3] - last_r[3]) > 4) && said < 200) {
            said++; LOG("[tl2map] %s map shown: %s rect %.0f,%.0f,%.0f,%.0f of %.0fx%.0f canvas = picture %.3f,%.3f..%.3f,%.3f (automap %#llx, mode %d)",
                        big ? "big" : "mini", which, r[0], r[1], r[2], r[3], cw, ch, f[0], f[1], f[2], f[3], (unsigned long long)am, mode_get()); }
        last_key = key; memcpy(last_r, r, sizeof r);
    } else if (last_key != 0) {
        if (said < 200) { said++; LOG("[tl2map] map not tappable (automap %#llx, shown %d, big %d, menu %d)", (unsigned long long)am, shown, big, am ? menu_up() : -1); }
        last_key = 0;
    }
    atomic_store_explicit(&g_hvalid, valid, memory_order_release);
}
int dm_tl2_map_hit(float x, float y);
int dm_tl2_map_hit(float x, float y) {
    if (g_ok <= 0 || !atomic_load_explicit(&g_hvalid, memory_order_acquire)) return 0;
    float f[4]; for (int i = 0; i < 4; i++) { uint32_t v = atomic_load_explicit(&g_hr[i], memory_order_relaxed); memcpy(&f[i], &v, 4); }
    return x >= f[0] && x < f[2] && y >= f[1] && y < f[3];
}
int dm_tl2_map_toggle(void);
int dm_tl2_map_toggle(void) { if (g_ok <= 0) return 0; atomic_store_explicit(&g_req, 3, memory_order_release); return 1; }
// dm_tl2_ui_tap (hle_tl2_ui.c) asks this first: no menu up and the point on the map = the toggle
int dm_tl2_map_tap(float x, float y);
int dm_tl2_map_tap(float x, float y) {
    if (!dm_tl2_map_hit(x, y)) return 0;
    LOG("[tl2map] tap %.3f,%.3f on the map: toggle queued", x, y);
    return dm_tl2_map_toggle();
}

// ---- per npad read (dm_tl2_ui_pump, the game's input thread)
void dm_tl2_map_pump(void);
void dm_tl2_map_pump(void) {
    if (g_ok < 0) setup();
    if (g_ok <= 0) return;
    static unsigned tick; tick++;
    int req = atomic_exchange_explicit(&g_req, -1, memory_order_acquire);
    if (req >= 0) {
        if (menu_up()) LOG("[tl2map] map %s: a menu / dialog / loading screen is up: nothing (the D-pad code would not run either)", req == 3 ? "toggle" : "mode");
        else {
            int m = mode_get(), n = req == 3 ? (m == 2 ? 1 : 2) : req;
            mode_set(n);
            LOG("[tl2map] map mode %d -> %d (%s; the game's own D-pad Right setting writes, its automap update applies it)", m, n,
                n == 2 ? "big map" : n == 1 ? "minimap" : "hidden");
        }
    }
    if (req >= 0 || tick % 6 == 0) publish(menu_up() ? 0 : automap());   // the level is not walked under a loading screen / menu
    if (tick % 30) return;
    char t[1200]; snprintf(t, sizeof t, "%s/map-now", g_dir);
    FILE *f = fopen(t, "r");
    if (f) {
        char b[16] = {0}; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); remove(t);
        int v = n > 0 && b[0] >= '0' && b[0] <= '2' ? b[0] - '0' : 3;
        LOG("[tl2map] %s: %s", t, v == 3 ? "toggle" : "set mode");
        atomic_store_explicit(&g_req, v, memory_order_release);
    }
}
#endif
