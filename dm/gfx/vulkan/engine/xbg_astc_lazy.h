// [DOS2] On-device lazy ASTC texture cache (docs/dos2/DOS2_PORT.md "ASTC-кэш без ПК"): the producer side.
// GPUs without BC (Mali, PowerVR, DM_GFX_SIM_GPU=mali) decode every BC texture at upload (BcUpload, GPU or CPU decoder). The engine
// (xbg_game.cpp namespace astccache) hands each BC1-BC3 2D texture it decoded, by the key of the D3 cache (CRC-32 + Adler-32 of the
// guest storage, its size, NVN format, size, levels: xbg_astc_cache.h), to these background workers. They deswizzle and decode the
// storage copy on the CPU (the engine's nvn_deswizzle / xbg_decode_block), encode it with ARM astc-encoder 5.7.0 by the D3 rule (mip 0
// >= 42 dB against the decoded BC: BC1 6x6 / 5x5 / 4x4 -fast, then 4x4 -medium / -thorough; BC2 / BC3 4x4 -fast / -medium / -thorough;
// none reaches it -> the texture stays BC), append the blocks to the pack's part file and journal the entry (the D3 on-phone format,
// xbg_ac_journal_parse) only after the data was synced. The engine takes the committed entries (take) into its index: the next
// creation of that texture, in this run or a later one, is made from the cache. Workers run at nice 10 and never touch Vulkan.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>
#include "xbg_astc_cache.h"
extern "C" {
#include "../../nvn/nvn_tex.h"
}

struct XbgAlKey { uint32_t crc, adler, bytes; uint16_t format, levels, width, height; };
struct XbgAlJob { XbgAlKey k; nvn_texdesc d; std::vector<uint8_t> storage; uint64_t serial; };
struct XbgAlStats { uint64_t queued, dropped_queue, dropped_cap, encoded, stayed_bc, failed, retried, zero_fixed, bytes_written, px_encoded, encode_ns, queue_bytes, pack_end; };

namespace astclazy {
// fd: the part file (read / write, shared with the engine's preads); end: its committed size (>= 64); journal: path of the journal
// (already holding its signature line); cap: pack bytes at which the producer stops; queue_cap: storage bytes waiting at most.
bool start(int fd, uint64_t end, const std::string &journal, int threads, uint64_t cap, uint64_t queue_cap);
bool running();
// Queue one texture (moves the job). false: not queued (queue full / pack full / not running), the texture just stays decoded.
bool submit(XbgAlJob &&job);
// Entries committed since the last call (appended to out); cheap when there are none (one atomic load).
size_t take(std::vector<XbgAcEntry> &out);
XbgAlStats stats();
}
