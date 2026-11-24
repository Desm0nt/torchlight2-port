// [TL2] Atmosphere dmnt cheat files, run by a small VM once per present (dm_dmnt_tick, called from hle_nvn.c nvnQueuePresentTexture).
// File: <data folder>/cheats/<main build id, first 8 bytes, upper-case hex>.txt (Download/Torchlight2Mobile/cheats/), Atmosphere format:
//   "[Name]" starts a cheat, "{Name}" a master cheat, the lines after it are opcode words of 8 hex digits.
// <data folder>/cheats/toggles.txt switches cheats by name, one "[Name] = true" / "[Name] = false" per line. Default: master cheats on, the others off.
// No cheats folder / no file for this build = dm_dmnt_tick returns at once and nothing is logged (the first tick after frame 60 decides).
// Opcodes (Atmosphere dmnt_cheat_vm): 0 store static, 1 conditional (> >= < <= == !=), 2 end conditional / else, 3 loop, 4 load register static,
// 5 load register from memory, 6 store static to register address, 7 legacy arithmetic, 8 key press, 9 register arithmetic, A store register to memory,
// C0 register conditional. Regions: 0 main = the main module base, 1 heap = guest AS + 0x40000000 (hle_svc.c: the fixed partition SetHeapSize hands out,
// g_heap), 2 alias, 3 aslr = the guest address space (dm_guest_as_bounds). Every address is checked against those bounds before it is touched; accesses
// outside the main image go through a pipe (write / read return EFAULT on an unmapped or protected page instead of killing the process), writes into the
// main image use dm_code_poke (the tw_poke pattern). A bad address ends that cheat's run for this tick (the pointer may not be valid yet; logged 3 times);
// a malformed opcode or a runaway loop disables that cheat for good ("[dmnt] cheat X disabled"). Budgets: 100000 opcodes per tick, 50000 per cheat.
// Key press (type 8): reads dm_hid_keys (hle_hid.c), the buttons and stick directions as the game got them at its latest controller read (Atmosphere HidNpadButton layout).
#ifdef DM_GAME_TL2
#include "dm.h"
#include "dm_input.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <strings.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <unistd.h>

enum { MAX_CHEATS = 128, MAX_OPS = 0x400, START_FRAME = 60, TICK_BUDGET = 100000, CHEAT_BUDGET = 50000 };
enum { R_OK_ = 0, R_FAULT = 1, R_BAD = 2 };   // run result: finished / bad address (retry next tick) / malformed or runaway (disable)

typedef struct { char name[64]; int master, enabled, dead; unsigned n, faults; uint32_t *op; } cheat_t;
static cheat_t g_ch[MAX_CHEATS]; static unsigned g_nch;
static module_t *g_main; static uint64_t g_as0, g_as1, g_heap, g_alias0, g_alias1;
static int g_pipe[2] = {-1, -1};
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

// ---- guest memory
static void pipe_drain(void) { uint8_t s[64]; while (read(g_pipe[0], s, sizeof s) > 0) {} }
static int addr_in_as(uint64_t a, size_t n) { return a >= g_as0 && a < g_as1 && n <= g_as1 - a; }
static int addr_in_main(uint64_t a, size_t n) {
    uint64_t b = (uint64_t)(uintptr_t)g_main->base;
    return a >= b && a - b < g_main->size && n <= g_main->size - (a - b);
}
static int mem_read(uint64_t a, unsigned w, uint64_t *out) {
    *out = 0;
    if (addr_in_main(a, w)) { memcpy(out, (const void *)(uintptr_t)a, w); return 0; }
    if (!addr_in_as(a, w)) return -1;
    if (write(g_pipe[1], (const void *)(uintptr_t)a, w) != (ssize_t)w) { pipe_drain(); return -1; }   // EFAULT on an unmapped / PROT_NONE page
    if (read(g_pipe[0], out, w) != (ssize_t)w) { pipe_drain(); return -1; }
    return 0;
}
static int mem_write(uint64_t a, unsigned w, uint64_t v) {
    if (addr_in_main(a, w)) {
        if (!memcmp((const void *)(uintptr_t)a, &v, w)) return 0;   // already holds the value: no mprotect round trip every tick
        return dm_code_poke(g_main, a - (uint64_t)(uintptr_t)g_main->base, &v, w);
    }
    if (!addr_in_as(a, w)) return -1;
    if (write(g_pipe[1], &v, w) != (ssize_t)w) { pipe_drain(); return -1; }
    if (read(g_pipe[0], (void *)(uintptr_t)a, w) != (ssize_t)w) { pipe_drain(); return -1; }   // EFAULT on a protected page
    return 0;
}
static int region_base(unsigned mt, uint64_t *b) {
    switch (mt) {
    case 0: *b = (uint64_t)(uintptr_t)g_main->base; return 0;
    case 1: *b = g_heap; return 0;
    case 2: *b = g_alias0; return 0;
    case 3: *b = g_as0; return 0;
    }
    return -1;
}

// ---- opcode helpers
static int width_ok(unsigned w) { return w == 1 || w == 2 || w == 4 || w == 8; }
static uint64_t wmask(unsigned w) { return w >= 8 ? ~0ull : (1ull << (w * 8)) - 1; }
static int cond_ok(unsigned c, uint64_t a, uint64_t b, int *bad) {
    switch (c) {
    case 1: return a > b; case 2: return a >= b; case 3: return a < b; case 4: return a <= b; case 5: return a == b; case 6: return a != b;
    }
    *bad = 1; return 0;
}
static int arith(unsigned t, uint64_t a, uint64_t b, unsigned w, uint64_t *out) {
    uint64_t r;
    switch (t) {
    case 0: r = a + b; break; case 1: r = a - b; break; case 2: r = a * b; break;
    case 3: r = b >= 64 ? 0 : a << b; break; case 4: r = b >= 64 ? 0 : a >> b; break;
    case 5: r = a & b; break; case 6: r = a | b; break; case 7: r = ~a; break; case 8: r = a ^ b; break; case 9: r = a; break;
    default: return -1;
    }
    *out = r & wmask(w); return 0;
}
// words of the opcode at p (avail words left), 0 = unknown opcode or truncated
static unsigned op_len(const uint32_t *p, unsigned avail) {
    uint32_t f = p[0]; unsigned len = 0, t = f >> 28;
    switch (t) {
    case 0: case 1: len = (f >> 24 & 0xF) == 8 ? 4 : 3; break;
    case 2: case 8: len = 1; break;
    case 3: len = (f >> 24 & 0xF) == 0 ? 2 : 1; break;
    case 4: case 6: len = 3; break;
    case 5: case 7: len = 2; break;
    case 9: len = (f >> 8 & 0xF) ? ((f >> 24 & 0xF) == 8 ? 3 : 2) : 1; break;
    case 0xA: { unsigned o = f >> 8 & 0xF; len = (o == 2 || o == 4 || o == 5) ? 2 : 1; break; }
    case 0xC: if ((f >> 24 & 0xF) == 0) { unsigned c = f >> 8 & 0xF; len = c == 0 ? ((f >> 20 & 0xF) == 8 ? 3 : 2) : (c == 2 || c == 4) ? 2 : 1; } break;
    }
    return len && len <= avail ? len : 0;
}
static int is_begin(uint32_t f) { unsigned t = f >> 28; return t == 1 || t == 8 || (t == 0xC && (f >> 24 & 0xF) == 0); }
// Skip from *pc (the opcode after a false Begin, or after an Else met while running a true block) to the matching End; with want_else an Else at
// this nesting level ends the skip too (return 1: run the else part). *used counts the words scanned against the budget. -1 malformed.
static int skip_block(const uint32_t *op, unsigned n, unsigned *pc, int want_else, uint32_t *used) {
    int nest = 0;
    while (*pc < n) {
        unsigned len = op_len(op + *pc, n - *pc);
        if (!len) return -1;
        uint32_t f = op[*pc]; *pc += len; *used += 1;
        if (is_begin(f)) nest++;
        else if (f >> 28 == 2) {
            int is_else = (f >> 24 & 0xF) == 1;
            if (!nest) return is_else && want_else;
            if (!is_else) nest--;
        }
    }
    return 0;   // unterminated: the block runs to the end of the program
}
static uint64_t pad_pressed(void) { return dm_hid_keys(); }

// ---- the VM: one cheat, one pass
static int run_inner(const cheat_t *c, uint32_t tick_used, uint32_t *used_out) {
    uint64_t r[16] = {0}; unsigned loop_top[16] = {0}, pc = 0, n = c->n; int depth = 0, bad = 0; uint32_t used = 0;
#define RETURN_(rc) do { *used_out = used; return (rc); } while (0)
    const uint32_t *op = c->op;
    while (pc < n) {
        if (used >= CHEAT_BUDGET) { LOG("[dmnt] cheat %s: more than %d opcodes in one tick (runaway loop)", c->name, CHEAT_BUDGET); RETURN_(R_BAD); }
        if (tick_used + used >= TICK_BUDGET) RETURN_(R_FAULT);
        unsigned len = op_len(op + pc, n - pc);
        if (!len) { LOG("[dmnt] cheat %s: unknown or truncated opcode %08X at word %u", c->name, op[pc], pc); RETURN_(R_BAD); }
        const uint32_t *p = op + pc; uint32_t f = p[0]; unsigned next = pc + len; used++;
        unsigned t = f >> 28, w = f >> 24 & 0xF, mt = f >> 20 & 0xF, rr = f >> 16 & 0xF; uint64_t base = 0, rel = (uint64_t)(f & 0xFF) << 32 | (len > 1 ? p[1] : 0);
        switch (t) {
        case 0: {   // 0TMR00AA AAAAAAAA VVVVVVVV (VVVVVVVV): *(region + reg[R] + A) = V
            uint64_t v = w == 8 ? (uint64_t)p[2] << 32 | p[3] : p[2];
            if (!width_ok(w) || region_base(mt, &base)) RETURN_(R_BAD);
            if (mem_write(base + r[rr] + rel, w, v)) RETURN_(R_FAULT);
            break; }
        case 1: {   // 1TMC00AA AAAAAAAA VVVVVVVV (VVVVVVVV): if (*(region + A) cond V) { ... }
            uint64_t v = w == 8 ? (uint64_t)p[2] << 32 | p[3] : p[2], mv;
            if (!width_ok(w) || region_base(mt, &base)) RETURN_(R_BAD);
            if (mem_read(base + rel, w, &mv)) RETURN_(R_FAULT);
            int ok = cond_ok(rr, mv, v & wmask(w), &bad); if (bad) RETURN_(R_BAD);
            if (ok) depth++; else { int e = skip_block(op, n, &next, 1, &used); if (e < 0) RETURN_(R_BAD); if (e) depth++; }
            break; }
        case 2:     // 2X000000: end of a conditional (X = 0) or else (X = 1: the true part ran, skip the rest)
            if ((f >> 24 & 0xF) == 1) { if (skip_block(op, n, &next, 0, &used) < 0) RETURN_(R_BAD); if (depth) depth--; }
            else if (depth) depth--;
            break;
        case 3:     // 300R0000 VVVVVVVV start loop (reg = count) / 310R0000 end loop: Atmosphere runs the body count + 1 times
            if (w == 0) { r[rr] = p[1]; loop_top[rr] = next; }
            else if (r[rr]) { r[rr]--; next = loop_top[rr]; }
            break;
        case 4: r[rr] = (uint64_t)p[1] << 32 | p[2]; break;   // 400R0000 VVVVVVVV VVVVVVVV
        case 5: {   // 5TMRI0AA AAAAAAAA: reg[R] = *(region + A) (I=0) / *(reg[R] + A) (I=1) / *(region + reg[R] + A) (I=2)
            unsigned li = f >> 12 & 0xF; uint64_t a, v;
            if (!width_ok(w)) RETURN_(R_BAD);
            if (li == 1) a = r[rr] + rel;
            else if (li == 0 || li == 2) { if (region_base(mt, &base)) RETURN_(R_BAD); a = base + rel + (li == 2 ? r[rr] : 0); }
            else RETURN_(R_BAD);
            if (mem_read(a, w, &v)) RETURN_(R_FAULT);
            r[rr] = v;
            break; }
        case 6: {   // 6T0RIFG0 VVVVVVVV VVVVVVVV: *(reg[R] (+ reg[G])) = V; I: reg[R] += width
            uint64_t a = r[rr], v = (uint64_t)p[1] << 32 | p[2];
            if (!width_ok(w)) RETURN_(R_BAD);
            if (f >> 8 & 0xF) a += r[f >> 4 & 0xF];
            if (mem_write(a, w, v)) RETURN_(R_FAULT);
            if (f >> 12 & 0xF) r[rr] += w;
            break; }
        case 7:     // 7T0RC000 VVVVVVVV: reg[R] = reg[R] op V (legacy)
            if (!width_ok(w) || arith(f >> 12 & 0xF, r[rr], p[1], w, &r[rr])) RETURN_(R_BAD);
            break;
        case 8: {   // 8kkkkkkk: the block runs when every key of the mask is down
            uint32_t km = f & 0x0FFFFFFF;
            if ((pad_pressed() & km) == km) depth++; else { int e = skip_block(op, n, &next, 1, &used); if (e < 0) RETURN_(R_BAD); if (e) depth++; }
            break; }
        case 9: {   // 9TCRS0s0 / 9TCRS1x0 VVVVVVVV: reg[R] = reg[S] op (reg[s] | V)
            unsigned sr = f >> 12 & 0xF; uint64_t b = (f >> 8 & 0xF) ? (w == 8 ? (uint64_t)p[1] << 32 | p[2] : p[1]) : r[f >> 4 & 0xF];
            if (!width_ok(w) || arith(mt, r[sr], b, w, &r[rr])) RETURN_(R_BAD);
            break; }
        case 0xA: { // ATSRIOxa (aaaaaaaa): *(address by offset type O) = reg[S]; I: reg[R] += width
            unsigned sr = f >> 20 & 0xF, ar = f >> 16 & 0xF, ot = f >> 8 & 0xF, m2 = f >> 4 & 0xF; uint64_t a = r[ar], arel = (uint64_t)(f & 0xF) << 32 | (len > 1 ? p[1] : 0);
            if (!width_ok(w)) RETURN_(R_BAD);
            switch (ot) {
            case 0: break;
            case 1: a += r[f >> 4 & 0xF]; break;
            case 2: a += arel; break;
            case 3: if (region_base(m2, &base)) RETURN_(R_BAD); a = base + r[ar]; break;
            case 4: if (region_base(m2, &base)) RETURN_(R_BAD); a = base + arel; break;
            case 5: if (region_base(m2, &base)) RETURN_(R_BAD); a = base + r[ar] + arel; break;
            default: RETURN_(R_BAD);
            }
            if (mem_write(a, w, r[sr] & wmask(w))) RETURN_(R_FAULT);
            if (f >> 12 & 0xF) r[ar] += w;
            break; }
        case 0xC: { // C0TcSX## : if (reg[S] cond operand) { ... }; X: 0 static, 1 register, 2 region + A, 3 region + reg, 4 reg + A, 5 reg + reg
            unsigned cw = f >> 20 & 0xF, cc = f >> 16 & 0xF, sr = f >> 12 & 0xF, ct = f >> 8 & 0xF, x = f >> 4 & 0xF, y = f & 0xF; uint64_t v = 0, a = 0;
            if (!width_ok(cw)) RETURN_(R_BAD);
            uint64_t crel = (uint64_t)y << 32 | (len > 1 ? p[1] : 0);
            switch (ct) {
            case 0: v = cw == 8 ? (uint64_t)p[1] << 32 | p[2] : p[1]; break;
            case 1: v = r[x]; break;
            case 2: case 3: if (region_base(x, &base)) RETURN_(R_BAD); a = base + (ct == 2 ? crel : r[y]); break;
            case 4: a = r[x] + crel; break;
            case 5: a = r[x] + r[y]; break;
            default: RETURN_(R_BAD);
            }
            if (ct >= 2 && mem_read(a, cw, &v)) RETURN_(R_FAULT);
            int ok = cond_ok(cc, r[sr] & wmask(cw), v & wmask(cw), &bad); if (bad) RETURN_(R_BAD);
            if (ok) depth++; else { int e = skip_block(op, n, &next, 1, &used); if (e < 0) RETURN_(R_BAD); if (e) depth++; }
            break; }
        default: RETURN_(R_BAD);
        }
        pc = next;
    }
    RETURN_(R_OK_);
#undef RETURN_
}
static int run_cheat(const cheat_t *c, uint32_t *tick_used) {
    uint32_t u = 0; int rc = run_inner(c, *tick_used, &u);
    *tick_used += u; return rc;
}


// ---- file loading
static char *slurp_text(const char *path) {
    FILE *f = fopen(path, "rb"); struct stat st;
    if (!f) return NULL;
    if (fstat(fileno(f), &st) || st.st_size < 0 || st.st_size > (4 << 20)) { fclose(f); return NULL; }
    char *p = malloc((size_t)st.st_size + 1);
    if (p && fread(p, 1, (size_t)st.st_size, f) != (size_t)st.st_size) { free(p); p = NULL; }
    fclose(f);
    if (p) p[st.st_size] = 0;
    return p;
}
static char *trim(char *s) { while (*s == ' ' || *s == '\t' || *s == '\r') s++; size_t l = strlen(s); while (l && isspace((unsigned char)s[l - 1])) s[--l] = 0; return s; }
static int parse_name(char *line, char open, char close, char *out, size_t n) {
    char *e = strchr(line + 1, close);
    if (line[0] != open || !e) return -1;
    *e = 0; snprintf(out, n, "%s", trim(line + 1)); return 0;
}
static void parse_cheats(char *txt) {
    cheat_t *cur = NULL; unsigned bad_words = 0;
    for (char *line = txt, *next; line && *line; line = next) {
        next = strchr(line, '\n'); if (next) *next++ = 0;
        line = trim(line);
        if (!*line || !strncmp(line, "//", 2)) continue;
        if (*line == '[' || *line == '{') {
            if (g_nch >= MAX_CHEATS) { LOG("[dmnt] more than %d cheats: the rest is ignored", MAX_CHEATS); return; }
            cur = &g_ch[g_nch++]; memset(cur, 0, sizeof *cur);
            cur->master = *line == '{';
            if (parse_name(line, *line, cur->master ? '}' : ']', cur->name, sizeof cur->name)) { g_nch--; cur = NULL; continue; }
            cur->enabled = cur->master;
            cur->op = malloc(MAX_OPS * sizeof(uint32_t));
            if (!cur->op) { cur->dead = 1; }
            continue;
        }
        if (!cur || cur->dead) continue;
        for (char *t = strtok(line, " \t"); t; t = strtok(NULL, " \t")) {
            char *e; unsigned long v = strtoul(t, &e, 16);
            if (strlen(t) != 8 || *e) { if (bad_words++ < 4) LOG("[dmnt] cheat %s: \"%s\" is not an 8-digit hex word, line skipped", cur->name, t); break; }
            if (cur->n >= MAX_OPS) { LOG("[dmnt] cheat %s: more than %d words, disabled", cur->name, MAX_OPS); cur->dead = 1; break; }
            cur->op[cur->n++] = (uint32_t)v;
        }
    }
}
static void parse_toggles(char *txt) {
    for (char *line = txt, *next; line && *line; line = next) {
        next = strchr(line, '\n'); if (next) *next++ = 0;
        line = trim(line);
        char name[64]; char *eq = strchr(line, '=');
        if ((*line != '[' && *line != '{') || !eq) continue;
        *eq = 0; char *key = trim(line), *val = trim(eq + 1);
        if (parse_name(key, *key, *key == '[' ? ']' : '}', name, sizeof name)) continue;
        int on = !strcasecmp(val, "true") || !strcmp(val, "1") || !strcasecmp(val, "on") || !strcasecmp(val, "yes");
        for (unsigned i = 0; i < g_nch; i++) if (!strcmp(g_ch[i].name, name)) g_ch[i].enabled = on;
    }
}

static int dmnt_init(void) {
    char dir[1100], path[1200]; uint8_t bid[0x20];
    if (dm_tl2_data_path("cheats", dir, sizeof dir)) return -1;
    struct stat st; if (stat(dir, &st) || !S_ISDIR(st.st_mode)) return -1;   // no folder: silent
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) g_main = &g_mod[i];
    if (!g_main || !dm_main_build_id(bid)) { LOG("[dmnt] %s exists but the main build id is not known: cheats off", dir); return -1; }
    char b8[17]; for (int i = 0; i < 8; i++) snprintf(b8 + 2 * i, 3, "%02X", bid[i]);
    snprintf(path, sizeof path, "%s/%s.txt", dir, b8);
    char *txt = slurp_text(path);
    if (!txt) { LOG("[dmnt] no cheat file %s: cheats off", path); return -1; }
    uint64_t b[4] = {0}; void dm_guest_as_bounds(uint64_t b[4]); dm_guest_as_bounds(b);
    g_as0 = b[0]; g_as1 = b[1]; g_alias0 = b[2]; g_alias1 = b[3]; g_heap = b[0] + 0x40000000ull;
    if (!g_as1 || g_heap >= g_as1) { LOG("[dmnt] the guest address space is not set up: cheats off"); free(txt); return -1; }
    if (pipe(g_pipe) || fcntl(g_pipe[0], F_SETFL, O_NONBLOCK) || fcntl(g_pipe[1], F_SETFL, O_NONBLOCK)
        || fcntl(g_pipe[0], F_SETFD, FD_CLOEXEC) || fcntl(g_pipe[1], F_SETFD, FD_CLOEXEC)) { LOG("[dmnt] pipe failed (errno %d): cheats off", errno); free(txt); return -1; }
    parse_cheats(txt); free(txt);
    snprintf(path, sizeof path, "%s/toggles.txt", dir);
    if ((txt = slurp_text(path))) { parse_toggles(txt); free(txt); }
    unsigned masters = 0, on = 0;
    for (unsigned i = 0; i < g_nch; i++) { masters += g_ch[i].master; on += g_ch[i].enabled && !g_ch[i].dead; }
    LOG("[dmnt] %s.txt: %u cheat(s) (%u master), %u enabled; main %p, heap %#llx, alias %#llx-%#llx, aslr %#llx", b8, g_nch, masters, on, g_main->base,
        (unsigned long long)g_heap, (unsigned long long)g_alias0, (unsigned long long)g_alias1, (unsigned long long)g_as0);
    for (unsigned i = 0; i < g_nch; i++) LOG("[dmnt]   %c%s%c %s, %u words%s", g_ch[i].master ? '{' : '[', g_ch[i].name, g_ch[i].master ? '}' : ']',
                                              g_ch[i].enabled ? "on" : "off", g_ch[i].n, g_ch[i].dead ? " (refused)" : "");
    return on ? 0 : -1;
}

void dm_dmnt_tick(uint64_t frame) {
    static _Atomic int state;   // 0 not looked yet, 1 running, -1 off for good
    int s = atomic_load_explicit(&state, memory_order_relaxed);
    if (s < 0 || frame < START_FRAME) return;
    if (pthread_mutex_trylock(&g_mu)) return;
    if (!s) { s = dmnt_init() ? -1 : 1; atomic_store(&state, s); }
    if (s > 0) {
        uint32_t used = 0; static int said_budget;
        for (unsigned i = 0; i < g_nch; i++) {
            cheat_t *c = &g_ch[i];
            if (!c->enabled || c->dead || !c->n) continue;
            int rc = run_cheat(c, &used);
            if (rc == R_BAD) { c->dead = 1; LOG("[dmnt] cheat %s disabled (malformed opcode or runaway loop)", c->name); }
            else if (rc == R_FAULT) {
                if (used >= TICK_BUDGET) { if (!said_budget++) LOG("[dmnt] tick budget of %d opcodes used up: the remaining cheats wait for the next tick", TICK_BUDGET); break; }
                if (c->faults++ < 3) LOG("[dmnt] cheat %s: an address is outside the guest memory or not mapped (frame %llu), retried next tick", c->name, (unsigned long long)frame);
            }
        }
    }
    pthread_mutex_unlock(&g_mu);
}
#endif
