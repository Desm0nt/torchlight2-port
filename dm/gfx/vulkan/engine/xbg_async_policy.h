// [TL2 stutter] Pure logic of the first-time pipeline work (issue #4, Mali-G615: 311 pipelines / 5085 ms built inside frames):
//   - which draws may be left out while their pipeline builds in the background (DM_GFX_ASYNC_PIPELINES 0 / 1 / 2),
//   - how old a render target is (drawn every present or once),
//   - the warm-up list reader shared by the device's own list and the seeds shipped in the APK.
// No device calls, so tests/async_policy_test.cpp calls the production code directly.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>
#include "xbg_vendor_policy.h"

// ---- DM_GFX_ASYNC_PIPELINES --------------------------------------------------------------------------------------------
// 0 every pipeline is built by the draw that needs it (Adreno default, as before)
// 1 blended draws only (the rule of cda4fe9 and before, unchanged)
// 2 also opaque / depth-writing draws into targets that are drawn every present (default when the GPU policy is not Adreno)
inline int xbg_async_mode_default(XbgGpuFamily policy){return policy==XBG_GPU_ADRENO?0:2;}
// Marker file gfx-async-pipelines (first byte, -1 = no marker) wins over the environment, which wins over the default.
// Old markers / values keep their meaning: '0' off, anything else on (= 1); only an explicit 2 selects mode 2.
inline int xbg_async_mode(int marker_byte,const char *env,XbgGpuFamily policy,const char **why){
    if(marker_byte>=0){if(why)*why="marker";return marker_byte=='0'?0:marker_byte=='2'?2:1;}
    if(env){if(why)*why="env";const int v=atoi(env);return v==0?0:v==2?2:1;}   // as before: atoi(e)!=0 = on, an empty value = off
    if(why)*why="default";
    return xbg_async_mode_default(policy);
}

// How a render target has been used lately, kept per image and advanced by every draw into it (mode 2 only).
//   run        consecutive presents, ending with `frame`, in which the image was drawn into
//   draws      draws into it in `frame` so far (the current one included, left-out ones too: what the game issued)
//   draws_prev draws into it in the present before `frame` (0 when it was not drawn then)
struct XbgTargetAge{uint64_t frame=UINT64_MAX;uint32_t run=0,draws=0,draws_prev=0;};
inline void xbg_target_age_touch(XbgTargetAge &a,uint64_t frame){
    if(a.frame!=frame){
        const bool next=a.frame!=UINT64_MAX && a.frame+1==frame;
        a.run=next?(a.run<0xffffffffu?a.run+1:a.run):1;a.draws_prev=next?a.draws:0;a.draws=0;a.frame=frame;
    }
    if(a.draws<0xffffffffu)++a.draws;
}
enum : uint32_t {
    XBG_ASYNC2_RUN=3,        // this present and each of the two before it (nx2android gfx_vk_exec.c: "drawn in each of the two presents before")
    XBG_ASYNC2_MIN_DRAWS=8   // "one draw among many": the target took at least this many draws in the present before
};
// A target a draw may be left out of (call after the touch of this draw):
//   - drawn in this present and in each of the two before, so it is drawn again next present and the draw appears there a few
//     presents late. A target drawn once (a portrait, a minimap, an environment map made at load) would keep the hole for good.
//     Not "two presents at any time": the run must end now (an image kept from the menu and drawn once at a level's load).
//   - it took XBG_ASYNC2_MIN_DRAWS or more draws in the present before: a scene, shadow or UI target, where one draw is one
//     object among many. A post-processing target takes one or two full-screen draws; leaving its only draw out would hand the
//     passes that sample it later in this frame a cleared (black) or one-frame-old image, so those always build at once.
//   - this is not the first draw into it in this present: the first one is the one a clear, a DONT_CARE load (DM_GFX_NOLOAD,
//     first_draw_covers) or a background fill relies on.
inline bool xbg_target_age_steady(const XbgTargetAge &a,uint64_t frame){
    return a.frame==frame && a.run>=XBG_ASYNC2_RUN && a.draws_prev>=XBG_ASYNC2_MIN_DRAWS && a.draws>1;
}

// What the draw is, as xbg_game.cpp's draw() knows it before it asks for the pipeline.
struct XbgAsyncDraw{
    bool capture=false;           // a frame capture is running: every draw is recorded as issued
    bool samples_reported=false;  // samples are counted and the game has read a samples-passed report (occlusion results feed the game)
    bool storage_writes=false;    // a stage writes a storage buffer or binds a storage image (memory later draws / the CPU read)
    bool raster_discard=false;    // rasterizer discard: the draw exists only for its side effects
    bool quad=false;              // at most 6 vertices, one instance, not GPU-indirect: a quad or a single triangle
    bool stencil_write=false;     // stencil test on with a non-zero write mask
    bool depth_write=false;
    bool color_write=false;       // some color target has a non-zero write mask
    bool blends_all=false;        // every written color target blends with the destination (destination factor != ZERO)
    bool pass_open=false;         // the draw continues the open render pass (same pass and framebuffer) and no lazy clear is pending
    bool targets_steady=false;    // xbg_target_age_steady() of every attachment of the pass (color and depth)
};
// May this draw be left out while its pipeline builds in the background? Its pipeline is queued, the draw is skipped this
// present and asked again the next; when the answer is no the pipeline is built at once, as always.
//
// Never (any mode): during a capture; when the game reads occlusion results; storage writes; stencil writes. A stencil
// write is a mask later draws test: without it a masked full-screen effect would cover everything or nothing.
//
// Mode 1 (and inside mode 2): a translucent effect -- every written color target blends with the destination, no depth write.
// Losing it for a few presents loses an effect, whatever the target is.
//
// Mode 2 adds opaque, alpha-tested and depth-only draws (the bulk of the 311 in the issue #4 log) when all of these hold:
//   1. no rasterizer discard (and the "never" list above);
//   1a. more than a quad: an opaque draw of at most 6 vertices is a full-screen copy, a tonemap / fog step, a backdrop or a panel,
//      and losing one of those changes the whole picture for a few presents, not one object (blended quads stay under mode 1's rule);
//   2. the draw writes color or depth (otherwise it has no visible effect to delay and is not worth a rule);
//   3. the draw continues an open render pass: it is not the draw that begins a pass or follows a clear, so the load-op
//      decision (LOAD / lazy CLEAR / DONT_CARE twin) was already made by a draw that was really recorded;
//   4. every attachment of the pass is "steady" (xbg_target_age_steady above): drawn every present, many draws, not its first.
// What a left-out depth-writing draw costs: the object is missing for the few presents its pipeline takes, and what it would
// hide (decals, particles behind it) shows meanwhile. What cannot happen under these rules: a target left holding undefined
// or cleared content that a later pass samples as if it were the finished image; a hole that stays (one-shot targets).
// The window's own textures alternate between presents, so they are never steady: the final blit / tonemap always builds at once.
inline bool xbg_async_may_skip(int mode,const XbgAsyncDraw &d){
    if(mode<=0 || d.capture || d.samples_reported || d.storage_writes || d.stencil_write)return false;
    if(!d.depth_write && d.color_write && d.blends_all)return true;
    if(mode<2)return false;
    return !d.raster_discard && !d.quad && (d.color_write || d.depth_write) && d.pass_open && d.targets_steady;
}

// ---- warm-up lists -----------------------------------------------------------------------------------------------------
// vk-pipeline-list.bin and the seeds: records { magic 0x57504c58, stage count (<= 5), each stage's shader key (u32 length
// <= 256, bytes), word count (<= max_words), words }. A bad record ends the file (it and everything after it is dropped), as
// warm::start always did. rec(keys, words, count) is called for each good record; returns the number of good records.
template<class F> inline size_t xbg_warm_list_parse(const uint8_t *b,size_t n,uint32_t max_words,F &&rec){
    size_t i=0,good=0;auto get=[&](uint32_t &v){if(i+4>n)return false;memcpy(&v,b+i,4);i+=4;return true;};
    while(i<n){
        uint32_t magic,ns,count;if(!get(magic) || magic!=0x57504c58u || !get(ns) || ns>5)break;
        std::vector<std::string> keys;bool ok=true;
        for(uint32_t s=0;s<ns;s++){uint32_t len;if(!get(len) || len>256 || i+len>n){ok=false;break;}keys.emplace_back(reinterpret_cast<const char*>(b+i),len);i+=len;}
        if(!ok || !get(count) || count>max_words || i+size_t(count)*4>n)break;
        rec(std::move(keys),b+i,count);i+=size_t(count)*4;++good;
    }
    return good;
}
// Seed files of a GPU family, in the order they are merged behind the device's own list (the first existing ones win the
// order; duplicates are dropped by key): the APK's copy, a copy beside the list, the shader directory's.
//   <seed_dir>/<family>.bin                         APK assets warm-seed/<family>.bin, unpacked by DmActivity (DM_WARM_SEED_DIR)
//   <base>/warm-seed/<family>.bin                   dropped in by hand (tests, a player's own)
//   <shaders>/vk-pipeline-list.<family>.seed        with the shader bundles
//   <shaders>/vk-pipeline-list.seed                 the old, family-less name
inline std::vector<std::string> xbg_warm_seed_paths(const char *seed_dir,const std::string &base,const std::string &shaders,XbgGpuFamily policy){
    const std::string fam=xbg_gpu_family_name(policy);std::vector<std::string> v;
    if(seed_dir && *seed_dir)v.push_back(std::string(seed_dir)+"/"+fam+".bin");
    v.push_back(base+"/warm-seed/"+fam+".bin");
    v.push_back(shaders+"/vk-pipeline-list."+fam+".seed");
    v.push_back(shaders+"/vk-pipeline-list.seed");
    return v;
}
