// [TL2] Tap = click on the console menus (DM_TL2_POINTER, default off until checked on the phone; docs/tl2/TL2_PORT.md "Тап по пунктам меню").
// TL2 Switch keeps Panic Button's console UI "PUI" (RTTI N3PUI7ElementE / 4List / 6Button / 6Radial / 7Manager); the PC UI is gone from the
// code (CEGUI 0.6.2 is linked, but System / layout loading were dead-stripped: the static String objects for System::EventGUISheetChanged
// (main+0x7680168) and the layout handler's "GUILayout" (main+0x76826d0) are referenced only by their own static initializer 0x179d850;
// WindowManager::createWindow 0x17d5890 has no caller outside CEGUI; no PC GUI class in RTTI; the PAKs still carry MEDIA/UI/*.LAYOUT, unused).
// So a tap is a click through the console menus' own code, with no pad emulation:
//   * the item under the finger is found in the PUI tree (laid-out rects, below);
//   * the owning menu is the top of the console UI stack; its focus is moved the way its own navigation code moves it (SetHighlighted(old, 0),
//     focus index = item, SetHighlighted(new, 1): MainMenu 0x145954..0x1459b8, ButtonMenu 0x739690) or, for list menus, with the list's
//     own SetSelected (0x1a02460, what their Up / Down code calls);
//   * the activation is the menu's own: the UI manager's dispatch (vt slot 0 = 0x143800, hands the event to the top menu) gets the logical
//     Accept event {type 0 press, code 1} (IsAccept 0x1139f0), so the confirm branch of that menu acts on the item now focused (MainMenu
//     0x145b60: switch on the item id; it needs the button's enabled byte +0xc3). The outer quarters of a button row get the logical Left /
//     Right {0, 4} / {0, 5} instead (value rows: MainMenu item types 0x26..0x28 / 0x36, character create).
//   * button menus: one tap = focus + activate. List rows: a tap selects (= hover / details), a tap on the selected row activates.
// Codes: IsUp 0x113950 = 6, IsDown 0x1139a0 = 7, IsAccept 0x1139f0 = 1, IsBack 0x13acd0 = 0; event {u32 type, u32 code, u32 axis, f32 value}.
//
// PUI layout (main v5 build a0c6c2d2, offsets = main.raw; all checked word for word at the first poll):
//   PUI::Manager   static main+0x927a970 (GOT 0x23a3678; init 0x19fef80 from the UI setup 0x140f00 with 1920x1080): +0x2f8 / +0x2fc canvas
//                  width / height (0x19ff3d8), +0x308 root element (0x19ff450); Render 0x19f5090 draws the root twice, pass 0 then pass 1.
//   PUI::Element   vtable main+0x23651f8: +0x38 parent, +0x40 first child, +0x58 next sibling (children drawn first to last = bottom to top),
//                  +0x60 l,t,r,b laid-out rect in canvas units (Layout vt+0x70 = 0x19f5a90), +0x74 drawn (Render 0x19f57b0 returns before
//                  drawing it and its children while 0), +0x80 layer (1 = second pass, above every pass-0 element).
//   PUI::List      vtable main+0x23656d8 (Draw 0x1a01ab0): +0x84 l,t,r,b row area, +0x100 row height (int), +0x104 rows, +0x108 selected
//                  (SetSelected 0x1a02460, GetSelected 0x1a02600), +0x10c scroll, +0x116 shows the selection; row i: y = t + i*h - scroll.
//   PUI::Button    vtable main+0x2365838: +0xc4 highlighted (SetHighlighted 0x1a05020), +0xc3 enabled (0x1a05060; Accept is refused while 0).
// Menus (CConsoleUIMenu subclasses, vt slot 0 = HandleInput(menu, player, event)):
//   button family A: MainMenu vt 0x230ddb8 (0x1457e0), Network vt 0x2309e28 (0x112ab0), CreateCharacter vt 0x232b008 (0x75fde0):
//                    items +0x48 (0x28-byte entries, id at +0), count +0x58, focus +0x5c, Button* array +0x98.
//   button family B: Options vt 0x23043d8 (0xc05f0), PlayerInvite vt 0x232a938 (0x73bb10), PartyMenu vt 0x232b2e8 (0x766fa0), base ButtonMenu
//                    (build 0x739210): items +0x30, count +0x38, focus +0x3c, Button* vector +0x50.
//   list menus:      any top menu holding the tapped PUI::List pointer in its first 0x400 bytes (CharacterList family 0x757310 / 0x7588b0 /
//                    0x762420 with the list at +0x40, Waypoints 0x765ab0, SkillsRespec 0x771580, PlayerInvite / PartyMenu lists): they read
//                    GetSelected when Accept comes.
//   Not covered: radial menus (CConsoleUIRadialMenu vt 0x230d6d8, stick angle), the in-game panels (class Panel vt 0x2303d60: PanelEquipment
//                0x23036f8, PanelSkills, PanelMerchant..., not CConsoleUIMenu, own input), tabs (shoulder codes), custom-drawn grids.
//
// "A menu is open" (dm_tl2_menu_open, published every npad read; clicks happen only while it holds, never in plain gameplay): the console UI
// manager (static main+0x74cfe70, GOT 0x23a3590) keeps its menu stack at +0x58 (array) with the top index at +0x60 (-1 = empty; pop
// 0x111e10). Its HandleInput 0x143800 gives the event to +0x80 (the gameplay HUD receiver) only while the stack is empty (0x1438f8), and
// first to the modal element +0x50 while that is drawn (+0x74). So: open = stack not empty or the modal drawn, and no loading screen (+0x90).
//   DM_TL2_POINTER=1   dm_tl2_ui_tap(x, y) (fractions of the game picture, any thread) queues a click; the npad read (the game's input
//                      update 0x13fd40 -> 0x13ea60, main thread, right before the game dispatches its own input events) performs it.
//   DM_TL2_UIDUMP=1    the file dm/ui-dump-now makes dm/tl2-ui-dump.tsv (visible PUI elements + the menu stack) and is deleted; =2 hidden
//                      elements too. A file dm/ui-tap-now holding "x y" queues a click (test without the front end; checked every 30 polls).
#ifdef DM_GAME_TL2
#include "hle.h"
#include "dm.h"
#include "dm_input.h"
#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

enum {
    P_MGR = 0x927a970, P_GOT_MGR = 0x23a3678,
    P_VT_ELEM = 0x23651f8, P_VT_LIST = 0x23656d8, P_VT_BUTTON = 0x2365838, P_VT_RADIAL = 0x2365628,
    P_LIST_DRAW = 0x1a01ab0, P_ELEM_DRAW = 0x19f5de0, P_BUTTON_DTOR = 0x1a046c0, P_RADIAL_DRAW = 0x19fce90,
    U_MGR = 0x74cfe70, U_GOT_MGR = 0x23a3590, U_STACK = 0x58, U_TOP = 0x60, U_MODAL = 0x50, U_LOADING = 0x90, U_HUD = 0x80,
    M_W = 0x2f8, M_H = 0x2fc, M_ROOT = 0x308,
    E_PARENT = 0x38, E_FIRST = 0x40, E_NEXT = 0x58, E_RECT = 0x60, E_DRAWN = 0x74, E_LAYER = 0x80,
    L_AREA = 0x84, L_ROWH = 0x100, L_COUNT = 0x104, L_SEL = 0x108, L_SCROLL = 0x10c, L_SHOWSEL = 0x116,
    B_ENABLED = 0xc3, B_HILITE = 0xc4,
    F_SETSEL = 0x1a02460, F_SETHI = 0x1a05020, F_DISPATCH = 0x143800, VT_UIMGR = 0x230d7f0,
    VT_MAIN = 0x230ddb8, VT_NETWORK = 0x2309e28, VT_CREATE = 0x232b008, VT_OPTIONS = 0x23043d8, VT_INVITE = 0x232a938, VT_PARTY = 0x232b2e8,
    EV_ACCEPT = 1, EV_LEFT = 4, EV_RIGHT = 5,
    MAX_EL = 4096, MAX_DEPTH = 48,
};
static const struct { uint32_t off, n, w[14]; } k_sites[] = {
    {0x1a02460, 14, {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0xaa0003f3, 0x3100043f, 0x5400006a, 0x12800001, 0x14000005, 0xb9410668 /* ldr w8,[x19,#0x104] */,
                     0x51000509, 0x6b01011f, 0x1a89c021, 0xaa1303e0, 0xb9010a61 /* str w1,[x19,#0x108] */}},
    {0x1a01dac, 4, {0xb9408e88 /* ldr w8,[x20,#0x8c] */, 0xf8484289 /* ldur x9,[x20,#0x84] */, 0xf90013e9, 0xb9002be8}},
    {0x1a01e08, 3, {0xb9410688 /* ldr w8,[x20,#0x104] */, 0x7100051f, 0x540003eb}},
    {0x1a01e94, 1, {0x39445a88 /* ldrb w8,[x20,#0x116] */}},
    {0x1a05020, 6, {0xf9404808, 0x1e2e1000, 0x1e2703e1, 0x12000029, 0xb900c81f, 0x39031009 /* strb w9,[x0,#0xc4] */}},
    {0x19f57c0, 3, {0x3941d008 /* ldrb w8,[x0,#0x74] */, 0x34000508, 0xb9408008 /* ldr w8,[x0,#0x80] */}},
    {0x19f5ac0, 4, {0x1e252842, 0x2d431c05, 0xbd006002 /* str s2,[x0,#0x60] */, 0xbd400022}},
    {0x19ff3d8, 2, {0xb902fab4 /* str w20,[x21,#0x2f8] */, 0xb902feb3 /* str w19,[x21,#0x2fc] */}},
    {0x19ff450, 1, {0xf90186b6 /* str x22,[x21,#0x308] */}},
    {0x143884, 3, {0xf9402a60 /* ldr x0,[x19,#0x50] */, 0x3941d008 /* ldrb w8,[x0,#0x74] */, 0x34000108}},
    {0x1438f8, 4, {0xf9404260 /* ldr x0,[x19,#0x80] */, 0xb4000140, 0xb9406268 /* ldr w8,[x19,#0x60] */, 0x3100051f /* cmn w8,#1 */}},
    {0x143930, 2, {0xf9402e68 /* ldr x8,[x19,#0x58] */, 0xf8767900 /* ldr x0,[x8,x22,lsl #3] */}},
    {0x143800, 8, {0xf81c0ff7, 0xa90157f6, 0xa9024ff4, 0xa9037bfd, 0x9100c3fd, 0xaa0203f4, 0x2a0103f5, 0xaa0003f3}},   // UI manager HandleInput
    {0x1139f0, 4, {0x29402428, 0x7100053f, 0x1a9f17e9, 0x7100011f}},   // IsAccept: code 1, type 0
    {0x145830, 1, {0xb9805c18 /* ldrsw x24,[x0,#0x5c] */}}, {0x145890, 1, {0xf9404e68 /* ldr x8,[x19,#0x98] */}},   // MainMenu focus / buttons
    {0x739690, 8, {0xb9403808 /* ldr w8,[x0,#0x38] */, 0x34000268, 0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0xb9803c08 /* ldrsw x8,[x0,#0x3c] */, 0x2a0103f4, 0xaa0003f3}},
    {0x114860, 8, {0xb9405808 /* ldr w8,[x0,#0x58] */, 0x34000268, 0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0xb9805c08 /* ldrsw x8,[x0,#0x5c] */, 0x2a0103f4, 0xaa0003f3}},
};

static uint8_t *g_base; static size_t g_size; static int g_ok = -1, g_mode, g_dump;
static char g_dir[1100];

// ---- safe guest reads (a stale pointer reads as absent, never faults)
int dm_guest_heap_readable(uint64_t a, size_t n);
static int rd(uint64_t a, void *out, size_t n) {
    if (a < 0x10000 || a >= (1ull << 48) || n > 4096) return 0;
    if (dm_guest_heap_readable(a, n)) { memcpy(out, (const void *)(uintptr_t)a, n); return 1; }
    uint64_t b = (uint64_t)(uintptr_t)g_base;
    if (a >= b && n <= g_size && a - b <= g_size - n) { memcpy(out, (const void *)(uintptr_t)a, n); return 1; }
    struct iovec l = {out, n}, r = {(void *)(uintptr_t)a, n};
    return syscall(SYS_process_vm_readv, getpid(), &l, 1UL, &r, 1UL, 0UL) == (ssize_t)n;
}
static uint64_t rq(uint64_t a) { uint64_t v = 0; return rd(a, &v, 8) ? v : 0; }
static uint64_t img(uint32_t off) { return (uint64_t)(uintptr_t)g_base + off; }

static int setup(void) {
    if (g_ok >= 0) return g_ok;
    g_ok = 0;
    const char *e = getenv("DM_TL2_POINTER"); g_mode = e && *e ? atoi(e) : 0;
    e = getenv("DM_TL2_UIDUMP"); g_dump = e && *e ? atoi(e) : 0;
    if (g_mode <= 0 && g_dump <= 0) return 0;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) { g_base = g_mod[i].base; g_size = g_mod[i].size; }
    if (!g_base || g_size < P_MGR + 0x340) { LOG("【红】[tl2ui] no main module: pointer off"); return 0; }
    for (size_t i = 0; i < sizeof k_sites / sizeof *k_sites; i++)
        if (memcmp(g_base + k_sites[i].off, k_sites[i].w, 4 * k_sites[i].n)) {
            LOG("【红】[tl2ui] main+%#x differs: not TL2 v5 main (build a0c6c2d2), pointer / dump off", k_sites[i].off); return 0; }
    if (rq(img(P_GOT_MGR)) != img(P_MGR) || rq(img(U_GOT_MGR)) != img(U_MGR) || rq(img(P_VT_LIST + 0x10)) != img(P_LIST_DRAW) || rq(img(P_VT_ELEM + 0x10)) != img(P_ELEM_DRAW) ||
        rq(img(P_VT_BUTTON + 8)) != img(P_BUTTON_DTOR) || rq(img(P_VT_RADIAL + 0x10)) != img(P_RADIAL_DRAW) || rq(img(VT_UIMGR)) != img(F_DISPATCH) ||
        rq(img(VT_MAIN)) != img(0x1457e0) || rq(img(VT_NETWORK)) != img(0x112ab0) || rq(img(VT_CREATE)) != img(0x75fde0) ||
        rq(img(VT_OPTIONS)) != img(0xc05f0) || rq(img(VT_INVITE)) != img(0x73bb10) || rq(img(VT_PARTY)) != img(0x766fa0)) {
        LOG("【红】[tl2ui] PUI vtables / GOT do not match: pointer / dump off"); return 0; }
    e = getenv("DM_RUNTIME_DIR"); snprintf(g_dir, sizeof g_dir, "%s", e && *e ? e : "dm");
    LOG("[tl2ui] on: pointer %d, dump %d (PUI manager main+%#x), game input thread tid %d", g_mode, g_dump, P_MGR, (int)gettid());
    return g_ok = 1;
}

// ---- snapshot of the PUI tree (visible part, or all with dump 2)
enum { K_ELEM, K_LIST, K_BUTTON, K_RADIAL, K_OTHER };
static const char *const k_kind[] = {"Element", "List", "Button", "Radial", "other"};
typedef struct {
    uint64_t a, vt; int parent, depth, kind, layer, vis;
    float r[4];
    float area[4]; int rowh, rows, sel, showsel; float scroll;   // List
    int hilite, enabled;                                            // Button
} el;
static el g_el[MAX_EL];
static int kind_of(uint64_t vt) {
    if (vt == img(P_VT_LIST)) return K_LIST;
    if (vt == img(P_VT_BUTTON)) return K_BUTTON;
    if (vt == img(P_VT_RADIAL)) return K_RADIAL;
    if (vt == img(P_VT_ELEM)) return K_ELEM;
    return K_OTHER;
}
static int snapshot(int all, float *cw, float *ch) {
    uint64_t mgr = img(P_MGR), root = rq(mgr + M_ROOT);
    int32_t wh[2] = {0, 0}; rd(mgr + M_W, wh, 8);
    *cw = wh[0] > 0 ? (float)wh[0] : 1920.f; *ch = wh[1] > 0 ? (float)wh[1] : 1080.f;
    if (!root) return 0;
    static struct { uint64_t a; int parent, depth, vis; } st[MAX_EL]; static uint64_t kids[512];   // the input thread only; off the guest stack
    int sp = 0, n = 0;
    st[sp].a = root; st[sp].parent = -1; st[sp].depth = 0; st[sp].vis = 1; sp++;
    while (sp > 0 && n < MAX_EL) {
        sp--; uint64_t a = st[sp].a; int parent = st[sp].parent, depth = st[sp].depth, pvis = st[sp].vis;
        uint8_t raw[0x120] = {0};
        if (!rd(a, raw, 0x88)) continue;   // the base element; a list's fields are read below
        el *e = &g_el[n]; memset(e, 0, sizeof *e);
        e->a = a; memcpy(&e->vt, raw, 8); e->parent = parent; e->depth = depth; e->kind = kind_of(e->vt);
        memcpy(e->r, raw + E_RECT, 16); memcpy(&e->layer, raw + E_LAYER, 4);
        e->vis = pvis && raw[E_DRAWN];
        if (!e->vis && !all) continue;
        if (e->kind == K_LIST) {
            if (!rd(a + 0x88, raw + 0x88, 0x120 - 0x88)) continue;
            memcpy(e->area, raw + L_AREA, 16); memcpy(&e->rowh, raw + L_ROWH, 4); memcpy(&e->rows, raw + L_COUNT, 4);
            memcpy(&e->sel, raw + L_SEL, 4); memcpy(&e->scroll, raw + L_SCROLL, 4); e->showsel = raw[L_SHOWSEL];
        } else if (e->kind == K_BUTTON) {
            uint8_t hb[2] = {0, 0}; if (rd(a + B_ENABLED, hb, 2)) { e->enabled = hb[0]; e->hilite = hb[1]; }
        }
        int me = n++;
        if (depth + 1 >= MAX_DEPTH) continue;
        // children first to last; pushed in reverse so the DFS visits them in drawing order (later = on top)
        int nk = 0;
        for (uint64_t c = rq(a + E_FIRST); c && nk < 512; c = rq(c + E_NEXT)) { kids[nk++] = c; if (nk > 1 && c == kids[0]) break; }
        for (int k = nk - 1; k >= 0 && sp < (int)(sizeof st / sizeof *st); k--) { st[sp].a = kids[k]; st[sp].parent = me; st[sp].depth = depth + 1; st[sp].vis = e->vis; sp++; }
    }
    return n;
}
static int above(const el *x, int xi, const el *y, int yi) { int lx = x->layer == 1, ly = y->layer == 1; return lx != ly ? lx > ly : xi > yi; }
static int in_rect(const float *r, float x, float y) { return x >= r[0] && x < r[2] && y >= r[1] && y < r[3]; }
static int list_row_at(const el *e, float x, float y) {
    if (e->rowh <= 0 || e->rows <= 0 || !in_rect(e->area, x, y)) return -1;
    int i = (int)floorf((y - e->area[1] + e->scroll) / (float)e->rowh);
    return i >= 0 && i < e->rows ? i : -1;
}
// ---- dump
static void dump_stack(FILE *f);
static void dump(void) {
    float cw, ch; int n = snapshot(g_dump >= 2, &cw, &ch);
    char p[1200]; snprintf(p, sizeof p, "%s/tl2-ui-dump.tsv", g_dir);
    FILE *f = fopen(p, "w"); if (!f) { LOG("[tl2ui] dump: cannot write %s (errno %d)", p, errno); return; }
    fprintf(f, "# canvas %.0fx%.0f; idx\tdepth\tparent\taddr\tvt\tclass\tvis\tlayer\tl\tt\tr\tb\textra\n", cw, ch);
    for (int i = 0; i < n; i++) {
        const el *e = &g_el[i];
        fprintf(f, "%d\t%d\t%d\t%#llx\tmain+%#llx\t%s\t%d\t%d\t%.1f\t%.1f\t%.1f\t%.1f\t", i, e->depth, e->parent, (unsigned long long)e->a,
                (unsigned long long)(e->vt - (uint64_t)(uintptr_t)g_base), k_kind[e->kind], e->vis, e->layer, e->r[0], e->r[1], e->r[2], e->r[3]);
        if (e->kind == K_LIST) fprintf(f, "area %.1f,%.1f,%.1f,%.1f rowh %d rows %d sel %d scroll %.1f showsel %d", e->area[0], e->area[1], e->area[2], e->area[3],
                                       e->rowh, e->rows, e->sel, e->scroll, e->showsel);
        if (e->kind == K_BUTTON) fprintf(f, "hilite %d enabled %d", e->hilite, e->enabled);
        fputc('\n', f);
    }
    dump_stack(f);
    fclose(f);
    LOG("[tl2ui] dump: %d elements -> %s", n, p);
}

// ---- "a menu is open", published by the input thread every npad read
static _Atomic int g_menu_open;
static int menu_open_now(int *top_out) {
    uint64_t m = img(U_MGR); int32_t top = -1; rd(m + U_TOP, &top, 4); *top_out = top;
    uint64_t load = rq(m + U_LOADING); uint8_t v = 0;
    if (load && rd(load + E_DRAWN, &v, 1) && v) return 0;
    if (top >= 0 && top < 64 && rq(m + U_STACK) && rq(rq(m + U_STACK) + 8ull * top)) return 1;
    uint64_t modal = rq(m + U_MODAL); v = 0;
    return modal && rd(modal + E_DRAWN, &v, 1) && v;
}
int dm_tl2_menu_open(void);
int dm_tl2_menu_open(void) { return g_ok > 0 && g_mode > 0 && atomic_load_explicit(&g_menu_open, memory_order_acquire); }

// ---- tap queue (any thread -> the game's input thread)
static _Atomic uint64_t g_tap_xy; static _Atomic int g_tap_pending;
int dm_tl2_ui_tap(float x, float y);
int dm_tl2_ui_tap(float x, float y) {
    static int mode = -1; if (mode < 0) { const char *e = getenv("DM_TL2_POINTER"); mode = e && *e ? atoi(e) : 0; }   // before the first poll too
    if (g_ok == 0 || mode <= 0 || !(x >= 0 && x <= 1 && y >= 0 && y <= 1)) return 0;
    if (g_ok > 0 && !atomic_load_explicit(&g_menu_open, memory_order_acquire)) return 0;   // plain gameplay: the caller keeps the touch
    uint32_t xb, yb; memcpy(&xb, &x, 4); memcpy(&yb, &y, 4);
    atomic_store(&g_tap_xy, (uint64_t)yb << 32 | xb); atomic_store_explicit(&g_tap_pending, 1, memory_order_release);
    return 1;
}

// ---- the click, on the game's input thread
static uint64_t top_menu(int *top_out) {
    uint64_t m = img(U_MGR); int32_t top = -1; rd(m + U_TOP, &top, 4); *top_out = top;
    return top >= 0 && top < 64 ? rq(rq(m + U_STACK) + 8ull * top) : 0;
}
static const char *menu_name(uint64_t vt) {
    static const struct { uint32_t vt; const char *n; } k[] = {
        {VT_MAIN, "MainMenu"}, {VT_NETWORK, "Network"}, {VT_CREATE, "CreateCharacter"}, {VT_OPTIONS, "Options"}, {VT_INVITE, "PlayerInvite"},
        {VT_PARTY, "PartyMenu"}, {0x232a788, "ButtonMenu"}, {0x232ac88, "MPCharacterSelect"}, {0x232ad60, "ChangeCharacter"}, {0x232b130, "ManageCharacters"},
        {0x232ae38, "CharacterList"}, {0x232b208, "Waypoints"}, {0x232b4e8, "SkillsRespec"}, {0x232af10, "Controls"}, {0x230d6d8, "RadialMenu"},
        {0x2304300, "StartMenu"}, {0x232a860, "Credits"}, {0x2309f50, "MoviePlayer"}, {0x232aa10, "Transition"}};
    for (size_t i = 0; i < sizeof k / sizeof *k; i++) if (vt == img(k[i].vt)) return k[i].n;
    return "?";
}
static void dispatch(uint32_t code, const char *what) {
    uint32_t ev[4] = {0 /* press */, code, 0, 0};
    int r = ((int (*)(uint64_t, int, const uint32_t *))img(F_DISPATCH))(img(U_MGR), 0, ev);
    LOG("[tl2ui]   %s: UI manager HandleInput(code %u) -> %s", what, code, (r & 1) ? "handled" : "not handled");
}
static void set_hi(uint64_t button, int on) { ((void (*)(uint64_t, int))img(F_SETHI))(button, on); }
// A button of the top menu's focus list: its index (-1 = not this menu's), the array and the focus field
static int menu_button(uint64_t menu, uint64_t vt, uint64_t button, uint64_t *arr_out, uint64_t *focus_at) {
    uint64_t arr = 0; int32_t n = 0;
    if (vt == img(VT_MAIN) || vt == img(VT_NETWORK) || vt == img(VT_CREATE)) { arr = rq(menu + 0x98); rd(menu + 0x58, &n, 4); *focus_at = menu + 0x5c; }
    else if (vt == img(VT_OPTIONS) || vt == img(VT_INVITE) || vt == img(VT_PARTY)) { arr = rq(menu + 0x50); rd(menu + 0x38, &n, 4); *focus_at = menu + 0x3c; }
    else return -1;
    if (!arr || n <= 0 || n > 256) return -1;
    for (int i = 0; i < n; i++) if (rq(arr + 8ull * i) == button) { *arr_out = arr; return i; }
    return -1;
}
static int menu_holds(uint64_t menu, uint64_t p) {
    uint64_t q[0x80]; if (!rd(menu, q, sizeof q)) return 0;
    for (int i = 1; i < 0x80; i++) if (q[i] == p) return 1;
    return 0;
}
static void click(float fx, float fy) {
    int top; uint64_t menu = top_menu(&top);
    if (!menu) { LOG("[tl2ui] tap %.3f,%.3f: no menu on the console UI stack (top %d): nothing", fx, fy, top); return; }
    uint64_t vt = rq(menu);
    float cw, ch; int n = snapshot(0, &cw, &ch);
    float ux = fx * cw, uy = fy * ch; int best = -1, row = -1, radial = -1;
    for (int i = 0; i < n; i++) {
        const el *e = &g_el[i];
        if (e->kind == K_LIST) { int r = list_row_at(e, ux, uy); if (r >= 0 && (best < 0 || above(e, i, &g_el[best], best))) { best = i; row = r; } }
        else if (e->kind == K_BUTTON && in_rect(e->r, ux, uy) && e->r[2] > e->r[0] && (best < 0 || above(e, i, &g_el[best], best))) { best = i; row = -1; }
        else if (e->kind == K_RADIAL && in_rect(e->r, ux, uy)) radial = i;
    }
    const char *mn = menu_name(vt);
    unsigned long long vto = (unsigned long long)(vt - img(0));
    if (best < 0) {
        LOG("[tl2ui] tap %.3f,%.3f = UI %.0f,%.0f, menu %s %#llx (vt main+%#llx): no button / list row under it%s", fx, fy, ux, uy, mn,
            (unsigned long long)menu, vto, radial >= 0 ? " (a radial menu: not handled)" : "");
        return;
    }
    const el *t = &g_el[best];
    if (t->kind == K_BUTTON) {
        uint64_t arr = 0, focus_at = 0; int idx = menu_button(menu, vt, t->a, &arr, &focus_at);
        if (idx < 0) {
            LOG("[tl2ui] tap %.3f,%.3f = UI %.0f,%.0f: button %#llx is not in the focus list of the top menu %s (vt main+%#llx): nothing", fx, fy, ux, uy,
                (unsigned long long)t->a, mn, vto);
            return;
        }
        int32_t cur = -1; rd(focus_at, &cur, 4);
        LOG("[tl2ui] tap %.3f,%.3f = UI %.0f,%.0f -> %s item %d (focus was %d)%s", fx, fy, ux, uy, mn, idx, cur, t->enabled ? "" : ", greyed: Accept refused by the menu");
        if (cur != idx) {   // the menu's own focus move: what its navigation code does (MainMenu 0x145954..0x1459b8, ButtonMenu 0x739690)
            if (cur >= 0) { uint64_t ob = rq(arr + 8ull * cur); if (ob) set_hi(ob, 0); }
            *(volatile int32_t *)(uintptr_t)focus_at = idx;
            set_hi(t->a, 1);
        }
        float w = t->r[2] - t->r[0], rel = w > 0 ? (ux - t->r[0]) / w : .5f;
        if (rel < .25f) dispatch(EV_LEFT, "left quarter: Left");
        else if (rel > .75f) dispatch(EV_RIGHT, "right quarter: Right");
        else dispatch(EV_ACCEPT, "Accept");
        return;
    }
    if (!menu_holds(menu, t->a)) {
        LOG("[tl2ui] tap %.3f,%.3f = UI %.0f,%.0f: list %#llx row %d is not held by the top menu %s (vt main+%#llx): nothing", fx, fy, ux, uy,
            (unsigned long long)t->a, row, mn, vto);
        return;
    }
    LOG("[tl2ui] tap %.3f,%.3f = UI %.0f,%.0f -> %s list %#llx row %d of %d (selected %d)", fx, fy, ux, uy, mn, (unsigned long long)t->a, row, t->rows, t->sel);
    if (row != t->sel) { ((void (*)(uint64_t, int))img(F_SETSEL))(t->a, row); LOG("[tl2ui]   selected with the list's SetSelected (tap it again to activate)"); }
    else dispatch(EV_ACCEPT, "Accept on the selected row");
}

static void dump_stack(FILE *f) {
    int top; top_menu(&top); uint64_t st = rq(img(U_MGR) + U_STACK);
    fprintf(f, "# console UI stack top %d\n", top);
    for (int i = 0; i <= top && i < 64; i++) {
        uint64_t me = rq(st + 8ull * i), vt = rq(me);
        fprintf(f, "# stack %d: %#llx vt main+%#llx %s\n", i, (unsigned long long)me, (unsigned long long)(vt - img(0)), menu_name(vt));
    }
}

// ---- per npad read (hle_hid.c fill(), after the front end's buttons; the buttons are left alone)
void dm_tl2_ui_pump(uint64_t *buttons);
void dm_tl2_ui_pump(uint64_t *buttons) {
    (void)buttons;
    if (g_ok < 0) setup();
    if (g_ok <= 0) return;
    if (g_mode > 0) {
        int top, open = menu_open_now(&top); static int was = -1, said;
        if (open != was && said < 2000) {
            said++; uint64_t me = top_menu(&top);
            LOG("[tl2ui] menu %s (console UI stack top %d %s)", open ? "open: taps click menu items" : "closed: taps stay with the touch layout", top,
                me ? menu_name(rq(me)) : "-");
        }
        was = open; atomic_store_explicit(&g_menu_open, open, memory_order_release);
        if (atomic_exchange_explicit(&g_tap_pending, 0, memory_order_acquire)) {
            uint64_t xy = atomic_load(&g_tap_xy); float x, y; uint32_t xb = (uint32_t)xy, yb = (uint32_t)(xy >> 32); memcpy(&x, &xb, 4); memcpy(&y, &yb, 4);
            if (!open) LOG("[tl2ui] tap %.3f,%.3f: no menu open any more, dropped", x, y);
            else click(x, y);
        }
    }
    static unsigned tick; if (++tick % 30) return;
    char t[1200];
    if (g_dump > 0) { snprintf(t, sizeof t, "%s/ui-dump-now", g_dir); if (!access(t, F_OK)) { remove(t); dump(); } }
    if (g_mode > 0) {
        snprintf(t, sizeof t, "%s/ui-tap-now", g_dir); FILE *f = fopen(t, "r");
        if (f) {
            float x = -1, y = -1; int ok = fscanf(f, "%f %f", &x, &y) == 2; fclose(f); remove(t);
            if (!ok || !dm_tl2_ui_tap(x, y)) LOG("[tl2ui] %s: want \"x y\" in 0..1 while a menu is open", t);
        }
    }
}
#endif
