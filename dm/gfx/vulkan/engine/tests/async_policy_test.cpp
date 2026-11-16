// [TL2 stutter] Calls the production DM_GFX_ASYNC_PIPELINES rule, the render-target age and the warm-up list reader
// (xbg_async_policy.h) directly; no device. Build (WSL or any host with the Vulkan headers, e.g. the NDK sysroot):
//   g++ -std=c++17 -Wall -idirafter <ndk>/toolchains/llvm/prebuilt/<host>/sysroot/usr/include -o async_policy_test async_policy_test.cpp
#include "../xbg_async_policy.h"
#include <stdio.h>

static int failures;
#define CHECK(expression) do { if(!(expression)){printf("FAIL line %d: %s\n",__LINE__,#expression);++failures;} } while(0)

static void put32(std::vector<uint8_t> &b,uint32_t v){for(int i=0;i<4;i++)b.push_back(uint8_t(v>>(8*i)));}
static void put_rec(std::vector<uint8_t> &b,const std::vector<std::string> &keys,const std::vector<uint32_t> &words){
    put32(b,0x57504c58u);put32(b,uint32_t(keys.size()));for(auto &k:keys){put32(b,uint32_t(k.size()));b.insert(b.end(),k.begin(),k.end());}
    put32(b,uint32_t(words.size()));for(uint32_t w:words)put32(b,w);
}
// An opaque, depth-writing scene draw in the middle of a pass into steady targets: the mode-2 case.
static XbgAsyncDraw opaque(){XbgAsyncDraw d;d.depth_write=d.color_write=d.pass_open=d.targets_steady=true;return d;}
// A translucent effect: the mode-1 case (targets and pass position do not matter).
static XbgAsyncDraw blended(){XbgAsyncDraw d;d.color_write=d.blends_all=true;return d;}

int main(){
    // ---- mode selection: Adreno keeps "off", the others get 2; marker over environment over default; old values keep their meaning
    const char *why=nullptr;
    CHECK(xbg_async_mode(-1,nullptr,XBG_GPU_ADRENO,&why)==0 && !strcmp(why,"default"));
    CHECK(xbg_async_mode(-1,nullptr,XBG_GPU_MALI,&why)==2 && !strcmp(why,"default"));
    CHECK(xbg_async_mode(-1,nullptr,XBG_GPU_POWERVR,nullptr)==2);
    CHECK(xbg_async_mode(-1,nullptr,XBG_GPU_XCLIPSE,nullptr)==2);
    CHECK(xbg_async_mode(-1,nullptr,XBG_GPU_OTHER,nullptr)==2);
    CHECK(xbg_async_mode(-1,"0",XBG_GPU_MALI,&why)==0 && !strcmp(why,"env"));
    CHECK(xbg_async_mode(-1,"1",XBG_GPU_MALI,nullptr)==1);
    CHECK(xbg_async_mode(-1,"2",XBG_GPU_ADRENO,nullptr)==2);
    CHECK(xbg_async_mode(-1,"1",XBG_GPU_ADRENO,nullptr)==1);
    CHECK(xbg_async_mode(-1,"3",XBG_GPU_ADRENO,nullptr)==1);    // before: any non-zero value = on
    CHECK(xbg_async_mode(-1,"",XBG_GPU_MALI,nullptr)==0);       // before: atoi("") = 0 = off
    CHECK(xbg_async_mode('0',"2",XBG_GPU_MALI,&why)==0 && !strcmp(why,"marker"));
    CHECK(xbg_async_mode('1',"0",XBG_GPU_ADRENO,nullptr)==1);
    CHECK(xbg_async_mode('2',"0",XBG_GPU_ADRENO,nullptr)==2);
    CHECK(xbg_async_mode('\n',nullptr,XBG_GPU_ADRENO,nullptr)==1);   // before: a marker whose first byte is not '0' = on

    // ---- render-target age
    {XbgTargetAge a;CHECK(!xbg_target_age_steady(a,0));
     // one draw a present (a post-processing target): never steady, however long it runs
     for(uint64_t f=10;f<20;f++){xbg_target_age_touch(a,f);CHECK(!xbg_target_age_steady(a,f));}
     CHECK(a.run==10 && a.draws==1 && a.draws_prev==1);}
    {XbgTargetAge a;   // a scene target: 20 draws a present
     for(uint64_t f=100;f<103;f++)for(int i=0;i<20;i++){xbg_target_age_touch(a,f);
         const bool want=f==102 && i>=1;   // third present in a row, not its first draw there
         CHECK(xbg_target_age_steady(a,f)==want);}
     CHECK(a.run==3 && a.draws==20 && a.draws_prev==20);
     CHECK(!xbg_target_age_steady(a,103));   // asked in a present it has not been drawn into yet
     // a present without draws breaks the run: two more presents before it is steady again
     for(int i=0;i<20;i++){xbg_target_age_touch(a,104);}CHECK(a.run==1 && a.draws_prev==0 && !xbg_target_age_steady(a,104));
     for(int i=0;i<20;i++){xbg_target_age_touch(a,105);}CHECK(a.run==2 && !xbg_target_age_steady(a,105));
     for(int i=0;i<20;i++){xbg_target_age_touch(a,106);}CHECK(a.run==3 && xbg_target_age_steady(a,106));}
    {XbgTargetAge a;   // drawn once, long ago, with many draws (a cached portrait), then once again: not steady
     for(int i=0;i<50;i++){xbg_target_age_touch(a,5);}for(int i=0;i<50;i++){xbg_target_age_touch(a,500);CHECK(!xbg_target_age_steady(a,500));}}
    {XbgTargetAge a;   // 7 draws a present: below "one among many"
     for(uint64_t f=0;f<5;f++)for(int i=0;i<7;i++){xbg_target_age_touch(a,f);}
     CHECK(!xbg_target_age_steady(a,4));
     for(int i=0;i<8;i++){xbg_target_age_touch(a,5);}CHECK(!xbg_target_age_steady(a,5));   // the present before had 7
     for(int i=0;i<2;i++){xbg_target_age_touch(a,6);}CHECK(xbg_target_age_steady(a,6));}   // the present before had 8
    {XbgTargetAge a;   // a window texture drawn every other present (two-image swap): never steady
     for(uint64_t f=0;f<40;f+=2)for(int i=0;i<30;i++){xbg_target_age_touch(a,f);CHECK(!xbg_target_age_steady(a,f));}}

    // ---- the rule
    CHECK(!xbg_async_may_skip(0,blended()) && !xbg_async_may_skip(0,opaque()));
    // mode 1: exactly the rule before mode 2
    CHECK(xbg_async_may_skip(1,blended()));
    CHECK(!xbg_async_may_skip(1,opaque()));
    {XbgAsyncDraw d=blended();d.depth_write=true;CHECK(!xbg_async_may_skip(1,d));}
    {XbgAsyncDraw d=blended();d.blends_all=false;CHECK(!xbg_async_may_skip(1,d));}
    {XbgAsyncDraw d=blended();d.color_write=false;CHECK(!xbg_async_may_skip(1,d));}
    {XbgAsyncDraw d=blended();d.stencil_write=true;CHECK(!xbg_async_may_skip(1,d));}
    {XbgAsyncDraw d=blended();d.storage_writes=true;CHECK(!xbg_async_may_skip(1,d));}
    {XbgAsyncDraw d=blended();d.capture=true;CHECK(!xbg_async_may_skip(1,d));}
    {XbgAsyncDraw d=blended();d.samples_reported=true;CHECK(!xbg_async_may_skip(1,d));}
    {XbgAsyncDraw d=blended();d.raster_discard=d.quad=true;CHECK(xbg_async_may_skip(1,d));}   // facts mode 1 never looked at
    // mode 2 keeps mode 1's answers for blended draws, wherever they go
    CHECK(xbg_async_may_skip(2,blended()));
    {XbgAsyncDraw d=blended();d.quad=true;CHECK(xbg_async_may_skip(2,d));}
    // mode 2: opaque draws
    CHECK(xbg_async_may_skip(2,opaque()));
    {XbgAsyncDraw d=opaque();d.color_write=false;CHECK(xbg_async_may_skip(2,d));}                 // depth only (shadow caster)
    {XbgAsyncDraw d=opaque();d.depth_write=false;CHECK(xbg_async_may_skip(2,d));}                 // color only
    {XbgAsyncDraw d=opaque();d.color_write=d.depth_write=false;CHECK(!xbg_async_may_skip(2,d));}  // writes nothing
    {XbgAsyncDraw d=opaque();d.pass_open=false;CHECK(!xbg_async_may_skip(2,d));}                  // begins a pass / follows a clear
    {XbgAsyncDraw d=opaque();d.targets_steady=false;CHECK(!xbg_async_may_skip(2,d));}             // one-shot or post-processing target
    {XbgAsyncDraw d=opaque();d.quad=true;CHECK(!xbg_async_may_skip(2,d));}                        // full-screen copy / backdrop
    {XbgAsyncDraw d=opaque();d.raster_discard=true;CHECK(!xbg_async_may_skip(2,d));}
    {XbgAsyncDraw d=opaque();d.stencil_write=true;CHECK(!xbg_async_may_skip(2,d));}               // a mask later draws test
    {XbgAsyncDraw d=opaque();d.storage_writes=true;CHECK(!xbg_async_may_skip(2,d));}
    {XbgAsyncDraw d=opaque();d.capture=true;CHECK(!xbg_async_may_skip(2,d));}
    {XbgAsyncDraw d=opaque();d.samples_reported=true;CHECK(!xbg_async_may_skip(2,d));}

    // ---- warm-up list reader: the device's list, a seed, a truncated tail
    {std::vector<uint8_t> b;put_rec(b,{"vs_a","fs_a"},{1,2,3});put_rec(b,{"vs_b"},{4});put_rec(b,{},{});
     std::vector<std::vector<std::string>> keys;std::vector<std::vector<uint32_t>> words;
     auto rec=[&](std::vector<std::string> &&k,const uint8_t *w,uint32_t n){keys.push_back(std::move(k));std::vector<uint32_t> v(n);if(n)memcpy(v.data(),w,size_t(n)*4);words.push_back(v);};
     CHECK(xbg_warm_list_parse(b.data(),b.size(),4096,rec)==3);
     CHECK(keys.size()==3 && keys[0].size()==2 && keys[0][1]=="fs_a" && keys[1][0]=="vs_b" && keys[2].empty());
     CHECK(words[0]==std::vector<uint32_t>({1,2,3}) && words[1]==std::vector<uint32_t>({4}) && words[2].empty());
     // every truncation gives only the whole records before the cut
     for(size_t cut=0;cut<b.size();cut++){size_t n=0;xbg_warm_list_parse(b.data(),cut,4096,[&](std::vector<std::string> &&,const uint8_t *,uint32_t){++n;});
         std::vector<uint8_t> first;put_rec(first,{"vs_a","fs_a"},{1,2,3});std::vector<uint8_t> two=first;put_rec(two,{"vs_b"},{4});
         CHECK(n==(cut>=two.size()?2u:cut>=first.size()?1u:0u));}
     // a bad magic ends the file there
     std::vector<uint8_t> bad;put_rec(bad,{"vs_a"},{7});put32(bad,0xdeadbeefu);put_rec(bad,{"vs_b"},{8});
     size_t n=0;CHECK(xbg_warm_list_parse(bad.data(),bad.size(),4096,[&](std::vector<std::string> &&,const uint8_t *,uint32_t){++n;})==1 && n==1);
     // limits: 6 stages, a 257-byte key, more words than the key holds
     std::vector<uint8_t> six;put_rec(six,{"a","b","c","d","e","f"},{1});CHECK(xbg_warm_list_parse(six.data(),six.size(),4096,[](std::vector<std::string> &&,const uint8_t *,uint32_t){})==0);
     std::vector<uint8_t> longkey;put_rec(longkey,{std::string(257,'k')},{1});CHECK(xbg_warm_list_parse(longkey.data(),longkey.size(),4096,[](std::vector<std::string> &&,const uint8_t *,uint32_t){})==0);
     std::vector<uint8_t> many;put_rec(many,{"a"},std::vector<uint32_t>(9,1));CHECK(xbg_warm_list_parse(many.data(),many.size(),8,[](std::vector<std::string> &&,const uint8_t *,uint32_t){})==0);
     CHECK(xbg_warm_list_parse(many.data(),many.size(),9,[](std::vector<std::string> &&,const uint8_t *,uint32_t){})==1);
     CHECK(xbg_warm_list_parse(nullptr,0,4096,[](std::vector<std::string> &&,const uint8_t *,uint32_t){})==0);}

    // ---- seed paths: the APK's copy first, per family; the old family-less name last
    {auto v=xbg_warm_seed_paths("/data/files/warm-seed","/sd/dm","/sd/dm/shaders",XBG_GPU_MALI);
     CHECK(v.size()==4 && v[0]=="/data/files/warm-seed/mali.bin" && v[1]=="/sd/dm/warm-seed/mali.bin" && v[2]=="/sd/dm/shaders/vk-pipeline-list.mali.seed" && v[3]=="/sd/dm/shaders/vk-pipeline-list.seed");
     auto w=xbg_warm_seed_paths(nullptr,"/sd/dm","/sd/dm/shaders",XBG_GPU_ADRENO);
     CHECK(w.size()==3 && w[0]=="/sd/dm/warm-seed/adreno.bin");
     CHECK(xbg_warm_seed_paths("","b","s",XBG_GPU_POWERVR)[0]=="b/warm-seed/powervr.bin");}

    printf(failures?"async_policy_test: %d FAILED\n":"async_policy_test: ok\n",failures);
    return failures?1:0;
}
