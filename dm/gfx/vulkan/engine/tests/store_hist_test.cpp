// [tile] Host test of the store-hist bookkeeping (xbg_store_hist.h, used by xbg_game.cpp for DM_GFX_STORE_HIST). From the DOS2 port
// (dos2-port c3dd199 tests/store_hist_test.cpp) without the NB plan parts (TL2 has no NB backend): the sequences of the ab7rp3 phone run.
// Build, from this folder:  clang++ -std=c++17 -Wall -Wextra store_hist_test.cpp -o store_hist_test   (or g++)
#include "../xbg_store_hist.h"
#include <stdio.h>

static int failures;
#define CHECK(expression) do { if(!(expression)){printf("FAIL line %d: %s\n",__LINE__,#expression);++failures;} } while(0)
enum { COL = 1, DEP = 2, STE = 4 };

// The xbg_game.cpp paths in miniature: a pass begin (hist_begin), the heuristic's decision (store_hist_mask), the pass end marks.
static int begin(XbgHist &h, uint64_t frame, uint32_t clear, uint32_t dc, unsigned asp) {
    xbg_hist_attached(h, frame);
    if (xbg_hist_att_overwritten(clear, dc, 0, asp)) { xbg_hist_overwrite(h); return 0; }
    return xbg_hist_read(h);
}
static void test_overwritten() {
    CHECK(xbg_hist_att_overwritten(1u, 0, 0, COL) && !xbg_hist_att_overwritten(0, 0, 0, COL));
    CHECK(xbg_hist_att_overwritten(0, 1u, 0, COL) && xbg_hist_att_overwritten(0, 1u, 0, DEP | STE));   // load DONT_CARE
    CHECK(xbg_hist_att_overwritten(3u, 0, 0, DEP | STE) && !xbg_hist_att_overwritten(1u, 0, 0, DEP | STE));   // D24S8: both aspects
    CHECK(!xbg_hist_att_overwritten(2u, 0, 0, DEP | STE) && xbg_hist_att_overwritten(1u, 0, 0, DEP));
    CHECK(xbg_hist_att_overwritten(3u << 4, 0, 2, DEP | STE) && !xbg_hist_att_overwritten(3u << 4, 0, 1, DEP | STE));
    CHECK(xbg_hist_att_overwritten(0, 4u, 2, COL) && !xbg_hist_att_overwritten(0, 4u, 1, COL));
}
static void test_heuristic() {
    XbgHist h;
    for (uint64_t f = 10; f < 18; f++) { CHECK(begin(h, f, 3u, 0, DEP | STE) == 0); CHECK(!xbg_hist_may_discard(h, f, 8)); }
    CHECK(begin(h, 18, 3u, 0, DEP | STE) == 0 && xbg_hist_may_discard(h, 18, 8));   // 8 frames as an attachment, never read
    xbg_hist_discarded(h, false, XBG_HIST_BY_HIST, 18, 0, -1);
    CHECK(begin(h, 19, 3u, 0, DEP | STE) == 0 && !h.dead && !h.banned);           // cleared next frame: no violation
    xbg_hist_discarded(h, false, XBG_HIST_BY_HIST, 19, 0, -1);
    CHECK(xbg_hist_read(h) == 1 && h.banned && h.read_ever);                       // sampled while discarded
    CHECK(!xbg_hist_may_discard(h, 100, 8));                                       // never again
    XbgHist r; CHECK(begin(r, 1, 0, 0, COL) == 0 && r.read_ever);                   // LOADed from the start: read, never discarded
    for (uint64_t f = 1; f < 50; f++) CHECK(!xbg_hist_may_discard(r, f, 8));
    XbgHist c; xbg_hist_attached(c, 0); for (int i = 0; i < 9; i++) xbg_hist_attached(c, uint64_t(i)); CHECK(!xbg_hist_may_discard(c, 7, 8) && xbg_hist_may_discard(c, 8, 8));
    xbg_hist_discarded(c, true, XBG_HIST_BY_HIST, 8, 0, -1);                       // check mode: decided, nothing discarded
    CHECK(c.vdead && !c.dead && xbg_hist_read(c) == 2 && c.banned);
}
// Full overwrites lift a discard (load-op CLEAR, load DONT_CARE, in-pass whole clear / transfer clear / full copy): the later sample is
// no violation. A depth-only clear of D24S8 is no full overwrite: a LOAD after the discard is one.
static void test_overwrites() {
    const struct { uint32_t clear, dc; bool pass; } way[] = { { 3u, 0, true }, { 0, 1u, true }, { 0, 0, false } };
    for (const auto &w : way) {
        XbgHist h; xbg_hist_attached(h, 1);
        xbg_hist_discarded(h, false, XBG_HIST_BY_HIST, 1, 0, -1);
        CHECK(h.dead && h.by == XBG_HIST_BY_HIST && h.dead_frame == 1);
        if (w.pass) CHECK(begin(h, 2, w.clear, w.dc, DEP | STE) == 0);
        else xbg_hist_overwrite(h);   // xbg_game.cpp hist_note_overwrite
        CHECK(xbg_hist_read(h) == 0 && !h.banned);
    }
    XbgHist h; xbg_hist_discarded(h, false, XBG_HIST_BY_HIST, 3, 0, -1);
    CHECK(begin(h, 4, 1u, 0, DEP | STE) == 1 && h.banned);
}

int main() {
    test_overwritten(); test_heuristic(); test_overwrites();
    if (failures) { printf("store_hist_test: %d failure(s)\n", failures); return 1; }
    printf("store_hist_test: all passed\n"); return 0;
}
