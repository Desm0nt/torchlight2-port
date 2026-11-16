// [TL2] ASTC texture cache for GPUs without BC (docs/tl2/ASTC_CACHE.md; KB TEX-15 = the D3 pack, TEX-DOS2-01 = the lazy encoder).
// Included by xbg_game.cpp inside the texture-creation path (after partial_upload, before get_image); kept in its own file so that
// the engine edits of other work on xbg_game.cpp merge without touching it.
//
// Without BC (Mali, PowerVR, stock Xclipse, DM_GFX_SIM_GPU=mali) every BC texture was decoded at upload (BcUpload): 8x the memory for
// BC1, 4x for BC2 / BC3 (player log, Mali-G615: decoded 284 MB where BC would be 64.6 MB) and the decode work on every upload. TL2's
// textures are XTX files in PAKS/DATA*.PAK (tools/tl2_mod.py); the game's loader (main+0x1995f40) builds the NVN texture from the XTX
// info block, refuses the texture unless nvnTextureBuilderGetStorageSize equals the XTX data size and memcpy's the XTX data block into
// its pool before TextureInitialize. So the guest storage the engine sees at image creation is byte for byte the XTX data block, and
// a pack made from the RomFS on the PC (tools/tl2_astc_cache.py) or on the phone (dm/android/astc_convert.cpp) uses the same key:
// CRC-32 + Adler-32 of the storage, its size, NVN format, width, height, levels (xbg_astc_cache.h, D3's 'D3AC' v1).
//
// Sources, looked up together (one sorted index; XbgAcEntry::pad[0] = the source):
//   0 setup pack   <data>/astc/tl2-astc.pack ('D3AC' v1, read only; tools/tl2_astc_cache.py by setup, or the finished phone build)
//   1 phone part   <data>/astc/tl2-astc.pack.part + .journal-phone (an unfinished on-phone build; only when there is no finished pack)
//   2 lazy cache   $DM_TEX_ASTC_CACHE_DIR/tl2-astc-lazy.pack.part + .journal-lazy (the app sets Android/data/<package>/files/astc):
//                  textures that miss (mods, a partial pack, no pack at all) are encoded in the background on first sight
//                  (xbg_astc_lazy.cpp, D3's 42 dB rule) and appended; they join the index at the next frame, live decoded images of
//                  them are made anew from the cache at their next use, later runs create them from the cache directly.
//   <data> = $DM_ASTC_DATA_DIR (the app sets Download/Torchlight2Mobile), else two folders above dm/shaders.
// A hit creates the image in the entry's ASTC format (UNORM / sRGB by the NVN format) and uploads the blocks read with pread (the
// prologue when possible); all-zero blocks are made void-extent black on the way (KB TEX-04). A miss or a texture the game changes
// later takes the BC decoder as before.
//   DM_GFX_ASTC_CACHE: auto (default) = on where the device does not sample BC (textureCompressionBC=0, which DM_GFX_SIM_GPU=mali /
//     nobc simulate) and samples ASTC LDR; 1 = also next to native BC (tests on Adreno: hits are used, the encoder only sees decoded
//     textures); 0 = off (the old BC decoder for everything). Adreno with BC: auto = off, nothing changes.
//   DM_ASTC_CACHE_PATH: another setup pack file. DM_TEX_ASTC_PRODUCE (1): 0 = no lazy encoder. DM_TEX_ASTC_CACHE_MB (2048): the lazy
//     pack stops growing there. DM_TEX_ASTC_THREADS (1 with a pack, 2 without): encoder threads (nice 10). DM_TEX_ASTC_QUEUE_MB (64).
// Log: "NVN astc cache: ..." once (sources and why not), the first misses with their reason ("miss (no texture of this shape)",
// "miss (other bytes)"), then lookups at 64, 128, 256 ... with hits / misses / MB saved; "NVN astc lazy: ..." from the encoder.
#ifdef DM_ASTC_CACHE
#include <fcntl.h>
namespace astccache {
typedef std::array<uint32_t,7> Key;   // crc, adler, bytes, format, width, height, levels
enum{SRC_PACK=0,SRC_PHONE=1,SRC_LAZY=2};
int state=-1;int fds[3]={-1,-1,-1};std::string paths[3],journal_path;std::vector<XbgAcEntry> idx;std::unordered_set<uint64_t> shapes,refused;std::set<Key> known;
bool block_ok[3]={},produce=false;uint64_t src_entries[3]={},src_bytes[3]={};
uint64_t hits=0,hits_src[3]={},miss_shape=0,miss_hash=0,miss_block=0,lost=0,upgraded=0,uploads=0,upload_bytes=0,zero_fixed=0,read_fails=0,hash_ns=0,offered=0,offer_known=0,offer_dropped=0,merged=0,swaps=0;
std::unordered_map<uint64_t,std::vector<uint64_t>> waiting;   // crc | adler << 32 of an offered texture -> serials of its decoded images
uint64_t k64(uint32_t crc,uint32_t adler){return uint64_t(crc)|uint64_t(adler)<<32;}
static const char *const LAZY_SIG="D3AC-phone 1 lazy-tl2 1";
static const char *const SRC_NAME[3]={"pack","phone","lazy"};
uint64_t shape_of(const XbgAcEntry &e){return xbg_ac_shape(e.bytes,e.format,e.levels,e.width,e.height);}
std::string data_root(){
    const char *e=getenv("DM_ASTC_DATA_DIR");if(e && *e)return e;
    std::string d=cfg.shader_directory?cfg.shader_directory:"";for(int i=0;i<2;i++){size_t c=d.find_last_of('/');d=c==std::string::npos?std::string("."):d.substr(0,c);}return d;}
void add(std::vector<XbgAcEntry> &e,int src){for(auto &x:e){x.pad[0]=uint8_t(src);src_bytes[src]+=x.data_bytes;}src_entries[src]+=e.size();idx.insert(idx.end(),e.begin(),e.end());}
// The finished pack (setup or a finished phone build). false + *why when it is there but unusable; false + no why when absent.
bool open_pack(const std::string &path,const char **why){
    const int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC);if(fd<0)return false;
    XbgAcHeader h{};struct stat st{};std::vector<XbgAcEntry> e;
    if(fstat(fd,&st) || pread(fd,&h,sizeof h,0)!=ssize_t(sizeof h) || xbg_ac_header_check(h,uint64_t(st.st_size),why)){if(!*why)*why="unreadable";close(fd);return false;}
    e.resize(h.entries);const size_t n=sizeof(XbgAcEntry)*h.entries;
    if(pread(fd,e.data(),n,off_t(h.index_off))!=ssize_t(n)){*why="index unreadable";close(fd);return false;}
    for(auto &x:e)if(!xbg_ac_entry_ok(x,h)){*why="corrupt index entry";close(fd);return false;}
    fds[SRC_PACK]=fd;paths[SRC_PACK]=path;add(e,SRC_PACK);return true;
}
// An unfinished on-phone build (D3 round 4): the textures committed in .journal-phone, ranges inside the part file as it is now. The
// builder (in this process, the game held) only appends past it; a fresh build makes a new file, so this descriptor keeps its bytes.
bool open_phone(const std::string &dir){
    const std::string part=dir+"/tl2-astc.pack.part",jp=dir+"/.journal-phone";
    const int pf=open(part.c_str(),O_RDONLY|O_CLOEXEC);if(pf<0)return false;
    struct stat st{};std::string text;if(fstat(pf,&st)){close(pf);return false;}
    if(FILE *j=fopen(jp.c_str(),"rb")){char b[65536];size_t r;while((r=fread(b,1,sizeof b,j))>0)text.append(b,r);fclose(j);}
    uint64_t total=0;std::vector<XbgAcEntry> e;
    if(xbg_ac_journal_parse(text.data(),text.size(),uint64_t(st.st_size),e,&total)!=0 || e.empty()){close(pf);
        xbg_logf("NVN astc cache: %s has no committed textures yet (journal %zu bytes)",part.c_str(),text.size());return false;}
    fds[SRC_PHONE]=pf;paths[SRC_PHONE]=part;add(e,SRC_PHONE);
    xbg_logf("NVN astc cache: partial phone build %s: %zu of %llu textures committed (the rest is decoded / encoded lazily; Prepare textures goes on)",part.c_str(),e.size(),(unsigned long long)total);
    return true;
}
// The lazy cache: its part file is shared with the encoder (pwrite) and read here (pread). Returns its committed end (0: not opened).
uint64_t open_lazy(const std::string &dir,uint64_t *stayed){
    mkdir(dir.c_str(),0775);const std::string part=dir+"/tl2-astc-lazy.pack.part";journal_path=dir+"/.journal-lazy";
    const int fd=open(part.c_str(),O_RDWR|O_CREAT|O_CLOEXEC,0664);struct stat st{};
    if(fd<0 || fstat(fd,&st)){xbg_logf("NVN astc cache: %s cannot be opened (errno %d): no lazy cache",part.c_str(),errno);if(fd>=0)close(fd);return 0;}
    uint64_t end=64;std::string text;
    if(FILE *j=fopen(journal_path.c_str(),"rb")){char b[65536];size_t r;while((r=fread(b,1,sizeof b,j))>0)text.append(b,r);fclose(j);}
    const size_t sl=strlen(LAZY_SIG);std::vector<XbgAcEntry> e;uint64_t total=0;
    if(text.size()>sl && !memcmp(text.data(),LAZY_SIG,sl) && text[sl]=='\n' && uint64_t(st.st_size)>=64 && xbg_ac_journal_parse(text.data(),text.size(),uint64_t(st.st_size),e,&total)==0){
        for(const auto &x:e)end=std::max<uint64_t>(end,x.data_off+x.data_bytes);
        for(size_t p=text.find('\n')+1;p<text.size();){const size_t nl=text.find('\n',p);if(nl==std::string::npos)break;   // block 0 lines: tried, stays BC
            unsigned crc,adler,bytes,fmt,w,h,lv,b,db,dlen,pr,stp;unsigned long long off;
            if(sscanf(text.c_str()+p,"%x %x %u %x %u %u %u %u %u %llu %u %u %u",&crc,&adler,&bytes,&fmt,&w,&h,&lv,&b,&db,&off,&dlen,&pr,&stp)==13 && !b){known.insert(Key{crc,adler,bytes,fmt,w,h,lv});++*stayed;}
            p=nl+1;}
        if(uint64_t(st.st_size)>end && ftruncate(fd,off_t(end))){}   // an interrupted write past the last committed texture
    }else{   // a new cache (or another format): start over
        if(ftruncate(fd,0)){}uint8_t z[64]={};if(pwrite(fd,z,64,0)!=64){}fsync(fd);
        const std::string tmp=journal_path+".tmp";if(FILE *j=fopen(tmp.c_str(),"wb")){fprintf(j,"%s\n",LAZY_SIG);fflush(j);fsync(fileno(j));fclose(j);rename(tmp.c_str(),journal_path.c_str());}
        if(!text.empty())xbg_logf("NVN astc cache: %s was not a lazy-tl2 journal (%zu bytes): started a new lazy cache",journal_path.c_str(),text.size());
    }
    fds[SRC_LAZY]=fd;paths[SRC_LAZY]=part;add(e,SRC_LAZY);return end;
}
void init(){
    state=0;const char *m=getenv("DM_GFX_ASTC_CACHE");const int mode=m && *m && strcmp(m,"auto")?atoi(m):-1;
    const char *why=nullptr;
    if(mode==0)why="DM_GFX_ASTC_CACHE=0";
    else if(!cfg.enabled_core.textureCompressionASTC_LDR)why="the device has no ASTC LDR";
    // D3 issue #15: "BC here" from the device's enabled features, not from the native_bc marker (Android sets it to 1 everywhere)
    else if(native_bc && cfg.enabled_core.textureCompressionBC && mode<0)why="the device samples BC (DM_GFX_ASTC_CACHE=1 uses the cache anyway)";
    const std::string root=data_root(),adir=root+"/astc";
    const char *pe=getenv("DM_ASTC_CACHE_PATH");const std::string pack=pe && *pe?pe:adir+"/tl2-astc.pack";
    if(why){xbg_logf("NVN astc cache: not used (%s); BC textures are decoded",why);return;}
    const uint8_t sizes[3]={4,5,6};const VkFormatFeatureFlags need_f=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    for(int k=0;k<3;k++){VkFormatProperties a{},b{};vkGetPhysicalDeviceFormatProperties(cfg.physical,xbg_ac_format(sizes[k],sizes[k],0x44),&a);vkGetPhysicalDeviceFormatProperties(cfg.physical,xbg_ac_format(sizes[k],sizes[k],0x48),&b);
        block_ok[k]=(a.optimalTilingFeatures&need_f)==need_f && (b.optimalTilingFeatures&need_f)==need_f;}
    const char *pack_why=nullptr;
    if(!open_pack(pack,&pack_why)){
        if(pack_why)xbg_logf("NVN astc cache: %s not used: %s",pack.c_str(),pack_why);
        open_phone(adir);
    }
    uint64_t lazy_end=0,stayed=0;const char *de=getenv("DM_TEX_ASTC_CACHE_DIR"),*pr=getenv("DM_TEX_ASTC_PRODUCE");
    const bool want_lazy=!(pr && *pr=='0') && de && *de;
    if(want_lazy)lazy_end=open_lazy(de,&stayed);
    if(idx.empty() && !lazy_end){
        xbg_logf("NVN astc cache: not used (no astc/tl2-astc.pack, no phone build, %s); BC textures are decoded",
            want_lazy?"the lazy cache could not be opened":pr && *pr=='0'?"DM_TEX_ASTC_PRODUCE=0":"no DM_TEX_ASTC_CACHE_DIR for the lazy cache");return;}
    std::sort(idx.begin(),idx.end(),xbg_ac_entry_before);   // a key in several sources: the first (pack before phone before lazy) wins
    idx.erase(std::unique(idx.begin(),idx.end(),[](const XbgAcEntry &a,const XbgAcEntry &b){return !xbg_ac_entry_before(a,b) && !xbg_ac_entry_before(b,a);}),idx.end());
    for(const auto &x:idx)shapes.insert(shape_of(x));
    const bool have_pack=src_entries[SRC_PACK] || src_entries[SRC_PHONE];
    const char *ce=getenv("DM_TEX_ASTC_CACHE_MB"),*te=getenv("DM_TEX_ASTC_THREADS"),*qe=getenv("DM_TEX_ASTC_QUEUE_MB");
    const uint64_t cap=uint64_t(std::max(64L,ce?atol(ce):2048L))<<20,queue=uint64_t(std::max(8L,qe?atol(qe):64L))<<20;const int threads=std::clamp(te?atoi(te):have_pack?1:2,1,8);
    if(lazy_end)produce=lazy_end<cap && astclazy::start(fds[SRC_LAZY],lazy_end,journal_path,threads,cap,queue);
    state=1;
    xbg_logf("NVN astc cache: using %zu textures: pack %llu (%.1f MB, %s), phone build %llu (%.1f MB), lazy %llu (%.1f MB, %llu stay BC); encoder %s (threads %d, cap %.0f MB, queue %.0f MB); blocks 4x4=%d 5x5=%d 6x6=%d; device BC=%d native_bc=%d; DM_GFX_ASTC_CACHE=%s",
        idx.size(),(unsigned long long)src_entries[0],src_bytes[0]/1048576.0,fds[0]>=0?paths[0].c_str():pack.c_str(),(unsigned long long)src_entries[1],src_bytes[1]/1048576.0,
        (unsigned long long)src_entries[2],src_bytes[2]/1048576.0,(unsigned long long)stayed,produce?"on":!lazy_end?"off (no lazy cache)":lazy_end>=cap?"off (cap reached)":"off",
        threads,cap/1048576.0,queue/1048576.0,int(block_ok[0]),int(block_ok[1]),int(block_ok[2]),int(cfg.enabled_core.textureCompressionBC),int(native_bc),m?m:"auto");
}
// Entries the encoder committed since the last call join the sorted index (and the shape filter).
void merge(){
    if(!produce)return;std::vector<XbgAcEntry> add_e;if(!astclazy::take(add_e))return;
    for(auto &e:add_e){e.pad[0]=SRC_LAZY;src_bytes[SRC_LAZY]+=e.data_bytes;}src_entries[SRC_LAZY]+=add_e.size();
    std::sort(add_e.begin(),add_e.end(),xbg_ac_entry_before);const size_t mid=idx.size();idx.insert(idx.end(),add_e.begin(),add_e.end());
    std::inplace_merge(idx.begin(),idx.begin()+mid,idx.end(),xbg_ac_entry_before);for(const auto &e:add_e)shapes.insert(shape_of(e));merged+=add_e.size();
    // The decoded images the entries were made from (still alive) are marked for upload: their next use re-uploads, would_hit() finds
    // the entry and the image is made anew from the cache (the decoded memory goes with the old image).
    for(const auto &e:add_e){auto w=waiting.find(k64(e.crc,e.adler));if(w==waiting.end())continue;
        for(uint64_t serial:w->second)if(Image *im=find_image(serial))if(im->decoded && im->uploaded && !im->gpu_written && !refused.count(serial)){im->uploaded=false;im->checked_submission=UINT64_MAX;++swaps;}
        waiting.erase(w);}
    if(waiting.size()>65536)waiting.clear();   // bound: entries that never commit (stay BC, dropped at the cap)
}
void frame(){if(state==1)merge();}
void note(){
    const uint64_t n=hits+miss_shape+miss_hash+miss_block;if(n<64 || (n&(n-1)))return;
    const XbgAlStats a=astclazy::stats();
    xbg_logf("NVN astc cache: lookups %llu: hits %llu (pack %llu, phone %llu, lazy %llu), misses %llu (not in the cache %llu, other bytes %llu, block size not on this device %llu), changed later %llu, filled later %llu, re-made from the lazy cache %llu; uploads %llu (%.1f MB read, zero blocks fixed %llu, read errors %llu), hashing %.1f ms; live %.1f MB of ASTC where decoded would be %.1f MB (saved %.1f MB); index %zu (+%llu this run); encoder: offered %llu (known %llu, dropped %llu), cached %llu, stay BC %llu, failed %llu, lazy pack %.1f MB, queue %.1f MB",
        (unsigned long long)n,(unsigned long long)hits,(unsigned long long)hits_src[0],(unsigned long long)hits_src[1],(unsigned long long)hits_src[2],(unsigned long long)(miss_shape+miss_hash+miss_block),(unsigned long long)miss_shape,(unsigned long long)miss_hash,(unsigned long long)miss_block,
        (unsigned long long)lost,(unsigned long long)upgraded,(unsigned long long)swaps,(unsigned long long)uploads,upload_bytes/1048576.0,(unsigned long long)zero_fixed,(unsigned long long)read_fails,hash_ns/1e6,
        astc_cache_bytes_live/1048576.0,astc_cache_rgba_live/1048576.0,(astc_cache_rgba_live-astc_cache_bytes_live)/1048576.0,idx.size(),(unsigned long long)merged,
        (unsigned long long)offered,(unsigned long long)offer_known,(unsigned long long)offer_dropped,(unsigned long long)a.encoded,(unsigned long long)a.stayed_bc,(unsigned long long)a.failed,a.pack_end/1048576.0,a.queue_bytes/1048576.0);
}
// Every 600 frames while something was looked up (the NVN PERF block): the same numbers, so a short session still shows them.
void perf(){
    static uint64_t last=0;if(state!=1)return;const uint64_t n=hits+miss_shape+miss_hash+miss_block;if(n==last)return;last=n;
    xbg_logf("NVN PERF astccache hits=%llu misses=%llu (shape %llu bytes %llu block %llu) live=%.1fMB decoded_would_be=%.1fMB saved=%.1fMB lazy_cached=%llu",(unsigned long long)hits,(unsigned long long)(miss_shape+miss_hash+miss_block),
        (unsigned long long)miss_shape,(unsigned long long)miss_hash,(unsigned long long)miss_block,astc_cache_bytes_live/1048576.0,astc_cache_rgba_live/1048576.0,(astc_cache_rgba_live-astc_cache_bytes_live)/1048576.0,(unsigned long long)astclazy::stats().encoded);
}
void key(const tex_t *t,uint32_t *crc,uint32_t *adler){
    const uint64_t t0=timing_clock();const uint8_t *m=reinterpret_cast<const uint8_t*>(t->mem);uLong c=crc32(0L,Z_NULL,0),a=adler32(0L,Z_NULL,0);
    for(uint64_t o=0;o<t->L.size;){const uInt n=uInt(std::min<uint64_t>(t->L.size-o,1u<<30));c=crc32(c,m+o,n);a=adler32(a,m+o,n);o+=n;}
    hash_ns+=timing_clock()-t0;*crc=uint32_t(c);*adler=uint32_t(a);}
// The textures the cache can hold: 2D single-layer single-sample BC1-BC3 with storage (TL2: BC1 0x42, BC2 0x43, BC3 0x44; 833 MB).
bool cacheable(const tex_t *t){return t->d.format>=0x41 && t->d.format<=0x48 && t->d.target==1 && t->mem && t->L.layers==1 && t->L.size && t->L.size<=UINT32_MAX && !(t->d.flags&0x10)
    && t->d.width<=65535 && t->d.height<=65535;}
uint16_t levels_of(const tex_t *t){return uint16_t(t->d.levels?t->d.levels:1);}
int64_t find(const tex_t *t,uint32_t c,uint32_t a){return xbg_ac_find(idx.data(),uint32_t(idx.size()),c,a,uint32_t(t->L.size),uint16_t(t->d.format),levels_of(t),uint16_t(t->d.width),uint16_t(t->d.height));}
// The entry for this texture (copied into *out), or false (then the usual BC path).
bool lookup(const tex_t *t,VkSampleCountFlagBits samples,XbgAcEntry *out){
    if(state<0)init();
    if(state!=1 || samples!=VK_SAMPLE_COUNT_1_BIT || !cacheable(t) || refused.count(t->serial))return false;
    merge();const uint16_t lv=levels_of(t);
    if(!shapes.count(xbg_ac_shape(uint32_t(t->L.size),uint16_t(t->d.format),lv,uint16_t(t->d.width),uint16_t(t->d.height)))){++miss_shape;
        {static unsigned n=0;if(n++<12)xbg_logf("NVN astc cache: miss (no texture of this shape) serial=%llu fmt=%#x %ux%u levels=%u storage=%llu",(unsigned long long)t->serial,t->d.format,t->d.width,t->d.height,unsigned(lv),(unsigned long long)t->L.size);}
        note();return false;}
    uint32_t c,a;key(t,&c,&a);const int64_t i=find(t,c,a);
    if(i<0){++miss_hash;{static unsigned n=0;if(n++<12)xbg_logf("NVN astc cache: miss (other bytes) serial=%llu fmt=%#x %ux%u levels=%u storage=%llu crc=%08x adler=%08x",(unsigned long long)t->serial,t->d.format,t->d.width,t->d.height,unsigned(lv),(unsigned long long)t->L.size,c,a);}
        note();return false;}
    const XbgAcEntry &e=idx[size_t(i)];if(!block_ok[e.bw-4]){++miss_block;note();return false;}
    *out=e;++hits;++hits_src[e.pad[0]<3?e.pad[0]:0];note();return true;
}
// lookup() without counting a miss: a decoded image whose storage the game completed later
bool would_hit(const tex_t *t){
    if(state!=1 || !cacheable(t))return false;
    merge();if(!shapes.count(xbg_ac_shape(uint32_t(t->L.size),uint16_t(t->d.format),levels_of(t),uint16_t(t->d.width),uint16_t(t->d.height))))return false;
    uint32_t c,a;key(t,&c,&a);const int64_t i=find(t,c,a);return i>=0 && block_ok[idx[size_t(i)].bw-4];}
// The guest storage still holds the bytes of entry e.
bool same(const XbgAcEntry &e,const tex_t *t){uint32_t c,a;key(t,&c,&a);return c==e.crc && a==e.adler && t->L.size==e.bytes;}
// After a decoded upload: hand the texture to the encoder unless the cache has it, it is queued already or it stayed BC. The storage
// is copied (the game may reuse it at once); the queue is bounded, a full one drops it (offered again at its next upload).
void offer(const Image &im,const tex_t *t){
    if(state!=1 || !produce || !cacheable(t) || im.layers!=1)return;
    uint32_t c,a;key(t,&c,&a);const Key k{c,a,uint32_t(t->L.size),uint32_t(t->d.format),t->d.width,t->d.height,levels_of(t)};
    if(known.count(k) || find(t,c,a)>=0){++offer_known;return;}
    XbgAlJob j;j.k={c,a,uint32_t(t->L.size),uint16_t(t->d.format),levels_of(t),uint16_t(t->d.width),uint16_t(t->d.height)};j.d=t->d;j.serial=t->serial;
    const uint8_t *m=reinterpret_cast<const uint8_t*>(t->mem);j.storage.assign(m,m+t->L.size);
    if(astclazy::submit(std::move(j))){known.insert(k);++offered;waiting[k64(c,a)].push_back(t->serial);}else ++offer_dropped;
}
// RGBA8 bytes of all levels: the decoded size the cache saves (counted live with the image).
int64_t rgba_bytes(const Image &im){int64_t n=0;for(uint32_t l=0;l<im.levels;l++)n+=int64_t(std::max(1u,im.width>>l))*std::max(1u,im.height>>l)*4;return n;}
// Read the entry's blocks into a stream buffer and copy them into the image (the prologue when `pro`). Returns the staging bytes.
size_t upload(Image &im,const tex_t *t,bool pro){
    const XbgAcEntry &e=im.pack_e;const int fd=fds[e.pad[0]<3?e.pad[0]:0];Buffer staging=stream_buffer(e.data_bytes,nullptr,0,false);
    // [TL2 16.11] read and sanitize in cached host memory, then one sequential copy into the staging map: the stream arena is
    // write-combined, and xbg_ac_sanitize reading it back block by block took ~60 % of a 130 ms inventory-open frame (19 MB, Mi 14).
    thread_local std::vector<uint8_t> scratch;if(scratch.size()<e.data_bytes)scratch.resize(e.data_bytes);
    uint8_t *const out=static_cast<uint8_t*>(staging.mapped);uint8_t *dst=scratch.data();uint64_t got=0;
    while(fd>=0 && got<e.data_bytes){const ssize_t r=pread(fd,dst+got,size_t(e.data_bytes-got),off_t(e.data_off+got));if(r<=0)break;got+=uint64_t(r);}
    if(got<e.data_bytes){memset(dst+got,0,size_t(e.data_bytes-got));++read_fails;static unsigned n=0;if(n++<8)xbg_logf("NVN astc cache: read of serial=%llu (%s) failed at %llu of %u bytes (errno %d): the rest is transparent black",(unsigned long long)t->serial,SRC_NAME[e.pad[0]<3?e.pad[0]:0],(unsigned long long)got,e.data_bytes,errno);}
    zero_fixed+=xbg_ac_sanitize(dst,e.data_bytes);memcpy(out,dst,e.data_bytes);
    if(scratch.size()>(64u<<20))std::vector<uint8_t>().swap(scratch);   // one huge texture: do not keep its scratch
    staging.flush();
    std::vector<VkBufferImageCopy> copies;uint64_t off=0;
    for(uint32_t l=0;l<im.levels && l<e.levels;l++){VkBufferImageCopy c{};c.bufferOffset=staging.offset+off;c.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,l,0,1};
        c.imageExtent={std::max(1u,im.width>>l),std::max(1u,im.height>>l),1};copies.push_back(c);off+=xbg_ac_level_bytes(e,l);}
    if(pro){Slot &s=recording_slot();s.pro_images.push_back({im.image,im.layout,im.aspects,im.levels,im.layers,staging.buffer,copies});im.layout=VK_IMAGE_LAYOUT_GENERAL;}
    else{Commands c;im.transition(c.command,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(c.command,staging.buffer,im.image,im.layout,uint32_t(copies.size()),copies.data());im.transition(c.command,VK_IMAGE_LAYOUT_GENERAL);c.finish();}
    ++uploads;upload_bytes+=e.data_bytes;
    {static unsigned n=0;if(n++<8)xbg_logf("NVN astc cache: upload serial=%llu fmt=%#x %ux%u levels=%u -> ASTC %ux%u %u bytes from %s (mip 0 %.1f dB) prologue=%d",(unsigned long long)t->serial,t->d.format,im.width,im.height,im.levels,e.bw,e.bh,e.data_bytes,SRC_NAME[e.pad[0]<3?e.pad[0]:0],e.psnr_x100/100.0,int(pro));}
    return e.data_bytes;
}
}
#else
namespace astccache {   // not built for this game: every texture takes the BC path
int state=0;std::unordered_set<uint64_t> refused;uint64_t lost=0,upgraded=0;
inline bool lookup(const tex_t*,VkSampleCountFlagBits,XbgAcEntry*){return false;}
inline bool would_hit(const tex_t*){return false;}
inline bool same(const XbgAcEntry&,const tex_t*){return true;}
inline void offer(const Image&,const tex_t*){}
inline size_t upload(Image&,const tex_t*,bool){return 0;}
inline int64_t rgba_bytes(const Image&){return 0;}
void frame(){}
void perf(){}
}
#endif
