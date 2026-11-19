// [TL2] Characters of classes added by PC mods crashed "Change Character" / "Manage Characters" (GitHub issue #23). docs/tl2/TL2_PORT.md
// "Персонажи модовых классов: вылет на «Изменить персонажа»". Main v5 build a0c6c2d2, offsets = main.raw; every site is checked at install.
// How the console builds the character list (main+0x759000..0x75930c, run for both screens from the main menu's item handler 0x145ebc):
//   for each saved character (0x170-byte entries) it compares the class name (a libc++ std::wstring at entry +0x28, x26) with the eight
//   stock unit names (0x1df98b0 = wstring::compare, 0 = equal): Hum_Engineer_F / _M, Hum_Arbiter_F / _M, Hum_Berserker_F / _M,
//   Hum_Wanderer_F / _M. A match stores that class's three icon names ("wanderericonmale", "...hovermale", "...selectedmale") into the
//   row (x22 / x27 / x28, cleared to 0 before the compares) and 0x7555d0 then looks each name up by string (0x19fc430 -> 0x19f7dd0, which
//   reads the name's first byte). After the last compare (Hum_Wanderer_M) "not equal" jumps straight to the lookup (0x759198
//   cbnz w0, 0x759304) with the names still 0: SIGSEGV at main+0x19f7dd0 reading address 0, for any class that is not one of the eight.
//   The Switch build has no mods, so it never sees one.
// What this does: the last compare's branch (0x759198 cbnz + the 3 instructions of the Wanderer male case, sub / adrp / add, none of
// them a branch target) becomes a jump to a stub that keeps "equal" as it was and sends a class the game does not know to the Wanderer
// case too: the female one (0x7592d0) when the name ends in "_F" / "_f", else the male one (0x75919c, its 3 replaced instructions are
// done in the stub, then back to 0x7591a8). So a modded character gets the Outlander's portrait icons instead of none. Code writes:
// the 4 words at 0x759198 only. DM_TL2_CLASSICON=0 = off (internal kill switch; players never set it).
#ifdef DM_GAME_TL2
#include "hle.h"
#include "dm.h"
#include "pc.h"
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>

enum { SITE = 0x759198, MALE_BACK = 0x7591a8, FEMALE = 0x7592d0, ICON_X1 = 0x2025218 };
// 0x759194 bl compare(Hum_Wanderer_M); 0x759198 cbnz w0, 0x759304; 0x75919c sub x0, x29, #0x80; adrp x1, 0x2025000; add x1, x1, #0x218;
// 0x7591a8 bl; 0x7591ac adrp x8 ("wanderericonmale"); female case 0x7592d0 sub x0, x29, #0x80; adrp x1; add x1, #0x218; bl
static const uint32_t k_site[6] = {0x945a81c7, 0x35000b60, 0xd10203a0, 0x9000c661, 0x91086021, 0x945a7ee6};
static const uint32_t k_female[4] = {0xd10203a0, 0x9000c661, 0x91086021, 0x945a7e99};
// The stub (NDK clang): w0 = the Hum_Wanderer_M compare, x26 = the class name (libc++ wstring: byte 0 bit 0 = long; short size = byte 0
// >> 1, data +4; long size +8, data +16; UTF-32). x9..x11 / x16 are free here (scratch, the next bl clobbers them anyway).
static const uint32_t k_stub[24] = {
    0x34000280,   // cbz  w0, male                       (Hum_Wanderer_M: as before)
    0x39400349,   // ldrb w9, [x26]
    0x37000089,   // tbnz w9, #0, long
    0x53017d2a,   // lsr  w10, w9, #1                    (short: size)
    0x9100134b,   // add  x11, x26, #4                   (short: data)
    0x14000003,   // b    have
    0xf940074a,   // long: ldr x10, [x26, #8]
    0xf9400b4b,   //       ldr x11, [x26, #16]
    0xf100095f,   // have: cmp x10, #2
    0x5400016b,   // b.lt male
    0x8b0a096b,   // add  x11, x11, x10, lsl #2          (one past the last character)
    0xb85fc169,   // ldur w9, [x11, #-4]
    0x321b0129,   // orr  w9, w9, #0x20
    0x7101993f,   // cmp  w9, #'f'
    0x540000c1,   // b.ne male
    0xb85f8169,   // ldur w9, [x11, #-8]
    0x71017d3f,   // cmp  w9, #'_'
    0x54000061,   // b.ne male
    0x58000150,   // ldr  x16, lit_female
    0xd61f0200,   // br   x16
    0xd10203a0,   // male: sub x0, x29, #0x80             (the replaced 0x75919c..0x7591a4)
    0x58000061,   //       ldr x1, lit_x1                 (= main+0x2025218)
    0x58000090,   //       ldr x16, lit_male
    0xd61f0200,   //       br  x16                         (-> 0x7591a8)
};

static int poke(uint8_t *base, uint32_t off, const uint32_t *w, int n) {   // code is sealed RX in 16 KiB granules by the loader
    uint32_t *at = (uint32_t *)(void *)(base + off);
    uint8_t *pg = (uint8_t *)((uintptr_t)at & ~(uintptr_t)0x3FFF), *end = (uint8_t *)(at + n);
    size_t len = (size_t)(((uintptr_t)end - (uintptr_t)pg + 0x3FFF) & ~(uintptr_t)0x3FFF);
    if (mprotect(pg, len, PROT_READ | PROT_WRITE)) return -1;
    for (int i = 0; i < n; i++) at[i] = w[i];
    if (mprotect(pg, len, PROT_READ | PROT_EXEC)) die("[tl2classicon] mprotect RX");
    __builtin___clear_cache((char *)at, (char *)end);
    return 0;
}

// ---- install (main.c host_preload, after load_modules, before guest code runs)
void dm_tl2_classicon_install(void);
void dm_tl2_classicon_install(void) {
    const char *e = getenv("DM_TL2_CLASSICON");
    if (e && *e == '0' && !e[1]) { pc_note("tl2-classicon", 0, "%s", "DM_TL2_CLASSICON=0"); return; }
    module_t *m = NULL;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    if (!m || m->text_end < FEMALE + 16 || m->size < ICON_X1 + 32) { pc_note("tl2-classicon", 0, "%s", "no main module"); return; }
    if (memcmp(m->base + SITE - 4, k_site, sizeof k_site) || memcmp(m->base + FEMALE, k_female, sizeof k_female)) {
        pc_note("tl2-classicon", 0, "main+%#x differs: not TL2 v5 main (build a0c6c2d2)", SITE); return; }
    uint32_t *t = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (t == MAP_FAILED) { pc_note("tl2-classicon", 0, "%s", "mmap failed"); return; }
    memcpy(t, k_stub, sizeof k_stub);
    uint64_t *lit = (uint64_t *)(void *)(t + 24);   // lit_x1, lit_male, lit_female (8-aligned: 24 words = 0x60)
    lit[0] = (uint64_t)(uintptr_t)(m->base + ICON_X1);
    lit[1] = (uint64_t)(uintptr_t)(m->base + MALE_BACK);
    lit[2] = (uint64_t)(uintptr_t)(m->base + FEMALE);
    if (mprotect(t, 0x4000, PROT_READ | PROT_EXEC)) { munmap(t, 0x4000); pc_note("tl2-classicon", 0, "%s", "mprotect RX failed"); return; }
    __builtin___clear_cache((char *)t, (char *)(t + 30));
    uint64_t to = (uint64_t)(uintptr_t)t;
    const uint32_t jump[4] = {0x58000050 /* ldr x16,#8 */, 0xd61f0200 /* br x16 */, (uint32_t)to, (uint32_t)(to >> 32)};
    if (poke(m->base, SITE, jump, 4)) { munmap(t, 0x4000); pc_note("tl2-classicon", 0, "%s", "mprotect RW of the class compare refused"); return; }
    pc_note("tl2-classicon", 1, "character list: a class other than the 8 stock ones gets the Wanderer icons (main+%#x -> stub)", SITE);
}
#endif
