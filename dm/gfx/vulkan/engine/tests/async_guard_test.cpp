// [async-pipe-guard] Calls the production rules and gate (xbg_async_guard.h) directly; no device. Build (any host C++17 compiler):
//   g++ -std=c++17 -Wall -Wextra -O1 -fsanitize=address,undefined -pthread -o async_guard_test async_guard_test.cpp
// With the old rule (xbg_async_policy.h, needs the Vulkan headers: -idirafter <ndk sysroot>/usr/include) it also checks the TL2 wiring's
// decision: the old rule's answer, then the guard's veto.
#include "../xbg_async_guard.h"
#include <stdio.h>
#include <string.h>
#include <thread>
#include <vector>
#ifdef WITH_POLICY
#include "../xbg_async_policy.h"
#endif

static int failures;
#define CHECK(expression) do { if(!(expression)){printf("FAIL line %d: %s\n",__LINE__,#expression);++failures;} } while(0)

static xbg_ag_draw D(uint32_t w,uint32_t h,int writes=0,int discard=0){xbg_ag_draw d{w,h,writes,discard};return d;}

int main(){
    // ---- DM_GFX_ASYNC_GUARD: new behaviour on when unset, 0 = the old rule
    CHECK(xbg_ag_mode(nullptr)==3 && xbg_ag_mode("")==3);
    CHECK(xbg_ag_mode("0")==0 && xbg_ag_mode("1")==1 && xbg_ag_mode("2")==2 && xbg_ag_mode("3")==3 && xbg_ag_mode("7")==3 && xbg_ag_mode("0x2")==2);

    // ---- the rule
    {xbg_ag_draw d=D(1,1);CHECK(xbg_ag_why(3,&d)==XBG_AG_SMALL);}            // exposure / luminance 1x1
    {xbg_ag_draw d=D(64,64);CHECK(xbg_ag_why(3,&d)==XBG_AG_SMALL);}          // the bound itself
    {xbg_ag_draw d=D(32,16);CHECK(xbg_ag_why(3,&d)==XBG_AG_SMALL);}          // a LUT slice
    {xbg_ag_draw d=D(65,64);CHECK(xbg_ag_why(3,&d)==0);}
    {xbg_ag_draw d=D(64,65);CHECK(xbg_ag_why(3,&d)==0);}
    {xbg_ag_draw d=D(1024,1);CHECK(xbg_ag_why(3,&d)==0);}                    // a strip is not small
    {xbg_ag_draw d=D(0,0);CHECK(xbg_ag_why(3,&d)==0);}                       // size not known: no small rule
    {xbg_ag_draw d=D(0,32);CHECK(xbg_ag_why(3,&d)==0);}
    {xbg_ag_draw d=D(2400,1080,1);CHECK(xbg_ag_why(3,&d)==XBG_AG_WRITES);}  // storage writes
    {xbg_ag_draw d=D(2400,1080,0,1);CHECK(xbg_ag_why(3,&d)==XBG_AG_WRITES);}// rasterizer discard
    {xbg_ag_draw d=D(16,16,1,1);CHECK(xbg_ag_why(3,&d)==XBG_AG_SMALL);}     // small first
    {xbg_ag_draw d=D(16,16,1);CHECK(xbg_ag_why(2,&d)==XBG_AG_WRITES);}      // bit 1 off: the writes rule alone
    {xbg_ag_draw d=D(16,16);CHECK(xbg_ag_why(2,&d)==0);}
    {xbg_ag_draw d=D(16,16,1);CHECK(xbg_ag_why(1,&d)==XBG_AG_SMALL);}
    {xbg_ag_draw d=D(2400,1080,1);CHECK(xbg_ag_why(1,&d)==0);}               // bit 2 off: writes not guarded
    {xbg_ag_draw d=D(1,1,1,1);CHECK(xbg_ag_why(0,&d)==0);}                   // 0: nothing guarded
    {xbg_ag_draw d=D(2400,1080);CHECK(xbg_ag_why(3,&d)==0);}                 // the bulk: the port's rule stands
    CHECK(!strcmp(xbg_ag_name(XBG_AG_SMALL),"small target") && !strcmp(xbg_ag_name(XBG_AG_WRITES),"writes memory") && !strcmp(xbg_ag_name(0),"none"));

#ifdef WITH_POLICY
    // ---- TL2 wiring: mode 1 used to leave a blended 1x1 adaptation step out; with the guard it is built in the frame
    {XbgAsyncDraw a;a.color_write=a.blends_all=true;CHECK(xbg_async_may_skip(1,a));xbg_ag_draw g=D(1,1);CHECK(xbg_ag_why(3,&g)==XBG_AG_SMALL);
     CHECK(!(xbg_async_may_skip(1,a) && !xbg_ag_why(3,&g)));                 // skip = old && !guard
     CHECK(xbg_async_may_skip(1,a) && !xbg_ag_why(0,&g));}                   // DM_GFX_ASYNC_GUARD=0: as before
    // a blended draw with rasterizer discard: mode 1 never looked at the discard
    {XbgAsyncDraw a;a.color_write=a.blends_all=a.raster_discard=true;CHECK(xbg_async_may_skip(1,a));xbg_ag_draw g=D(1920,1080,0,1);CHECK(xbg_ag_why(3,&g)==XBG_AG_WRITES);}
    // mode 2: a 64x64 target with many draws a present passes the age rule; the guard keeps it
    {XbgAsyncDraw a;a.depth_write=a.color_write=a.pass_open=a.targets_steady=true;CHECK(xbg_async_may_skip(2,a));xbg_ag_draw g=D(64,64);CHECK(xbg_ag_why(3,&g)==XBG_AG_SMALL);}
#endif

    // ---- DM_GFX_PIPE_BG_SLOTS: 1 on Turnip, 4 elsewhere; 0 = no gate; clamp
    CHECK(xbg_pg_slots(nullptr,1)==1 && xbg_pg_slots(nullptr,0)==4 && xbg_pg_slots("",1)==1);
    CHECK(xbg_pg_slots("0",1)==0 && xbg_pg_slots("0",0)==0 && xbg_pg_slots("-3",0)==0);
    CHECK(xbg_pg_slots("2",1)==2 && xbg_pg_slots("3",0)==3 && xbg_pg_slots("99",0)==XBG_PG_CAP_MAX);

    // ---- admission
    {xbg_pg_state s{};CHECK(xbg_pg_may_enter(&s,1,1) && xbg_pg_may_enter(&s,1,0));
     s.busy=1;CHECK(!xbg_pg_may_enter(&s,1,1) && !xbg_pg_may_enter(&s,1,0) && xbg_pg_may_enter(&s,2,0));
     s.busy=0;s.frame_wait=1;CHECK(!xbg_pg_may_enter(&s,4,0) && xbg_pg_may_enter(&s,4,1));   // speculative waits behind a waiting frame build
     s.frame_wait=0;s.spec_wait=3;CHECK(xbg_pg_may_enter(&s,1,1) && xbg_pg_may_enter(&s,1,0));
     s.busy=9;s.frame_wait=5;CHECK(xbg_pg_may_enter(&s,0,0) && xbg_pg_may_enter(&s,0,1));}   // no gate

    // ---- the gate with threads: never more than slots at once; frame builds queued while a slot is busy go before the speculative ones
    for(int slots=1;slots<=3;slots++){
        XbgPipeGate g;g.slots=slots;std::atomic<int> in{0},worst{0};std::mutex om;std::vector<int> order;
        auto job=[&](bool frame,int id,int ms){XbgPipeGate::Hold h(g,frame);const int n=++in;if(n>worst)worst=n;
            {std::lock_guard<std::mutex> l(om);order.push_back(id);}std::this_thread::sleep_for(std::chrono::milliseconds(ms));--in;};
        std::vector<std::thread> t;
        // fill the slots with long speculative builds, then queue 4 speculative and 3 frame builds while they run
        for(int i=0;i<slots;i++)t.emplace_back(job,false,100+i,60);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        for(int i=0;i<4;i++)t.emplace_back(job,false,200+i,5);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        for(int i=0;i<3;i++)t.emplace_back(job,true,300+i,5);
        for(auto &x:t)x.join();
        CHECK(worst.load()<=slots);CHECK(g.s.busy==0 && g.s.frame_wait==0 && g.s.spec_wait==0);
        CHECK(g.builds[1].load()==3 && g.builds[0].load()==uint64_t(slots)+4);
        // after the first `slots` builds, the three frame builds come before every queued speculative one
        int last_frame=-1,first_spec=1<<30;for(int i=0;i<int(order.size());i++){if(order[i]>=300)last_frame=i;else if(order[i]>=200 && first_spec==(1<<30))first_spec=i;}
        CHECK(last_frame<first_spec);
        if(failures)printf("slots=%d worst=%d order:",slots,worst.load());
        if(failures){for(int id:order)printf(" %d",id);printf("\n");}
    }
    // gate off: enter / leave do nothing
    {XbgPipeGate g;g.slots=0;{XbgPipeGate::Hold a(g,false),b(g,true),c(g,false);CHECK(g.s.busy==0);}CHECK(g.builds[0]==0 && g.builds[1]==0);}

    printf(failures?"async_guard_test: %d FAILED\n":"async_guard_test: all passed\n",failures);
    return failures?1:0;
}
