// [TL2] Pointer input for the console menus (DM_TL2_POINTER, default off until checked on the phone; docs/tl2/TL2_PORT.md "Тап по пунктам меню").
// TL2 Switch has no mouse path: its menus are Panic Button's own retained UI "PUI" (RTTI N3PUI7ElementE / 4List / 6Button / 6Radial /
// 7Manager), driven by the gamepad through each CConsoleUI* menu's own focus model (CConsoleUIMainMenu keeps its focus index at +0x5c
// over a Button array at +0x98, other menus move a PUI::List's selection). CEGUI 0.6.2 is linked but its mouse injection is never fed.
// What PUI keeps that a host can use: every element's laid-out rectangle in canvas units and the visible focus state. So a tap is turned
// into the game's own navigation, closed loop: find the item under the finger, read which item is focused now, press the d-pad toward
// the target one step at a time (re-reading the focus after each step), and press A only once the game itself shows the target focused.
// Nothing is written into the game; a step that does not move the focus ends the attempt without A.
//
// PUI layout (main v5 build a0c6c2d2, offsets = main.raw; all checked word for word at the first poll):
//   PUI::Manager   static main+0x927a970 (GOT 0x23a3678; init 0x19fef80 from the UI setup 0x140f00 with 1920x1080): +0x2f8 / +0x2fc canvas
//                  width / height (0x19ff3d8), +0x308 root element (0x19ff450); Render 0x19f5090 draws the root twice, pass 0 then pass 1.
//   PUI::Element   vtable main+0x23651f8: +0x38 parent, +0x40 first child, +0x58 next sibling (children drawn first to last = bottom to top),
//                  +0x60 l,t,r,b laid-out rect in canvas units (Layout vt+0x70 = 0x19f5a90, from anchors +0x10 / offsets +0x20 / pivot
//                  +0x30), +0x74 drawn (Render vt+0x18 = 0x19f57b0 returns before drawing it and its children while 0), +0x80 layer (drawn
//                  in pass 0 when 0, in pass 1 when 1 = above every pass-0 element).
//   PUI::List      vtable main+0x23656d8 (Draw vt+0x10 = 0x1a01ab0): +0x84 l,t,r,b row area, +0x100 row height (int), +0x104 rows,
//                  +0x108 selected row (-1 none; SetSelected 0x1a02460), +0x10c scroll (float), +0x116 shows the selection. Row i spans
//                  y = area.t + i*h - scroll .. + h across the area (0x1a01dac..0x1a01e88).
//   PUI::Button    vtable main+0x2365838: +0xc4 highlighted (SetHighlighted 0x1a05020, used by the button menus 0x739xxx / main menu 0x1458xx),
//                  +0xc3 greyed (0x1a05060).
//   PUI::Radial    vtable main+0x2365628: radial quick menus are stick-driven; a tap on one is logged, not handled.
//
//   DM_TL2_POINTER=1   dm_tl2_ui_tap(x, y) (fractions of the game picture, top left origin, any thread) queues a tap; the npad poll (the
//                      game's input update 0x13fd40 -> 0x13ea60, main thread) runs the navigation: d-pad held 50 ms / released 70 ms per
//                      step, at most 24 steps or 3 s, then A for 60 ms. A tap on the item already focused = A at once.
//   DM_TL2_POINTER=2   the same, and a tap that hits no item presses A (the old "tap = A" of the touch menu mode).
//   DM_TL2_UIDUMP=1    the file dm/ui-dump-now makes dm/tl2-ui-dump.tsv (visible elements: depth, address, class, rect, layer, list / button
//                      state) and is deleted; =2 dumps hidden elements too. A file dm/ui-tap-now holding "x y" queues a tap (test without
//                      the front end; both checked every 30 polls).
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
    M_W = 0x2f8, M_H = 0x2fc, M_ROOT = 0x308,
    E_PARENT = 0x38, E_FIRST = 0x40, E_NEXT = 0x58, E_RECT = 0x60, E_DRAWN = 0x74, E_LAYER = 0x80,
    L_AREA = 0x84, L_ROWH = 0x100, L_COUNT = 0x104, L_SEL = 0x108, L_SCROLL = 0x10c, L_SHOWSEL = 0x116,
    B_GREY = 0xc3, B_HILITE = 0xc4,
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
    if (rq(img(P_GOT_MGR)) != img(P_MGR) || rq(img(P_VT_LIST + 0x10)) != img(P_LIST_DRAW) || rq(img(P_VT_ELEM + 0x10)) != img(P_ELEM_DRAW) ||
        rq(img(P_VT_BUTTON + 8)) != img(P_BUTTON_DTOR) || rq(img(P_VT_RADIAL + 0x10)) != img(P_RADIAL_DRAW)) {
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
    int hilite, grey;                                            // Button
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
            uint8_t hb[2] = {0, 0}; if (rd(a + B_GREY, hb, 2)) { e->grey = hb[0]; e->hilite = hb[1]; }
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
static void center(const el *e, int row, float *cx, float *cy) {
    if (e->kind == K_LIST && row >= 0) { *cx = (e->area[0] + e->area[2]) * .5f; *cy = e->area[1] + (row + .5f) * e->rowh - e->scroll; }
    else { *cx = (e->r[0] + e->r[2]) * .5f; *cy = (e->r[1] + e->r[3]) * .5f; }
}
static int find_addr(int n, uint64_t a) { for (int i = 0; i < n; i++) if (g_el[i].a == a) return i; return -1; }
static int common_depth(int x, int y) {   // levels from x up to the nearest ancestor shared with y (-1 none)
    for (int up = 0, p = x; p >= 0; p = g_el[p].parent, up++) for (int q = y; q >= 0; q = g_el[q].parent) if (q == p) return up;
    return -1;
}
// The highlighted button that belongs to the target's group: the nearest shared ancestor at most 4 levels above the target
static int focused_button(int n, int t) {
    int best = -1, bu = 99;
    for (int i = 0; i < n; i++) {
        if (g_el[i].kind != K_BUTTON || !g_el[i].vis || !g_el[i].hilite) continue;
        int up = common_depth(t, i);
        if (up >= 0 && up <= 4 && up < bu) { bu = up; best = i; }
    }
    return best;
}

// ---- dump
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
        if (e->kind == K_BUTTON) fprintf(f, "hilite %d grey %d", e->hilite, e->grey);
        fputc('\n', f);
    }
    fclose(f);
    LOG("[tl2ui] dump: %d elements -> %s", n, p);
}

// ---- tap queue (any thread -> the game's input thread)
static _Atomic uint64_t g_tap_xy; static _Atomic int g_tap_pending;
int dm_tl2_ui_tap(float x, float y);
int dm_tl2_ui_tap(float x, float y) {
    static int mode = -1; if (mode < 0) { const char *e = getenv("DM_TL2_POINTER"); mode = e && *e ? atoi(e) : 0; }   // before the first poll too
    if (g_ok == 0 || mode <= 0 || !(x >= 0 && x <= 1 && y >= 0 && y <= 1)) return 0;
    uint32_t xb, yb; memcpy(&xb, &x, 4); memcpy(&yb, &y, 4);
    atomic_store(&g_tap_xy, (uint64_t)yb << 32 | xb); atomic_store_explicit(&g_tap_pending, 1, memory_order_release);
    return 1;
}

// ---- navigation state machine (time based: the game reads the npad several times per frame)
enum { S_IDLE, S_STEP_DOWN, S_STEP_UP, S_A_DOWN, S_A_UP };
enum { HOLD_NS = 50000000, GAP_NS = 70000000, A_NS = 60000000, MAX_STEPS = 24, MAX_NS = 3000000000u };
static struct {
    int st, steps, row, stalls, tried;   // tried: bit per direction already stalled at the current focus
    uint64_t target, focus_a, t, t0, dir; int focus_row;
} g_nav;
static const uint64_t k_dirs[4] = {DM_BTN_UP, DM_BTN_DOWN, DM_BTN_LEFT, DM_BTN_RIGHT};
static const char *const k_dirn[4] = {"up", "down", "left", "right"};
static int dir_index(uint64_t d) { for (int i = 0; i < 4; i++) if (k_dirs[i] == d) return i; return -1; }

// Where the focus is now for the current target: 1 = on it (A may go), 0 = elsewhere (fills focus_a / focus_row and the next direction),
// -1 = target gone or no usable focus
static int evaluate(uint64_t *dir_out, const char **why) {
    float cw, ch; int n = snapshot(0, &cw, &ch), t = find_addr(n, g_nav.target);
    if (t < 0) { *why = "target no longer visible"; return -1; }
    const el *te = &g_el[t]; float tx, ty, fx, fy;
    if (te->kind == K_LIST) {
        if (te->sel == g_nav.row) return 1;
        if (te->sel < 0) { *why = "list has no selection"; return -1; }
        g_nav.focus_a = te->a; g_nav.focus_row = te->sel;
        *dir_out = g_nav.row > te->sel ? DM_BTN_DOWN : DM_BTN_UP;
        return 0;
    }
    if (te->hilite) return 1;
    int f = focused_button(n, t);
    if (f < 0) { *why = "no highlighted button in the target's group"; return -1; }
    g_nav.focus_a = g_el[f].a; g_nav.focus_row = -1;
    center(te, -1, &tx, &ty); center(&g_el[f], -1, &fx, &fy);
    float dx = tx - fx, dy = ty - fy;
    // directions toward the target, best first; skip the ones that already did not move the focus from here
    uint64_t order[4]; int k = 0;
    uint64_t v = dy > 0 ? DM_BTN_DOWN : DM_BTN_UP, h = dx > 0 ? DM_BTN_RIGHT : DM_BTN_LEFT;
    int vok = fabsf(dy) > 4, hok = fabsf(dx) > 4;
    if (fabsf(dy) >= fabsf(dx)) { if (vok) order[k++] = v; if (hok) order[k++] = h; } else { if (hok) order[k++] = h; if (vok) order[k++] = v; }
    for (int i = 0; i < k; i++) if (!(g_nav.tried & 1 << dir_index(order[i]))) { *dir_out = order[i]; return 0; }
    *why = k ? "no direction moves the focus toward it" : "focus and target at the same place";
    return -1;
}
static void nav_end(const char *what) {
    LOG("[tl2ui] tap done after %d steps, %.0f ms: %s", g_nav.steps, (now_ns() - g_nav.t0) / 1e6, what);
    g_nav.st = S_IDLE;
}
static void start_tap(float fx, float fy) {
    float cw, ch; int n = snapshot(0, &cw, &ch);
    float ux = fx * cw, uy = fy * ch; int best = -1, row = -1, radial = -1;
    for (int i = 0; i < n; i++) {
        const el *e = &g_el[i];
        if (e->kind == K_LIST) { int r = list_row_at(e, ux, uy); if (r >= 0 && (best < 0 || above(e, i, &g_el[best], best))) { best = i; row = r; } }
        else if (e->kind == K_BUTTON && in_rect(e->r, ux, uy) && e->r[2] > e->r[0] && (best < 0 || above(e, i, &g_el[best], best))) { best = i; row = -1; }
        else if (e->kind == K_RADIAL && in_rect(e->r, ux, uy)) radial = i;
    }
    if (best >= 0 && g_el[best].kind == K_BUTTON && !g_el[best].hilite && focused_button(n, best) < 0) {
        LOG("[tl2ui] tap %.3f,%.3f = UI %.0f,%.0f: button %#llx has no focused button in its group (a label or prompt?), ignored", fx, fy, ux, uy,
            (unsigned long long)g_el[best].a);
        best = -1;
    }
    if (best < 0) {
        LOG("[tl2ui] tap %.3f,%.3f = UI %.0f,%.0f (canvas %.0fx%.0f, %d visible elements): no list row / button%s%s", fx, fy, ux, uy, cw, ch, n,
            radial >= 0 ? " (over a radial menu: not handled)" : "", g_mode >= 2 ? "; A (DM_TL2_POINTER=2)" : "");
        if (g_mode >= 2) { memset(&g_nav, 0, sizeof g_nav); g_nav.t0 = g_nav.t = now_ns(); g_nav.st = S_A_DOWN; }
        return;
    }
    memset(&g_nav, 0, sizeof g_nav);
    g_nav.target = g_el[best].a; g_nav.row = row; g_nav.t0 = g_nav.t = now_ns();
    LOG("[tl2ui] tap %.3f,%.3f = UI %.0f,%.0f -> %s %#llx rect %.0f,%.0f,%.0f,%.0f", fx, fy, ux, uy, k_kind[g_el[best].kind], (unsigned long long)g_nav.target,
        g_el[best].r[0], g_el[best].r[1], g_el[best].r[2], g_el[best].r[3]);
    if (row >= 0) LOG("[tl2ui]   list row %d of %d (selected %d, row height %d, scroll %.0f)", row, g_el[best].rows, g_el[best].sel, g_el[best].rowh, g_el[best].scroll);
    const char *why = ""; uint64_t d = 0; int r = evaluate(&d, &why);
    if (r == 1) { LOG("[tl2ui]   already focused: A"); g_nav.st = S_A_DOWN; return; }
    if (r < 0) { nav_end(why); return; }
    g_nav.dir = d; g_nav.st = S_STEP_DOWN;
    LOG("[tl2ui]   focus %#llx row %d: step %s", (unsigned long long)g_nav.focus_a, g_nav.focus_row, k_dirn[dir_index(d)]);
}
static void nav_tick(uint64_t *buttons) {
    uint64_t now = now_ns();
    const uint64_t mask = DM_BTN_A | DM_BTN_UP | DM_BTN_DOWN | DM_BTN_LEFT | DM_BTN_RIGHT;
    if (g_nav.st == S_IDLE) return;
    *buttons &= ~mask;   // the sequence owns A and the d-pad while it runs
    if (now - g_nav.t0 > MAX_NS && g_nav.st != S_A_DOWN && g_nav.st != S_A_UP) { nav_end("timeout, no A"); return; }
    switch (g_nav.st) {
    case S_STEP_DOWN:
        *buttons |= g_nav.dir;
        if (now - g_nav.t >= HOLD_NS) { g_nav.st = S_STEP_UP; g_nav.t = now; }
        break;
    case S_STEP_UP:
        if (now - g_nav.t < GAP_NS) break;
        {
            uint64_t prev_a = g_nav.focus_a; int prev_row = g_nav.focus_row, prev_dir = dir_index(g_nav.dir);
            g_nav.steps++;
            const char *why = ""; uint64_t d = 0; int r = evaluate(&d, &why);
            if (r == 1) { LOG("[tl2ui]   step %d (%s): target focused, A", g_nav.steps, k_dirn[prev_dir]); g_nav.st = S_A_DOWN; g_nav.t = now; break; }
            if (r < 0) { nav_end(why); break; }
            if (g_nav.focus_a == prev_a && g_nav.focus_row == prev_row) {   // that press did not move the focus
                g_nav.tried |= 1 << prev_dir;
                if (++g_nav.stalls > 3) { nav_end("focus does not move, no A"); break; }
                r = evaluate(&d, &why);
                if (r == 1) { g_nav.st = S_A_DOWN; g_nav.t = now; break; }
                if (r < 0 || g_nav.tried & 1 << dir_index(d)) { nav_end(r < 0 ? why : "focus does not move, no A"); break; }
            } else g_nav.tried = 0;
            if (g_nav.steps >= MAX_STEPS) { nav_end("too many steps, no A"); break; }
            LOG("[tl2ui]   step %d: focus %#llx row %d, next %s", g_nav.steps, (unsigned long long)g_nav.focus_a, g_nav.focus_row, k_dirn[dir_index(d)]);
            g_nav.dir = d; g_nav.st = S_STEP_DOWN; g_nav.t = now;
        }
        break;
    case S_A_DOWN:
        *buttons |= DM_BTN_A;
        if (now - g_nav.t >= A_NS) { g_nav.st = S_A_UP; g_nav.t = now; }
        break;
    case S_A_UP:
        if (now - g_nav.t >= GAP_NS) nav_end("A pressed");
        break;
    }
}

// ---- per npad read (hle_hid.c fill(), after the front end's buttons)
void dm_tl2_ui_pump(uint64_t *buttons);
void dm_tl2_ui_pump(uint64_t *buttons) {
    if (g_ok < 0) setup();
    if (g_ok <= 0) return;
    if (g_mode > 0 && atomic_exchange_explicit(&g_tap_pending, 0, memory_order_acquire)) {
        uint64_t xy = atomic_load(&g_tap_xy); float x, y; uint32_t xb = (uint32_t)xy, yb = (uint32_t)(xy >> 32); memcpy(&x, &xb, 4); memcpy(&y, &yb, 4);
        if (g_nav.st == S_A_DOWN || g_nav.st == S_A_UP) LOG("[tl2ui] tap %.3f,%.3f while A is pressed: dropped", x, y);
        else { if (g_nav.st != S_IDLE) LOG("[tl2ui] new tap: the previous one is abandoned"); g_nav.st = S_IDLE; start_tap(x, y); }
    }
    if (g_mode > 0) nav_tick(buttons);
    static unsigned tick; if (++tick % 30) return;
    char t[1200];
    if (g_dump > 0) { snprintf(t, sizeof t, "%s/ui-dump-now", g_dir); if (!access(t, F_OK)) { remove(t); dump(); } }
    if (g_mode > 0) {
        snprintf(t, sizeof t, "%s/ui-tap-now", g_dir); FILE *f = fopen(t, "r");
        if (f) { float x = -1, y = -1; int ok = fscanf(f, "%f %f", &x, &y) == 2; fclose(f); remove(t);
                 if (!ok || !dm_tl2_ui_tap(x, y)) LOG("[tl2ui] %s: want \"x y\" in 0..1", t); }
    }
}
#endif
