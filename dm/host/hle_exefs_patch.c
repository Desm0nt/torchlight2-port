// [TL2] ExeFS patches in the Atmosphere formats (IPS, IPS32, pchtxt), applied once to the main module image before the guest runs.
// Folder: <data folder>/exefs_patches/<name>/ (Download/Torchlight2Mobile/exefs_patches/<name>/), holding
//   <build id>.ips   the first 8 bytes of the main build id in upper-case hex (as Atmosphere names them; "PATCH" ... "EOF", or "IPS32" ... "EEOF"),
//   *.pchtxt         "@nsobid-<hex>", "@flag offset_shift 0xN", "@enabled" / "@disabled" blocks of "OFFSET HEXBYTES" lines, "//" comments.
// An offset in the file is an NSO file offset: the module image offset is the file offset minus 0x100 (the NSO header; for pchtxt the
// "@flag offset_shift" value replaces the 0x100). Off by default: no exefs_patches folder = this function returns at once, nothing logged.
// A record is refused (logged "[exefs] refused ...") when it lies below the header shift or outside the module's code / read-only / data image
// (the rewriter's patch area, key page and anything past the image are never written). Not done: the overlap check against the sites the offline
// rewriter changed (the data folder ships only main.patched.bin / fixups.tsv / gates.tsv, not sites.tsv or equiv.json), so a patch over a rewritten
// svc / timer site is the author's responsibility.
// The shared helpers (dm_code_poke, dm_tl2_data_path) live here too; hle_dmnt.c uses them.
#ifdef DM_GAME_TL2
#include "dm.h"
#include <ctype.h>
#include <dirent.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// <data folder>/<rel>: DM_ASTC_DATA_DIR is the Download/Torchlight2Mobile folder (jni_android.c); off a phone DM_ROMFS/.. stands in for it
int dm_tl2_data_path(const char *rel, char *out, size_t n) {
    const char *e = getenv("DM_ASTC_DATA_DIR");
    if (e && *e) return snprintf(out, n, "%s/%s", e, rel) < (int)n ? 0 : -1;
    e = getenv("DM_ROMFS");
    if (e && *e) return snprintf(out, n, "%s/../%s", e, rel) < (int)n ? 0 : -1;
    return -1;
}

// Write into a module image. The code pages are read-only + execute (loader.c map_image seals [0, text_end) and the patch area); the part between
// stays writable. Same pattern as hle_tl2_wide.c tw_poke (16 KB granule, clear_cache), but RWX first so a guest thread running on the page
// meanwhile (the cheat VM pokes while the game runs) does not fault; RW only when RWX is refused. 0 / -1. Never writes the patch area.
int dm_code_poke(module_t *m, uint64_t off, const void *src, size_t n) {
    if (!m || !m->base || !n || off > m->size || n > m->size - off) return -1;
    uint64_t po = (int64_t)m->patch_off > 0 ? m->patch_off & ~0x3FFFull : m->size, rx_end = (m->text_end + 0x3FFF) & ~0x3FFFull;
    if (off + n > po) return -1;
    uint8_t *at = m->base + off, *end = at + n;
    if (off >= rx_end) { memcpy(at, src, n); return 0; }   // read-only data / data pages: plainly writable
    uint8_t *pg = (uint8_t *)((uintptr_t)at & ~(uintptr_t)0x3FFF), *rx_top = m->base + rx_end;
    size_t len = (size_t)(((uintptr_t)end - (uintptr_t)pg + 0x3FFF) & ~(uintptr_t)0x3FFF);
    if (mprotect(pg, len, PROT_READ | PROT_WRITE | PROT_EXEC) && mprotect(pg, len, PROT_READ | PROT_WRITE)) return -1;
    memcpy(at, src, n);
    size_t rx_len = pg + len > rx_top ? (size_t)(rx_top - pg) : len;   // the pages above the code stay writable, as the loader left them
    if (mprotect(pg, rx_len, PROT_READ | PROT_EXEC)) die("[exefs] mprotect RX");
    __builtin___clear_cache((char *)at, (char *)end);
    return 0;
}

static module_t *main_module(void) {
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) return &g_mod[i];
    return NULL;
}

static uint8_t *slurp_file(const char *path, size_t *n) {
    struct stat st; FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fstat(fileno(f), &st) || st.st_size <= 0 || st.st_size > (16 << 20)) { fclose(f); return NULL; }
    uint8_t *p = malloc((size_t)st.st_size + 1);
    if (p && fread(p, 1, (size_t)st.st_size, f) != (size_t)st.st_size) { free(p); p = NULL; }
    fclose(f);
    if (p) { *n = (size_t)st.st_size; p[*n] = 0; }
    return p;
}

typedef struct { module_t *m; const char *tag; unsigned applied, refused; uint64_t bytes; } ctx_t;

// One record: raw = file offset, shift = the header shift. 0 applied / -1 refused (logged; at most 16 refusal lines per file)
static int apply_record(ctx_t *c, uint64_t raw, uint64_t shift, const uint8_t *b, size_t n) {
    const char *why = NULL;
    uint64_t limit = (int64_t)c->m->patch_off > 0 ? c->m->patch_off : c->m->size;
    if (raw < shift) why = "below the NSO header shift";
    else if (raw - shift >= limit || n > limit - (raw - shift)) why = "outside the code / read-only / data image";
    else if (dm_code_poke(c->m, raw - shift, b, n)) why = "write failed";
    if (why) {
        if (c->refused++ < 16) LOG("[exefs] refused %s: offset %#llx +%zu: %s", c->tag, (unsigned long long)raw, n, why);
        return -1;
    }
    c->applied++; c->bytes += n;
    return 0;
}

static void finish(ctx_t *c) {
    LOG("[exefs] %s: %u record(s), %llu byte(s) applied, %u refused", c->tag, c->applied, (unsigned long long)c->bytes, c->refused);
}

// IPS ("PATCH" / "EOF", 3-byte offsets) and IPS32 ("IPS32" / "EEOF", 4-byte offsets); size 0 = RLE (2-byte count, 1-byte value)
static void apply_ips(ctx_t *c, const uint8_t *d, size_t n) {
    int wide = n >= 5 && !memcmp(d, "IPS32", 5);
    if (!wide && !(n >= 5 && !memcmp(d, "PATCH", 5))) { LOG("[exefs] refused %s: not an IPS file (no PATCH / IPS32 header)", c->tag); return; }
    size_t p = 5, ow = wide ? 4 : 3; uint8_t *rle = NULL; size_t rle_cap = 0;
    for (;;) {
        if (p + ow > n) { LOG("[exefs] %s: no EOF marker, stopped at the end of the file", c->tag); break; }
        if (!memcmp(d + p, wide ? "EEOF" : "EOF", ow)) break;
        uint64_t off = 0; for (size_t i = 0; i < ow; i++) off = off << 8 | d[p + i];
        p += ow;
        if (p + 2 > n) { LOG("[exefs] refused %s: truncated record at %#llx", c->tag, (unsigned long long)off); c->refused++; break; }
        size_t len = (size_t)d[p] << 8 | d[p + 1]; p += 2;
        if (len) {
            if (p + len > n) { LOG("[exefs] refused %s: truncated record at %#llx", c->tag, (unsigned long long)off); c->refused++; break; }
            apply_record(c, off, 0x100, d + p, len); p += len;
        } else {
            if (p + 3 > n) { LOG("[exefs] refused %s: truncated RLE record at %#llx", c->tag, (unsigned long long)off); c->refused++; break; }
            size_t cnt = (size_t)d[p] << 8 | d[p + 1]; uint8_t v = d[p + 2]; p += 3;
            if (cnt > rle_cap) { free(rle); rle = malloc(cnt); rle_cap = rle ? cnt : 0; }
            if (!rle || !cnt) { c->refused++; continue; }
            memset(rle, v, cnt); apply_record(c, off, 0x100, rle, cnt);
        }
    }
    free(rle);
}

static int hex_nibble(int ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1; }

// pchtxt: "@nsobid-<hex>" must match the main build id (a prefix compare, the id may be given in 16 or 32 bytes), else the whole file is refused
static void apply_pchtxt(ctx_t *c, char *txt, const uint8_t *bid) {
    char bidhex[65]; for (int i = 0; i < 0x20; i++) snprintf(bidhex + 2 * i, 3, "%02X", bid[i]);
    uint64_t shift = 0x100; int active = 0, seen_bid = 0;
    for (char *line = txt, *next; line && *line; line = next) {
        next = strchr(line, '\n'); if (next) *next++ = 0;
        char *cm = strstr(line, "//"); if (cm && (cm == line || cm[-1] == ' ' || cm[-1] == '\t')) *cm = 0;   // comments: from a "//" at the start or after a blank
        while (*line == ' ' || *line == '\t') line++;
        size_t ll = strlen(line); while (ll && isspace((unsigned char)line[ll - 1])) line[--ll] = 0;
        if (!*line) continue;
        if (*line == '@') {
            if (!strncmp(line, "@nsobid-", 8)) {
                const char *h = line + 8; size_t hl = strlen(h);
                if (hl > 64) hl = 64;
                if (!hl || strncasecmp(h, bidhex, hl)) { LOG("[exefs] refused %s: @nsobid-%s is not this build (main %.16s)", c->tag, h, bidhex); return; }
                seen_bid = 1;
            } else if (!strncmp(line, "@flag", 5)) {
                char key[40] = {0}, val[40] = {0};
                if (sscanf(line + 5, "%39s %39s", key, val) == 2 && !strcmp(key, "offset_shift")) shift = strtoull(val, NULL, 0);
            } else if (!strcmp(line, "@enabled")) active = 1;
            else if (!strcmp(line, "@disabled")) active = 0;
            else if (!strcmp(line, "@stop")) break;
            continue;   // @title / @name / @version / @author ...
        }
        if (!active) continue;
        if (!seen_bid) { LOG("[exefs] refused %s: patch lines before any @nsobid-", c->tag); return; }
        char *end; uint64_t off = strtoull(line, &end, 16);
        if (end == line) { c->refused++; continue; }
        while (*end == ' ' || *end == '\t') end++;
        uint8_t buf[512]; size_t n = 0;
        if (*end == '"') {   // string patch: the characters between the quotes, then a NUL (as the pchtxt tools write it)
            end++;
            while (*end && *end != '"' && n < sizeof buf - 1) buf[n++] = (uint8_t)*end++;
            buf[n++] = 0;
        } else {
            while (n < sizeof buf) { int a = hex_nibble(end[0]), b = a < 0 ? -1 : hex_nibble(end[1]); if (b < 0) break; buf[n++] = (uint8_t)(a << 4 | b); end += 2; }
            if (!n || hex_nibble(*end) >= 0) { if (c->refused++ < 16) LOG("[exefs] refused %s: offset %#llx: bad hex bytes", c->tag, (unsigned long long)off); continue; }
        }
        apply_record(c, off, shift, buf, n);
    }
}

static void apply_file(module_t *m, const char *path, const char *tag, int ips, const uint8_t *bid) {
    size_t n = 0; uint8_t *d = slurp_file(path, &n);
    if (!d) { LOG("[exefs] refused %s: cannot read %s", tag, path); return; }
    ctx_t c = {m, tag, 0, 0, 0};
    if (ips) apply_ips(&c, d, n); else apply_pchtxt(&c, (char *)d, bid);
    finish(&c);
    free(d);
}

void dm_exefs_patch_apply(void) {
    static int done; if (done) return; done = 1;
    char root[1100];
    if (dm_tl2_data_path("exefs_patches", root, sizeof root)) return;
    DIR *d = opendir(root);
    if (!d) return;   // no folder = no patches, nothing logged
    uint8_t bid[0x20]; module_t *m = main_module();
    if (!m || !dm_main_build_id(bid)) { LOG("[exefs] %s exists but the main build id is not known: no patches applied", root); closedir(d); return; }
    char b8[17]; for (int i = 0; i < 8; i++) snprintf(b8 + 2 * i, 3, "%02X", bid[i]);
    LOG("[exefs] patch folder %s, main build id %s", root, b8);
    struct dirent *de; int found = 0;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char dir[1300]; snprintf(dir, sizeof dir, "%s/%s", root, de->d_name);
        struct stat st; if (stat(dir, &st) || !S_ISDIR(st.st_mode)) continue;
        char path[1500], tag[300];
        snprintf(path, sizeof path, "%s/%s.ips", dir, b8);
        if (access(path, R_OK)) { char lo[17]; for (int i = 0; i < 16; i++) lo[i] = (char)tolower((unsigned char)b8[i]); lo[16] = 0; snprintf(path, sizeof path, "%s/%s.ips", dir, lo); }
        if (!access(path, R_OK)) { snprintf(tag, sizeof tag, "%s/%s.ips", de->d_name, b8); apply_file(m, path, tag, 1, bid); found++; }
        DIR *sd = opendir(dir); struct dirent *fe;
        while (sd && (fe = readdir(sd))) {
            size_t l = strlen(fe->d_name);
            if (l > 7 && !strcasecmp(fe->d_name + l - 7, ".pchtxt")) {
                snprintf(path, sizeof path, "%s/%s", dir, fe->d_name); snprintf(tag, sizeof tag, "%s/%s", de->d_name, fe->d_name);
                apply_file(m, path, tag, 0, bid); found++;
            }
        }
        if (sd) closedir(sd);
    }
    closedir(d);
    if (!found) LOG("[exefs] no <build id>.ips or *.pchtxt for this build in %s", root);
}
#endif
