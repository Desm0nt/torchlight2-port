#!/usr/bin/env python3
"""Torchlight II Mobile: ASTC texture cache for phones without BC textures (Mali, PowerVR, Xclipse). Run by setup (step "astc").

  tl2_astc_cache.py ROMFS OUT [--mods DIR] [--jobs N] [--formats bc1,bc2,bc3] [--force] [--check] [--limit N] [--only GLOB]
                    [--keys FILE] [--verify]

The game's textures are XTX files inside ROMFS/PAKS/DATA*.PAK (tools/tl2_mod.py; docs/tl2/TL2_PORT.md): an info block (format,
size, levels, block height) and the storage block-linear, BC1 (0x42) / BC2 (0x43) / BC3 (0x44) or RGBA8. GPUs without BC sampled the
BC ones only after decoding them to RGBA8 (8x the memory for BC1, 4x for BC2 / BC3; Mali-G615 player log: 284 MB decoded where BC is
64.6 MB). This tool encodes every BC texture once, on the PC, to ASTC (docs/tl2/ASTC_CACHE.md):

Every texture must keep mip 0 at >= 42 dB PSNR (RGBA) against its decoded BC (the D3 rule, owner 13.11.2026). Per texture the steps
are tried in order and the first one that reaches 42 dB is kept; one that stays below even at 4x4 -thorough is left out (the engine
decodes its BC as before) and listed in quality.tsv as "bc":
  BC1      (8 bytes / 4x4)  -> 6x6 -fast, 5x5 -fast, 4x4 -fast, 4x4 -medium, 4x4 -thorough (alpha encoded as well: BC1 0x42 is RGBA)
  BC2, BC3 (16 bytes / 4x4) -> 4x4 -fast, 4x4 -medium, 4x4 -thorough
TL2's packs hold no normal maps (no *_N / NORMAL names; _SPEC / _GLOW maps are plain RGBA data), so no texture needs a channel
layout of its own: every channel is encoded and measured as it is decoded from BC.

The engine recognises a texture by its storage (dm/gfx/vulkan/engine/xbg_astc_cache.h: CRC-32 + Adler-32 of the bytes, their length,
NVN format, size and levels) when it creates the Vulkan image. The game's loader (main+0x1995f40) refuses a texture unless NVN's storage
size equals the XTX info's and memcpy's the XTX storage block into its pool, so the key here is taken over that block, exactly what the
engine hashes (checked for all 5863 BC textures by dm/android/tests/tl2_astc_key_test.cpp: same size, same block height).

Output in OUT (by setup: <work>/Torchlight2Mobile/astc, i.e. Download/Torchlight2Mobile/astc on the phone):
  tl2-astc.pack     'D3AC' v1: 64-byte header, the ASTC blocks of every texture (level 0 first, each level tight rows of
                    blocks), then the index (48 bytes an entry, sorted by key) - the D3 pack format, read by xbg_astc_cache_tl2.inc
  manifest.json     written last: version, encoder and settings, counts, sizes, source identity, seconds
  quality.tsv       per texture: name, NVN format, size, levels, ASTC block (or bc), preset, steps, PSNR of mip 0 against the decoded BC
All-zero ASTC blocks (the error block on Mali, KB TEX-04) never reach the file: such a block becomes the void-extent block of
transparent black (counted in the manifest; the engine checks again when it uploads).

The RomFS is only read. The output is deterministic for the same input and encoder. An interrupted run resumes from OUT/.journal; a
finished one is skipped while manifest.json matches this script (its SHA-256), the encoder versions, the settings and the source packs
(names and sizes). --force rebuilds, --check only reports whether OUT is up to date (exit 0) or not (exit 1). --limit / --only make a
small test pack. --mods DIR adds the textures of converted mod packs (DIR/PAKS/ZMOD_*.PAK, tools/tl2_mod.py convert); without it mod
textures are left to the phone's lazy cache. --keys FILE writes every BC texture's key (the host test's format) and exits. --verify
reads OUT/tl2-astc.pack back: header, index order, every entry's range and size, no all-zero blocks, and decodes a sample against
the BC source.
Dependencies (tools/requirements-astc.txt, installed by setup into its private Python): astc-encoder-py (MIT; ARM astc-encoder,
Apache-2.0), numpy (BSD-3-Clause). The BC decoders are this file's (the engine's CPU decoder in numpy; from the Diablo III port's
tools/d3_astc_cache.py).
"""
import argparse, fnmatch, hashlib, json, os, struct, sys, time, zlib
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

CACHE_FORMAT, CACHE_VERSION = 'tl2-astc-cache', 1
PACK, MAGIC = 'tl2-astc.pack', b'D3AC'
HEADER = struct.Struct('<4sIIIQQQ24x')         # magic, version, entries, flags, index offset, data offset, data bytes (64)
ENTRY = struct.Struct('<IIIHHHHBBHQII8x')      # crc32, adler32, storage bytes, NVN format, levels, width, height, block w, block h,
                                               # flags, data offset, data bytes, PSNR x100 of mip 0 (48)
assert HEADER.size == 64 and ENTRY.size == 48
BC = {0x41: ('bc1', 8, False), 0x42: ('bc1', 8, False), 0x43: ('bc2', 16, False), 0x44: ('bc3', 16, False),
      0x45: ('bc1', 8, True), 0x46: ('bc1', 8, True), 0x47: ('bc2', 16, True), 0x48: ('bc3', 16, True)}   # NVN format -> (kind, block bytes, sRGB)
QUALITY = {'fastest': 0, 'fast': 10, 'medium': 60, 'thorough': 98}
TARGET_DB = 42.0
STEPS = {'bc1': ((6, 'fast'), (5, 'fast'), (4, 'fast'), (4, 'medium'), (4, 'thorough')),
         'bc2': ((4, 'fast'), (4, 'medium'), (4, 'thorough')), 'bc3': ((4, 'fast'), (4, 'medium'), (4, 'thorough'))}
PRESET_CODE = {'fast': 0, 'medium': 1, 'thorough': 2}
VOID_BLACK = bytes([0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF]) + bytes(8)
PACKAGES = ('astc-encoder-py', 'numpy')
TEX_EXT = ('.DDS', '.PNG', '.JPG', '.TGA')


class Fail(Exception):
    pass


# ---------------------------------------------------------------- PAK / MAN / XTX (tools/tl2_mod.py; dm/android/tl2_pak.h)
def man_read(b):
    """-> [(path upper with '/', type, offset, size)]"""
    o = 0
    def ws():
        nonlocal o
        n = struct.unpack_from('<H', b, o)[0]; o += 2
        s = b[o:o + 2 * n].decode('utf-16le', 'surrogatepass'); o += 2 * n; return s
    ver = struct.unpack_from('<H', b, o)[0]; o += 2
    if ver >= 2: o += 4
    ws(); _total, nd = struct.unpack_from('<II', b, o); o += 8
    out = []
    for _ in range(nd):
        d = ws(); n = struct.unpack_from('<I', b, o)[0]; o += 4
        for _ in range(n):
            _crc, typ = struct.unpack_from('<IB', b, o); o += 5
            name = ws(); off, size = struct.unpack_from('<II', b, o); o += 16
            out.append(((d + name).replace('\\', '/').upper(), typ, off, size))
    return out


def pak_get(f, off):
    f.seek(off); dl, cl = struct.unpack('<II', f.read(8)); raw = f.read(cl or dl)
    return zlib.decompress(raw) if cl and cl != dl else raw


def parse_xtx(d):
    """XTX bytes -> dict(fmt, w, h, levels, th, offsets, storage, target, depth) or None. Format 0x41 is given as 0x42 (the loader's remap)."""
    if len(d) < 16 or d[:4] != b'DFvN': return None
    o = struct.unpack_from('<I', d, 4)[0]; info = st = None
    while o + 0x24 <= len(d) and d[o:o + 4] == b'HBvN':
        _, dsz, doff, typ, _, _ = struct.unpack_from('<IQQIII', d, o + 4)
        if doff < 0x24 or o + doff + dsz > len(d): return None
        p = d[o + doff:o + doff + dsz]
        if typ == 2 and info is None: info = p
        elif typ == 3 and st is None: st = p
        o += doff + dsz
    if info is None or st is None or len(info) < 0x70: return None
    size, _al, w, h, dep, tgt, fmt, mips, _ = struct.unpack_from('<QIIIIIIII', info, 0)
    if fmt == 0x41: fmt = 0x42
    if not w or not h or not 1 <= mips <= 16: return None
    return dict(fmt=fmt, w=w, h=h, levels=mips, depth=dep, target=tgt, size=size, th=struct.unpack_from('<I', info, 0x6c)[0],
                offsets=struct.unpack_from('<16I', info, 0x28)[:mips], storage=bytes(st))


def key_of(t):
    s = t['storage']
    return (zlib.crc32(s) & 0xffffffff, zlib.adler32(s) & 0xffffffff, len(s), t['fmt'], t['w'], t['h'], t['levels'])


def cacheable(t):
    return t is not None and t['fmt'] in BC and t['target'] == 1 and t['depth'] == 1 and t['size'] == len(t['storage'])


# ---------------------------------------------------------------- block-linear (Tegra X1; dm/gfx/tex/nvn_deswizzle.c, nvn_tex.c)
def _np():
    import numpy
    return numpy


def adj_tile(shift, unit, dim):
    if not shift: return 0
    x = unit << (shift - 1)
    if x >= dim:
        while True:
            shift -= 1
            if not shift: break
            x >>= 1
            if x < dim: break
    return shift


def deswizzle_level(t, level, bpb):
    """Level `level` of the storage -> (blocks_x, blocks_y, tight linear block bytes)."""
    np = _np()
    w, h = max(1, t['w'] >> level), max(1, t['h'] >> level)
    bx, by = (w + 3) // 4, (h + 3) // 4
    row_bytes = bx * bpb
    th = adj_tile(t['th'], 8, by)
    bh = 1 << th
    gobs_x = (row_bytes + 63) // 64
    block = 512 * bh
    x = (np.arange(bx, dtype=np.int64) * bpb)[None, :]
    y = np.arange(by, dtype=np.int64)[:, None]
    gob = (x % 64 // 32) * 256 + (y % 8 // 2) * 64 + (x % 32 // 16) * 32 + (y % 2) * 16 + x % 16
    off = (y // (8 * bh)) * gobs_x * block + (x // 64) * block + ((y % (8 * bh)) // 8) * 512 + gob + t['offsets'][level]
    idx = off[..., None] + np.arange(bpb, dtype=np.int64)
    src = np.frombuffer(t['storage'], np.uint8)
    if idx.max() >= len(src): raise Fail(f'level {level} reaches past the storage')
    return bx, by, src[idx.reshape(-1)].tobytes()


# ---------------------------------------------------------------- BC decode (the engine's CPU decoder semantics, xbg_texture.c)
def _bc_color(b, four, opaque):
    np = _np()
    c0 = b[:, 0].astype(np.uint32) | b[:, 1].astype(np.uint32) << 8
    c1 = b[:, 2].astype(np.uint32) | b[:, 3].astype(np.uint32) << 8
    def rgb(c):
        r, g, bl = (c >> 11) & 31, (c >> 5) & 63, c & 31
        return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (bl << 3) | (bl >> 2)], 1)
    v0, v1 = rgb(c0), rgb(c1)
    f4 = (four | (c0 > c1))[:, None]
    pal = np.zeros((len(b), 4, 4), np.uint32)
    pal[:, 0, :3], pal[:, 1, :3] = v0, v1
    pal[:, 2, :3] = np.where(f4, (2 * v0 + v1) // 3, (v0 + v1) // 2)
    pal[:, 3, :3] = np.where(f4, (v0 + 2 * v1) // 3, 0)
    pal[:, :3, 3] = 255
    pal[:, 3, 3] = np.where(f4[:, 0] | opaque, 255, 0)
    bits = np.ascontiguousarray(b[:, 4:8]).view('<u4').ravel()
    idx = ((bits[:, None] >> (2 * np.arange(16, dtype=np.uint32))) & 3).astype(np.int64)
    return np.take_along_axis(pal, idx[..., None].repeat(4, 2), 1)


def _bc3_alpha(b):
    np = _np()
    a0, a1 = b[:, 0].astype(np.int64) * 257, b[:, 1].astype(np.int64) * 257
    k = np.arange(8, dtype=np.int64)[None, :]
    seven = ((8 - k) * a0[:, None] + (k - 1) * a1[:, None] + 3) // 7
    five = np.where(k < 6, ((6 - k) * a0[:, None] + (k - 1) * a1[:, None] + 2) // 5, np.where(k == 6, 0, 65535))
    pal = np.where((b[:, 0] > b[:, 1])[:, None], seven, five)
    pal[:, 0], pal[:, 1] = a0, a1
    bits = np.zeros(len(b), np.uint64)
    for i in range(6): bits |= b[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
    idx = ((bits[:, None] >> (np.uint64(3) * np.arange(16, dtype=np.uint64))) & np.uint64(7)).astype(np.int64)
    return ((np.take_along_axis(pal, idx, 1) + 128) // 257).astype(np.uint32)


def decode_bc(kind, fmt, data, bx, by):
    """Linear BC blocks -> (by*4, bx*4, 4) RGBA8 exactly as the engine's CPU decoder (sRGB stays encoded)."""
    np = _np()
    if kind == 'bc1':
        px = _bc_color(np.frombuffer(data, np.uint8).reshape(-1, 8), False, fmt in (0x41, 0x45))
    else:
        b = np.frombuffer(data, np.uint8).reshape(-1, 16)
        px = _bc_color(b[:, 8:], True, True)
        if kind == 'bc2':
            a = np.ascontiguousarray(b[:, :8]).view('<u8').ravel()
            px[:, :, 3] = ((a[:, None] >> (np.uint64(4) * np.arange(16, dtype=np.uint64))) & np.uint64(15)).astype(np.uint32) * 17
        else:
            px[:, :, 3] = _bc3_alpha(b[:, :8])
    return px.astype(np.uint8).reshape(by, bx, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(by * 4, bx * 4, 4)


# ---------------------------------------------------------------- ASTC (worker processes)
_ctx = {}


def _astc(srgb, bw, bh, quality):
    k = (srgb, bw, bh, quality)
    if k not in _ctx:
        import astc_encoder as ae
        prof = ae.ASTCProfile.LDR_SRGB if srgb else ae.ASTCProfile.LDR
        _ctx[k] = (ae, ae.ASTCContext(ae.ASTCConfig(prof, bw, bh, 1, QUALITY[quality]), 1), ae.ASTCSwizzle())
    return _ctx[k]


def encode(img, w, h, srgb, bw, bh, quality):
    np = _np()
    ae, ctx, sw = _astc(srgb, bw, bh, quality)
    im = ae.ASTCImage(ae.ASTCType.U8, w, h, 1, np.ascontiguousarray(img[:h, :w]).tobytes())
    out = bytearray(ctx.compress(im, sw))
    n = ((w + bw - 1) // bw) * ((h + bh - 1) // bh)
    if len(out) != n * 16: raise Fail(f'astcenc gave {len(out)} bytes for {n} blocks')
    a = np.frombuffer(bytes(out), np.uint8).reshape(n, 16)
    zero = np.nonzero(~a.any(1))[0]
    for i in zero: out[i * 16:i * 16 + 16] = VOID_BLACK
    return bytes(out), len(zero)


def decode_astc(blocks, w, h, srgb, bw, bh, quality='fast'):
    np = _np()
    ae, ctx, sw = _astc(srgb, bw, bh, quality)
    im = ctx.decompress(blocks, ae.ASTCImage(ae.ASTCType.U8, w, h, 1), sw)
    return np.frombuffer(im.data, np.uint8).reshape(h, w, 4)


def psnr(a, b):
    np = _np()
    mse = float(np.mean((a.astype(np.float64) - b) ** 2))
    return 99.0 if mse == 0 else float(10 * np.log10(255.0 ** 2 / mse))


def levels_rgba(t):
    kind, bpb, _ = BC[t['fmt']]
    out = []
    for lv in range(t['levels']):
        bx, by, data = deswizzle_level(t, lv, bpb)
        out.append((max(1, t['w'] >> lv), max(1, t['h'] >> lv), decode_bc(kind, t['fmt'], data, bx, by)))
    return out


def convert(t):
    """One texture -> (block w, block h, [level bytes], PSNR of mip 0, zero blocks fixed, preset, steps tried); block 0 and no levels
    when no step reaches TARGET_DB (the texture stays BC; the PSNR is the best one seen)."""
    kind, _, srgb = BC[t['fmt']]
    imgs = levels_rgba(t)
    w0, h0, i0 = imgs[0]
    best, tried = -1.0, 0
    for b, q in STEPS[kind]:
        tried += 1
        enc, _ = encode(i0, w0, h0, srgb, b, b, q)
        db = psnr(i0[:h0, :w0], decode_astc(enc, w0, h0, srgb, b, b, q))
        best = max(best, db)
        if db >= TARGET_DB: break
    else:
        return 0, 0, [], best, 0, 'bc', tried
    levels, zeros = [], 0
    for w, h, img in imgs:
        enc, z = encode(img, w, h, srgb, b, b, q); levels.append(enc); zeros += z
    return b, b, levels, db, zeros, q, tried


def job(args):
    path, off = args
    with open(path, 'rb') as f: t = parse_xtx(pak_get(f, off))
    if not cacheable(t): raise Fail(f'{path}@{off}: not a BC texture any more')
    return (key_of(t),) + convert(t)


# ---------------------------------------------------------------- planning
def packs(romfs, mods):
    """[(label, path)]: ROMFS/PAKS/DATA*.PAK in the game's order (DATA, DATA1 ... DATA5), then MODS/PAKS/ZMOD_*.PAK by name."""
    out = []
    pd = Path(romfs) / 'PAKS'
    if pd.is_dir():
        names = sorted((p for p in pd.iterdir() if p.suffix.upper() == '.PAK' and (pd / (p.name + '.MAN')).is_file()),
                       key=lambda p: (len(p.stem), p.stem.upper()))
        out += [(p.name, str(p)) for p in names]
    for m in mods:
        md = Path(m) / 'PAKS'
        if md.is_dir(): out += [(f'mods/{p.name}', str(p)) for p in sorted(md.iterdir()) if p.suffix.upper() == '.PAK' and (md / (p.name + '.MAN')).is_file()]
    return out


def plan(paks, formats, only=None, progress=None):
    """Unique BC textures of every pack (a texture in several packs, or under several names, is one job keyed by its storage):
    [(key, pack path, offset, name)] sorted by key, and counts of what was left out."""
    seen, skipped = {}, {}
    for lab, p in paks:
        ents = man_read(open(p + '.MAN', 'rb').read())
        with open(p, 'rb') as f:
            for name, typ, off, size in ents:
                if typ == 7 or not size or not name.endswith(TEX_EXT): continue
                if only and not fnmatch.fnmatch(name.lower(), only.lower()): continue
                t = parse_xtx(pak_get(f, off))
                why = ('not an XTX texture' if t is None else f'format {t["fmt"]:#x}' if t['fmt'] not in BC else
                       'cube / volume' if t['target'] != 1 or t['depth'] != 1 else 'storage size differs from the info' if t['size'] != len(t['storage']) else
                       f'{BC[t["fmt"]][0]} not selected' if BC[t['fmt']][0] not in formats else None)
                if why: skipped[why] = skipped.get(why, 0) + 1; continue
                k = key_of(t)
                if k not in seen: seen[k] = (p, off, name)
        if progress: progress(f'{lab}: {len(seen)} unique BC textures so far')
    return sorted((k, p, o, n) for k, (p, o, n) in seen.items()), skipped


# ---------------------------------------------------------------- driver
def sha256_file(p, chunk=1 << 22):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(chunk), b''): h.update(b)
    return h.hexdigest()


def encoder_versions():
    from importlib import metadata
    v = {}
    for p in PACKAGES:
        try: v[p] = metadata.version(p)
        except Exception: v[p] = None
    return v


def missing_packages():
    import importlib.util as iu
    return [p for p, m in zip(PACKAGES, ('astc_encoder', 'numpy')) if iu.find_spec(m) is None]


def identity(paks, a):
    return dict(format=CACHE_FORMAT, version=CACHE_VERSION, tool_sha256=sha256_file(__file__), encoders=encoder_versions(),
                settings=dict(formats=sorted(a.formats), target_db=TARGET_DB, steps={k: [f'{b}x{b} {q}' for b, q in v] for k, v in STEPS.items()}, only=a.only, limit=a.limit),
                sources=[[lab, os.path.getsize(p)] for lab, p in paks])


def up_to_date(out, ident):
    m = Path(out) / 'manifest.json'
    if not m.is_file() or not (Path(out) / PACK).is_file(): return False
    try: old = json.loads(m.read_text(encoding='utf-8'))
    except Exception: return False
    return all(old.get(k) == v for k, v in ident.items()) and old.get('complete') is True and \
        os.path.getsize(Path(out) / PACK) == old.get('pack_bytes')


def write_keys(paks, path, log):
    n = 0
    with open(path, 'w', encoding='utf-8', newline='\n') as kf:
        for lab, p in paks:
            with open(p, 'rb') as f:
                for name, typ, off, size in man_read(open(p + '.MAN', 'rb').read()):
                    if typ == 7 or not size or not name.endswith(TEX_EXT): continue
                    t = parse_xtx(pak_get(f, off))
                    if not cacheable(t): continue
                    c, ad, sz, fmt, w, h, lv = key_of(t); n += 1
                    kf.write(f'{c:08x} {ad:08x} {sz} {fmt:x} {w} {h} {lv} {lab} {name}\n')
    log(f'{n} BC texture keys -> {path}')
    return 0


def verify(out, paks, log, sample=64):
    """Read the pack back: header, index sorted and self-consistent, no all-zero block, and `sample` entries decoded against their source."""
    np = _np()
    p = Path(out) / PACK
    b = p.read_bytes()
    magic, ver, n, _fl, io, do, db = HEADER.unpack_from(b, 0)
    if magic != MAGIC or ver != 1: raise Fail(f'{p}: not a D3AC v1 pack')
    if io + n * ENTRY.size != len(b) or do != 64 or do + db != io: raise Fail(f'{p}: header does not match the file ({len(b)} bytes)')
    ents = [ENTRY.unpack_from(b, io + i * ENTRY.size) for i in range(n)]
    keys = [(e[0], e[1], e[2], e[3], e[5], e[6], e[4]) for e in ents]
    if keys != sorted(keys) or len(set(keys)) != len(keys): raise Fail('index not sorted by key / duplicate keys')
    zero = 0
    for e in ents:
        crc, adl, size, fmt, lv, w, h, bw, bh, fl, off, nb, pr = e
        want = sum(((max(1, w >> l) + bw - 1) // bw) * ((max(1, h >> l) + bh - 1) // bh) * 16 for l in range(lv))
        if bw not in (4, 5, 6) or bw != bh or want != nb or off < 64 or off + nb > io: raise Fail(f'entry {crc:08x}: bad range / size')
        a = np.frombuffer(b, np.uint8, nb, off).reshape(-1, 16)
        zero += int((~a.any(1)).sum())
    log(f'verify: {n} entries, index sorted, ranges ok, all-zero blocks {zero}')
    if zero: raise Fail(f'{zero} all-zero ASTC blocks in the pack')
    # sample: re-read the sources, decode the pack's mip 0, compare with the decoded BC
    want = {k: e for k, e in zip(keys, ents)}
    picked = set(sorted(want)[::max(1, len(want) // sample)][:sample])
    worst, checked = 99.0, 0
    for lab, pp in paks:
        with open(pp, 'rb') as f:
            for name, typ, off, size in man_read(open(pp + '.MAN', 'rb').read()):
                if not picked or typ == 7 or not size or not name.endswith(TEX_EXT): continue
                t = parse_xtx(pak_get(f, off))
                if not cacheable(t): continue
                k = key_of(t)
                if k not in picked: continue
                picked.discard(k); e = want[k]; _, _, srgb = BC[t['fmt']]
                w0, h0, i0 = levels_rgba(t)[0]; nb0 = ((w0 + e[7] - 1) // e[7]) * ((h0 + e[8] - 1) // e[8]) * 16
                d = psnr(i0[:h0, :w0], decode_astc(b[e[10]:e[10] + nb0], w0, h0, srgb, e[7], e[8]))
                if d + 0.05 < e[12] / 100 - 0.5 or d < TARGET_DB - 0.05: raise Fail(f'{name}: pack decodes at {d:.2f} dB (index says {e[12] / 100:.2f})')
                worst = min(worst, d); checked += 1
    log(f'verify: {checked} sampled textures decode at >= {worst:.2f} dB against their BC')
    return 0


def build(a, log=lambda s: print(s, flush=True)):
    paks = packs(a.romfs, a.mods)
    if not paks: raise Fail(f'no PAKS/*.PAK (+ .MAN) under {a.romfs}')
    if a.keys: return write_keys(paks, a.keys, log)
    miss = missing_packages()
    if miss: raise Fail('missing Python packages: ' + ', '.join(miss) + ' (pip install -r tools/requirements-astc.txt)')
    if a.verify: return verify(a.out, paks, log)
    ident = identity(paks, a)
    out = Path(a.out)
    if up_to_date(out, ident) and not a.force:
        log(f'ASTC cache in {out} is up to date'); return 0
    if a.check: log(f'ASTC cache in {out} is missing or out of date'); return 1
    out.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    jobs, skipped = plan(paks, a.formats, a.only, lambda s: log('  ' + s))
    if a.limit: jobs = jobs[:a.limit]
    log(f'{len(jobs)} BC textures to encode (left as they are: {skipped or "none"}), plan {time.time() - t0:.0f} s')
    part, jpath = out / (PACK + '.part'), out / '.journal'
    jsig = hashlib.sha256(json.dumps(ident, sort_keys=True).encode()).hexdigest()
    done, end = {}, HEADER.size
    if not a.force and part.is_file() and jpath.is_file():   # resume: entries journaled before the interruption
        lines = jpath.read_text(encoding='utf-8').splitlines()
        if lines and lines[0] == jsig:
            for ln in lines[1:]:
                f = ln.split('\t')
                if len(f) == 13:
                    k = tuple(int(x) for x in f[:7]); done[k] = tuple(int(x) for x in f[7:12]) + (f[12],); end = max(end, done[k][3] + done[k][4])
    if not done:
        with open(part, 'wb') as f: f.write(bytes(HEADER.size))
        jpath.write_text(jsig + '\n', encoding='utf-8')
    else:
        with open(part, 'r+b') as f: f.truncate(end)
        log(f'resuming: {len(done)} textures already encoded')
    todo = [j for j in jobs if j[0] not in done]
    zeros = 0
    with open(part, 'ab') as f, open(jpath, 'a', encoding='utf-8') as jf, ProcessPoolExecutor(max_workers=a.jobs) as ex:
        t1 = time.time()
        for i, (k, bw, bh, levels, db, z, q, tried) in enumerate(ex.map(job, [(p, o) for _, p, o, _ in todo], chunksize=4)):
            data = b''.join(levels); at = f.tell(); f.write(data)
            done[k] = (bw, bh, int(round(db * 100)), at if bw else 0, len(data), f'{q}/{tried}')
            jf.write('\t'.join(str(x) for x in k + done[k][:5]) + f'\t{q}/{tried}\n')
            zeros += z
            if (i + 1) % 200 == 0 or i + 1 == len(todo):
                f.flush(); jf.flush()
                el = time.time() - t1
                log(f'  {i + 1} / {len(todo)} textures, {f.tell() / 2**20:.0f} MB, {el:.0f} s, about {el / (i + 1) * (len(todo) - i - 1) / 60:.1f} min left')
    names = {k: n for k, _, _, n in jobs}
    stayed = sorted(k for k in done if k in names and not done[k][0])
    keys = sorted(k for k in done if k in names and done[k][0])
    with open(part, 'r+b') as f:
        f.seek(0, 2); index_off = f.tell(); data_bytes = index_off - HEADER.size
        for k in keys:
            crc, adl, size, fmt, w, h, lv = k; bw, bh, db, at, n, q = done[k]
            f.write(ENTRY.pack(crc, adl, size, fmt, lv, w, h, bw, bh, PRESET_CODE[q.split('/')[0]], at, n, db))
        f.seek(0); f.write(HEADER.pack(MAGIC, CACHE_VERSION, len(keys), 0, index_off, HEADER.size, data_bytes))
    by_block, retried, bc_bytes, cached_bc = {}, 0, 0, 0
    with open(out / 'quality.tsv', 'w', encoding='utf-8', newline='\n') as q:
        q.write('name\tformat\twidth\theight\tlevels\tblock\tpreset\tsteps\tpsnr_db\n')
        for k in sorted(keys + stayed):
            bw, bh, db, at, n, pre = done[k]; blk = f'{bw}x{bh}' if bw else 'bc'; preset, tried = pre.split('/'); retried += int(tried) > 1
            s = by_block.setdefault(f'{BC[k[3]][0]} {blk} {preset}', [0, 0, 99.0]); s[0] += 1; s[1] += n; s[2] = min(s[2], db / 100)
            bc_bytes += k[2]; cached_bc += k[2] if bw else 0
            q.write(f'{names[k]}\t{k[3]:#x}\t{k[4]}\t{k[5]}\t{k[6]}\t{blk}\t{preset}\t{tried}\t{db / 100:.2f}\n')
    pack = out / PACK
    if pack.exists(): pack.unlink()
    part.rename(pack)
    jpath.unlink()
    man = dict(ident, complete=True, entries=len(keys), pack_bytes=os.path.getsize(pack), zero_blocks_fixed=zeros, skipped=skipped,
               stayed_bc=len(stayed), retried=retried, bc_storage_bytes=bc_bytes, cached_bc_storage_bytes=cached_bc,
               by_kind_block_preset={k: dict(textures=v[0], bytes=v[1], min_psnr_db=round(v[2], 2)) for k, v in sorted(by_block.items())},
               seconds=round(time.time() - t0))
    (out / 'manifest.json').write_text(json.dumps(man, indent=1), encoding='utf-8')
    log(f'ASTC cache: {len(keys)} textures ({retried} needed more than the first step, {len(stayed)} stay BC), {man["pack_bytes"] / 2**20:.0f} MB '
        f'in {out / PACK} (BC storage of the cached textures {cached_bc / 2**20:.0f} MB), {man["seconds"]} s')
    return 0


def setup_step(romfs, out, mods=(), jobs=None, log=lambda s: print(s, flush=True)):
    """For tools/setup_tl2.py (step 'astc'): build or keep <out>; returns 0 when the cache is complete."""
    a = argparse.Namespace(romfs=str(romfs), out=str(out), mods=[str(m) for m in mods], jobs=jobs or max(1, (os.cpu_count() or 2) - 1),
                           formats={'bc1', 'bc2', 'bc3'}, force=False, check=False, limit=0, only=None, keys=None, verify=False)
    return build(a, log)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('romfs', help='RomFS folder with PAKS/DATA*.PAK (+ .MAN)')
    ap.add_argument('out', nargs='?', default=None)
    ap.add_argument('--mods', action='append', default=[], help='a converted mods folder with PAKS/ZMOD_*.PAK (repeatable)')
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument('--formats', default='bc1,bc2,bc3', help='BC kinds to encode (default bc1,bc2,bc3)')
    ap.add_argument('--force', action='store_true')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--only', default=None, help='member glob, e.g. "media/ui/*" (tests)')
    ap.add_argument('--keys', default=None, help='write every BC texture key to this file and exit')
    ap.add_argument('--verify', action='store_true', help='check OUT/tl2-astc.pack against the RomFS')
    a = ap.parse_args(argv)
    a.formats = {f.strip() for f in a.formats.split(',') if f.strip()}
    if not a.formats <= {'bc1', 'bc2', 'bc3'}: ap.error('--formats takes bc1, bc2, bc3')
    if a.out is None and not a.keys: ap.error('OUT is required')
    try:
        return build(a)
    except Fail as e:
        print(f'tl2_astc_cache: {e}', file=sys.stderr); return 2


if __name__ == '__main__':
    sys.exit(main())
