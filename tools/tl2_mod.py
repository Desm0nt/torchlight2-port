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
  Skill trees = the console never reads a class's PC skill tree layout; its trees become MEDIA/PORT/SKILLTREES.TXT in the pack
              (skilltrees_text below; read by dm/host/hle_tl2_skilltree.c).

Device port: dm/android/.../Tl2ModConv.java (the app's mod manager, Tl2Mods.java) writes the same bytes; tools/tl2_mod_java_test.py checks.

Usage:
  tl2_mod.py info    <mod>                         header, file counts, texture kinds
  tl2_mod.py convert <mod> <out_overlay> [--slot N] [--no-tex]   write <out_overlay>/PAKS/ZMOD_<NN>_<name>.PAK(.MAN) + .json
  tl2_mod.py unpack  <mod|pak> <dest>              extract files (as stored, decompressed)
  tl2_mod.py skilltrees <mod>                      the console skill-tree table the converter stores (MEDIA/PORT/SKILLTREES.TXT)
  tl2_mod.py rawsim  <romfs> [mod ...] [--out dir] [-v]   the game's own merge of MEDIA/*.RAW over the base packs + mods in load
                                                   order: per pack entries kept / shadowed (dup) / dropped (no DAT); --out
                                                   writes the merged RAWs (inspection only); --selftest: RAW round trip
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

# ---------------------------------------------------------------- skill trees of PC-mod classes (issue #6)
# The console skill screen has no layout of its own: CConsoleUIRadialMenu::Create (main+0x134ec0) picks one of four static rings by a
# substring of the player unit's NAME (HUM_BERSERKER / HUM_ARBITER / HUM_ENGINEER / HUM_WANDERER, 0x136740..0x1369ec), each ring = 3
# segments (tree ids 0x65..0x70), and PanelSkillList (vt main+0x23031d0) fills a tree from a hard-coded table of 120 skill names
# (main+0x23022c0, 10 per tree: 7 active + 3 passive). The PC skill tree layout (UISKILLTREE of the class DAT) is never read. So the
# converter turns each class's PC layout into a small text table, stored in the pack as MEDIA/PORT/SKILLTREES.TXT (a path the game never
# opens), and dm/host/hle_tl2_skilltree.c puts it into the console ring / table while the radial menu is built. Format (UTF-8, tabs):
#   TL2SKILLTREES 2 | L i <layout path> | T i <tree 0..11> <label> <icon key> | S i <tree> <slot 0..9> <skill name> | X i <note> |
#   C <unit NAME> i      (upper-case ASCII where noted; Tl2ModConv.java writes the same bytes)
ST_PATH = ('MEDIA/PORT/', 'SKILLTREES.TXT')
ST_TREES = 12   # tree slots: the console's skill table has 120 rows = 12 x 10 (borrowed per class by hle_tl2_skilltree.c)
ST_STOCK = tuple((f'HUM_{c}_BASE', f'MEDIA/UI/MENUS/SKILLTREEOVERRIDES/SKILL_TREE_{t}.LAYOUT') for c, t in
                 (('BERSERKER', 'BERSERKER'), ('ARBITER', 'ARBITER'), ('ENGINEER', 'RAILMAN'), ('WANDERER', 'OUTLANDER')))   # the game's UISKILLTREE

def tl2_hash(s):   # the key hash of TL2 binary DATs (UNITTYPE 0x176b64fe, NAME 0x660de5, UISKILLTREE 0xd83631af, ...)
    h = len(s)
    for c in s: h = ((h >> 27) ^ (h << 5) ^ ord(c)) & 0xffffffff
    return h
K_NAME, K_BASEFILE, K_UISKILLTREE = tl2_hash('NAME'), tl2_hash('BASEFILE'), tl2_hash('UISKILLTREE')

def _up(s): return ''.join(chr(ord(c) - 32) if 'a' <= c <= 'z' else c for c in s)   # ASCII only (same in Java)
def _lo(s): return ''.join(chr(ord(c) + 32) if 'A' <= c <= 'Z' else c for c in s)
def _clean(s): return ''.join(' ' if c in '\t\r\n' else c for c in s).strip(' ')   # ASCII spaces only (same in Java)

def dat_top(b):
    """TL2 binary DAT v2 -> {key hash: value} of the root block (strings resolved), or None"""
    try:
        ver, n = struct.unpack_from('<II', b, 0); o = 8
        if ver != 2: return None
        st = {}
        for _ in range(n):
            h, ln = struct.unpack_from('<IH', b, o); o += 6
            if o + 2 * ln > len(b): return None
            st[h] = b[o:o + 2 * ln].decode('utf-16le', 'replace'); o += 2 * ln
        _, np_ = struct.unpack_from('<II', b, o); o += 8
        out = {}
        for _ in range(np_):
            k, t = struct.unpack_from('<II', b, o); o += 8
            if t in (5, 8): out[k] = st.get(struct.unpack_from('<I', b, o)[0], ''); o += 4
            elif t in (1, 2, 4, 6): o += 4
            elif t in (3, 7): o += 8
            else: return None
        return out
    except (struct.error, IndexError):
        return None

def layout_objects(b):
    """TL2 binary LAYOUT (version 0x0b) -> flat list of (type, name, props {id: bytes}, parent index)"""
    if len(b) < 8 or b[0] != 0x0b: return None
    objs = []
    def ws(o):
        n = struct.unpack_from('<H', b, o)[0]
        if o + 2 + 2 * n > len(b): raise ValueError('string past the end')
        return b[o + 2:o + 2 + 2 * n].decode('utf-16le', 'replace'), o + 2 + 2 * n
    def obj(o, parent):
        st = o; size, typ = struct.unpack_from('<IB', b, o); o += 13   # u32 size, u8 type, u64 guid
        name, o = ws(o); np_ = b[o]; o += 1; props = {}
        for _ in range(np_):
            sz, pid = struct.unpack_from('<HB', b, o)
            if sz < 1 or o + 2 + sz > len(b): raise ValueError('bad property')
            props[pid] = bytes(b[o + 3:o + 2 + sz]); o += 2 + sz
        o += 4 + struct.unpack_from('<I', b, o)[0]
        me = len(objs); objs.append((typ, name, props, parent))
        nc = struct.unpack_from('<H', b, o)[0]; o += 2
        for _ in range(nc): o = obj(o, me)
        if o != st + size: raise ValueError('object size mismatch')
        return o
    try:
        o = 8
        for _ in range(struct.unpack_from('<H', b, 6)[0]): o = obj(o, -1)
    except (struct.error, IndexError, ValueError):
        return None
    return objs

def _pws(v):
    if len(v) < 2: return None
    n = struct.unpack_from('<H', v, 0)[0]
    return v[2:2 + 2 * n].decode('utf-16le', 'replace') if 2 + 2 * n == len(v) else None
def _pf(v): return struct.unpack('<f', v)[0] if v is not None and len(v) == 4 else 0.0

def layout_trees(b):
    """-> ([(tree, label, icon key)], [(tree, slot, skill)], [notes]) or None. Trees = TabN / SkillPaneN in N order (tree 0, 1, ...). A
    skill is a window (type 0x3d) with a skill name (prop 0x40) and prop 0x7b inside SkillPaneN; order = rows (y, prop 0x22) then x
    (0x3f), as the console table orders the stock trees; the lowest row (<= 3 skills) is the passive row -> the last slots of 7..9. A tree
    holds 10 (the console panel's slots): actives past the 7th go to overflow trees after the layout's own ("<tree> II" when they come
    from one tree, else "More Skills"), 10 each; at most ST_TREES trees in all (the console table's 120 rows / 10)."""
    objs = layout_objects(b)
    if objs is None: return None
    def num(name, pre):
        u = _up(name); r = u[len(pre):]
        return int(r) if u.startswith(pre) and 1 <= len(r) <= 3 and all('0' <= c <= '9' for c in r) else 0
    tabs, panes, notes = {}, {}, []
    for i, (typ, name, props, parent) in enumerate(objs):
        n = num(name, 'TAB')
        if typ == 0x58 and n and n not in tabs:
            icon = _pws(props.get(0x41, b'')) or ''
            m = re.match(r'IG_SKILLTABS_(.+?)_(OFF|ON|ROLLOVER)$', _up(icon))
            tabs[n] = (_clean(_pws(props.get(0x23, b'')) or ''), _lo(m.group(1)) if m else '-')
        if typ == 0x3d and 0x40 in props and 0x7b in props:
            sk = _pws(props[0x40])
            p, pane, inside = parent, 0, False
            while p >= 0:
                if objs[p][0] == 0x3d and 0x40 in objs[p][2] and 0x7b in objs[p][2]: inside = True; break
                pane = num(objs[p][1], 'SKILLPANE')
                if pane: break
                p = objs[p][3]
            if sk and pane and not inside:
                panes.setdefault(pane, []).append((_pf(props.get(0x22)), _pf(props.get(0x3f)), len(panes.get(pane, [])), _clean(sk)))
    trees, skills, over = [], [], []
    for n in sorted(set(tabs) | set(panes)):
        if len(trees) >= ST_TREES:
            notes.append(f'tree {n} ({tabs.get(n, ("", ""))[0] or "no tab"}, {len(panes.get(n, []))} skills) dropped: no free tree slot'); continue
        t = len(trees); label, icon = tabs.get(n, ('', '-')); label = label or f'Skills {n}'; icon = icon or '-'
        trees.append((t, label, icon))
        rows = sorted(panes.get(n, []))
        if not rows: continue
        ymax = rows[-1][0]
        pas = [r for r in rows if r[0] == ymax] if any(r[0] != ymax for r in rows) else []
        if len(pas) > 3: pas = []
        act = [r for r in rows if r not in pas]
        if len(act) + len(pas) > 10:
            over += [(t, label, icon, r[3]) for r in act[7:]]; act = act[:7]
        for s, r in enumerate(act): skills.append((t, s, r[3]))
        for s, r in enumerate(pas): skills.append((t, 10 - len(pas) + s, r[3]))
    while over:
        if len(trees) >= ST_TREES:
            notes += [f'{o[3]} (tree {o[0]}) dropped: no free tree slot' for o in over]; break
        chunk, over = over[:10], over[10:]; t = len(trees)
        one = all(o[0] == chunk[0][0] for o in chunk)
        trees.append((t, chunk[0][1] + ' II' if one else 'More Skills', chunk[0][2] if one else '-'))
        skills += [(t, s, o[3]) for s, o in enumerate(chunk)]
    return trees, skills, notes

def skilltrees_text(files):
    """files: {UPPER path: bytes} of the mod (decompressed) -> MEDIA/PORT/SKILLTREES.TXT bytes, or None (no class with a tree)"""
    dats = {}
    for p, d in files.items():
        if p.startswith('MEDIA/UNITS/PLAYERS/') and p.endswith('.DAT'):
            t = dat_top(d)
            if t is not None: dats[p] = t
    def norm(s): return _up(s.replace('\\', '/')).lstrip('/')
    units, lays = [], {}
    for p in sorted(dats):
        t, name, tree, seen = dats[p], dats[p].get(K_NAME, ''), '', 0
        while t is not None and seen < 8:
            if t.get(K_UISKILLTREE): tree = norm(t[K_UISKILLTREE]); break
            t = dats.get(norm(t.get(K_BASEFILE, ''))); seen += 1
        if name and tree and tree in files:
            if tree not in lays: lays[tree] = layout_trees(files[tree])
            if lays[tree] and lays[tree][1]: units.append((_up(_clean(name)), tree))
    # a mod that replaces a stock class's layout but not its DAT (TL2 ACE: SKILL_TREE_RAILMAN): the stock base unit uses that layout
    mine = {_up(_clean(t.get(K_NAME, ''))) for t in dats.values()}
    for name, tree in ST_STOCK:
        if name not in mine and tree in files:
            if tree not in lays: lays[tree] = layout_trees(files[tree])
            if lays[tree] and lays[tree][1]: units.append((name, tree))
    used = sorted({t for _, t in units})
    if not used: return None
    out = ['TL2SKILLTREES\t2']
    for i, t in enumerate(used):
        trees, skills, notes = lays[t]
        out.append(f'L\t{i}\t{t}')
        out += [f'T\t{i}\t{n}\t{lb}\t{ic}' for n, lb, ic in trees]
        out += [f'S\t{i}\t{n}\t{s}\t{sk}' for n, s, sk in skills]
        out += [f'X\t{i}\t{x}' for x in notes]
    done = set()
    for name, t in sorted(units):
        if name not in done: done.add(name); out.append(f'C\t{name}\t{used.index(t)}')
    return ('\n'.join(out) + '\n').encode('utf-8')

# ---------------------------------------------------------------- skill icons of PC mods -> a console UI atlas
# The console UI draws a skill's icon (DAT SKILL_ICON / SKILL_ICON_INACTIVE, e.g. "skillicon_eviscerate") from its texture atlases:
# at boot main+0x141040 lists every file of type 26 (.ATLAS) under MEDIA/UICONSOLE/ATLASES/ in all packs (0x141e90) and loads each
# (0x1a00360: XML <atlas texture="X.DDS"><entries><entry name x y w h/>, the texture from the same folder). PC mods ship their icons as
# CEGUI imagesets (<Imageset Imagefile="media/ui/icons/skills/x.dds"><Image Name XPos YPos Width Height/>), which the console never
# reads. So every icon a mod skill names and a mod imageset defines is copied into extra atlas pages ZMODICONS_<mod id>_<k>.ATLAS / .DDS
# (RGBA8 XTX, shelf-packed, padding 2 like the game's) in the mod's pack. Tl2ModConv.java writes the same bytes.
K_ICONS = (tl2_hash('SKILL_ICON'), tl2_hash('SKILL_ICON_INACTIVE'))
ATLAS_DIR, ATLAS_MAX = 'MEDIA/UICONSOLE/ATLASES/', 2048

def _text(b):   # imageset bytes -> str (UTF-8 / UTF-16 with BOM)
    if b[:2] == b'\xff\xfe': return b[2:].decode('utf-16le', 'replace')
    if b[:3] == b'\xef\xbb\xbf': b = b[3:]
    return b.decode('utf-8', 'replace')

def imageset_parse(b):
    """-> (Imagefile, [(name, x, y, w, h)]); attributes in any order, case-insensitive; bad images skipped"""
    t = _text(b); out = []
    m = re.search(r'<Imageset\b([^>]*)>', t, re.I)
    if not m: return None
    head = {_up(k): v for k, v in re.findall(r'([A-Za-z0-9_]+)\s*=\s*"([^"]*)"', m.group(1))}
    for im in re.finditer(r'<Image\b([^>]*)>', t, re.I):
        a = {_up(k): v for k, v in re.findall(r'([A-Za-z0-9_]+)\s*=\s*"([^"]*)"', im.group(1))}
        try:
            v = [a['XPOS'], a['YPOS'], a['WIDTH'], a['HEIGHT']]
            if not all(re.fullmatch(r'-?[0-9]{1,6}', s) for s in v) or not a.get('NAME'): continue
            x, y, w, h = map(int, v)
        except KeyError: continue
        if 1 <= w <= 512 and 1 <= h <= 512: out.append((a['NAME'], x, y, w, h))
    return head.get('IMAGEFILE', ''), out

def _bc_colors(b, o, four):   # 4 RGBA colours of a BC1-style colour block (integer rules, the same in Java)
    c0, c1 = struct.unpack_from('<HH', b, o)
    def rgb(c): r, g, bl = c >> 11, (c >> 5) & 63, c & 31; return [(r << 3) | (r >> 2), (g << 2) | (g >> 4), (bl << 3) | (bl >> 2), 255]
    p0, p1 = rgb(c0), rgb(c1)
    if four or c0 > c1: return [p0, p1, [(2 * p0[i] + p1[i]) // 3 for i in range(3)] + [255], [(p0[i] + 2 * p1[i]) // 3 for i in range(3)] + [255]]
    return [p0, p1, [(p0[i] + p1[i]) // 2 for i in range(3)] + [255], [0, 0, 0, 0]]

def bc_block(b, o, fmt):   # -> 16 RGBA lists (row-major) of the 4x4 block at offset o; fmt 0x42 BC1, 0x43 BC2, 0x44 BC3
    co = o + (0 if fmt == 0x42 else 8); cols = _bc_colors(b, co, fmt != 0x42)
    idx = struct.unpack_from('<I', b, co + 4)[0]
    px = [list(cols[(idx >> (2 * i)) & 3]) for i in range(16)]
    if fmt == 0x43:
        a = struct.unpack_from('<Q', b, o)[0]
        for i in range(16): px[i][3] = ((a >> (4 * i)) & 15) * 17
    elif fmt == 0x44:
        a0, a1 = b[o], b[o + 1]; bits = int.from_bytes(b[o + 2:o + 8], 'little')
        if a0 > a1: al = [a0, a1] + [((7 - k) * a0 + k * a1) // 7 for k in range(1, 7)]
        else: al = [a0, a1] + [((5 - k) * a0 + k * a1) // 5 for k in range(1, 5)] + [0, 255]
        for i in range(16): px[i][3] = al[(bits >> (3 * i)) & 7]
    return px

class _Tex:
    """level 0 of a PC texture, read pixel by pixel: DDS (DXT1/3/5, 24/32-bit with byte masks) or PNG / JPG / TGA (RGBA rows)"""
    def __init__(s, name, d):
        s.rgba = None; s.bc = None
        if d[:4] == b'DDS ':
            _, flags, s.h, s.w = struct.unpack_from('<4I', d, 4)
            pff, fourcc, bits, rm, gm, bm, am = struct.unpack_from('<II5I', d, 80); s.d = d
            if pff & 4:
                s.bc = {b'DXT1': 0x42, b'DXT2': 0x43, b'DXT3': 0x43, b'DXT4': 0x44, b'DXT5': 0x44}.get(struct.pack('<I', fourcc))
                if s.bc is None: raise ValueError('DDS fourcc not supported')
                s.cache = {}
            elif pff & 0x40 and bits in (24, 32):
                byte = {0xff: 0, 0xff00: 1, 0xff0000: 2, 0xff000000: 3}; s.bpp = bits // 8
                if any(m not in byte or byte[m] >= s.bpp for m in (rm, gm, bm)): raise ValueError('DDS masks not supported')
                s.idx = [byte[rm], byte[gm], byte[bm], byte[am] if pff & 1 and am in byte and byte[am] < s.bpp else -1]
            else: raise ValueError('DDS format not supported')
        elif name.upper().endswith('.TGA') and d[:8] != PNG_MAGIC and d[:3] != JPG_MAGIC:
            s.w, s.h, s.rgba = tga_decode(d)
        else:
            from io import BytesIO
            from PIL import Image
            im = Image.open(BytesIO(d)).convert('RGBA'); s.w, s.h = im.size; s.rgba = im.tobytes()
    def px(s, x, y):
        if not (0 <= x < s.w and 0 <= y < s.h): return (0, 0, 0, 0)
        if s.rgba is not None: o = 4 * (y * s.w + x); return tuple(s.rgba[o:o + 4])
        if s.bc is None:
            o = 128 + s.bpp * (y * s.w + x)
            if o + s.bpp > len(s.d): return (0, 0, 0, 0)
            i = s.idx; return (s.d[o + i[0]], s.d[o + i[1]], s.d[o + i[2]], s.d[o + i[3]] if i[3] >= 0 else 255)
        bx, by = x >> 2, y >> 2; k = (bx, by)
        if k not in s.cache:
            o = 128 + (by * ((s.w + 3) >> 2) + bx) * (8 if s.bc == 0x42 else 16)
            s.cache[k] = bc_block(s.d, o, s.bc) if o + (8 if s.bc == 0x42 else 16) <= len(s.d) else [[0, 0, 0, 0]] * 16
        return tuple(s.cache[k][(y & 3) * 4 + (x & 3)])

def icon_atlases(files, tex_get, modid):
    """files: {UPPER path: bytes} (skill DATs, imagesets); tex_get(UPPER path) -> texture bytes or None.
    -> ([(dir, name, MAN type, bytes)], number of icons, [notes])"""
    want = {}
    for p in sorted(files):
        if p.startswith('MEDIA/SKILLS/') and p.endswith('.DAT'):
            t = dat_top(files[p])
            for k in K_ICONS:
                v = (t or {}).get(k, '')
                if v: want.setdefault(_up(v), set()).add(v)
    defs = {}
    for p in sorted(files):
        if not p.endswith('.IMAGESET'): continue
        r = imageset_parse(files[p])
        if not r or not r[0]: continue
        for nm, x, y, w, h in r[1]:
            u = _up(nm)
            if u in want and u not in defs: defs[u] = (nm, _up(r[0].replace('\\', '/')).lstrip('/'), x, y, w, h)
    items = sorted(defs.values(), key=lambda e: (-e[5], _up(e[0])))
    if not items: return [], 0, []
    pages, cur, x, y, rowh, notes = [], [], 0, 0, 0, []
    for e in items:   # shelf packing, cell = image + 2 on every side
        cw, ch = e[4] + 4, e[5] + 4
        if x + cw > ATLAS_MAX: x, y, rowh = 0, y + rowh, 0
        if y + ch > ATLAS_MAX: pages.append(cur); cur, x, y, rowh = [], 0, 0, 0
        cur.append((e, x + 2, y + 2)); x += cw; rowh = max(rowh, ch)
    pages.append(cur)
    texs, out, n = {}, [], 0
    for k, pg in enumerate(pages):
        W = H = 64
        for e, px, py in pg: W, H = max(W, px + e[4] + 2), max(H, py + e[5] + 2)
        W, H = 1 << (W - 1).bit_length(), 1 << (H - 1).bit_length()
        img = bytearray(4 * W * H); ents = []
        for e, px, py in pg:
            nm, tp, sx, sy, w, h = e
            if tp not in texs:
                d = tex_get(tp)
                try: texs[tp] = _Tex(tp, d) if d is not None else None
                except Exception as ex: texs[tp] = None; notes.append(f'{tp}: {ex}')
            t = texs[tp]
            if t is None: notes.append(f'{nm}: texture {tp} not in the mod'); continue
            for yy in range(h):
                row = 4 * ((py + yy) * W + px)
                for xx in range(w): img[row + 4 * xx:row + 4 * xx + 4] = bytes(t.px(sx + xx, sy + yy))
            for sp in sorted({nm} | want[_up(nm)]): ents.append((sp, px, py, w, h))   # the imageset's spelling + every DAT spelling
            n += 1
        base = f'ZMODICONS_{modid & 0xffffffffffffffff:016X}_{k}'
        esc = lambda s: s.replace('&', '&amp;').replace('"', '&quot;').replace('<', '&lt;').replace('>', '&gt;')
        xml = '\ufeff<?xml version="1.0" encoding="utf-8"?>\r\n<atlas texture="' + base + '.DDS" padding="2">\r\n    <entries>\r\n' + ''.join(
            f'        <entry name="{esc(nm)}" x="{px}" y="{py}" w="{w}" h="{h}" />\r\n' for nm, px, py, w, h in ents) + '    </entries>\r\n</atlas>'
        out.append((ATLAS_DIR, base + '.ATLAS', 26, xml.encode('utf-8')))
        out.append((ATLAS_DIR, base + '.DDS', 4, xtx_build([bytes(img)], W, H, 0x25)))
    return out, n, notes

def convert(mod_path, out_dir, slot=None, tex=True, log=print):
    info, pak, root, ents = mod_read(mod_path)
    pdir = os.path.join(out_dir, 'PAKS'); os.makedirs(pdir, exist_ok=True)
    if slot is None:
        used = [int(m.group(1)) for f in os.listdir(pdir) for m in [re.match(r'ZMOD_(\d\d)_', f)] if m]
        slot = max(used, default=-1) + 1
    base = f'ZMOD_{slot:02d}_{_slug(info["title"] or os.path.splitext(os.path.basename(mod_path))[0])}'
    body = bytearray(8); out = []; stats = {'files': 0, 'textures': {}, 'failed': [], 'deleted': 0, 'skilltrees': 0, 'icons': 0}; largest = 0
    stf = {}   # player DATs + UI layouts for the console skill-tree table
    for d, n, typ, off, size, crc, ft in ents:
        if typ == 7 or (off == 0 and size == 0):
            out.append((d, n, typ, 0, 0, crc if typ == 7 else 0, ft)); continue
        dl, cl = struct.unpack_from('<II', pak, off)
        raw = bytes(pak[off + 8:off + 8 + (cl or dl)]); full = None
        up = _up(d + n)
        if (up.startswith('MEDIA/UNITS/PLAYERS/') and up.endswith('.DAT')) or (up.startswith('MEDIA/UI/') and up.endswith('.LAYOUT')) or                 (up.startswith('MEDIA/SKILLS/') and up.endswith('.DAT')) or up.endswith('.IMAGESET'):
            try: stf[up] = zlib.decompress(raw) if cl and cl != dl else raw
            except zlib.error: pass
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
    st = skilltrees_text(stf)
    if st is not None:   # stored (packed size = size), a directory record for MEDIA/PORT/ in MEDIA/, then the file
        stats['skilltrees'] = sum(l.startswith('C\t') for l in st.decode('utf-8').split('\n'))
        out.append(('MEDIA/', 'PORT/', 7, 0, 0, 0, 0)); out.append((ST_PATH[0], ST_PATH[1], 0, len(body), len(st), zlib.crc32(st), 0))
        body += struct.pack('<II', len(st), len(st)) + st; largest = max(largest, len(st))
    texoff = {_up(d + n): off for d, n, typ, off, size, *_ in ents if typ != 7 and size and n.upper().endswith(TEX_EXT)}
    ic, stats['icons'], inotes = icon_atlases(stf, lambda pth: pak_get(pak, texoff[pth]) if pth in texoff else None, info['modid'])
    if ic:   # directory records MEDIA/UICONSOLE/ and its ATLASES/ unless the mod has them, then the atlas pages (stored)
        have = {e[0] + e[1] for e in ents if e[2] == 7}
        for dd, nn in (('MEDIA/', 'UICONSOLE/'), ('MEDIA/UICONSOLE/', 'ATLASES/')):
            if dd + nn not in have: out.append((dd, nn, 7, 0, 0, 0, 0))
        for dd, nn, typ, data in ic:
            out.append((dd, nn, typ, len(body), len(data), zlib.crc32(data), 0))
            body += struct.pack('<II', len(data), len(data)) + data; largest = max(largest, len(data))
    stats['icon_notes'] = inotes
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
    log(f'{base}: {stats["files"]} files, textures {stats["textures"]}, deleted {stats["deleted"]}, failed {len(stats["failed"])}, '
        f'skill-tree classes {stats["skilltrees"]}, skill icons {stats["icons"]}')
    for x in inotes[:20]: log('  icon:', x)
    for f in stats['failed'][:20]: log('  texture kept as PC bytes:', f)
    return meta

# ---------------------------------------------------------------- RAW indexes (MEDIA/*.RAW)
# Compiled indexes GUTS writes for the DATs of a pack; every pack (DATA..DATA5 and each PC mod) carries only its OWN entries (DATA's
# UNITDATA.RAW 7623 units, DATA1..5 and mods a few dozen). Formats as Awkward-im/Torchlight TL2Lib/rgio.raw.pas (round trip checked on
# all base RAWs: `rawsim --selftest`). The Switch game reads every pack's copy itself: each loader (UNITDATA main+0x27d800, SKILLS
# 0x218190, MISSILES 0x36dcc0, ROOMPIECES 0x289300, AFFIXES 0x558880, TRIGGERABLES 0x41d0f0) asks the pack manager for the list of
# all copies of its path (main+0x163fb0) and parses each in turn; UNITDATA skips a GUID it already has (first copy wins, 0x27db6c) and a
# unit whose FILE no pack has (0x163350). UI.RAW is not read by the console game (no such string in main).
RAW_KINDS = ('UNITDATA', 'SKILLS', 'AFFIXES', 'MISSILES', 'ROOMPIECES', 'TRIGGERABLES')
UNIT_SECTIONS = ('ITEMS', 'MONSTERS', 'PLAYERS', 'PROPS')

class _R:
    def __init__(s, b): s.b, s.o = b, 0
    def u(s, f):
        v = struct.unpack_from(f, s.b, s.o); s.o += struct.calcsize(f); return v if len(v) > 1 else v[0]
    def w(s): v, s.o = _ws(s.b, s.o); return v

def raw_read(kind, b):
    """-> list of entries (dicts); UNITDATA entries carry 'section' (0 items, 1 monsters, 2 players, 3 props)"""
    r = _R(b); out = []
    if kind == 'UNITDATA':
        for sec in range(4):
            for _ in range(r.u('<I')):
                e = {'section': sec, 'guid': r.u('<q'), 'name': r.w(), 'file': r.w()}
                e['equip'], e['level'], e['minlevel'], e['maxlevel'], e['rarity'], e['rarityhc'] = r.u('<B5I'); e['unittype'] = r.w(); out.append(e)
    elif kind == 'SKILLS':
        for _ in range(r.u('<I')): out.append({'name': r.w(), 'file': r.w(), 'guid': r.u('<q')})
    elif kind == 'AFFIXES':
        for _ in range(r.u('<H')):
            e = {'file': r.w(), 'name': r.w()}; e['min'], e['max'], e['weight'], e['diff'] = r.u('<4I')
            e['types'] = [r.w() for _ in range(r.u('<B'))]; e['nottypes'] = [r.w() for _ in range(r.u('<B'))]; out.append(e)
    elif kind == 'MISSILES':
        for _ in range(r.u('<H')): out.append({'file': r.w(), 'names': [r.w() for _ in range(r.u('<B'))]})
    elif kind == 'ROOMPIECES':
        files = [r.w() for _ in range(r.u('<I'))]
        for f in files: out.append({'file': f, 'guids': [r.u('<q') for _ in range(r.u('<I'))]})
    elif kind == 'TRIGGERABLES':
        for _ in range(r.u('<H')): out.append({'file': r.w(), 'name': r.w()})
    else: raise ValueError(f'unknown RAW kind {kind}')
    if r.o != len(b): raise ValueError(f'{kind}.RAW: {len(b) - r.o} trailing bytes')
    return out

def raw_write(kind, ents):
    p = struct.pack; o = []
    if kind == 'UNITDATA':
        for sec in range(4):
            es = [e for e in ents if e['section'] == sec]; o.append(p('<I', len(es)))
            o += [p('<q', e['guid']) + _wsw(e['name']) + _wsw(e['file']) + p('<B5I', e['equip'], e['level'], e['minlevel'], e['maxlevel'],
                  e['rarity'], e['rarityhc']) + _wsw(e['unittype']) for e in es]
    elif kind == 'SKILLS':
        o.append(p('<I', len(ents))); o += [_wsw(e['name']) + _wsw(e['file']) + p('<q', e['guid']) for e in ents]
    elif kind == 'AFFIXES':
        o.append(p('<H', len(ents)))
        o += [_wsw(e['file']) + _wsw(e['name']) + p('<4I', e['min'], e['max'], e['weight'], e['diff']) + p('<B', len(e['types'])) +
              b''.join(map(_wsw, e['types'])) + p('<B', len(e['nottypes'])) + b''.join(map(_wsw, e['nottypes'])) for e in ents]
    elif kind == 'MISSILES':
        o.append(p('<H', len(ents))); o += [_wsw(e['file']) + p('<B', len(e['names'])) + b''.join(map(_wsw, e['names'])) for e in ents]
    elif kind == 'ROOMPIECES':
        o.append(p('<I', len(ents))); o += [_wsw(e['file']) for e in ents]
        o += [p('<I', len(e['guids'])) + b''.join(p('<q', g) for g in e['guids']) for e in ents]
    elif kind == 'TRIGGERABLES':
        o.append(p('<H', len(ents))); o += [_wsw(e['file']) + _wsw(e['name']) for e in ents]
    return b''.join(o)

def raw_key(kind, e):   # what identifies an entry: GUID where the RAW has one, else the DAT / LAYOUT path
    return e['guid'] if kind in ('UNITDATA', 'SKILLS') else e['file'].upper().replace('\\', '/')

def _pack_files(pak_path=None, mod_path=None):
    """-> (label, {upper path: (pak bytes, offset)}) of a Switch pack or a .MOD"""
    if mod_path:
        info, pak, root, ents = mod_read(mod_path); label = info['title'] or os.path.basename(mod_path)
    else:
        pak = open(pak_path, 'rb').read(); root, ents = man_read(open(pak_path + '.MAN', 'rb').read()); label = os.path.basename(pak_path)
    files = {}
    for d, n, typ, off, size, *_ in ents:
        if typ != 7: files[(d + n).upper().replace('\\', '/')] = (pak, off, size)
    return label, files

def raw_sim(romfs, mods, out_dir=None, log=print):
    """The game's own RAW merge for the base packs + mods in load order (DATA, DATA1..DATA5, then the mods as given = ZMOD_00..):
    every pack's copy is read in that order; UNITDATA keeps the first entry per GUID and drops units whose FILE no pack has. For the
    other kinds the game's duplicate rule is not traced, so a key present in several packs is reported as a conflict.
    -> {kind: [(pack label, entry, status)]}; with out_dir writes the merged RAWs there (inspection only, the game does not need them)."""
    pd = os.path.join(romfs, 'PAKS')
    packs = [_pack_files(os.path.join(pd, f)) for f in sorted(os.listdir(pd)) if f.upper().endswith('.PAK')]
    packs += [_pack_files(mod_path=m) for m in mods]
    allfiles = set().union(*(f for _, f in packs))
    res = {}
    for kind in RAW_KINDS:
        seen = {}; rows = []
        for label, files in packs:
            hit = files.get(f'MEDIA/{kind}.RAW')
            if not hit or not hit[2]: continue
            for e in raw_read(kind, pak_get(hit[0], hit[1])):
                k = raw_key(kind, e)
                if k in seen: st = f'dup of {seen[k]}'
                elif kind == 'UNITDATA' and e['file'].upper().replace('\\', '/') not in allfiles: st = 'no DAT'
                else: st = 'ok'; seen[k] = label
                rows.append((label, e, st))
        res[kind] = rows
        if out_dir:
            os.makedirs(out_dir, exist_ok=True)
            open(os.path.join(out_dir, kind + '.RAW'), 'wb').write(raw_write(kind, [e for _, e, st in rows if st == 'ok']))
    return res

def raw_selftest(romfs):
    """round trip of every RAW in the base packs and the mods' RAW parser against the game's own files"""
    pd = os.path.join(romfs, 'PAKS'); bad = 0
    for f in sorted(os.listdir(pd)):
        if not f.upper().endswith('.PAK'): continue
        _, files = _pack_files(os.path.join(pd, f))
        for kind in RAW_KINDS:
            hit = files.get(f'MEDIA/{kind}.RAW')
            if not hit: continue
            b = pak_get(hit[0], hit[1]); ents = raw_read(kind, b); same = raw_write(kind, ents) == b; bad += not same
            print(f'{f} {kind}.RAW: {len(ents)} entries, {len(b)} bytes, round trip', 'ok' if same else 'MISMATCH')
    return bad == 0

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
    elif len(a) >= 2 and a[0] == 'skilltrees':
        info, pak, root, ents = mod_read(a[1]); files = {}
        for d, n, typ, off, size, *_ in ents:
            up = _up(d + n)
            if typ != 7 and size and ((up.startswith('MEDIA/UNITS/PLAYERS/') and up.endswith('.DAT')) or (up.startswith('MEDIA/UI/') and up.endswith('.LAYOUT'))):
                files[up] = pak_get(pak, off)
        t = skilltrees_text(files); sys.stdout.reconfigure(encoding='utf-8'); print(t.decode('utf-8') if t else '(no class with a skill tree layout)', end='')
    elif len(a) >= 3 and a[0] == 'unpack':
        src = open(a[1], 'rb').read()
        if a[1].upper().endswith('.MOD'): _, pak, root, ents = mod_read(a[1])
        else: pak = src; root, ents = man_read(open(a[1] + '.MAN', 'rb').read())
        for d, n, typ, off, size, *_ in ents:
            if typ == 7 or not size: continue
            t = os.path.join(a[2], (d + n).replace('\\', '/')); os.makedirs(os.path.dirname(t), exist_ok=True)
            open(t, 'wb').write(pak_get(pak, off))
    elif len(a) >= 2 and a[0] == 'rawsim':
        if '--selftest' in a: sys.exit(0 if raw_selftest(a[1]) else 1)
        out = a[a.index('--out') + 1] if '--out' in a else None
        mods = [m for i, m in enumerate(a[2:], 2) if not m.startswith('--') and a[i - 1] != '--out']
        res = raw_sim(a[1], mods, out); import collections
        for kind, rows in res.items():
            if not rows: continue
            by = collections.OrderedDict()
            for label, e, st in rows: by.setdefault(label, collections.Counter())[st.split(' of ')[0]] += 1
            print(f'{kind}.RAW: ' + '; '.join(f'{l} ' + ', '.join(f'{k} {v}' for k, v in c.items()) for l, c in by.items()))
            for label, e, st in rows:
                if label.endswith('.PAK') and st == 'ok': continue
                if '-v' in a or st != 'ok':
                    print(f'  [{label}] {st:<22} {e.get("guid", "")!s:>20} {e.get("name", "")} {e["file"]}' +
                          (f' ({UNIT_SECTIONS[e["section"]]})' if 'section' in e else ''))
    elif len(a) >= 2 and a[0] == 'selftest':
        sys.exit(0 if selftest(a[1]) else 1)
    else:
        print(__doc__); sys.exit(2)

if __name__ == '__main__':
    main(sys.argv[1:])
