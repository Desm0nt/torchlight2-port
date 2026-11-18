// [TL2] Skill trees of classes added by PC mods (GitHub issue #6: a TL2 ACE class shows an empty skill tree). docs/tl2/TL2_PORT.md
// "Дерево навыков классов из модов". Main v5 build a0c6c2d2, offsets = main.raw; every site / table is checked at install.
// How the console builds the skill screen (it never reads the PC layout named by the class DAT's UISKILLTREE):
//   CConsoleUIRadialMenu::Create (vt slot 4 = 0x134ec0, run on every push 0x111f40) takes, for the + menu (type +0x2c = 0), the local
//   player (client 0x6c9270() +0x58), its unit NAME (0x458f10 -> std::wstring) and looks for the substrings HUM_BERSERKER /
//   HUM_ARBITER / HUM_ENGINEER / HUM_WANDERER (0x136740..0x1369ec, wmemchr + wmemcmp); a match stores that class's ring (GOT 0x23a3e70 +
//   8 c: headers 0x230b220 / 340 / 460 / 580) into the skills tab of the tab table (GOT 0x23a3e68 = 0x230c140, entry 2 node +0x40),
//   then 0x136c40 builds the tabs from that table. No match: nothing is stored, the tab keeps the last class's ring (berserker at boot).
//   A ring = 3 records of 0x40 (id 0x65 + 3 c + tree, panel type 8 = PanelSkillList, +0x10 icon name "skill_tab_hunter", +0x18 label
//   L"Hunter Skills"), ids in reverse order, then a zero record.
//   PanelSkillList (vt 0x23031d0): tree k = id - 0x65 (0..11) reads skill names 10 k .. 10 k + 9 of the table main+0x23022c0 (120 x
//   {u64 1000 + i, const wchar_t *name}; 7 actives then 3 passives per tree; 0x5a8f0) and looks each up in the player's skill list
//   (player +0x210, 0x20dba0); a name the player lacks gives an empty slot (that is the bug: a modded class gets berserker's names).
//   Preview movies: main+0x2302a40 (120 x {id, const char *"MOVIES/EVISCERATE.MP4"}). Both tables are read only by PanelSkillList.
// The ring is generic (PUI::Radial SetSegments 0x19fd7a0 counts records up to a zero label +0x18, any number; header byte +0xc set =
// segments evenly spaced from the start angle +0x10, so +8 / +0xc of a record are not used; the inventory rings have 5..12 records), and
// every user of the tree index (PanelSkillList 0x5a930 names, 0x5ae68 / 0x607bc / 0x60850 preview movies, all "k = id - 0x65, k < 12")
// reads rows 10 k .. 10 k + 9; the touch code (hle_tl2_ui.c) and the wide-UI centre shift (hle_tl2_wide.c) walk the segment vector.
// What this does (only when DM_TL2_MODS is on and a mod pack carries MEDIA/PORT/SKILLTREES.TXT, written by tools/tl2_mod.py /
// Tl2ModConv.java from the mod's PC layouts: up to 12 trees of 10, overflow actives already moved to extra trees): a wrapper around
// Create, before the original, finds the player's NAME in the tables (exact, else <name up to the last '_'>_BASE) and, when found,
// builds our own ring of N segments (ids 0x65 .. 0x65 + N - 1) and writes the mod's names into table rows 0 .. 10 N - 1 (the stock
// classes' rows, borrowed for this player). Modded class: the skills tab's node = our ring (the original matches nothing and keeps it).
// Stock class whose tree a mod replaced (TL2 ACE's Embermage, ...): its GOT ring entry = our ring, so the original's own match stores it
// (skipped when the mod's 3 trees equal the console's). Every Create first restores rows, GOT entries and the tab node, so a stock class
// played afterwards sees the console's own data. Patched rows show the movie MOVIES/PASSIVE.MP4 unless the name is the stock one. Only
// data is written (the data segment is RW); the only code write is the Create prologue jump. Game thread only.
// DM_TL2_SKILLTREE=0 = off (internal kill switch; players never set it).
#ifdef DM_GAME_TL2
#include "hle.h"
#include "dm.h"
#include "pc.h"
#include <dirent.h>
#include <sys/mman.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    F_CREATE = 0x134ec0, F_CLIENT = 0x6c9270, F_UNITNAME = 0x458f10, F_DELETE = 0x1df89f0,
    T_SKILLS = 0x23022c0, T_MOVIES = 0x2302a40, N_ROWS = 120, TABS = 0x230c140, TAB_SKILLS_NODE = 0x40,
    GOT_SKILLS = 0x23a36e8, GOT_MOVIES = 0x23a36f8, GOT_TABS = 0x23a3e68, GOT_RING = 0x23a3e70,
    M_TYPE = 0x2c, C_PLAYER = 0x58, R_ICON = 0x10, R_LABEL = 0x18, REC = 0x40, ID0 = 0x65, ROW_PASSIVE = 7,
};
static const uint32_t k_rings[4] = {0x230b220, 0x230b340, 0x230b460, 0x230b580};
static const uint32_t k_create[4] = {0xd106c3ff /* sub sp,sp,#0x1b0 */, 0xfd00a3ea, 0x6d14a3e9, 0xf900affc};
static const uint32_t k_match_end[5] = {0xb0011368, 0xb0011369, 0xf9473508 /* ldr x8,[x8,#0xe68] */, 0xf9474529 /* ldr x9,[x9,#0xe88] */,
                                        0xf9002109 /* str x9,[x8,#0x40] */};
static const uint32_t k_create_type[3] = {0xb9402e68 /* ldr w8,[x19,#0x2c] */, 0x35001668, 0x94164acd /* bl 0x6c9270 */};
static const uint32_t k_list_tree[3] = {0x940383b5 /* bl 0x13b800 */, 0x51019408 /* sub w8,w0,#0x65 */, 0x52800149};
static const uint32_t k_name_fn[2] = {0xd10183ff, 0xa90357f6};
typedef struct { uint32_t off, n; const uint32_t *want; } st_site;
static const st_site k_sites[] = {{F_CREATE, 4, k_create}, {0x1369dc, 5, k_match_end}, {0x136734, 3, k_create_type}, {0x5a92c, 3, k_list_tree},
                                  {F_UNITNAME, 2, k_name_fn}};
static const char *const k_stock[4] = {"HUM_BERSERKER", "HUM_ARBITER", "HUM_ENGINEER", "HUM_WANDERER"};   // the game's order

enum { N_TREES = 12 };   // the table's 120 rows = 12 trees x 10
typedef struct { uint32_t *label[N_TREES]; uint64_t icon[N_TREES]; uint32_t *skill[N_TREES][10]; int has[N_TREES]; } st_lay;   // icon: a game string or 0
typedef struct { uint32_t *name; int lay; } st_class;
static st_lay *g_lay; static int g_nlay;
static st_class *g_cls; static int g_ncls;
static uint8_t *g_base;
static uint64_t g_orig_name[N_ROWS], g_orig_movie[N_ROWS], g_rec[12], g_orig_icon[12], g_movie_passive;
static uint64_t g_ring[4], g_orig_node;
// our ring: a header (copied from berserker's) + up to 12 records (id 0x65 + tree, panel 8 = PanelSkillList, copied from its record) +
// the zero record; host memory the guest reads like its own data
static uint64_t g_myring[(0x20 + REC * (N_TREES + 1)) / 8];
static int g_dirty;
static uint32_t g_empty[1], g_dash[2] = {'-', 0};
typedef uint64_t (*create_fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static create_fn g_create_orig;

static int poke(uint8_t *base, uint32_t off, const uint32_t *w, int n) {   // code is sealed RX in 16 KiB granules by the loader
    uint32_t *at = (uint32_t *)(void *)(base + off);
    uint8_t *pg = (uint8_t *)((uintptr_t)at & ~(uintptr_t)0x3FFF), *end = (uint8_t *)(at + n);
    size_t len = (size_t)(((uintptr_t)end - (uintptr_t)pg + 0x3FFF) & ~(uintptr_t)0x3FFF);
    if (mprotect(pg, len, PROT_READ | PROT_WRITE)) return -1;
    for (int i = 0; i < n; i++) at[i] = w[i];
    if (mprotect(pg, len, PROT_READ | PROT_EXEC)) die("[tl2skill] mprotect RX");
    __builtin___clear_cache((char *)at, (char *)end);
    return 0;
}
static uint64_t img(uint32_t off) { return (uint64_t)(uintptr_t)g_base + off; }
static uint64_t rq(uint64_t a) { return *(const uint64_t *)(uintptr_t)a; }
static uint32_t r32(uint64_t a) { return *(const uint32_t *)(uintptr_t)a; }
static void wq(uint64_t a, uint64_t v) { *(uint64_t *)(uintptr_t)a = v; }
static uint64_t row_name(int i) { return img(T_SKILLS) + 16u * (unsigned)i + 8; }
static uint64_t row_movie(int i) { return img(T_MOVIES) + 16u * (unsigned)i + 8; }

// ---- wide (UTF-32, the guest's wchar_t) strings
static uint32_t *u32dup_utf8(const char *s) {
    size_t n = strlen(s); uint32_t *o = malloc((n + 1) * sizeof *o); size_t k = 0;
    if (!o) return NULL;
    for (size_t i = 0; i < n;) {
        unsigned c = (unsigned char)s[i], len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!len || i + len > n) { o[k++] = 0xfffd; i++; continue; }
        uint32_t v = len == 1 ? c : c & (0x7fu >> len);
        for (unsigned j = 1; j < len; j++) v = v << 6 | ((unsigned char)s[i + j] & 0x3f);
        o[k++] = v; i += len;
    }
    o[k] = 0; return o;
}
static uint32_t up32(uint32_t c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
static int eq32ci(const uint32_t *a, const uint32_t *b) {
    if (!a) a = g_empty;
    if (!b) b = g_empty;
    for (; *a && *b; a++, b++) if (up32(*a) != up32(*b)) return 0;
    return *a == *b;
}
static int has_sub(const uint32_t *s, const char *sub) {   // case-sensitive, as the game's wmemcmp
    size_t m = strlen(sub);
    for (; *s; s++) { size_t i = 0; while (i < m && s[i] == (unsigned char)sub[i]) i++; if (i == m) return 1; }
    return 0;
}
static void u32_ascii(const uint32_t *s, char *o, size_t n) {   // for the log (non-ASCII as '?')
    size_t k = 0; for (; s && *s && k + 1 < n; s++) o[k++] = *s < 0x80 ? (char)*s : '?';
    o[k] = 0;
}

// ---- the tables in the mod packs (<DM_TL2_MODS_DIR>/PAKS/ZMOD_*.PAK: MEDIA/PORT/SKILLTREES.TXT, stored unpacked)
static int ws_is(const uint8_t *p, unsigned n, const char *s) {
    if (strlen(s) != n) return 0;
    for (unsigned i = 0; i < n; i++) { unsigned c = p[2 * i] | p[2 * i + 1] << 8; if (up32(c) != up32((unsigned char)s[i])) return 0; }
    return 1;
}
static char *pack_table(const char *pak) {   // -> malloc'd text or NULL
    char man[1200]; snprintf(man, sizeof man, "%s.MAN", pak);
    FILE *f = fopen(man, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = n > 0 && n < (64L << 20) ? malloc((size_t)n) : NULL;
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    size_t o = 0, N = (size_t)n; uint32_t off = 0, size = 0, nd = 0; int found = 0;
#define NEED(k) if (o + (k) > N) goto done
    NEED(2); unsigned ver = b[o] | b[o + 1] << 8; o += 2; if (ver >= 2) o += 4;
    NEED(2); o += 2 + 2 * (size_t)(b[o] | b[o + 1] << 8);   // root
    NEED(8); memcpy(&nd, b + o + 4, 4); o += 8;
    for (uint32_t d = 0; d < nd && !found; d++) {
        NEED(2); unsigned dl = b[o] | b[o + 1] << 8; NEED(2 + 2 * (size_t)dl); int dir = ws_is(b + o + 2, dl, "MEDIA/PORT/"); o += 2 + 2 * (size_t)dl;
        NEED(4); uint32_t ne; memcpy(&ne, b + o, 4); o += 4;
        for (uint32_t e = 0; e < ne; e++) {   // u32 crc, u8 type, wstr name, u32 offset, u32 size, u64 time
            NEED(7); unsigned nl = b[o + 5] | b[o + 6] << 8; NEED(7 + 2 * (size_t)nl + 16);
            int hit = dir && b[o + 4] != 7 && ws_is(b + o + 7, nl, "SKILLTREES.TXT");
            o += 7 + 2 * (size_t)nl;
            if (hit) { memcpy(&off, b + o, 4); memcpy(&size, b + o + 4, 4); found = 1; break; }
            o += 16;
        }
    }
done:
#undef NEED
    free(b);
    if (!found || !size || size > (1u << 20)) return NULL;
    f = fopen(pak, "rb"); if (!f) return NULL;
    uint32_t h[2]; char *t = NULL;
    if (!fseek(f, (long)off, SEEK_SET) && fread(h, 4, 2, f) == 2 && h[0] == size && (h[1] == size || !h[1]) && (t = malloc((size_t)size + 1))) {
        if (fread(t, 1, size, f) != size) { free(t); t = NULL; } else t[size] = 0;
    }
    fclose(f); return t;
}
static uint64_t icon_for(const char *key) {   // "inferno" -> the game's "skill_tab_Inferno" (one of the 12 console icons), 0 = keep
    size_t n = strlen(key);
    for (int i = 0; i < 12; i++) {
        const char *s = (const char *)(uintptr_t)g_orig_icon[i]; if (!s || strncmp(s, "skill_tab_", 10)) continue;
        s += 10; if (strlen(s) != n) continue;
        size_t j = 0; while (j < n && up32((unsigned char)s[j]) == up32((unsigned char)key[j])) j++;
        if (j == n) return g_orig_icon[i];
    }
    return 0;
}
static void load_table(char *t, const char *pack) {
    char *save = NULL, *f[6]; int base = g_nlay, lays = 0, cls = 0, first = 1;
    for (char *line = strtok_r(t, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        int n = 0; f[n++] = line;
        for (char *p = line; *p && n < 6; p++) if (*p == '\t') { *p = 0; f[n++] = p + 1; }
        if (first) {
            first = 0;
            if (n < 2 || strcmp(f[0], "TL2SKILLTREES") || (strcmp(f[1], "1") && strcmp(f[1], "2"))) { LOG("【红】[tl2skill] %s: not a version 1 / 2 table, ignored", pack); return; }
            continue;
        }
        int li = n > 1 ? atoi(f[1]) : -1;
        if (!strcmp(f[0], "L") && n >= 3 && li == lays) {
            st_lay *l = realloc(g_lay, (size_t)(g_nlay + 1) * sizeof *l); if (!l) return;
            g_lay = l; memset(&g_lay[g_nlay], 0, sizeof *g_lay); g_nlay++; lays++;
        } else if (!strcmp(f[0], "T") && n >= 5 && li >= 0 && li < lays) {
            int tr = atoi(f[2]); if (tr < 0 || tr >= N_TREES) continue;
            st_lay *l = &g_lay[base + li]; l->has[tr] = 1;
            free(l->label[tr]); l->label[tr] = u32dup_utf8(f[3]);
            l->icon[tr] = strcmp(f[4], "-") ? icon_for(f[4]) : 0;
        } else if (!strcmp(f[0], "S") && n >= 5 && li >= 0 && li < lays) {
            int tr = atoi(f[2]), sl = atoi(f[3]); if (tr < 0 || tr >= N_TREES || sl < 0 || sl > 9) continue;
            st_lay *l = &g_lay[base + li]; l->has[tr] = 1;
            free(l->skill[tr][sl]); l->skill[tr][sl] = u32dup_utf8(f[4]);
        } else if (!strcmp(f[0], "C") && n >= 3) {
            int k = atoi(f[2]); if (k < 0 || k >= lays) continue;
            uint32_t *nm = u32dup_utf8(f[1]); if (!nm) continue;
            int i = 0; while (i < g_ncls && !eq32ci(g_cls[i].name, nm)) i++;
            if (i == g_ncls) {
                st_class *c = realloc(g_cls, (size_t)(g_ncls + 1) * sizeof *c); if (!c) { free(nm); continue; }
                g_cls = c; g_ncls++; g_cls[i].name = nm;
            } else free(nm);   // a later pack overrides an earlier one (the last mod of the list wins, as with files)
            g_cls[i].lay = base + k; cls++;
        }
    }
    LOG("[tl2skill] %s: %d skill tree layout(s), %d class name(s)", pack, lays, cls);
}
static int load_tables(void) {
    char d[1024], p[1200]; const char *e = getenv("DM_TL2_MODS_DIR");
    if (e && *e) snprintf(d, sizeof d, "%s/PAKS", e);
    else { const char *r = getenv("DM_ROMFS"); snprintf(d, sizeof d, "%s/../mods/overlay/PAKS", r && *r ? r : "data/switch/3.3.94012/Update/RomFS"); }
    struct dirent **nl = NULL; int n = scandir(d, &nl, NULL, alphasort);   // ZMOD_00 .. : load order, a later pack wins
    for (int i = 0; i < n; i++) {
        const char *nm = nl[i]->d_name; size_t l = strlen(nm);
        if (!strncmp(nm, "ZMOD_", 5) && l > 4 && !strcasecmp(nm + l - 4, ".PAK")) {
            snprintf(p, sizeof p, "%s/%s", d, nm);
            char *t = pack_table(p);
            if (t) { load_table(t, nm); free(t); }
        }
        free(nl[i]);
    }
    free(nl);
    return g_ncls;
}

// ---- applying (game thread, before the original Create)
static void restore(void) {
    if (!g_dirty) return;
    for (int i = 0; i < N_ROWS; i++) { wq(row_name(i), g_orig_name[i]); wq(row_movie(i), g_orig_movie[i]); }
    for (int c = 0; c < 4; c++) wq(img(GOT_RING) + 8u * (unsigned)c, g_ring[c]);
    uint64_t node = rq(img(GOT_TABS)) + TAB_SKILLS_NODE;
    if (rq(node) == (uint64_t)(uintptr_t)g_myring) wq(node, g_orig_node);   // never leave our ring for a class without a table
    g_dirty = 0;
}
static const st_class *find_class(const uint32_t *name) {
    for (int i = 0; i < g_ncls; i++) if (eq32ci(g_cls[i].name, name)) return &g_cls[i];
    uint32_t b[160]; int n = 0, us = -1;   // HUM_WANDERER_M -> HUM_WANDERER_BASE (a mod that replaces only the class's base DAT)
    for (; name[n] && n < 140; n++) { b[n] = name[n]; if (name[n] == '_') us = n; }
    if (us <= 0 || name[n]) return NULL;
    static const char k_base[] = "_BASE"; for (int i = 0; i < 6; i++) b[us + i] = (unsigned char)k_base[i];
    for (int i = 0; i < g_ncls; i++) if (eq32ci(g_cls[i].name, b)) return &g_cls[i];
    return NULL;
}
static uint32_t g_seen[16][96]; static int g_nseen;   // names already logged
static int first_time(const uint32_t *name) {
    for (int i = 0; i < g_nseen; i++) if (eq32ci(g_seen[i], name)) return 0;
    if (g_nseen < 16) { int k = 0; for (; name[k] && k < 95; k++) g_seen[g_nseen][k] = name[k]; g_seen[g_nseen][k] = 0; g_nseen++; }
    return 1;
}
typedef struct { uint64_t w[3]; } guest_wstring;   // libc++ std::wstring: short = size << 1 in byte 0, chars at +4; long = cap | 1, size, data
static void apply(void) {
    restore();
    uint64_t client = ((uint64_t (*)(void))(uintptr_t)img(F_CLIENT))(); if (!client) return;
    uint64_t player = rq(client + C_PLAYER); if (!player) return;
    guest_wstring s = ((guest_wstring (*)(uint64_t))(uintptr_t)img(F_UNITNAME))(player);   // x8 = the result, as the game calls it
    uint32_t name[128]; size_t len; const uint32_t *src;
    if (s.w[0] & 1) { len = (size_t)s.w[1]; src = (const uint32_t *)(uintptr_t)s.w[2]; }
    else { len = (uint8_t)s.w[0] >> 1; src = (const uint32_t *)((const uint8_t *)&s + 4); }
    if (len > 127) len = 127;
    for (size_t i = 0; i < len; i++) name[i] = src ? src[i] : 0;
    name[len] = 0;
    if (s.w[0] & 1) ((void (*)(void *))(uintptr_t)img(F_DELETE))((void *)(uintptr_t)s.w[2]);
    int stock = -1; for (int k = 0; k < 4 && stock < 0; k++) if (has_sub(name, k_stock[k])) stock = k;
    const st_class *c = find_class(name);
    int log = first_time(name); char an[128]; u32_ascii(name, an, sizeof an);
    if (!c) { if (log) LOG("[tl2skill] player %s: no mod skill tree, the console's own (%s)", an, stock >= 0 ? k_stock[stock] : "ring of the last stock class"); return; }
    const st_lay *l = &g_lay[c->lay]; int nt = 0;
    for (int t = 0; t < N_TREES; t++) if (l->has[t]) nt = t + 1;
    if (!nt) return;
    if (stock >= 0 && nt == 3) {   // a stock class: only when the mod's tree differs from the console's table
        int same = 1;
        for (int t = 0; t < 3 && same; t++) for (int k = 0; k < 10 && same; k++)
            same = eq32ci(l->skill[t][k], (const uint32_t *)(uintptr_t)g_orig_name[30 * stock + 10 * t + k]);
        if (same) { if (log) LOG("[tl2skill] player %s: the mod's tree equals the console's %s table: unchanged", an, k_stock[stock]); return; }
    }
    // trees 0 .. nt-1 = ids 0x65 + t = table rows 10 t .. 10 t + 9 (PanelSkillList's own k = id - 0x65 < 12): the rows of the stock
    // classes are borrowed for this player and given back by restore() at the next Create
    int nsk = 0;
    for (int t = 0; t < nt; t++) for (int k = 0; k < 10; k++) {
        int row = 10 * t + k; const uint32_t *sk = l->skill[t][k];
        wq(row_name(row), (uint64_t)(uintptr_t)(sk ? sk : g_empty));
        if (!eq32ci(sk, (const uint32_t *)(uintptr_t)g_orig_name[row])) wq(row_movie(row), g_movie_passive);
        nsk += sk && *sk;
    }
    uint8_t *hdr = (uint8_t *)g_myring, *recs = hdr + 0x20;
    memcpy(hdr, (const void *)(uintptr_t)g_ring[0], 0x20);   // {records, first id 0x65, 1 = evenly spaced, 180.0 = start angle}
    *(uint64_t *)hdr = (uint64_t)(uintptr_t)recs; *(uint32_t *)(hdr + 8) = ID0;
    for (int j = 0; j < nt; j++) {   // the stock rings list their records in reverse id order
        int t = nt - 1 - j; uint8_t *r = recs + REC * j;
        memcpy(r, (const void *)(uintptr_t)g_rec[2], REC);   // berserker's 0x65 record: panel type 8, frame / glow images
        *(uint32_t *)r = (uint32_t)(ID0 + t);
        *(uint64_t *)(r + R_ICON) = l->icon[t] ? l->icon[t] : g_orig_icon[t % 12];
        *(uint64_t *)(r + R_LABEL) = (uint64_t)(uintptr_t)(l->label[t] ? l->label[t] : g_dash);   // non-zero: a zero label ends the ring
    }
    memset(recs + REC * nt, 0, REC);
    if (stock >= 0) wq(img(GOT_RING) + 8u * (unsigned)stock, (uint64_t)(uintptr_t)g_myring);   // the original's match stores our ring
    else wq(rq(img(GOT_TABS)) + TAB_SKILLS_NODE, (uint64_t)(uintptr_t)g_myring);   // the original matches nothing and keeps it
    g_dirty = 1;
    if (log) LOG("[tl2skill] player %s: skill tree from the mod (%d skills in %d trees, ring of %d segments, table rows 0..%d)%s", an, nsk, nt,
                 nt, 10 * nt - 1, stock >= 0 ? " for a stock class" : "");
}
static uint64_t create_hook(uint64_t menu, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    if (r32(menu + M_TYPE) == 0) apply();
    return g_create_orig(menu, a1, a2, a3, a4, a5, a6, a7);
}

// ---- install (main.c host_preload, after load_modules, before guest code runs)
static int tables_ok(void) {
    if (rq(img(GOT_SKILLS)) != img(T_SKILLS) || rq(img(GOT_MOVIES)) != img(T_MOVIES) || rq(img(GOT_TABS)) != img(TABS)) return 0;
    for (int i = 0; i < N_ROWS; i++)
        if (rq(img(T_SKILLS) + 16u * (unsigned)i) != 1000u + (unsigned)i || rq(img(T_MOVIES) + 16u * (unsigned)i) != 1000u + (unsigned)i) return 0;
    static const char k_ev[] = "Eviscerate"; const uint32_t *ev = (const uint32_t *)(uintptr_t)rq(row_name(0));
    for (int i = 0; i < 11; i++) if (ev[i] != (unsigned char)k_ev[i]) return 0;
    if (strcmp((const char *)(uintptr_t)rq(row_movie(ROW_PASSIVE)), "MOVIES/PASSIVE.MP4")) return 0;
    if (rq(img(TABS) + TAB_SKILLS_NODE) != img(k_rings[0])) return 0;
    for (int c = 0; c < 4; c++) {
        g_ring[c] = img(k_rings[c]);
        if (rq(img(GOT_RING) + 8u * (unsigned)c) != g_ring[c] || r32(g_ring[c] + 8) != (uint32_t)(ID0 + 3 * c)) return 0;
        uint64_t recs = rq(g_ring[c]);
        for (int r = 0; r < 3; r++) {
            uint64_t rec = recs + (uint64_t)REC * (unsigned)r;
            if (r32(rec) != (uint32_t)(ID0 + 3 * c + 2 - r) || r32(rec + 4) != 8) return 0;
            g_rec[3 * c + r] = rec; g_orig_icon[3 * c + r] = rq(rec + R_ICON);
        }
        if (r32(recs + 3u * REC)) return 0;
    }
    for (int i = 0; i < N_ROWS; i++) { g_orig_name[i] = rq(row_name(i)); g_orig_movie[i] = rq(row_movie(i)); }
    g_orig_node = rq(img(TABS) + TAB_SKILLS_NODE);
    g_movie_passive = g_orig_movie[ROW_PASSIVE];
    return 1;
}
void dm_tl2_skilltree_install(void);
void dm_tl2_skilltree_install(void) {
    const char *e = getenv("DM_TL2_SKILLTREE");
    if (e && *e == '0' && !e[1]) { pc_note("tl2-skilltree", 0, "%s", "DM_TL2_SKILLTREE=0"); return; }
    const char *mo = getenv("DM_TL2_MODS");
    if (!mo || !*mo || !strcmp(mo, "0")) { pc_note("tl2-skilltree", 0, "%s", "no PC mods (DM_TL2_MODS off)"); return; }
    module_t *m = NULL;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    if (!m || m->size < T_MOVIES + 16 * N_ROWS || m->size < 0x230c200) { pc_note("tl2-skilltree", 0, "%s", "no main module"); return; }
    for (size_t i = 0; i < sizeof k_sites / sizeof *k_sites; i++)
        if (m->text_end < k_sites[i].off + 4ull * k_sites[i].n || memcmp(m->base + k_sites[i].off, k_sites[i].want, 4ull * k_sites[i].n)) {
            pc_note("tl2-skilltree", 0, "main+%#x differs: not TL2 v5 main (build a0c6c2d2)", k_sites[i].off); return; }
    g_base = m->base;
    if (!tables_ok()) { pc_note("tl2-skilltree", 0, "%s", "skill name table / class rings not as in main v5 a0c6c2d2"); return; }
    if (!load_tables()) { pc_note("tl2-skilltree", 0, "%s", "no mod pack carries MEDIA/PORT/SKILLTREES.TXT (no class with a PC skill tree, or packs converted before CONVERTER_VERSION 2)"); return; }
    // the original's first 4 instructions (no PC-relative ones) + a jump to the rest, in a page of their own
    uint32_t *t = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (t == MAP_FAILED) { pc_note("tl2-skilltree", 0, "%s", "mmap failed"); return; }
    uint64_t rest = (uint64_t)(uintptr_t)(m->base + F_CREATE + 16);
    memcpy(t, k_create, 16); t[4] = 0x58000050 /* ldr x16,#8 */; t[5] = 0xd61f0200 /* br x16 */; t[6] = (uint32_t)rest; t[7] = (uint32_t)(rest >> 32);
    if (mprotect(t, 0x4000, PROT_READ | PROT_EXEC)) { munmap(t, 0x4000); pc_note("tl2-skilltree", 0, "%s", "mprotect RX failed"); return; }
    __builtin___clear_cache((char *)t, (char *)(t + 8));
    g_create_orig = (create_fn)(void *)t;
    uint64_t to = (uint64_t)(uintptr_t)create_hook;
    const uint32_t jump[4] = {0x58000050 /* ldr x16,#8 */, 0xd61f0200 /* br x16 */, (uint32_t)to, (uint32_t)(to >> 32)};
    if (poke(m->base, F_CREATE, jump, 4)) { pc_note("tl2-skilltree", 0, "%s", "mprotect RW of the Create prologue refused"); return; }
    pc_note("tl2-skilltree", 1, "%d class name(s) with a PC skill tree; CConsoleUIRadialMenu::Create main+%#x wrapped (rings / table main+%#x)",
            g_ncls, F_CREATE, T_SKILLS);
}
#endif
