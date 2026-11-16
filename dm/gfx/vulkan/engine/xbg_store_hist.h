// [tile] DM_GFX_STORE_HIST / DM_NB_RP store bookkeeping of one image (docs/dos2/DOS2_PORT.md "Тайловая политика проходов",
// "store-hist: учёт выброшенного store"). Pure C++, no Vulkan: xbg_game.cpp keeps one XbgHist per Image, tests/store_hist_test.cpp
// drives the same functions.
// [TL2] copied unchanged from the DOS2 port (dos2-port c3dd199). TL2 has no NB backend: only the history heuristic discards
// (XBG_HIST_BY_HIST); the plan fields stay unused.
//
// A discarded store (storeOp DONT_CARE, chosen by the history heuristic or applied from the NB-06 plan) leaves the image `dead`
// until all of it is overwritten (deferred ClearTexture, load-op CLEAR / DONT_CARE of the whole attachment, an in-pass clear of the
// whole attachment, a transfer clear or a full copy into a one-level one-layer image) or something reads it. A read while dead is a
// violation: counted, logged with who discarded and how it was read, and the image is banned - from then on neither the heuristic
// nor the plan (nbrp_begin / nbrp_lazy_begin mask it with nb_rp_keep_store) discards its store.
// Before 16.11 nothing but a deferred ClearTexture and the heuristic's own clear twin cleared `dead`: an attachment cleared through the
// plan's load-op CLEAR, an in-pass clear or a host-run clear-only pass and then sampled was reported as a violation it was not.
#ifndef XBG_STORE_HIST_H
#define XBG_STORE_HIST_H
#include <stdint.h>

enum { XBG_HIST_BY_NONE = 0, XBG_HIST_BY_HIST = 1, XBG_HIST_BY_PLAN = 2 };
struct XbgHist {
    uint64_t first_frame = 0;   // first frame seen as an attachment + 1 (0: never)
    uint64_t dead_frame = 0;    // frame of the last discard
    uint32_t dead_seq = 0;      // IR part (OP_X_NB_PASS seq) of a plan discard
    int32_t dead_node = -1;     // IR node of a plan discard
    bool read_ever = false, dead = false, vdead = false, banned = false;   // vdead: check mode (DM_GFX_STORE_HIST=2) would have discarded
    uint8_t by = XBG_HIST_BY_NONE;   // who discarded last
};
// A read of the old contents. 1: violation of an applied discard, 2: of a check-mode one (nothing was discarded), 0: none.
inline int xbg_hist_read(XbgHist &h) {
    const int v = h.dead ? 1 : h.vdead ? 2 : 0;
    if (v) h.banned = true;
    h.dead = h.vdead = false; h.read_ever = true;
    return v;
}
// All of the image overwritten before anything read it: the discarded contents can no longer be read.
inline void xbg_hist_overwrite(XbgHist &h) { h.dead = h.vdead = false; }
// The image is an attachment of a pass begun in `frame` (stats.presents): its first such frame is remembered.
inline void xbg_hist_attached(XbgHist &h, uint64_t frame) { if (!h.first_frame) h.first_frame = frame + 1; }
// The history heuristic may discard its store: never read in its life, an attachment for `frames` frames, never banned.
inline bool xbg_hist_may_discard(const XbgHist &h, uint64_t frame, unsigned frames) {
    return h.first_frame && !h.banned && !h.read_ever && frame + 1 >= h.first_frame + frames;
}
// The store was discarded at the end of a pass (check: decided only, STORE_HIST=2; by: XBG_HIST_BY_*).
inline void xbg_hist_discarded(XbgHist &h, bool check, uint8_t by, uint64_t frame, uint32_t seq, int32_t node) {
    if (check) h.vdead = true; else h.dead = true;
    h.by = by; h.dead_frame = frame; h.dead_seq = seq; h.dead_node = node;
}
// Attachment k of a pass begun with clear bits `clear` (2k colour / depth, 2k+1 stencil), `dc` (bit k: load DONT_CARE) and `aspects`
// (bit 0 colour, 1 depth, 2 stencil): 1 = all of it overwritten, 0 = something is loaded (a read).
inline int xbg_hist_att_overwritten(uint32_t clear, uint32_t dc, unsigned k, unsigned aspects) {
    if (dc >> k & 1u) return 1;
    if (aspects & 1u) return (clear >> (2 * k) & 1u) != 0;
    if ((aspects & 2u) && !(clear >> (2 * k) & 1u)) return 0;
    if ((aspects & 4u) && !(clear >> (2 * k + 1) & 1u)) return 0;
    return (aspects & 6u) != 0;
}
#endif
