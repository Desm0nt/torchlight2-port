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
// (main+0x1f71810 / +0x1f69298). UI modes (DM_TL2_WIDE_UI):
//   2 / unset  hybrid (default): the canvas is W x 1080 (0x140f28 -> W), the in-game HUD lays out on the whole width (edge-anchored,
//              unstretched), every other screen (menus, panels, title, dialogs, loading screen) in the centred 1920-wide band
//              (ui_band_*, below); canvas units = the wide canvas, so taps / icon crops map plainly (fraction x W)
//   1          wide canvas only (experiment: menus pin to the left, full-screen art stretches)
//   0          the 1920x1080 canvas scaled to the window (everything stretched x W/1920)
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


// ---- The UI manager and the loading screen
// The console UI manager is the static main+0x74cfe70 (GOT 0x23a3590). Its setup 0x141020 makes the PUI root's children: +0xa0 the screen
// layer (a plain element, anchors 0..1) that every CConsoleUI screen's Show (vt slot 6) puts its root into (container->AddChild +0x58,
// then container->Relayout +0x48: MainMenu 0x1453c0, Options 0xc0140, Network 0x10fd00, Hud 0x125cd0, ...), +0x90 the loading screen
// (CConsoleUILoading), +0x50 the modal dialog (CConsoleUIDialog), +0xf8 / +0x100 the invite toast / alert, +0x108 / +0x110 two texts.
// The loading screen node (vtable main+0x2304530, built by 0xc1e40): byte +0x74 = drawn (the base Render 0x19f57b0 returns at once while
// it is 0), +0x75 = updated; Show 0xc2a30 sets both (0xc2ae4 strh #0x101), Update 0xc30f0 clears both when the fade-out ends (0xc3168);
// the game tests it itself (0x112718). While it is drawn the presenter (vulkan_present.cpp dm_present_pillarbox, translation thread):
//   hybrid UI: shows only the centred 16:9 band of the W x 1080 window (where the loading art now is), black beside it; with
//              DM_TL2_WIDE_LOADSTRETCH=1 that band stretched to the whole width
//   UI mode 0: squeezes the whole window (the 1920 canvas stretched to W) back to 16:9; LOADSTRETCH=1 = left as it is
enum { TL_UIMGR = 0x74cfe70, TL_GOT_UIMGR = 0x23a3590, TL_SHOW = 0xc2ad8, TL_TEST = 0x112708, TL_HIDE = 0xc3164,
       U_DIALOG = 0x50, U_LOADING = 0x90, U_LAYER = 0xa0 };
static const uint32_t k_tl_show[4] = {0x52802028 /* mov w8,#0x101 */, 0xb9008a7f, 0xb9018a7f, 0x7900ea68 /* strh w8,[x19,#0x74] */};
static const uint32_t k_tl_test[6] = {0xb0011488, 0xf942c908 /* ldr x8,[GOT 0x23a3590] */, 0xf9404908 /* ldr x8,[x8,#0x90] */, 0xb4000068,
                                      0x3941d108 /* ldrb w8,[x8,#0x74] */, 0x350017c8};
static const uint32_t k_tl_hide[2] = {0xd61f0100, 0x7900ea7f /* strh wzr,[x19,#0x74] */};
static const tw_site k_tl_sites[] = {{TL_SHOW, 4, k_tl_show}, {TL_TEST, 6, k_tl_test}, {TL_HIDE, 2, k_tl_hide}};
static const uint8_t *g_tl_uimgr;
static int g_tl_mode;   // what dm_present_pillarbox answers while the loading screen is drawn
int (*dm_present_pillarbox)(void);   // vulkan_present.cpp present(): 1 = squeeze the window to 16:9, 2 = its centred 16:9 band with bars, 3 = that band stretched
int dm_tl2_splash_band;              // xbg_game.cpp draw_texture: a full-window 16:9 DrawTexture (the boot splash) goes into the centred band, black beside it
#include <sys/uio.h>
#include <sys/syscall.h>
#include <unistd.h>
static int tl_loading_up(void) {
    static int was = -1, nth;
    uint64_t node = *(const volatile uint64_t *)(g_tl_uimgr + U_LOADING); uint8_t vis = 0;
    if (node >= 0x10000 && node < (1ull << 48)) {   // a heap object: read through the kernel (a stale pointer gives EFAULT, never a fault)
        struct iovec l = {&vis, 1}, r = {(void *)(uintptr_t)(node + 0x74), 1};
        if (syscall(SYS_process_vm_readv, getpid(), &l, 1UL, &r, 1UL, 0UL) != 1) vis = 0;
    }
    int on = vis != 0;
    static const char *const how[4] = {"", "window squeezed to 16:9, black side bars", "the centred 16:9 band, black side bars", "the centred band stretched to the screen"};
    if (on != was && nth < 400) { nth++; LOG("[wide] loading screen %s (CConsoleUILoading %#llx +0x74 = %u): %s", on ? "up" : "down", (unsigned long long)node, vis,
                                            on ? how[g_tl_mode & 3] : "whole width again"); }
    was = on;
    return on ? g_tl_mode : 0;
}

// ---- Hybrid UI: the centred 1920 band for everything but the HUD (DM_TL2_WIDE_UI unset / 2)
// PUI::Element (hle_tl2_ui.c): +0x38 parent, +0x40 first / +0x48 last child, +0x50 prev / +0x58 next sibling, +0x60 l,t,r,b laid-out rect
// (canvas units), +0x70 child count. Layout = vt+0x70, base 0x19f5a90 (CConsoleUILoading, CConsoleUIDialog, plain elements):
// rect = parent rect l + anchor (+0x10..+0x1c) x parent size + offset (+0x20..+0x2c), pivot +0x30 for fixed-size axes, then the
// children (vt+0x78). AddChild: vt+0x50 = 0x19f59a0 (first in the list = drawn first), vt+0x58 = 0x19f59e0 (last = drawn on top).
//   * 0x19f59a0 / 0x19f59e0 are replaced by the same list code in C (ui_add_front / ui_add_back), which also notes, for a child of the
//     screen layer, whether its caller is a HUD site (return address below): those keep the whole width;
//   * 0x19f5a90 is entered through ui_layout: a child of the screen layer that is not a HUD one, the loading screen and the modal dialog
//     (children of the root) are laid out in the band [ (W - 1920) / 2, + 1920 ] of their parent's rect instead of all of it. Their
//     subtrees follow (children lay out in their parent's rect). The rest of the original runs from a copy of its 4-instruction prologue.
// HUD sites (return addresses of the AddChild calls; each found as container->AddChild in the class's Show / Create):
//   Hud Show 0x125cfc, Hud Create 0x124a24 / 0x124a9c / 0x124b30 / 0x124bec (damage overlays), LocalPlayerHud 0x12013c,
//   MultiPlayerHud 0x1227ec, TextEventsHud 0xbc77c, TargetEnemyHud 0xbded4, StoryHud 0xbed1c, Loot (item labels) 0x1193ac,
//   CConsoleAutomap 0x129ce0 / 0x129da0 / 0x129e3c / 0x12bfec / 0x12c010 / 0x12c034 / 0x12c44c.
// DM_TL2_WIDE_KEEP=<hex>,<hex>,... more return addresses that keep the whole width (the log names each new screen-layer child's caller).
//
// In-game menus (inventory / character / skills / quests / pet / stash / NPC vendors ...) are NOT screen-layer children: they are all
// CConsoleUIRadialMenu (vt 0x230d6d8; the panels, class Panel, live inside it, docs "Радиальные меню и панели"), whose Show (vt slot 6 =
// 0x137760) puts its root (+0x50, a plain PUI::Element 0x88 bytes made in Create 0x134ec0, anchors 0..1) straight onto the PUI root
// (PUI manager main+0x927a970 +0x308, root->AddChild vt+0x58 = 0x19f59e0, return address 0x13778c) - so it laid out on the whole wide
// canvas and its 1920-designed children (anchor 0 + offsets) sat at the left edge. Root children added from a "band" site
// (k_root_band: 0x13778c; DM_TL2_WIDE_BAND=<hex>,... adds more; the log names every other root child's caller once) are laid out in the
// centred band like the layer screens. The other root children stay as they are: the screen layer +0xa0, loading +0x90 / dialog +0x50
// (banded by pointer), toasts +0xf8 / +0x100, texts, the per-player elements +0x198 (re-added on each menu push, 0x1121d4).
// The wheel itself (PUI::Radial, vt 0x2365628) places its segments without its rect: Layout 0x19fd5e0 (after the base Layout) and
// SetSegments 0x19fd7a0 (from the radial menu 0x13ab70) write each segment centre (+0xa8.. x 0x118, +0x4c x / +0x50 y) from the radial's
// own offsets (+0x20 / +0x28) only, so they would stay at the left while the frame / lists moved. Both enter through C wrappers that,
// after the original, add the band shifts of the radial's ancestors (recorded by ui_layout) to every centre x. Taps follow without
// changes: hle_tl2_ui.c hit-tests the laid-out rects (+0x60, list areas +0x84 come from the rect) and these segment centres, all in
// canvas units of the same wide canvas.
enum { UI_LAYOUT = 0x19f5a90, UI_ADD_FRONT = 0x19f59a0, UI_ADD_BACK = 0x19f59e0, E_PARENT = 0x38, E_FIRST = 0x40, E_LAST = 0x48, E_PREV = 0x50,
       E_NEXT = 0x58, E_COUNT = 0x70,
       P_MGR = 0x927a970, P_ROOT = 0x308, RA_LAYOUT = 0x19fd5e0, RA_SETSEG = 0x19fd7a0, RM_SHOW = 0x137760, RA_SEG = 0xa8, RA_SEGEND = 0xb0,
       SEG_STRIDE = 0x118, SEG_X = 0x4c, VT_RADMENU = 0x230d6d8, RM_ROOT = 0x50, RM_TYPE = 0x2c, RM_TAB = 0x44, RM_PANEL = 0x2d8,
       U_STACK = 0x58, U_TOP = 0x60 };
static const uint32_t k_ui_layout[4] = {0xfc1d0fe8 /* str d8,[sp,#-0x30]! */, 0xa9014ff4, 0xa9027bfd, 0x910083fd /* add x29,sp,#0x20 */};
static const uint32_t k_ui_add_front[14] = {0xf9402008, 0xb40000c8, 0xf9002c28, 0xf9402008, 0xf9002901, 0xf9002001, 0x14000003, 0xa9040401,
                                            0xa9057c3f, 0xf9001c20, 0xb9407008, 0x11000508, 0xb9007008, 0xd65f03c0};
static const uint32_t k_ui_add_back[14] = {0xf9402408, 0xb40000c8, 0xf9002828, 0xf9402408, 0xf9002d01, 0xf9002401, 0x14000003, 0xa9040401,
                                           0xa9057c3f, 0xf9001c20, 0xb9407008, 0x11000508, 0xb9007008, 0xd65f03c0};
// PUI::Radial Layout 0x19fd5e0: prologue (4, no PC-relative ones), bl base Layout, ..., the centre store 0x19fd658 str s0,[x20,#0x4c]
static const uint32_t k_ra_layout[8] = {0xfc1c0fea /* str d10,[sp,#-0x40]! */, 0x6d00a3e9, 0xf9000ff5, 0xa9024ff4, 0xa9037bfd, 0x9100c3fd, 0xaa0003f3,
                                        0x97ffe125 /* bl 0x19f5a90 */};
static const uint32_t k_ra_layout_x[2] = {0xbd004e80 /* str s0,[x20,#0x4c] */, 0xbd400e80};
// PUI::Radial SetSegments 0x19fd7a0 (radial, node): prologue (4, no PC-relative ones); its centre store 0x19fe2c8; its only caller 0x13ab70
static const uint32_t k_ra_setseg[4] = {0xfc170fec /* str d12,[sp,#-0x90]! */, 0x6d012beb, 0x6d0223e9, 0xa9036ffc};
static const uint32_t k_ra_setseg_x[2] = {0xbd004e80 /* str s0,[x20,#0x4c] */, 0xbd400e80};
static const uint32_t k_ra_setseg_call[1] = {0x94630b0c /* bl 0x19fd7a0 */};
// CConsoleUIRadialMenu Show 0x137760: PUI root (GOT 0x23a3678 -> +0x308)->AddChild vt+0x58 (this +0x50), return address 0x13778c
static const uint32_t k_rm_show[11] = {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0x90011373, 0xf9433e73 /* ldr x19,[GOT 0x23a3678] */,
                                       0xf9418668 /* ldr x8,[x19,#0x308] */, 0xf9400109, 0xf9402801 /* ldr x1,[x0,#0x50] */, 0xaa0803e0,
                                       0xf9402d29 /* ldr x9,[x9,#0x58] */, 0xd63f0120 /* blr x9 */};
static const tw_site k_ui_sites[] = {{UI_LAYOUT, 4, k_ui_layout}, {UI_ADD_FRONT, 14, k_ui_add_front}, {UI_ADD_BACK, 14, k_ui_add_back},
                                     {RA_LAYOUT, 8, k_ra_layout}, {0x19fd658, 2, k_ra_layout_x}, {RA_SETSEG, 4, k_ra_setseg},
                                     {0x19fe2c8, 2, k_ra_setseg_x}, {0x13ab70, 1, k_ra_setseg_call}, {RM_SHOW, 11, k_rm_show}};
static uint32_t g_keep[48]; static int g_nkeep;
static const uint32_t k_keep[] = {0x125cfc, 0x124a24, 0x124a9c, 0x124b30, 0x124bec, 0x12013c, 0x1227ec, 0xbc77c, 0xbded4, 0xbed1c, 0x1193ac,
                                  0x129ce0, 0x129da0, 0x129e3c, 0x12bfec, 0x12c010, 0x12c034, 0x12c44c};
static uint32_t g_rband[16]; static int g_nrband;
static const uint32_t k_root_band[] = {0x13778c /* CConsoleUIRadialMenu Show */};
static uintptr_t g_ui_base;
static void (*g_ui_layout_orig)(uint64_t node, const float *pr);   // the copied prologue + jump to 0x19f5aa0
static void (*g_ra_layout_orig)(uint64_t node, const float *pr);   // PUI::Radial Layout, prologue copy + jump to 0x19fd5f0
static void (*g_ra_setseg_orig)(uint64_t radial, uint64_t node);   // PUI::Radial SetSegments, prologue copy + jump to 0x19fd7b0
#define UI_NTAG 128
static struct { uint64_t el; uint32_t wide; } g_tag[UI_NTAG];   // screen-layer children seen being added (game main thread only)
#define UI_NROOT 16
static struct { uint64_t el; uint32_t ra; } g_root[UI_NROOT];  // PUI root children added from a band site (ring; radial menu roots)
static int g_root_next;
#define UI_NBAND 64
static struct { uint64_t el; float shift; } g_band[UI_NBAND];   // nodes ui_layout last laid out in the band, and by how much they moved
static int ui_is_keep(uint32_t ra) { for (int i = 0; i < g_nkeep; i++) if (g_keep[i] == ra) return 1; return 0; }
static int ui_is_rband(uint32_t ra) { for (int i = 0; i < g_nrband; i++) if (g_rband[i] == ra) return 1; return 0; }
static uint64_t ui_root(void) { return *(const uint64_t *)(g_ui_base + P_MGR + P_ROOT); }
static void ui_note_root(uint64_t child, uint32_t ra) {
    int band = ra && ui_is_rband(ra), at = -1;
    for (int i = 0; i < UI_NROOT; i++) if (g_root[i].el == child) { at = i; break; }
    if (band) {
        if (at < 0) { at = g_root_next; g_root_next = (g_root_next + 1) % UI_NROOT; }
        int is_new = g_root[at].el != child;
        g_root[at].el = child; g_root[at].ra = ra;
        static int told; if (is_new && told < 200) { told++; LOG("[wide] UI panel root %#llx added to the PUI root by main+%#x (%s): centred 1920 band",
                                                               (unsigned long long)child, ra, ra == 0x13778c ? "CConsoleUIRadialMenu Show: inventory / character / skills / quests / pet / NPC menus" : "DM_TL2_WIDE_BAND"); }
    } else {
        if (at >= 0) g_root[at].el = 0;   // the address was reused by something else
        static uint32_t seen[32]; static int nseen;
        int known = 0; for (int i = 0; i < nseen; i++) if (seen[i] == ra) { known = 1; break; }
        if (!known && nseen < 32) { seen[nseen++] = ra; LOG("[wide] PUI root child %#llx added by main+%#x: whole width (DM_TL2_WIDE_BAND=%x would centre it)",
                                                            (unsigned long long)child, ra, ra); }
    }
}
static void ui_note(uint64_t parent, uint64_t child, uintptr_t lr) {
    uint64_t layer = *(const uint64_t *)(g_tl_uimgr + U_LAYER);
    uint32_t ra = lr >= g_ui_base && lr - g_ui_base < 0x8000000 ? (uint32_t)(lr - g_ui_base) : 0;
    if (parent && parent == ui_root()) { ui_note_root(child, ra); return; }
    if (!layer || parent != layer) return;
    int wide = ra && ui_is_keep(ra), slot = -1;
    for (int i = 0; i < UI_NTAG; i++) { if (g_tag[i].el == child) { slot = i; break; } if (slot < 0 && !g_tag[i].el) slot = i; }
    if (slot < 0) slot = (int)(child >> 4) % UI_NTAG;   // full (should not happen: a few dozen screens): overwrite one
    int is_new = g_tag[slot].el != child || g_tag[slot].wide != (uint32_t)wide;
    g_tag[slot].el = child; g_tag[slot].wide = (uint32_t)wide;
    static int told; if (is_new && told < 200) { told++; LOG("[wide] UI screen %#llx added by main+%#x: %s", (unsigned long long)child, ra, wide ? "HUD, whole width" : "centred 1920 band"); }
}
static void ui_add_front(uint64_t p, uint64_t c) {   // = 0x19f59a0
    uintptr_t lr = (uintptr_t)__builtin_return_address(0);
    uint64_t f = *(uint64_t *)(p + E_FIRST);
    if (f) { *(uint64_t *)(c + E_NEXT) = f; *(uint64_t *)(f + E_PREV) = c; *(uint64_t *)(p + E_FIRST) = c; }
    else { *(uint64_t *)(p + E_FIRST) = c; *(uint64_t *)(p + E_LAST) = c; *(uint64_t *)(c + E_PREV) = 0; *(uint64_t *)(c + E_NEXT) = 0; }
    *(uint64_t *)(c + E_PARENT) = p; (*(int32_t *)(p + E_COUNT))++;
    ui_note(p, c, lr);
}
static void ui_add_back(uint64_t p, uint64_t c) {   // = 0x19f59e0
    uintptr_t lr = (uintptr_t)__builtin_return_address(0);
    uint64_t l = *(uint64_t *)(p + E_LAST);
    if (l) { *(uint64_t *)(c + E_PREV) = l; *(uint64_t *)(l + E_NEXT) = c; *(uint64_t *)(p + E_LAST) = c; }
    else { *(uint64_t *)(p + E_FIRST) = c; *(uint64_t *)(p + E_LAST) = c; *(uint64_t *)(c + E_PREV) = 0; *(uint64_t *)(c + E_NEXT) = 0; }
    *(uint64_t *)(c + E_PARENT) = p; (*(int32_t *)(p + E_COUNT))++;
    ui_note(p, c, lr);
}
static void ui_band_set(uint64_t node, float shift) {
    int at = -1;
    for (int i = 0; i < UI_NBAND; i++) { if (g_band[i].el == node) { at = i; break; } if (at < 0 && !g_band[i].el) at = i; }
    if (shift == 0.f) { if (at >= 0 && g_band[at].el == node) g_band[at].el = 0; return; }
    if (at < 0) at = (int)(node >> 4) % UI_NBAND;
    g_band[at].el = node; g_band[at].shift = shift;
}
static float ui_band_of(uint64_t node) { for (int i = 0; i < UI_NBAND; i++) if (g_band[i].el == node) return g_band[i].shift; return 0.f; }
// The radial menu on top of the UI manager's stack whose root is `root` (0 = none): for the log only (main thread, live objects)
static uint64_t ui_top_radial(uint64_t root) {
    int32_t top = *(const int32_t *)(g_tl_uimgr + U_TOP);
    uint64_t st = *(const uint64_t *)(g_tl_uimgr + U_STACK);
    for (int i = top; st && i >= 0 && i > top - 4 && i < 64; i--) {
        uint64_t mn = *(const uint64_t *)(st + 8ull * i);
        if (mn && *(const uint64_t *)mn == (uint64_t)(g_ui_base + VT_RADMENU) && *(const uint64_t *)(mn + RM_ROOT) == root) return mn;
    }
    return 0;
}
static const char *ui_panel_name(uint64_t vt) {
    static const struct { uint32_t vt; const char *n; } k[] = {{0x23036f8, "PanelEquipment (inventory / pet)"}, {0x2302200, "PanelSkills"},
        {0x2303870, "PanelAssignStats (character)"}, {0x2303240, "PanelQuests"}};
    for (size_t i = 0; i < sizeof k / sizeof *k; i++) if (vt == (uint64_t)(g_ui_base + k[i].vt)) return k[i].n;
    return vt ? "panel" : "no panel yet (wheel)";
}
// One line per change of (radial root, tab, hosted panel): which in-game menu / panel now sits in the band, and where
static void ui_log_panel(uint64_t root) {
    static uint64_t last_root, last_vt; static int last_tab = -2, told;
    uint64_t mn = ui_top_radial(root), pn = mn ? *(const uint64_t *)(mn + RM_PANEL) : 0, vt = pn ? *(const uint64_t *)pn : 0;
    int tab = mn ? *(const int32_t *)(mn + RM_TAB) : -1;
    if ((root == last_root && vt == last_vt && tab == last_tab) || told >= 300) return;
    told++; last_root = root; last_vt = vt; last_tab = tab;
    uint32_t vto = vt >= g_ui_base && vt - g_ui_base < 0x8000000 ? (uint32_t)(vt - g_ui_base) : 0;
    const float *b = (const float *)(root + 0x60), *c = (const float *)(ui_root() + 0x60);
    LOG("[wide] centred 1920 band: radial menu %#llx (type %d, tab %d), %s (vt main+%#x), root %#llx at x %.0f..%.0f of canvas %.0f..%.0f",
        (unsigned long long)mn, mn ? *(const int32_t *)(mn + RM_TYPE) : -1, tab, ui_panel_name(vt), vto, (unsigned long long)root, b[0], b[2], c[0], c[2]);
}
static void ui_layout(uint64_t node, const float *pr) {   // = 0x19f5a90
    float w = pr[2] - pr[0];
    if (w > 1920.5f) {
        uint64_t parent = *(const uint64_t *)(node + E_PARENT), layer = *(const uint64_t *)(g_tl_uimgr + U_LAYER);
        int band = 0, panel = 0;
        if (parent && parent == layer) {
            band = 1;
            for (int i = 0; i < UI_NTAG; i++) if (g_tag[i].el == node) { band = !g_tag[i].wide; break; }
        } else if (parent && parent == ui_root()) {
            for (int i = 0; i < UI_NROOT; i++) if (g_root[i].el == node) { band = panel = 1; break; }
            if (!band && (node == *(const uint64_t *)(g_tl_uimgr + U_LOADING) || node == *(const uint64_t *)(g_tl_uimgr + U_DIALOG))) band = 1;
        } else if (node == *(const uint64_t *)(g_tl_uimgr + U_LOADING) || node == *(const uint64_t *)(g_tl_uimgr + U_DIALOG)) band = 1;
        if (band) {
            float sh = floorf((w - 1920.f) * 0.5f), l = pr[0] + sh, b[4] = {l, pr[1], l + 1920.f, pr[3]};
            ui_band_set(node, sh);
            g_ui_layout_orig(node, b);
            if (panel) ui_log_panel(node);
            return;
        }
    }
    ui_band_set(node, 0.f);
    g_ui_layout_orig(node, pr);
}
// [TL2] hle_tl2_icons.c: is this screen-layer child a HUD one (keeps the whole width)? -1 = not known (hybrid band not installed),
// 1 = added by a HUD site, 0 = any other screen (menus, panels, dialogs; also a child never seen being added, as ui_layout treats it)
int dm_tl2_ui_screen_is_hud(uint64_t el);
int dm_tl2_ui_screen_is_hud(uint64_t el) {
    if (!g_ui_layout_orig) return -1;
    for (int i = 0; i < UI_NTAG; i++) if (g_tag[i].el == el) return g_tag[i].wide ? 1 : 0;
    return 0;
}
// The wheel: move every segment centre by the band shifts of the radial's ancestors (its own Layout / SetSegments placed them from its
// offsets alone, as if every ancestor sat at x 0)
static void ui_radial_fix(uint64_t ra, const char *who) {
    float dx = 0.f; uint64_t p = *(const uint64_t *)(ra + E_PARENT), top = 0;
    for (int d = 0; p && d < 48; d++, p = *(const uint64_t *)(p + E_PARENT)) {
        float s = ui_band_of(p); dx += s;
        if (s != 0.f) for (int i = 0; i < UI_NROOT; i++) if (g_root[i].el == p) top = p;
    }
    if (top) ui_log_panel(top);   // SetSegments runs on a tab / ring change, often without a relayout of the menu
    if (dx == 0.f) return;
    uint64_t s0 = *(const uint64_t *)(ra + RA_SEG), s1 = *(const uint64_t *)(ra + RA_SEGEND);
    int n = s1 > s0 && (s1 - s0) / SEG_STRIDE <= 256 ? (int)((s1 - s0) / SEG_STRIDE) : 0;
    for (int i = 0; i < n; i++) *(float *)(s0 + (uint64_t)i * SEG_STRIDE + SEG_X) += dx;
    static uint64_t last; static int last_n, told;
    if ((ra != last || n != last_n) && told < 200) { told++; last = ra; last_n = n;
        LOG("[wide] radial wheel %#llx (%s): %d segment centres moved +%.0f with the centred band", (unsigned long long)ra, who, n, dx); }
}
static void ui_ra_layout(uint64_t node, const float *pr) { g_ra_layout_orig(node, pr); ui_radial_fix(node, "Layout"); }      // = 0x19fd5e0
static void ui_ra_setseg(uint64_t ra, uint64_t node) { g_ra_setseg_orig(ra, node); ui_radial_fix(ra, "SetSegments"); }    // = 0x19fd7a0
static int ui_jump(module_t *m, uint32_t off, void *to) {   // ldr x16, #8; br x16; .quad to
    uint32_t w[4] = {0x58000050, 0xd61f0200, (uint32_t)(uintptr_t)to, (uint32_t)((uint64_t)(uintptr_t)to >> 32)};
    return tw_poke(m, off, w, 4);
}
static int ui_band_install(module_t *m) {
    for (size_t i = 0; i < sizeof k_ui_sites / sizeof k_ui_sites[0]; i++)
        if (m->text_end < k_ui_sites[i].off + 4 * k_ui_sites[i].n || memcmp(m->base + k_ui_sites[i].off, k_ui_sites[i].want, 4 * k_ui_sites[i].n)) {
            LOG("【红】[wide] main+%#x differs: no centred UI band", k_ui_sites[i].off); return -1; }
    g_nkeep = 0;
    for (size_t i = 0; i < sizeof k_keep / sizeof k_keep[0]; i++) g_keep[g_nkeep++] = k_keep[i];
    const char *ke = getenv("DM_TL2_WIDE_KEEP");
    for (const char *s = ke; s && *s && g_nkeep < (int)(sizeof g_keep / sizeof g_keep[0]);) {
        char *end; unsigned long v = strtoul(s, &end, 16); if (end == s) break;
        if (v) g_keep[g_nkeep++] = (uint32_t)v;
        s = *end == ',' ? end + 1 : end;
    }
    g_nrband = 0;
    for (size_t i = 0; i < sizeof k_root_band / sizeof k_root_band[0]; i++) g_rband[g_nrband++] = k_root_band[i];
    const char *be = getenv("DM_TL2_WIDE_BAND");
    for (const char *s = be; s && *s && g_nrband < (int)(sizeof g_rband / sizeof g_rband[0]);) {
        char *end; unsigned long v = strtoul(s, &end, 16); if (end == s) break;
        if (v) g_rband[g_nrband++] = (uint32_t)v;
        s = *end == ',' ? end + 1 : end;
    }
    if (m->size < P_MGR + P_ROOT + 8) { LOG("【红】[wide] main is smaller than the PUI manager main+%#x: no centred UI band", P_MGR); return -1; }
    // each original's prologue (4 instructions, no PC-relative ones) + a jump to the rest of it, in a page of their own
    uint32_t *t = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (t == MAP_FAILED) { LOG("【红】[wide] mmap failed: no centred UI band"); return -1; }
    static const struct { uint32_t off; const uint32_t *pro; } tr[3] = {{UI_LAYOUT, k_ui_layout}, {RA_LAYOUT, k_ra_layout}, {RA_SETSEG, k_ra_setseg}};
    for (int i = 0; i < 3; i++) {
        uint32_t *s = t + 8 * i; uint64_t rest = (uint64_t)(uintptr_t)(m->base + tr[i].off + 16);
        memcpy(s, tr[i].pro, 16); s[4] = 0x58000050 /* ldr x16,#8 */; s[5] = 0xd61f0200 /* br x16 */; s[6] = (uint32_t)rest; s[7] = (uint32_t)(rest >> 32);
    }
    if (mprotect(t, 0x4000, PROT_READ | PROT_EXEC)) { LOG("【红】[wide] mprotect RX failed: no centred UI band"); munmap(t, 0x4000); return -1; }
    __builtin___clear_cache((char *)t, (char *)(t + 24));
    g_ui_layout_orig = (void (*)(uint64_t, const float *))(void *)t;
    g_ra_layout_orig = (void (*)(uint64_t, const float *))(void *)(t + 8);
    g_ra_setseg_orig = (void (*)(uint64_t, uint64_t))(void *)(t + 16);
    g_ui_base = (uintptr_t)m->base;
    if (ui_jump(m, UI_ADD_FRONT, (void *)ui_add_front) || ui_jump(m, UI_ADD_BACK, (void *)ui_add_back) || ui_jump(m, UI_LAYOUT, (void *)ui_layout) ||
        ui_jump(m, RA_LAYOUT, (void *)ui_ra_layout) || ui_jump(m, RA_SETSEG, (void *)ui_ra_setseg)) {
        LOG("【红】[wide] mprotect RW failed: the UI hooks may be half in"); return -1; }
    LOG("[wide] hybrid UI: HUD on the whole %s canvas, screens / dialog / loading screen in the centred 1920 band (Layout main+%#x, AddChild "
        "main+%#x / +%#x; %d HUD sites); in-game menus (CConsoleUIRadialMenu roots on the PUI root, %d band sites) centred too, wheel "
        "segments follow (Radial Layout main+%#x, SetSegments main+%#x)", "wide", UI_LAYOUT, UI_ADD_FRONT, UI_ADD_BACK, g_nkeep, g_nrband,
        RA_LAYOUT, RA_SETSEG);
    return 0;
}

static int tl_uimgr_ok(module_t *m) {
    for (size_t i = 0; i < sizeof k_tl_sites / sizeof k_tl_sites[0]; i++)
        if (m->text_end < k_tl_sites[i].off + 4 * k_tl_sites[i].n || memcmp(m->base + k_tl_sites[i].off, k_tl_sites[i].want, 4 * k_tl_sites[i].n)) {
            LOG("【红】[wide] main+%#x differs: UI manager not recognised", k_tl_sites[i].off); return 0; }
    if (m->size < TL_UIMGR + 0x200 || *(const uint64_t *)(m->base + TL_GOT_UIMGR) != (uint64_t)(uintptr_t)(m->base + TL_UIMGR)) {
        LOG("【红】[wide] GOT main+%#x does not point at the UI manager main+%#x", TL_GOT_UIMGR, TL_UIMGR); return 0; }
    g_tl_uimgr = m->base + TL_UIMGR;
    return 1;
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
    const char *ue = getenv("DM_TL2_WIDE_UI");
    int ui = !ue || !*ue ? 2 : atoi(ue); if (ui < 0 || ui > 2) ui = 2;   // 2 hybrid (default), 1 wide canvas only, 0 1920 canvas stretched
    int ok = tl_uimgr_ok(m);
    if (ui == 2 && !ok) { LOG("【红】[wide] hybrid UI needs the UI manager: the 1920 canvas stays (stretched)"); ui = 0; }
    const uint32_t win_dk = movz_w(27, w), win_hh = movz_w(27, wh), init_dk = movz_w(8, w), init_hh = movz_w(20, wh),
                   msg_hh = movz_w(21, wh), msg_dk = movz_w(2, w), canvas = movz_w(3, w);
    const uint32_t snap[3] = {0xb9000061 /* str w1,[x3] */, 0xb9000082 /* str w2,[x4] */, 0xd65f03c0 /* ret */};
    if (tw_poke(m, TW_WIN + 12, &win_dk, 1) || tw_poke(m, TW_WIN + 28, &win_hh, 1) || tw_poke(m, TW_INIT + 16, &init_dk, 1) ||
        tw_poke(m, TW_INIT + 40, &init_hh, 1) || tw_poke(m, TW_MSG_HH + 4, &msg_hh, 1) || tw_poke(m, TW_MSG_DK + 20, &msg_dk, 1) ||
        tw_poke(m, TW_SNAP, snap, 3)) {
        LOG("【红】[wide] mprotect RW failed part-way: the game may mix sizes"); return; }
    if (ui == 2 && ui_band_install(m)) ui = 0;   // the band hooks refused: fall back to the stretched 1920 canvas
    if (ui >= 1 && tw_poke(m, TW_CANVAS + 8, &canvas, 1)) { LOG("【红】[wide] canvas patch failed: the 1920 canvas stays"); ui = 0; }
    static const char *const uis[3] = {"1920x1080 scaled to the window (stretched; DM_TL2_WIDE_UI=0)", "widened, no band (DM_TL2_WIDE_UI=1, experiment)",
                                       "widened: HUD on the whole width, the rest in the centred 1920 band (hybrid, default)"};
    LOG("[wide] TL2 window 1920x1080 -> %ux1080, handheld 1280x720 -> %ux720, aspect %.4f (Hor+: cameras take the viewport's aspect); "
        "video settings WIDTH/HEIGHT the same (main+%#x / +%#x), display-mode snap main+%#x off; console UI canvas %s",
        w, wh, w / 1080.0, TW_INIT + 16, TW_MSG_DK + 20, TW_SNAP, uis[ui]);
    dm_tl2_splash_band = 1;
    const char *ls = getenv("DM_TL2_WIDE_LOADSTRETCH"); int stretch = ls && *ls && !(*ls == '0' && !ls[1]);
    g_tl_mode = ui == 2 ? (stretch ? 3 : 2) : ui == 0 ? (stretch ? 0 : 1) : 0;
    if (ok && g_tl_mode) { dm_present_pillarbox = tl_loading_up; LOG("[wide] loading screens: %s while CConsoleUILoading (UI manager main+%#x +0x90) is drawn%s",
        g_tl_mode == 1 ? "16:9 with black side bars (window squeezed)" : g_tl_mode == 2 ? "16:9 with black side bars (the centred band)" : "the centred band stretched to the screen",
        TL_UIMGR, stretch ? " (DM_TL2_WIDE_LOADSTRETCH=1)" : "; DM_TL2_WIDE_LOADSTRETCH=1 stretches them"); }
    else LOG("[wide] loading screens: as the UI draws them (%s)", stretch && ui == 0 ? "DM_TL2_WIDE_LOADSTRETCH=1: stretched" : "wide canvas experiment");
}
#endif
