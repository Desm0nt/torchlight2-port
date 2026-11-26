// [async-pipe-guard] port-common module (KB PNH-B-07 a, c; peanuts-native vka_psosync.h / vka_psogate.h, MIT, rules taken, code new).
// Two things around the background pipeline builds (DM_GFX_ASYNC_PIPELINES / gfx-async-pipelines, the warm-up threads):
//
// 1. Which draws are NOT left out while their pipeline builds in the background (xbg_ag_why). A left-out draw is missing from a few
//    frames, which is the point, except where the game keeps what the draw made: then the picture stays wrong after every pipeline
//    is there. peanuts-native, Super Mario Odyssey on an AYN Thor (Adreno 740, Turnip), first run without a pipeline cache: black
//    with only the HUD for the whole session (`7 % of draws skipped ... pipelines not made 11521`), right with async off and right
//    once the cache existed. Two kinds get their pipeline built in the frame, as with async off:
//      XBG_AG_SMALL   a draw into a target of at most 64x64 texels, whatever its geometry or blending: luminance / exposure
//                     adaptation (a tiny float target blended with its own last value every frame never recovers from an undefined
//                     start), lookup tables. This kind alone made Odyssey's first run right. Our mode-1 rule (blended draws) let
//                     exactly that draw through: an adaptation step blends with the destination.
//      XBG_AG_WRITES  a draw whose result is memory, not a picture: a stage writes a storage buffer, a storage image or a storage
//                     texel buffer, or the rasterizer is off (transform feedback, side-effect draws). Nothing redraws that next
//                     frame unless the game does.
//    Everything else keeps the port's own rule. Tried and dropped by peanuts (2026-11-15), not taken here: "at most 6 vertices"
//    (a 2D game draws its whole scene as quads: OCTOPATH TRAVELER 0, 99 pipelines held in the frame, frames up to 564 ms) and
//    "a target not drawn every frame" (no game needed it; TL2 / ETR already have their own target-age rules).
//
// 2. A gate over the background compiles (XbgPipeGate): the threads that build for a draw that waits (async builders: "frame"
//    builds) go first; the speculative ones (warm-up list, BC prewarm) start only while no frame build waits; at most `slots` run
//    at once. The frame path's own build (the translator thread) is never gated. Default slots: 1 on Turnip (driverID
//    VK_DRIVER_ID_MESA_TURNIP), 4 elsewhere (peanuts vkpg_cap_default). Their reasons, both on Turnip Gen8 V39 (Adreno 830 / 840):
//    Mario Kart World lost the device twice at 4 background slots, ran on at 1 (rendering.md "Concurrent background pipeline
//    compiles lose the device on Turnip", cause open, small sample); Ocarina of Time on an S25: three or four long Turnip compiles at
//    once took the process past vm.max_map_count and crashed the driver (memory.md, issues #127 / #132). Neither is seen on our
//    Adreno 7xx Turnip runs (2 warm-up threads for weeks): DM_GFX_PIPE_BG_SLOTS=2 gives our old count back on Turnip, =0 the old
//    ungated threads everywhere.
//
// Pure logic + a small C++ gate, no Vulkan and no engine types: tests/async_guard_test.cpp checks it on the host.
#pragma once
#include <stdint.h>
#include <stdlib.h>

enum { XBG_AG_SMALL = 1, XBG_AG_WRITES = 2,   // xbg_ag_why reasons = DM_GFX_ASYNC_GUARD bits
       XBG_AG_ALL = 3,                        // DM_GFX_ASYNC_GUARD default (unset)
       XBG_AG_SMALL_PX = 64 };                // a small target's side at most (peanuts VKPS_SMALL_PX; fixed, as theirs)

// DM_GFX_ASYNC_GUARD: unset / empty = both rules; "0" = neither (the port's rule exactly as before); else its low two bits
static inline int xbg_ag_mode(const char *env) {
    if (!env || !*env) return XBG_AG_ALL;
    return (int)(strtol(env, 0, 0) & XBG_AG_ALL);
}

// The draw as the port knows it where the pipeline is looked up. w, h: the pass's render area (0 = not known); writes: a stage writes
// memory (storage buffer / image / texel buffer); raster_discard: rasterizer discard on
typedef struct { uint32_t w, h; int writes, raster_discard; } xbg_ag_draw;

// The reason this draw's pipeline must be built in the frame although the port's rule would leave the draw out (XBG_AG_SMALL before
// XBG_AG_WRITES); 0 = no reason, the port's rule stands. mode: DM_GFX_ASYNC_GUARD bits
static inline int xbg_ag_why(int mode, const xbg_ag_draw *d) {
    if ((mode & XBG_AG_SMALL) && d->w && d->h && d->w <= XBG_AG_SMALL_PX && d->h <= XBG_AG_SMALL_PX) return XBG_AG_SMALL;
    if ((mode & XBG_AG_WRITES) && (d->writes || d->raster_discard)) return XBG_AG_WRITES;
    return 0;
}
static inline const char *xbg_ag_name(int why) { return why == XBG_AG_SMALL ? "small target" : why == XBG_AG_WRITES ? "writes memory" : "none"; }

// ---- the gate ----
enum { XBG_PG_CAP = 4,        // DM_GFX_PIPE_BG_SLOTS default off Turnip (peanuts VKPG_CAP)
       XBG_PG_CAP_MAX = 8 };  // the most it may ask for

// DM_GFX_PIPE_BG_SLOTS: unset / empty = 1 on Turnip, 4 elsewhere; "0" = no gate (the old ungated threads); n = n slots (1..8)
static inline int xbg_pg_slots(const char *env, int turnip) {
    if (!env || !*env) return turnip ? 1 : XBG_PG_CAP;
    long v = strtol(env, 0, 0);
    if (v <= 0) return 0;
    return v > XBG_PG_CAP_MAX ? XBG_PG_CAP_MAX : (int)v;
}

// The gate's state (the caller's lock): builds running, threads waiting (not counting the one asking)
typedef struct { int busy, frame_wait, spec_wait; } xbg_pg_state;

// May a thread start its build now? frame: a draw waits for it (else speculative). A frame build starts while a slot is free; a
// speculative one only while a slot is free and no frame build waits (peanuts vkpg_may_enter without the .heavy rule)
static inline int xbg_pg_may_enter(const xbg_pg_state *s, int slots, int frame) {
    if (slots < 1) return 1;   // no gate
    if (!frame && s->frame_wait > 0) return 0;
    return s->busy < slots;
}

#ifdef __cplusplus
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
// slots is set once before the first background thread starts (0 = off: enter / leave do nothing). Counters: waits and wait time
// per kind, builds per kind, the most running at once.
struct XbgPipeGate {
    std::mutex mu; std::condition_variable cv; xbg_pg_state s{}; int slots = 0;
    std::atomic<uint64_t> builds[2]{{0}, {0}}, waits[2]{{0}, {0}}, wait_ns[2]{{0}, {0}}; std::atomic<int> peak{0};   // [0] speculative, [1] frame
    void enter(bool frame) {
        if (slots <= 0) return;
        std::unique_lock<std::mutex> l(mu);
        if (!xbg_pg_may_enter(&s, slots, frame)) {
            const auto t0 = std::chrono::steady_clock::now(); int &w = frame ? s.frame_wait : s.spec_wait; ++w;
            cv.wait(l, [&] { xbg_pg_state o = s; --(frame ? o.frame_wait : o.spec_wait); return xbg_pg_may_enter(&o, slots, frame) != 0; });
            --w; ++waits[frame];
            wait_ns[frame] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
            cv.notify_all();   // a frame waiter gone may let a speculative one in
        }
        ++s.busy; ++builds[frame]; if (s.busy > peak.load()) peak = s.busy;
    }
    void leave() {
        if (slots <= 0) return;
        { std::lock_guard<std::mutex> l(mu); --s.busy; }
        cv.notify_all();
    }
    struct Hold { XbgPipeGate &g; Hold(XbgPipeGate &gate, bool frame) : g(gate) { g.enter(frame); } ~Hold() { g.leave(); } };
};
#endif
