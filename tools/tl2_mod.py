"""Torchlight II PC mods (.MOD) -> Switch-port data pack (PAKS/ZMOD_<nn>_<name>.PAK + .PAK.MAN) for the DM_TL2_MODS overlay.

Formats (docs/tl2/TL2_PORT.md, "Моды PC" has the evidence):
  .MOD      = header (version 4: u16 ver, u16 modver, u16 gamever[4], u32 offData, u32 offMan, wstr title/author/descr/website/
              download, u64 modid, u32 flags, u64 reqHash, u16 nreq {wstr name, u64 id, u16 ver}, u16 ndel {wstr path})
              + a TL2 PAK body at offData (u32 largest packed size, u32 hash, then {u32 size, u32 packed size, zlib|raw} per file,
              MAN offsets relative to offData) + a PAK.MAN at offMan. wstr = u16 count + UTF-16LE.
  PAK/MAN   = byte-identical to the Switch RomFS PAKS/DATA*.PAK(.MAN) (same header hash: tools check "pakhash"), so the PAK body and
              the MAN of a mod can be used as a Switch pack unchanged -- except textures.
  Textures  = on the Switch every .DDS/.PNG/.JPG/.TGA inside the packs is an XTX container ('DFvN' / 'HBvN' blocks, NVN format
              0x42 BC1, 0x43 BC2, 0x44 BC3, 0x25 RGBA8, block-linear GOB layout). PC mods carry real DDS / PNG / JPG / TGA, so they
              are converted here (DXT1/3/5 and 32-bit uncompressed DDS; PNG/JPG/TGA through Pillow to RGBA8).

Device port: dm/android/.../Tl2ModConv.java (the app's mod manager, Tl2Mods.java) writes the same bytes; tools/tl2_mod_java_test.py checks.

Usage:
  tl2_mod.py info    <mod>                         header, file counts, texture kinds
  tl2_mod.py convert <mod> <out_overlay> [--slot N] [--no-tex]   write <out_overlay>/PAKS/ZMOD_<NN>_<name>.PAK(.MAN) + .json
  tl2_mod.py unpack  <mod|pak> <dest>              extract files (as stored, decompressed)
  tl2_mod.py selftest <romfs>                      XTX layout: rebuild game textures from their own pixels, compare byte for byte;
                                                   PAK hash of DATA*.PAK
"""
import json, os, re, struct, sys, zlib

# ---------------------------------------------------------------- MAN / PAK

def _ws(b, o):
    n = struct.unpack_from('<H', b, o)[0]; o += 2
    return b[o:o + 2 * n].decode('utf-16le', 'surrogatepass'), o + 2 * n

def _wsw(s):
    e = s.encode('utf-16le', 'surrogatepass'); return struct.pack('<H', len(e) // 2) + e

def man_read(b):
    """-> (root, [(dir, name, type, offset, size, crc, filetime)])"""
    o = 0
    ver = struct.unpack_from('<H', b, o)[0]; o += 2
    if ver >= 2: o += 4
    root, o = _ws(b, o)
    _total, nd = struct.unpack_from('<II', b, o); o += 8
    out = []
    for _ in range(nd):
        d, o = _ws(b, o); n = struct.unpack_from('<I', b, o)[0]; o += 4
        for _ in range(n):
            crc, typ = struct.unpack_from('<IB', b, o); o += 5
            name, o = _ws(b, o)
            off, size, ft = struct.unpack_from('<IIQ', b, o); o += 16
            out.append((d, name, typ, off, size, crc, ft))
    return root, out

def man_write(root, ents):
    """ents: same tuples as man_read; directory records (type 7) are kept as given. Directories are grouped in first-seen order."""
    dirs = {}
    for e in ents: dirs.setdefault(e[0], []).append(e)
    body = b''.join(_wsw(d) + struct.pack('<I', len(es)) + b''.join(
        struct.pack('<IB', e[5], e[2]) + _wsw(e[1]) + struct.pack('<IIQ', e[3], e[4], e[6]) for e in es) for d, es in dirs.items())
    # header: version 2, a hash the game does not check (0, as GUTS / rgman write it), root, a capacity hint (records + directories,
    # as rgman's writer; the game's own packs carry larger values), the directory count
    return struct.pack('<HI', 2, 0) + _wsw(root) + struct.pack('<II', len(ents) + len(dirs), len(dirs)) + body

def pak_hash(b):
    """TL2 PAK header hash over the whole pack (header included in the size, sampling starts after it). Same as the game's packs."""
    n = len(b)
    if not n: return 0
    seed = 25 + ((((n >> 32) + (n & 0xffffffff) * 0x29777B41) & 0xffffffff) % 51)
    step = max(2, n // min(seed, 75))
    h = n
    for o in range(8, n, step): h = (h * 33 + (b[o] - 256 if b[o] > 127 else b[o])) & 0xffffffff
    c = b[n - 1]; return (h * 33 + (c - 256 if c > 127 else c)) & 0xffffffff

def pak_get(pak, off):
    dl, cl = struct.unpack_from('<II', pak, off)
    raw = pak[off + 8:off + 8 + (cl or dl)]
    return zlib.decompress(raw) if cl and cl != dl else bytes(raw)

# ---------------------------------------------------------------- .MOD

def mod_read(path):
    b = open(path, 'rb').read()
    ver = struct.unpack_from('<H', b, 0)[0]
    if not 1 <= ver <= 4: raise ValueError(f'{path}: not a TL2 .MOD (header version {ver})')
    o = 2; info = {'format': ver}
    info['modver'] = struct.unpack_from('<H', b, o)[0]; o += 2
    if ver >= 4:
        g = struct.unpack_from('<4H', b, o); o += 8; info['gamever'] = '.'.join(map(str, g))
    if ver == 1: o += 9 * (info['modver'] + 1)
    off_data, off_man = struct.unpack_from('<II', b, o); o += 8
    for k in ('title', 'author', 'descr', 'website', 'download'): info[k], o = _ws(b, o)
    info['modid'], info['flags'], info['reqhash'] = struct.unpack_from('<qIq', b, o); o += 20
    nreq = struct.unpack_from('<H', b, o)[0]; o += 2; reqs = []
    for _ in range(nreq):
        nm, o = _ws(b, o); rid = struct.unpack_from('<q', b, o)[0]; o += 8; rv = 0
        if ver != 1: rv = struct.unpack_from('<H', b, o)[0]; o += 2
        reqs.append({'name': nm, 'id': rid, 'ver': rv})
    info['requires'] = reqs; dels = []
    if ver >= 3:
        nd = struct.unpack_from('<H', b, o)[0]; o += 2
        for _ in range(nd): s, o = _ws(b, o); dels.append(s)
    info['deletes'] = dels
    pak = memoryview(b)[off_data:off_man]
    root, ents = man_read(b[off_man:])
    return info, pak, root, ents

# ---------------------------------------------------------------- XTX (Switch textures)

FMT = {0x42: ('BC1', 8, 4), 0x43: ('BC2', 16, 4), 0x44: ('BC3', 16, 4), 0x25: ('RGBA8', 4, 1)}   # NVN format -> name, bytes/block, block size
# Type-4 block = the Ogre::PixelFormat the texture reports (the XTX reader main+0x19973f0 returns it, the caller main+0x1997794 maps
# 11 B8G8R8 / 13 A8B8G8R8 to 14 B8G8R8A8): DXT1 17, DXT3 19, DXT5 21; RGBA8 data carries the source format (10 R8G8B8, 11 B8G8R8,
# 12 A8R8G8B8, 13 A8B8G8R8 in the game's packs). New RGBA8 textures: 12 with alpha, 11 opaque (as the game's own PNG / JPG).
OGRE_PF = {0x42: 17, 0x43: 19, 0x44: 21}

def _cdiv(a, b): return (a + b - 1) // b

def _bh0(rows):   # GOB block height of level 0 (GOBs = 8 rows), NVN default: next power of two of the GOB rows, at most 16
    g = _cdiv(rows, 8); bh = 1
    while bh < g and bh < 16: bh *= 2
    return bh

def xtx_levels(w, h, fmt, mips):
    """-> [(offset, size, width_blocks, height_blocks, block_height)], total; block height shrinks for small levels as on Tegra"""
    _, bpb, bs = FMT[fmt]; bh0 = _bh0(_cdiv(h, bs)); out = []; off = 0
    for i in range(mips):
        lw, lh = max(1, w >> i), max(1, h >> i); wb, hb = _cdiv(lw, bs), _cdiv(lh, bs)
        bh = bh0
        while bh > 1 and hb <= (bh // 2) * 8: bh //= 2
        size = _cdiv(wb * bpb, 64) * 64 * _cdiv(hb, 8 * bh) * 8 * bh
        out.append((off, size, wb, hb, bh)); off += size
    return out, off

def _gob_index(wb, hb, bpb, bh):
    """byte offset in the block-linear image of every linear byte (x bytes, y rows), as a numpy array of shape (hb, wb*bpb)"""
    import numpy as np
    pitch = _cdiv(wb * bpb, 64)                      # GOBs per row
    x = np.arange(wb * bpb, dtype=np.int64)[None, :]
    y = np.arange(hb, dtype=np.int64)[:, None]
    gob = ((x % 64) // 32) * 256 + ((y % 8) // 2) * 64 + ((x % 32) // 16) * 32 + (y % 2) * 16 + (x % 16)
    return (y // (8 * bh)) * (pitch * bh * 512) + (x // 64) * (bh * 512) + ((y % (8 * bh)) // 8) * 512 + gob

def xtx_swizzle(levels_linear, w, h, fmt):
    import numpy as np
    lv, total = xtx_levels(w, h, fmt, len(levels_linear)); _, bpb, _ = FMT[fmt]
    out = np.zeros(total, np.uint8)
    for (off, size, wb, hb, bh), lin in zip(lv, levels_linear):
        a = np.frombuffer(lin, np.uint8)[:wb * bpb * hb].reshape(hb, wb * bpb)
        out[off + _gob_index(wb, hb, bpb, bh)] = a
    return out.tobytes(), lv

def xtx_deswizzle(data, w, h, fmt, mips):
    import numpy as np
    lv, _ = xtx_levels(w, h, fmt, mips); _, bpb, _ = FMT[fmt]; src = np.frombuffer(data, np.uint8); res = []
    for off, size, wb, hb, bh in lv: res.append(src[off + _gob_index(wb, hb, bpb, bh)].tobytes())
    return res

def _blk(typ, gid, payload):
    return b'HBvN' + struct.pack('<IQQIII', 0x24, len(payload), 0x24, typ, gid, 0) + payload

def xtx_build(levels_linear, w, h, fmt, pf=None):
    if pf is None:
        if fmt in OGRE_PF: pf = OGRE_PF[fmt]
        else:
            import numpy as np
            pf = 12 if (np.frombuffer(levels_linear[0], np.uint8)[3::4] < 255).any() else 11
    img, lv = xtx_swizzle(levels_linear, w, h, fmt)
    mo = [o for o, *_ in lv] + [0] * (17 - len(lv))
    info = struct.pack('<QIIIIIIII', len(img), 0x200, w, h, 1, 1, fmt, len(lv), len(img)) + struct.pack('<17I', *mo) + \
        struct.pack('<III', lv[0][4].bit_length() - 1, 0x10007, 0)   # log2 GOB block height of level 0, then two constants
    tail = struct.pack('<QIIII', 0, 1, 1, 1, 0)
    return b'DFvN' + struct.pack('<III', 0x10, 1, 1) + _blk(2, 0, info) + _blk(3, 1, img) + _blk(4, 2, struct.pack('<I', pf)) + _blk(5, 3, tail)

def xtx_parse(d):
    if d[:4] != b'DFvN': return None
    o = struct.unpack_from('<I', d, 4)[0]; r = {}
    while o + 0x24 <= len(d) and d[o:o + 4] == b'HBvN':
        _, dsz, doff, typ, _, _ = struct.unpack_from('<IQQIII', d, o + 4); p = d[o + doff:o + doff + dsz]
        if typ == 2:
            dsz2, al, w, h, dep, tgt, fmt, mips, ss = struct.unpack_from('<QIIIIIIII', p, 0)
            r.update(w=w, h=h, depth=dep, target=tgt, fmt=fmt, mips=mips, size=dsz2, mipoff=list(struct.unpack_from('<17I', p, 0x28))[:mips],
                     layout=struct.unpack_from('<III', p, 0x6c), info=bytes(p))
        elif typ == 3: r['data'] = bytes(p)
        elif typ == 4: r['pf'] = struct.unpack_from('<I', p)[0]
        elif typ == 5: r['tail'] = bytes(p)
        o += doff + dsz
    return r

# ---------------------------------------------------------------- DDS / images -> XTX

def dds_to_xtx(b):
    if b[:4] != b'DDS ': raise ValueError('not DDS')
    _, flags, h, w, _, _, mips = struct.unpack_from('<7I', b, 4)
    pf_flags, fourcc, bits, rm, gm, bm, am = struct.unpack_from('<II5I', b, 80)
    caps2 = struct.unpack_from('<I', b, 112)[0]
    if caps2 & 0x200 or flags & 0x800000: raise ValueError('cube / volume DDS not supported')
    mips = max(1, mips if flags & 0x20000 else 1)
    data = b[128:]
    if pf_flags & 4:
        fmt = {b'DXT1': 0x42, b'DXT2': 0x43, b'DXT3': 0x43, b'DXT4': 0x44, b'DXT5': 0x44}.get(struct.pack('<I', fourcc))
        if fmt is None: raise ValueError(f'DDS fourcc {struct.pack("<I", fourcc)!r} not supported')
    elif pf_flags & 0x40 and bits in (24, 32):   # uncompressed RGB(A) with byte-aligned 8-bit masks -> RGBA8 (every level)
        fmt = 0x25; bpp = bits // 8
        byte = {0xff: 0, 0xff00: 1, 0xff0000: 2, 0xff000000: 3}
        if any(m not in byte or byte[m] >= bpp for m in (rm, gm, bm)) or (pf_flags & 1 and am and (am not in byte or byte[am] >= bpp)):
            raise ValueError(f'DDS masks {rm:#x} {gm:#x} {bm:#x} {am:#x} not supported')
        import numpy as np
        a = np.frombuffer(data, np.uint8)[:len(data) // bpp * bpp].reshape(-1, bpp)
        alpha = a[:, [byte[am]]] if pf_flags & 1 and am else np.full((len(a), 1), 255, np.uint8)
        data = np.concatenate([a[:, [byte[rm], byte[gm], byte[bm]]], alpha], 1).tobytes()
    else: raise ValueError(f'DDS pixel format flags {pf_flags:#x} / {bits} bits not supported')
    _, bpb, bs = FMT[fmt]; lv = []; o = 0
    for i in range(mips):
        n = _cdiv(max(1, w >> i), bs) * _cdiv(max(1, h >> i), bs) * bpb
        if o + n > len(data): break
        lv.append(data[o:o + n]); o += n
    return xtx_build(lv, w, h, fmt)

PNG_MAGIC, JPG_MAGIC = bytes.fromhex("89504e470d0a1a0a"), bytes.fromhex("ffd8ff")

def tga_decode(b):
    """-> (w, h, RGBA bytes top row first). Types 1/2/3 and RLE 9/10/11; 8 (grey / palette index), 15/16, 24, 32 bits; palettes of
    15/16/24/32-bit entries. Own decoder (not Pillow) so the device converter (Tl2ModConv.java) can match it byte for byte."""
    idl, cmt, it, c0, clen, csz, _, _, w, h, bpp, desc = struct.unpack_from('<BBBHHBHHHHBB', b, 0)
    base = it & 7
    if base not in (1, 2, 3) or not w or not h: raise ValueError(f'TGA type {it} not supported')
    o = 18 + idl; pal = None
    def px(c, sz):   # one stored pixel -> (r, g, b, a)
        if sz in (15, 16): v = c[0] | c[1] << 8; r, g, bl = (v >> 10) & 31, (v >> 5) & 31, v & 31; e = lambda x: x << 3 | x >> 2; return e(r), e(g), e(bl), (255 if sz == 15 or not desc & 15 or v & 0x8000 else 0)
        if sz == 24: return c[2], c[1], c[0], 255
        if sz == 32: return c[2], c[1], c[0], c[3]
        raise ValueError(f'TGA pixel size {sz} not supported')
    if cmt == 1:
        es = (csz + 7) // 8; pal = [px(b[o + i * es:o + i * es + es], csz) for i in range(clen)]; o += clen * es
    ps = (bpp + 7) // 8; n = w * h; raw = bytearray()
    if it & 8:
        while len(raw) < n * ps:
            hd = b[o]; o += 1; k = (hd & 0x7f) + 1
            if hd & 0x80: raw += b[o:o + ps] * k; o += ps
            else: raw += b[o:o + k * ps]; o += k * ps
    else: raw = b[o:o + n * ps]
    if len(raw) < n * ps: raise ValueError('TGA data truncated')
    out = bytearray(n * 4)
    for i in range(n):
        c = raw[i * ps:i * ps + ps]
        if base == 1: k = (c[0] | (c[1] << 8 if ps > 1 else 0)) - c0; q = pal[k] if pal and 0 <= k < len(pal) else (0, 0, 0, 255)
        elif base == 3: q = (c[0], c[0], c[0], c[1] if ps > 1 else 255)
        else: q = px(c, 15 if bpp == 15 else bpp)
        out[i * 4:i * 4 + 4] = bytes(q)
    rows = [out[y * w * 4:(y + 1) * w * 4] for y in range(h)]
    if desc & 0x10: rows = [bytes(b''.join(r[x * 4:x * 4 + 4] for x in range(w - 1, -1, -1))) for r in rows]
    if not desc & 0x20: rows.reverse()
    return w, h, bytes(b''.join(rows))

def image_to_xtx(b, name=''):   # PNG / JPG / TGA: one level RGBA8, as every image of this kind in the game's packs
    if name.upper().endswith('.TGA') and b[:8] != PNG_MAGIC and b[:3] != JPG_MAGIC:
        w, h, px = tga_decode(b); return xtx_build([px], w, h, 0x25)
    from io import BytesIO
    from PIL import Image
    im = Image.open(BytesIO(b)).convert('RGBA')
    return xtx_build([im.tobytes()], im.size[0], im.size[1], 0x25)

TEX_EXT = ('.DDS', '.PNG', '.JPG', '.TGA')

def to_switch_texture(name, d):
    if d[:4] == b'DFvN': return d, 'xtx'
    if d[:4] == b'DDS ': return dds_to_xtx(d), 'dds'
    return image_to_xtx(d, name), 'image'

# ---------------------------------------------------------------- convert

def _slug(s): return re.sub(r'[^A-Z0-9]+', '_', s.upper()).strip('_')[:40] or 'MOD'

def convert(mod_path, out_dir, slot=None, tex=True, log=print):
    info, pak, root, ents = mod_read(mod_path)
    pdir = os.path.join(out_dir, 'PAKS'); os.makedirs(pdir, exist_ok=True)
    if slot is None:
        used = [int(m.group(1)) for f in os.listdir(pdir) for m in [re.match(r'ZMOD_(\d\d)_', f)] if m]
        slot = max(used, default=-1) + 1
    base = f'ZMOD_{slot:02d}_{_slug(info["title"] or os.path.splitext(os.path.basename(mod_path))[0])}'
    body = bytearray(8); out = []; stats = {'files': 0, 'textures': {}, 'failed': [], 'deleted': 0}; largest = 0
    for d, n, typ, off, size, crc, ft in ents:
        if typ == 7 or (off == 0 and size == 0):
            out.append((d, n, typ, 0, 0, crc if typ == 7 else 0, ft)); continue
        dl, cl = struct.unpack_from('<II', pak, off)
        raw = bytes(pak[off + 8:off + 8 + (cl or dl)]); full = None
        if tex and n.upper().endswith(TEX_EXT):
            full = zlib.decompress(raw) if cl and cl != dl else raw
            try:
                conv, kind = to_switch_texture(n, full)
                stats['textures'][kind] = stats['textures'].get(kind, 0) + 1
                if kind != 'xtx': full = conv
                else: full = None
            except Exception as e:   # keep the PC bytes: the game shows the texture as missing, the rest of the mod still loads
                stats['failed'].append(f'{d}{n}: {e}'); full = None
        if full is not None:   # converted textures are stored (packed size = size, as the game's uncompressed entries): no deflate, so
            raw = full; dl = cl = len(full); crc = zlib.crc32(full)   # this tool and the device converter (Tl2ModConv.java) write the same bytes
        out.append((d, n, typ, len(body), dl, crc, ft))
        body += struct.pack('<II', dl, cl) + raw; largest = max(largest, cl or dl); stats['files'] += 1
    for p in info['deletes']:   # deleted files: a zero entry, as the game's own patch packs do (DATA3 deletes a DATA file this way)
        p = p.replace('\\', '/').upper(); d, _, n = p.rpartition('/')
        out.append((d + '/', n, 0, 0, 0, 0, 0)); stats['deleted'] += 1
    struct.pack_into('<I', body, 0, largest); struct.pack_into('<I', body, 4, pak_hash(body))
    open(os.path.join(pdir, base + '.PAK'), 'wb').write(body)
    # root as in every Switch pack ('MEDIA/'; a mod's own root is its folder name, the directory records carry the full paths)
    open(os.path.join(pdir, base + '.PAK.MAN'), 'wb').write(man_write('MEDIA/', out))
    meta = dict(info, source=os.path.basename(mod_path), pack=base, **stats)
    os.makedirs(os.path.join(out_dir, 'meta'), exist_ok=True)   # outside PAKS: the game lists PAKS
    json.dump(meta, open(os.path.join(out_dir, 'meta', base + '.json'), 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
    log(f'{base}: {stats["files"]} files, textures {stats["textures"]}, deleted {stats["deleted"]}, failed {len(stats["failed"])}')
    for f in stats['failed'][:20]: log('  texture kept as PC bytes:', f)
    return meta

# ---------------------------------------------------------------- selftest against the game's own data

def selftest(romfs, per_kind=40):
    import collections
    paks = os.path.join(romfs, 'PAKS'); ok = bad = 0; seen = collections.Counter(); tails = set()
    for p in sorted(os.listdir(paks)):
        if not p.endswith('.PAK'): continue
        b = open(os.path.join(paks, p), 'rb').read()
        h = pak_hash(b); print(f'{p}: header hash {struct.unpack_from("<I", b, 4)[0]:#x} computed {h:#x}', 'ok' if h == struct.unpack_from('<I', b, 4)[0] else 'MISMATCH')
        m = open(os.path.join(paks, p + '.MAN'), 'rb').read(); root, ents = man_read(m)
        if man_read(man_write(root, ents)) != (root, ents): print(p, 'MAN rewrite MISMATCH'); bad += 1
        for d, n, typ, off, size, crc, ft in ents:
            if not n.upper().endswith(TEX_EXT) or not size: continue
            x = xtx_parse(pak_get(b, off))
            if not x or x['target'] != 1 or x['fmt'] not in FMT: continue
            k = (x['fmt'], x['mips'] > 1, min(x['w'], x['h']) < 64)
            if seen[k] >= per_kind: continue
            seen[k] += 1; tails.add(x['tail'])
            lin = xtx_deswizzle(x['data'], x['w'], x['h'], x['fmt'], x['mips'])
            rebuilt = xtx_parse(xtx_build(lin, x['w'], x['h'], x['fmt'], x['pf'] if x['fmt'] == 0x25 else None))   # RGBA8: source format is metadata
            same = rebuilt['data'] == x['data'] and rebuilt['info'] == x['info'] and rebuilt['pf'] == x['pf'] and rebuilt['tail'] == x['tail']
            if same: ok += 1
            else:
                bad += 1
                if bad < 12: print('  XTX MISMATCH', d + n, x['w'], x['h'], hex(x['fmt']), x['mips'], 'info', rebuilt['info'] == x['info'],
                                   'data', rebuilt['data'] == x['data'], 'pf', x['pf'], 'layout', x['layout'], x['mipoff'][:4])
    print(f'XTX rebuild: {ok} identical, {bad} different; kinds {dict(seen)}; type-5 variants {len(tails)}')
    return bad == 0

def main(a):
    if len(a) >= 2 and a[0] == 'info':
        info, pak, root, ents = mod_read(a[1]); import collections
        kinds = collections.Counter()
        for d, n, typ, off, size, *_ in ents:
            if typ != 7 and size and n.upper().endswith(TEX_EXT): kinds[pak_get(pak, off)[:4]] += 1
        print(json.dumps(info, ensure_ascii=False, indent=1)); print('files', sum(e[2] != 7 for e in ents), 'textures by magic', dict(kinds))
    elif len(a) >= 3 and a[0] == 'convert':
        slot = int(a[a.index('--slot') + 1]) if '--slot' in a else None
        convert(a[1], a[2], slot, '--no-tex' not in a)
    elif len(a) >= 3 and a[0] == 'unpack':
        src = open(a[1], 'rb').read()
        if a[1].upper().endswith('.MOD'): _, pak, root, ents = mod_read(a[1])
        else: pak = src; root, ents = man_read(open(a[1] + '.MAN', 'rb').read())
        for d, n, typ, off, size, *_ in ents:
            if typ == 7 or not size: continue
            t = os.path.join(a[2], (d + n).replace('\\', '/')); os.makedirs(os.path.dirname(t), exist_ok=True)
            open(t, 'wb').write(pak_get(pak, off))
    elif len(a) >= 2 and a[0] == 'selftest':
        sys.exit(0 if selftest(a[1]) else 1)
    else:
        print(__doc__); sys.exit(2)

if __name__ == '__main__':
    main(sys.argv[1:])
