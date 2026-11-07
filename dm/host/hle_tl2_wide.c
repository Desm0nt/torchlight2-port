// [TL2] Widescreen (DM_TL2_WIDE, default on; docs/tl2/TL2_PORT.md "Широкий экран"): Torchlight II renders at the phone's aspect with a Hor+ view.
// TL2 never asks the system for a display size (of oe:: / vi:: it imports only GetOperationMode / OpenDefaultDisplay / CreateLayer /
// GetNativeWindow). Its size comes from immediates next to nn::oe::GetOperationMode (main v5 build a0c6c2d2, offsets = main.raw):
//   0x199417c  orr w27,wzr,#1920 / mov w8,#1080 (docked), 0x199418c mov w27,#1280 / mov w8,#720 (handheld): the NVN window setup
//              (re)built when its flag +0x5f4 is set: three swap textures W x H (TextureBuilderSetSize2D 0x1994430), the render
//              window's size (+0x24 / +0x28, 0x1994954) and its colour target 0x1991260.
//   0x7264b4   orr w8,wzr,#1920 (docked), 0x7264cc mov w20,#1280 (handheld): game init stores the size into the PC-style video settings
//              WIDTH / HEIGHT (ids main+0x753f4c4 / +0x753f4c8, registered with defaults 800x600 at 0x270ad0; 0x1a0f30 = set int).
//   0x1317b8   orr w2,wzr,#1920 (docked), 0x131720 mov w21,#1280 (handheld): the same settings on an operation-mode change message
//              (TryPopNotificationMessage loop; it also raises the window rebuild flag +0x5f4).
// The cameras follow the viewport: CreateViewports (0x72cce0) sets aspect = viewport actual width / height (0x1823190 / 0x18231a0 ->
// Camera vtable +0x240, Ogre::Frustum::setAspectRatio) and Ogre keeps the vertical FOV, so a wider window = Hor+. The PC video-options
// apply (0x72e330) does the same from WIDTH / HEIGHT after snapping them to the closest display mode in a list (0x275a90, |dw|+|dh|);
// that snap is made the identity so a mode list built for 16:9 can never pull the size (and the camera aspect) back to 1920x1080.
// The console UI (CConsoleUI*, "UI_CONSOLE") lays out in a fixed 1920x1080 canvas (0x140f28 orr w3,wzr,#1920 -> 0x19fef80: root size
// 1920x1080 in both modes, so it is scaled to the window) and many full-screen panels are sized with the literal (1920, 1080)
// (main+0x1f71810 / +0x1f69298). The canvas is left at 1920x1080 by default = the HUD keeps the layout it had, scaled to the screen
// exactly as the stretched 16:9 present showed it (x W/1920); DM_TL2_WIDE_UI=1 widens the canvas too (experiment: the anchored
// panels spread to the edges, the full-screen 1920-wide panels may then miss the right strip).
//   DM_TL2_WIDE unset / 1   W = 1080 x screen aspect (front_game_res: the screen size is known, nativeSurface runs before nativeStart)
//   DM_TL2_WIDE=<width>     explicit docked width (1921..3840)
//   DM_TL2_WIDE=0           the Switch's 1920x1080 / 1280x720
// Widths are rounded to 16; handheld = W x 2/3 (720 high). All sites are checked word for word: another build -> nothing changes.
// Touch needs nothing: the game reads the touch panel only once at boot (0x13ea04), and the front end's normalized coordinates already
// cover the whole presented window.
#ifdef DM_GAME_TL2
#include "hle.h"
#include "front.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

typedef struct { uint32_t off; uint32_t n; const uint32_t *want; } tw_site;
static const uint32_t k_tw_win[10] = {0x941194f8 /* bl oe::GetOperationMode */, 0x7100041f, 0x54000081, 0x32190ffb /* orr w27,wzr,#1920 */,
    0x52808708 /* mov w8,#1080 */, 0x14000004, 0x941194f2, 0x5280a01b /* mov w27,#1280 */, 0x52805a08 /* mov w8,#720 */, 0xb81ac3a8};
static const uint32_t k_tw_init[11] = {0x945b4c2b, 0x34000100, 0x945b4c29, 0x7100041f, 0x32190fe8 /* orr w8,wzr,#1920 */, 0x1a940114,
    0x52808708 /* mov w8,#1080 */, 0x1a950115, 0x14000003, 0x52805a15 /* mov w21,#720 */, 0x5280a014 /* mov w20,#1280 */};
static const uint32_t k_tw_msg_hh[3] = {0x52808714 /* mov w20,#1080 */, 0x5280a015 /* mov w21,#1280 */, 0x52805a16 /* mov w22,#720 */};
static const uint32_t k_tw_msg_dk[8] = {0x94731f6b, 0x7100041f, 0x54000301, 0x9404ecbc, 0xb94002e1, 0x32190fe2 /* orr w2,wzr,#1920 */, 0x9401bddd, 0x9404ecb8};
static const uint32_t k_tw_canvas[5] = {0x52800c82 /* mov w2,#100 */, 0x52808704 /* mov w4,#1080 */, 0x32190fe3 /* orr w3,wzr,#1920 */, 0xaa1503e0, 0x9462f814};
static const uint32_t k_tw_snap[3] = {0xb941980a /* ldr w10,[x0,#0x198] */, 0x3400046a /* cbz w10 */, 0xaa1f03e8};
enum { TW_WIN = 0x1994170, TW_INIT = 0x7264a4, TW_MSG_HH = 0x13171c, TW_MSG_DK = 0x1317a4, TW_CANVAS = 0x140f20, TW_SNAP = 0x275a90 };
static const tw_site k_tw_sites[] = {{TW_WIN, 10, k_tw_win}, {TW_INIT, 11, k_tw_init}, {TW_MSG_HH, 3, k_tw_msg_hh}, {TW_MSG_DK, 8, k_tw_msg_dk},
                                     {TW_CANVAS, 5, k_tw_canvas}, {TW_SNAP, 3, k_tw_snap}};

static uint32_t movz_w(unsigned rd, unsigned imm) { return 0x52800000u | (imm & 0xffff) << 5 | (rd & 31); }
static int tw_poke(module_t *m, uint32_t off, const uint32_t *w, int n) {
    uint32_t *at = (uint32_t *)(m->base + off);
    uint8_t *pg = (uint8_t *)((uintptr_t)at & ~(uintptr_t)0x3FFF), *end = (uint8_t *)(at + n);
    size_t len = (size_t)(((uintptr_t)end - (uintptr_t)pg + 0x3FFF) & ~(uintptr_t)0x3FFF);   // 16 KB = the loader's sealing granule
    if (mprotect(pg, len, PROT_READ | PROT_WRITE)) return -1;
    for (int i = 0; i < n; i++) at[i] = w[i];
    if (mprotect(pg, len, PROT_READ | PROT_EXEC)) die("[wide] mprotect RX");
    __builtin___clear_cache((char *)at, (char *)end);
    return 0;
}

// The docked width for the screen: 1080 x its aspect, rounded to 16 (0 = not wider than 16:9 / unknown). front_game_res answers at the
// D2R/D3 height (900, or DM_GAME_RES's); only its aspect is used.
static unsigned tw_auto_width(void) {
    uint64_t r = front_game_res(1); unsigned w = (uint32_t)r, h = (uint32_t)(r >> 32);
    if (!w || !h || (uint64_t)w * 9 <= (uint64_t)h * 16) return 0;
    return (unsigned)(llround(1080.0 * w / h / 16) * 16);
}

void dm_tl2_wide_install(void);   // main.c host_preload, right after load_modules (before any guest code runs)
void dm_tl2_wide_install(void) {
    const char *e = getenv("DM_TL2_WIDE");
    if (e && *e == '0' && !e[1]) { LOG("[wide] DM_TL2_WIDE=0: window 1920x1080 (handheld 1280x720), the 16:9 picture scaled to the screen"); return; }
    long want = e ? atol(e) : 0; unsigned w;
    if (want > 1920) w = (unsigned)(want + 8) / 16 * 16; else w = tw_auto_width();
    if (w > 3840) w = 3840;
    if (w <= 1920) { LOG("[wide] DM_TL2_WIDE=%s: screen not wider than 16:9 (or unknown): window stays 1920x1080", e ? e : "(default)"); return; }
    unsigned wh = (w * 2 / 3 + 8) / 16 * 16;
    module_t *m = NULL;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    for (size_t i = 0; m && i < sizeof k_tw_sites / sizeof k_tw_sites[0]; i++) {
        const tw_site *s = &k_tw_sites[i];
        if (m->text_end < s->off + 4 * s->n || memcmp(m->base + s->off, s->want, 4 * s->n)) {
            LOG("【红】[wide] main+%#x differs: not TL2 v5 main (build a0c6c2d2): window stays 1920x1080", s->off); return; }
    }
    if (!m) { LOG("【红】[wide] no main module: window stays 1920x1080"); return; }
    const char *ue = getenv("DM_TL2_WIDE_UI"); int ui = ue && *ue && !(*ue == '0' && !ue[1]);
    const uint32_t win_dk = movz_w(27, w), win_hh = movz_w(27, wh), init_dk = movz_w(8, w), init_hh = movz_w(20, wh),
                   msg_hh = movz_w(21, wh), msg_dk = movz_w(2, w), canvas = movz_w(3, w);
    const uint32_t snap[3] = {0xb9000061 /* str w1,[x3] */, 0xb9000082 /* str w2,[x4] */, 0xd65f03c0 /* ret */};
    if (tw_poke(m, TW_WIN + 12, &win_dk, 1) || tw_poke(m, TW_WIN + 28, &win_hh, 1) || tw_poke(m, TW_INIT + 16, &init_dk, 1) ||
        tw_poke(m, TW_INIT + 40, &init_hh, 1) || tw_poke(m, TW_MSG_HH + 4, &msg_hh, 1) || tw_poke(m, TW_MSG_DK + 20, &msg_dk, 1) ||
        tw_poke(m, TW_SNAP, snap, 3) || (ui && tw_poke(m, TW_CANVAS + 8, &canvas, 1))) {
        LOG("【红】[wide] mprotect RW failed part-way: the game may mix sizes"); return; }
    LOG("[wide] TL2 window 1920x1080 -> %ux1080, handheld 1280x720 -> %ux720, aspect %.4f (Hor+: cameras take the viewport's aspect); "
        "video settings WIDTH/HEIGHT the same (main+%#x / +%#x), display-mode snap main+%#x off; console UI canvas %s",
        w, wh, w / 1080.0, TW_INIT + 16, TW_MSG_DK + 20, TW_SNAP,
        ui ? "widened too (DM_TL2_WIDE_UI=1)" : "kept 1920x1080 (scaled to the window; DM_TL2_WIDE_UI=1 widens it)");
}
#endif
