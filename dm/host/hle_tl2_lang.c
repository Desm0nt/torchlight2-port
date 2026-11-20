// [TL2] Brazilian Portuguese and Polish game text (issue #16).
// The game picks its CONSOLELOC/<code>.LANGUAGE from the system language (main+0x134474: oe::GetDesiredLanguage compared with nine
// nn::settings::Language values -> EN_US DE_DE JA_JP ZH_CN RU_RU ZH_TW FR_FR ES_ES IT_IT). Anything else, Portuguese and Polish included,
// takes the EN_US branch (main+0x134494: adrp/add x1 = "EN_US", b main+0x1345c8 = std::string::assign). PT_BR.LANGUAGE and
// PL_PL.LANGUAGE are complete translations in the RomFS that this picker never reaches on a Switch (PL_PL only through another check).
// When the player's language (DM_LANG: the gear choice or the system's) is Portuguese or Polish, that EN_US branch is pointed at the
// matching code instead: the code string goes into the 8 bytes of padding after the function (main+0x1345e8, two udf #0) and
// adrp/add become adr x1, main+0x1345e8 / nop. English players and every other language keep the game's own choice.
#ifdef DM_GAME_TL2
#include "hle.h"
#include "dm.h"
#include "pc.h"
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum { EN_BRANCH = 0x134494, PAD = 0x1345e8 };
static const uint32_t k_en[3] = {0xf000f261 /* adrp x1, main+0x1f83000 */, 0x9104d021 /* add x1, x1, #0x134 ("EN_US") */, 0x1400004b /* b main+0x1345c8 */};

static int poke(uint8_t *base, uint32_t off, const void *src, size_t n) {   // code is sealed RX in 16 KiB granules by the loader
    uint8_t *at = base + off, *pg = (uint8_t *)((uintptr_t)at & ~(uintptr_t)0x3FFF), *end = at + n;
    size_t len = (size_t)(((uintptr_t)end - (uintptr_t)pg + 0x3FFF) & ~(uintptr_t)0x3FFF);
    if (mprotect(pg, len, PROT_READ | PROT_WRITE)) return -1;
    memcpy(at, src, n);
    if (mprotect(pg, len, PROT_READ | PROT_EXEC)) die("[tl2lang] mprotect RX");
    __builtin___clear_cache((char *)at, (char *)end);
    return 0;
}

void dm_tl2_lang_install(void);
void dm_tl2_lang_install(void) {
    const char *e = getenv("DM_LANG"), *code = NULL;
    if (!e || !*e) return;
    if (!strncasecmp(e, "pt", 2) && (!e[2] || e[2] == '-' || e[2] == '_')) code = "PT_BR";
    else if (!strncasecmp(e, "pl", 2) && (!e[2] || e[2] == '-' || e[2] == '_')) code = "PL_PL";
    if (!code) return;
    module_t *m = NULL;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    static const uint32_t pad0[2] = {0, 0};
    if (!m || m->text_end < PAD + 8) { pc_note("tl2-lang", 0, "%s", "no main module"); return; }
    if (memcmp(m->base + EN_BRANCH, k_en, sizeof k_en) || memcmp(m->base + PAD, pad0, sizeof pad0)) {
        pc_note("tl2-lang", 0, "main+%#x differs: not TL2 v5 main (build a0c6c2d2)", EN_BRANCH); return; }
    char str[8] = {0}; memcpy(str, code, 5);
    const uint32_t adr = 0x10000001u | ((uint32_t)((PAD - EN_BRANCH) & 3) << 29) | ((uint32_t)((PAD - EN_BRANCH) >> 2) << 5);   // adr x1, PAD
    const uint32_t two[2] = {adr, 0xd503201f /* nop */};
    if (poke(m->base, PAD, str, sizeof str) || poke(m->base, EN_BRANCH, two, sizeof two)) { pc_note("tl2-lang", 0, "%s", "mprotect RW refused"); return; }
    pc_note("tl2-lang", 1, "DM_LANG %s: the game's English fallback reads CONSOLELOC/%s.LANGUAGE (issue #16)", e, code);
}
#endif
