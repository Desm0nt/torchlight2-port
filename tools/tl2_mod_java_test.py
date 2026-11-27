"""Device converter (Tl2ModConv.java) == reference converter (tl2_mod.py), byte for byte, on a synthetic PC .MOD built from the game.

Usage: python tools/tl2_mod_java_test.py <romfs> [workdir] [--mods a.MOD ...]   (needs a JDK: $JAVA_HOME/bin or PATH, numpy, Pillow)
The .MOD holds the selftest's texture sample (the same per-kind sample as `tl2_mod.py selftest`, 461 textures) turned back into PC
files -- BC1/2/3 as DXT1/3/5 DDS with all mip levels, RGBA8 as 32-bit DDS, 24-bit DDS (opaque ones), PNG and TGA (raw / RLE, both
origins) -- plus some data files (packed as GUTS packs them) and a deleted path. Checks:
  1. both converters write identical PAK and PAK.MAN;
  2. every converted texture equals the game's own XTX (layout, info block, pixels; the RGBA8 source-format block aside);
  3. the TGA decoder agrees with Pillow on every TGA written;
  4. four synthetic classes' PC skill trees (4 full tabs + overflow, overflow into a free 4th, and two with no SkillPane<N> name as in
     SYNERGIES) become the expected MEDIA/PORT/SKILLTREES.TXT (issues #6, #25);
  5. skill icons of mod imagesets -> the expected console atlas (entries, page size, pixels; DXT against Pillow's decoder);
  6. with --mods: real PC mods convert to identical bytes in both (skill-tree tables and icon atlases included).
"""
import collections, io, os, shutil, struct, subprocess, sys, zlib
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import tl2_mod as T
import numpy as np
from PIL import Image

def dds(x, lin):
    fourcc = {0x42: b'DXT1', 0x43: b'DXT3', 0x44: b'DXT5'}.get(x['fmt'])
    if fourcc: pf = struct.pack('<II4s5I', 32, 4, fourcc, 0, 0, 0, 0, 0); data = b''.join(lin)
    else:
        a = np.frombuffer(b''.join(lin), np.uint8).reshape(-1, 4)
        if (a[:, 3] == 255).all() and x['w'] % 3:   # some opaque ones as 24-bit BGR
            pf = struct.pack('<8I', 32, 0x40, 0, 24, 0xff0000, 0xff00, 0xff, 0); data = a[:, [2, 1, 0]].tobytes()
        else: pf = struct.pack('<8I', 32, 0x41, 0, 32, 0xff0000, 0xff00, 0xff, 0xff000000); data = a[:, [2, 1, 0, 3]].tobytes()
    return b'DDS ' + struct.pack('<7I', 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000, x['h'], x['w'], 0, 0, x['mips']) + b'\0' * 44 + pf + \
        struct.pack('<4I', 0x1000 | 0x400000 | 8, 0, 0, 0) + b'\0' * 4 + data

def tga(w, h, rgba, rle, top, bits):
    a = np.frombuffer(rgba, np.uint8).reshape(h, w, 4)
    rows = a if top else a[::-1]
    px = rows[:, :, [2, 1, 0, 3]] if bits == 32 else rows[:, :, [2, 1, 0]]
    ps = bits // 8; flat = px.reshape(-1, ps); out = bytearray()
    if rle:   # packets within a row (TGA 2.0; Pillow refuses packets across rows)
        for r0 in range(0, len(flat), w):
            i = r0; n = r0 + w
            while i < n:
                j = i
                while j + 1 < n and j - i < 127 and (flat[j + 1] == flat[i]).all(): j += 1
                if j > i: out += bytes([0x80 | (j - i)]) + flat[i].tobytes(); i = j + 1
                else:
                    k = i
                    while k + 1 < n and k - i < 127 and not (flat[k + 1] == flat[k]).all(): k += 1
                    out += bytes([k - i]) + flat[i:k + 1].tobytes(); i = k + 1
    else: out = flat.tobytes()
    return struct.pack('<BBBHHBHHHHBB', 0, 0, 10 if rle else 2, 0, 0, 0, 0, 0, w, h, bits, (8 if bits == 32 else 0) | (0x20 if top else 0)) + bytes(out)

def st_dat(props):   # TL2 binary DAT v2, root block UNIT with string properties {key: value}
    strs = sorted({v for v in props.values()}); h = T.tl2_hash
    b = struct.pack('<II', 2, len(strs)) + b''.join(struct.pack('<IH', h(v), len(v)) + v.encode('utf-16le') for v in strs)
    b += struct.pack('<II', h('UNIT'), len(props)) + b''.join(struct.pack('<III', h(k), 5, h(v)) for k, v in props.items())
    return b + struct.pack('<I', 0)

def st_obj(typ, name, props, children=()):   # TL2 binary LAYOUT object (size, type, guid, name, properties, no extra block)
    pb = b''.join(struct.pack('<HB', 1 + len(v), k) + v for k, v in props)
    body = struct.pack('<B', typ) + bytes(8) + struct.pack('<H', len(name)) + name.encode('utf-16le') + bytes([len(props)]) + pb + \
        struct.pack('<IH', 0, len(children)) + b''.join(children)
    return struct.pack('<I', 4 + len(body)) + body

def st_files():
    """two synthetic classes with PC skill trees:
    TEST (TEST_BASE.DAT; TEST_F.DAT inherits the tree through BASEFILE): 4 full tabs, each 12 skills in 4 rows x 3 (passive row 3 + 9
      actives: 2 per tree overflow) -> trees 0..3 and a 5th "More Skills" with the 8 overflow skills; tab 2's label carries a tab and a
      trailing space, one of its skills holds a nested skill (ignored); pane order in the file differs from the tab numbers.
    TEST2 (TEST2_M.DAT): 3 tabs; tree 1 has 8 actives + 3 passives -> 1 overflow into a 4th tree "Alpha II"; tab 3 has no pane.
    TEST3 (TEST3_M.DAT): no SkillPane<N> at all (as SYNERGIES) -- 3 panes named after their tabs, in another file order, 2 skills each;
      paired by name, trees in the tabs' display order (prop 0x3f), and the file order plus the shown pane would pair them differently.
    TEST4 (TEST4_M.DAT): no SkillPane<N> and nothing named -- 3 unnamed panes under the tabs' own order, the second one shown; paired
      in file order from the shown one, with the "may belong to another pane" note."""
    w = lambda s: struct.pack('<H', len(s)) + s.encode('utf-16le'); f = lambda v: struct.pack('<f', v)
    sk = lambda nm, x, y, kids=(): st_obj(0x3d, nm, [(0x3f, f(x)), (0x22, f(y)), (0x40, w(nm)), (0x7b, struct.pack('<i', -1))], kids)
    tab = lambda n, lb, ic: st_obj(0x58, f'Tab{n}', [(0x23, w(lb)), (0x41, w(ic))])
    xtab = lambda nm, x, lb, ic: st_obj(0x58, nm, [(0x23, w(lb)), (0x41, w(ic)), (0x3f, f(x))])   # a tab placed by hand: x = display order
    hide = lambda shown: [] if shown else [(0x29, struct.pack('<i', 0))]   # prop 0x29 = hidden; the layout shows exactly one pane
    lay = lambda roots: bytes([0x0b, 0]) + struct.pack('<IH', 0, len(roots)) + b''.join(roots)
    def pane(n):   # 12 skills T<n>S<row><col>, written out of order; T2S00 holds a nested INNER
        return st_obj(0x3d, f'SkillPane{n}', [], [sk(f'T{n}S{r}{c}', 9 + 155 * c, 56 + 140 * r, [sk('INNER', 0, 0)] if (n, r, c) == (2, 0, 0) else ())
                                                  for r in (3, 0, 2, 1) for c in (2, 0, 1)])
    l1 = lay([st_obj(0x3d, 'Tabs', [], [tab(2, 'Second\tTab ', 'IG_SKILLTABS_FROST_ON'), tab(1, 'First', 'IG_SKILLTABS_HUNTER_OFF'),
                                        tab(4, 'Fourth', 'X'), tab(3, 'Third', 'IG_SKILLTABS_AEGIS_OFF')]),
              st_obj(0x3d, 'skillGroup', [], [pane(2), pane(1), pane(4), pane(3)])])
    p21 = st_obj(0x3d, 'SkillPane1', [], [sk(f'A{c}', 9 + 40 * c, 56 + 140 * (c // 4)) for c in range(8)] + [sk(f'AP{c}', 9 + 155 * c, 491) for c in range(3)])
    p22 = st_obj(0x3d, 'SkillPane2', [], [sk('B0', 9, 56)])
    l2 = lay([st_obj(0x3d, 'Tabs', [], [tab(1, 'Alpha', 'IG_SKILLTABS_BLITZ_OFF'), tab(2, 'Beta', 'IG_SKILLTABS_LORE_OFF'), tab(3, 'Gamma', 'IG_SKILLTABS_SIGIL_OFF')]),
              st_obj(0x3d, 'skillGroup', [], [p21, p22])])
    p3 = lambda nm, shown, ks: st_obj(0x3d, nm, hide(shown), [sk(k, 9 + 155 * c, 56) for c, k in enumerate(ks)])
    l3 = lay([st_obj(0x3d, 'Tabs', [], [xtab('GAMMA', 269, 'Gamma', 'IG_SKILLTABS_SIGIL_OFF'), xtab('ALPHA', 138, 'Alpha', 'IG_SKILLTABS_WARFARE_OFF'),
                                        xtab('BETA', 203, 'Beta', 'IG_SKILLTABS_LORE_OFF')]),
              st_obj(0x3d, 'skillGroup', [], [p3('BETA', False, ['B3_1', 'B3_2']), p3('GAMMA', True, ['G3_1', 'G3_2']), p3('ALPHA', False, ['A3_1', 'A3_2'])])])
    l4 = lay([st_obj(0x3d, 'Tabs', [], [xtab('', 203, 'Second', 'IG_SKILLTABS_FROST_OFF'), xtab('', 138, 'First', 'IG_SKILLTABS_BLITZ_OFF'),
                                        xtab('', 269, 'Third', 'IG_SKILLTABS_AEGIS_OFF')]),
              st_obj(0x3d, 'skillGroup', [], [p3('', False, ['D4_1']), p3('', True, ['E4_1']), p3('', False, ['F4_1'])])])
    d, u = 'MEDIA/UNITS/PLAYERS/TESTCLASS/', 'MEDIA/UI/MENUS/SKILLTREEOVERRIDES/'
    return [(d, 'TEST_BASE.DAT', 0, st_dat({'NAME': 'Test_Base', 'UISKILLTREE': 'media/ui/menus/skilltreeoverrides/skill_tree_test.layout'}), 0),
            (d, 'TEST_F.DAT', 0, st_dat({'NAME': 'Test_F', 'BASEFILE': 'media\\units\\players\\testclass\\test_base.dat'}), 0),
            (d, 'TEST2_M.DAT', 0, st_dat({'NAME': 'Test2_M', 'UISKILLTREE': '/MEDIA/UI/MENUS/SKILLTREEOVERRIDES/SKILL_TREE_TEST2.LAYOUT'}), 0),
            (d, 'TEST3_M.DAT', 0, st_dat({'NAME': 'Test3_M', 'UISKILLTREE': 'media/ui/menus/skilltreeoverrides/skill_tree_test3.layout'}), 0),
            (d, 'TEST4_M.DAT', 0, st_dat({'NAME': 'Test4_M', 'UISKILLTREE': 'media/ui/menus/skilltreeoverrides/skill_tree_test4.layout'}), 0),
            (u, 'SKILL_TREE_TEST.LAYOUT', 1, l1, 0), (u, 'SKILL_TREE_TEST2.LAYOUT', 1, l2, 0),
            (u, 'SKILL_TREE_TEST3.LAYOUT', 1, l3, 0), (u, 'SKILL_TREE_TEST4.LAYOUT', 1, l4, 0)]

# what both converters must store for st_files, written out by hand (not by the converter's rules)
def _st_want():
    o = ['TL2SKILLTREES\t2', 'L\t0\tMEDIA/UI/MENUS/SKILLTREEOVERRIDES/SKILL_TREE_TEST.LAYOUT',
         'T\t0\t0\tFirst\thunter', 'T\t0\t1\tSecond Tab\tfrost', 'T\t0\t2\tThird\taegis', 'T\t0\t3\tFourth\t-', 'T\t0\t4\tMore Skills\t-']
    for t in range(4):   # rows 0, 1 and the first of row 2 = 7 actives, row 3 = passives 7..9
        o += [f'S\t0\t{t}\t{i}\tT{t + 1}S{n}' for i, n in enumerate(['00', '01', '02', '10', '11', '12', '20', '30', '31', '32'])]
    o += [f'S\t0\t4\t{i}\tT{t}S2{c}' for i, (t, c) in enumerate((t, c) for t in (1, 2, 3, 4) for c in (1, 2))]
    o += ['L\t1\tMEDIA/UI/MENUS/SKILLTREEOVERRIDES/SKILL_TREE_TEST2.LAYOUT',
          'T\t1\t0\tAlpha\tblitz', 'T\t1\t1\tBeta\tlore', 'T\t1\t2\tGamma\tsigil', 'T\t1\t3\tAlpha II\tblitz']
    o += [f'S\t1\t0\t{i}\tA{i}' for i in range(7)] + [f'S\t1\t0\t{7 + i}\tAP{i}' for i in range(3)]
    o += ['S\t1\t1\t0\tB0', 'S\t1\t3\t0\tA7']
    o += ['L\t2\tMEDIA/UI/MENUS/SKILLTREEOVERRIDES/SKILL_TREE_TEST3.LAYOUT',   # panes paired with the tabs by name
          'T\t2\t0\tAlpha\twarfare', 'T\t2\t1\tBeta\tlore', 'T\t2\t2\tGamma\tsigil',
          'S\t2\t0\t0\tA3_1', 'S\t2\t0\t1\tA3_2', 'S\t2\t1\t0\tB3_1', 'S\t2\t1\t1\tB3_2', 'S\t2\t2\t0\tG3_1', 'S\t2\t2\t1\tG3_2']
    o += ['L\t3\tMEDIA/UI/MENUS/SKILLTREEOVERRIDES/SKILL_TREE_TEST4.LAYOUT',   # nothing named: file order from the shown pane
          'T\t3\t0\tFirst\tblitz', 'T\t3\t1\tSecond\tfrost', 'T\t3\t2\tThird\taegis',
          'S\t3\t0\t0\tE4_1', 'S\t3\t1\t0\tF4_1', 'S\t3\t2\t0\tD4_1',
          'X\t3\t3 skill pane(s) with no SkillPane<N> name: paired with the tabs in file order from the first shown one, '
          'so a tab label may belong to another pane']
    o += ['C\tTEST2_M\t1', 'C\tTEST3_M\t2', 'C\tTEST4_M\t3', 'C\tTEST_BASE\t0', 'C\tTEST_F\t0']
    return '\n'.join(o) + '\n'
ST_WANT = _st_want()

def ic_dds(w, h, pff, fourcc, bits, masks, data):
    return b'DDS ' + struct.pack('<7I', 124, 0x1 | 0x2 | 0x4 | 0x1000, h, w, 0, 0, 1) + b'\0' * 44 + \
        struct.pack('<2I4s5I', 32, pff, fourcc, bits, *masks) + struct.pack('<4I', 0x1000, 0, 0, 0) + b'\0' * 4 + data

def ic_files():
    """skill icons: skill DATs naming icons (one in another case, one undefined), imagesets (UTF-8 BOM, UTF-16, attributes in any order,
    a repeated attribute, a backslash path) over a 32-bit DDS, a PNG, DXT1 / DXT3 / DXT5 DDS (random blocks: every BC mode), a TGA and a
    missing texture. -> (files, {icon: (texture kind, source RGBA rows, x, y, w, h)})"""
    import random
    rnd = random.Random(6); src = {}; d = 'MEDIA/UI/ICONS/TEST/'
    raw = bytes(rnd.randrange(256) for _ in range(32 * 32 * 4))   # BGRA in the file
    rgba = bytes(raw[i + j] for i in range(0, len(raw), 4) for j in (2, 1, 0, 3))
    f = [(d, 'RAW32.DDS', 4, ic_dds(32, 32, 0x41, b'\0\0\0\0', 32, (0xff0000, 0xff00, 0xff, 0xff000000), raw), 0)]; src['RAW'] = rgba
    pim = bytes(rnd.randrange(256) for _ in range(40 * 40 * 4)); bio = io.BytesIO(); Image.frombytes('RGBA', (40, 40), pim).save(bio, 'PNG')
    f.append((d, 'ICONS.PNG', 5, bio.getvalue(), 0)); src['PNG'] = pim
    for fc, bs in ((b'DXT1', 8), (b'DXT3', 16), (b'DXT5', 16)):
        data = bytes(rnd.randrange(256) for _ in range(16 * bs))   # 16 x 16 = 4 x 4 blocks
        dds = ic_dds(16, 16, 4, fc, 0, (0, 0, 0, 0), data); f.append((d, fc.decode() + '.DDS', 4, dds, 0))
        src[fc.decode()] = Image.open(io.BytesIO(dds)).convert('RGBA').tobytes()
    tpx = bytes(rnd.randrange(256) for _ in range(16 * 8 * 4)); f.append((d, 'T.TGA', 25, tga(16, 8, tpx, False, True, 32), 0)); src['TGA'] = tpx
    def iset(file, imgs, enc='bom'):
        t = f'<?xml version="1.0" encoding="UTF-8"?>\r\n<Imageset Name="x" Imagefile="{file}" NativeHorzRes="1024">\r\n' + ''.join(f'\t<Image {i} />\r\n' for i in imgs) + '</Imageset>\r\n'
        return b'\xef\xbb\xbf' + t.encode() if enc == 'bom' else b'\xff\xfe' + t.encode('utf-16le')
    f += [(d, 'A.IMAGESET', 22, iset('media/ui/icons/test/raw32.dds', ['Width="20" Height="20" XPos="9" YPos="5" XPos="3" Name="Ico_Raw"', 'Name="unused" XPos="0" YPos="0" Width="4" Height="4"']), 0),
          (d, 'B.IMAGESET', 22, iset('media\\ui\\icons\\test\\icons.png', ['Name="ico_png" XPos="4" YPos="6" Width="32" Height="32"']), 0),
          (d, 'C.IMAGESET', 22, iset('/MEDIA/UI/ICONS/TEST/DXT1.DDS', ['Name="ico_dxt1" XPos="1" YPos="1" Width="10" Height="10"', 'Name="ico_dxt3x" XPos="0" YPos="0" Width="2" Height="2"']), 0),
          (d, 'D.IMAGESET', 22, iset('media/ui/icons/test/dxt3.dds', ['Name="ico_dxt3" XPos="0" YPos="0" Width="16" Height="16"', 'Name="ico_bad" XPos="a" YPos="0" Width="16" Height="16"'], 'utf16'), 0),
          (d, 'E.IMAGESET', 22, iset('media/ui/icons/test/dxt5.dds', ['Name="ico_dxt5" XPos="0" YPos="0" Width="16" Height="16"']), 0),
          (d, 'F.IMAGESET', 22, iset('media/ui/icons/test/t.tga', ['Name="ico_tga" XPos="2" YPos="0" Width="12" Height="8"', 'Name="ico_missing" XPos="0" YPos="0" Width="8" Height="8"'.replace('ico_missing', 'ico_tga_dup')]), 0),
          (d, 'G.IMAGESET', 22, iset('media/ui/icons/test/none.dds', ['Name="ico_missing" XPos="0" YPos="0" Width="8" Height="8"']), 0)]
    s = 'MEDIA/SKILLS/TESTCLASS/'
    f += [(s, 'S1.DAT', 0, st_dat({'NAME': 'S1', 'SKILL_ICON': 'ICO_RAW', 'SKILL_ICON_INACTIVE': 'ico_png'}), 0),
          (s, 'S2.DAT', 0, st_dat({'NAME': 'S2', 'SKILL_ICON': 'ico_dxt1', 'SKILL_ICON_INACTIVE': 'ico_dxt3'}), 0),
          (s, 'S3.DAT', 0, st_dat({'NAME': 'S3', 'SKILL_ICON': 'ico_dxt5', 'SKILL_ICON_INACTIVE': 'ico_tga'}), 0),
          (s, 'S4.DAT', 0, st_dat({'NAME': 'S4', 'SKILL_ICON': 'ico_missing', 'SKILL_ICON_INACTIVE': 'ico_undefined'}), 0),
          (s, 'S5.DAT', 0, st_dat({'NAME': 'S5', 'SKILL_ICON': 'ico_bad'}), 0)]
    # expected placement (hand-made: heights 32, 20, 16, 16, 10, 8, 8, cells +4, x from 2), source texture / rect per icon
    want = [('ico_png', 2, 'PNG', 40, 4, 6, 32, 32), ('ICO_RAW', 38, 'RAW', 32, 3, 5, 20, 20), ('Ico_Raw', 38, 'RAW', 32, 3, 5, 20, 20),
            ('ico_dxt3', 62, 'DXT3', 16, 0, 0, 16, 16), ('ico_dxt5', 82, 'DXT5', 16, 0, 0, 16, 16), ('ico_dxt1', 102, 'DXT1', 16, 1, 1, 10, 10),
            ('ico_tga', 128, 'TGA', 16, 2, 0, 12, 8)]   # ico_missing (cell at 114) has no texture: no entry
    return f, src, want

def ic_check(atlas, page, src, want, modid):
    """atlas XML + page XTX of the converter against the hand-made expectation; pixels exact except DXT (Pillow's decoder, +-1)"""
    base = f'ZMODICONS_{modid:016X}_0'
    xml = '﻿<?xml version="1.0" encoding="utf-8"?>\r\n<atlas texture="' + base + '.DDS" padding="2">\r\n    <entries>\r\n' + ''.join(
        f'        <entry name="{n}" x="{x}" y="2" w="{w}" h="{h}" />\r\n' for n, x, _, _, _, _, w, h in want) + '    </entries>\r\n</atlas>'
    ok = atlas == xml.encode('utf-8')
    if not ok: print(atlas.decode('utf-8', 'replace')); print('--- want\n' + xml)
    x = T.xtx_parse(page); ok &= bool(x) and (x['w'], x['h'], x['fmt']) == (256, 64, 0x25)
    if not ok: return False
    img = T.xtx_deswizzle(x['data'], x['w'], x['h'], x['fmt'], 1)[0]; worst = 0
    for n, px, kind, sw, sx, sy, w, h in want:
        for yy in range(h):
            for xx in range(w):
                a = img[4 * ((2 + yy) * 256 + px + xx):][:4]; a = bytes((a[2], a[1], a[0], a[3])); b = src[kind][4 * ((sy + yy) * sw + sx + xx):][:4]
                dmax = max(abs(a[i] - b[i]) for i in range(4))
                if kind.startswith('DXT'): worst = max(worst, dmax)
                elif dmax: print(f'  icon {n} pixel {xx},{yy}: {list(a)} != {list(b)}'); return False
    print(f'  icon atlas: {len(want)} entries as expected, pixels exact (DXT within {worst} of Pillow)')
    return ok and worst <= 1

def pack_file(paks, base, path):
    b = open(os.path.join(paks, base + '.PAK'), 'rb').read(); _, out = T.man_read(open(os.path.join(paks, base + '.PAK.MAN'), 'rb').read())
    for d, n, typ, off, size, crc, ft in out:
        if d + n == path and size: return T.pak_get(b, off)
    return None

def main(romfs, work, mods=()):
    shutil.rmtree(work, ignore_errors=True); os.makedirs(work)
    paks = os.path.join(romfs, 'PAKS'); files = []; expect = {}; seen = collections.Counter(); tga_bad = 0; ntga = 0
    for p in sorted(os.listdir(paks)):
        if not p.endswith('.PAK'): continue
        b = open(os.path.join(paks, p), 'rb').read(); root, ents = T.man_read(open(os.path.join(paks, p + '.MAN'), 'rb').read())
        ndat = 0
        for d, n, typ, off, size, crc, ft in ents:
            if not size or typ == 7: continue
            data = T.pak_get(b, off)
            if not n.upper().endswith(T.TEX_EXT):
                if ndat < 3 and p == 'DATA5.PAK': files.append((d, n, typ, data, ft)); ndat += 1
                continue
            x = T.xtx_parse(data)
            if not x or x['target'] != 1 or x['fmt'] not in T.FMT: continue
            k = (x['fmt'], x['mips'] > 1, min(x['w'], x['h']) < 64)
            if seen[k] >= 40: continue
            seen[k] += 1; lin = T.xtx_deswizzle(x['data'], x['w'], x['h'], x['fmt'], x['mips']); stem = n.rsplit('.', 1)[0]
            files.append((d, stem + '.DDS', 4, dds(x, lin), ft)); expect[d + stem + '.DDS'] = x
            if x['fmt'] == 0x25 and x['mips'] == 1:
                bio = io.BytesIO(); Image.frombytes('RGBA', (x['w'], x['h']), lin[0]).save(bio, 'PNG')
                files.append((d, stem + '_V.PNG', 5, bio.getvalue(), ft)); expect[d + stem + '_V.PNG'] = x
                opaque = bool((np.frombuffer(lin[0], np.uint8)[3::4] == 255).all())
                for i, (rle, top) in enumerate(((False, False), (True, True), (True, False)) if x['w'] * x['h'] <= 65536 else ((False, False),)):
                    t = tga(x['w'], x['h'], lin[0], rle, top, 24 if opaque and i == 2 else 32); nm = f'{stem}_T{i}.TGA'
                    files.append((d, nm, 25, t, ft)); expect[d + nm] = x; ntga += 1
                    w, h, px = T.tga_decode(t)
                    if px != Image.open(io.BytesIO(t)).convert('RGBA').tobytes(): tga_bad += 1
    files += st_files()   # a modded class's PC skill tree -> MEDIA/PORT/SKILLTREES.TXT
    icf, icsrc, icwant = ic_files(); files += icf   # skill icons -> MEDIA/UICONSOLE/ATLASES/ZMODICONS_*
    # the .MOD: header v4 + PAK (zlib as GUTS packs, smaller of the two) + MAN
    body = bytearray(8); man = []
    for d, n, typ, data, ft in files:
        z = zlib.compress(data); st = z if len(z) < len(data) else data
        man.append((d, n, typ, len(body), len(data), zlib.crc32(data), ft)); body += struct.pack('<II', len(data), len(st)) + st
    struct.pack_into('<I', body, 4, T.pak_hash(body))
    ws = lambda s: struct.pack('<H', len(s)) + s.encode('utf-16le')
    tail = ws('Java Test Mod') + ws('tester') + ws('synthetic') + ws('') + ws('') + struct.pack('<qIq', 77, 0, 0) + struct.pack('<H', 0) + \
        struct.pack('<H', 2) + ws('MEDIA\\SKILLS\\OLD.DAT') + ws('MEDIA/X.DAT')
    head = struct.pack('<HH4H', 4, 3, 1, 25, 5, 2); off_data = len(head) + 8 + len(tail)
    mod = os.path.join(work, 'JavaTest.MOD')
    open(mod, 'wb').write(head + struct.pack('<II', off_data, off_data + len(body)) + tail + bytes(body) + T.man_write('JAVATEST/', man))
    # 1. both converters
    T.convert(mod, os.path.join(work, 'py'), 3)
    cls = os.path.join(work, 'classes'); src = os.path.join(HERE, '..', 'dm', 'android', 'app', 'src', 'main', 'java', 'com', 'xiaoboluo', 'd2r', 'Tl2ModConv.java')
    jb = lambda t: os.path.join(os.environ['JAVA_HOME'], 'bin', t) if os.environ.get('JAVA_HOME') else t   # JAVA_HOME first
    subprocess.check_call([jb('javac'), '-d', cls, src, os.path.join(HERE, 'Tl2ModConvMain.java')])
    print(subprocess.check_output([jb('java'), '-cp', cls, 'com.xiaoboluo.d2r.Tl2ModConvMain', mod, os.path.join(work, 'java'), '3'], text=True).strip())
    base = 'ZMOD_03_JAVA_TEST_MOD'; ok = True
    for ext in ('.PAK', '.PAK.MAN'):
        a = open(os.path.join(work, 'py', 'PAKS', base + ext), 'rb').read(); b = open(os.path.join(work, 'java', 'PAKS', base + ext), 'rb').read()
        same = a == b; ok &= same; print(f'{base}{ext}: python {len(a)} bytes, java {len(b)} bytes, identical: {same}')
    # 2. converted textures against the game's XTX
    pb = open(os.path.join(work, 'py', 'PAKS', base + '.PAK'), 'rb').read(); _, out = T.man_read(open(os.path.join(work, 'py', 'PAKS', base + '.PAK.MAN'), 'rb').read())
    good = bad = 0
    for d, n, typ, off, size, crc, ft in out:
        if d + n in expect:
            x0 = expect[d + n]; x = T.xtx_parse(T.pak_get(pb, off))
            if x and x['data'] == x0['data'] and x['info'] == x0['info'] and (x['pf'] == x0['pf'] or x0['fmt'] == 0x25): good += 1
            else: bad += 1; print('  texture differs from the game:', d + n)
    print(f'textures equal to the game: {good}, different: {bad} (sample kinds {len(seen)}, {sum(seen.values())} game textures); '
          f'TGA decoder vs Pillow: {ntga - tga_bad}/{ntga} equal')
    ok &= bad == 0 and tga_bad == 0
    # 3. the skill-tree table of the synthetic class
    stt = pack_file(os.path.join(work, 'py', 'PAKS'), base, ''.join(T.ST_PATH))
    st_ok = stt == ST_WANT.encode('utf-8'); ok &= st_ok
    print(f'skill-tree table MEDIA/PORT/SKILLTREES.TXT as expected: {st_ok}')
    if not st_ok: print(stt.decode('utf-8') if stt else '(missing)')
    # 3b. the skill icon atlas of the synthetic mod
    at = pack_file(os.path.join(work, 'py', 'PAKS'), base, f'{T.ATLAS_DIR}ZMODICONS_{77:016X}_0.ATLAS')
    pg = pack_file(os.path.join(work, 'py', 'PAKS'), base, f'{T.ATLAS_DIR}ZMODICONS_{77:016X}_0.DDS')
    ic_ok = at is not None and pg is not None and ic_check(at, pg, icsrc, icwant, 77); ok &= ic_ok
    print(f'skill icon atlas ZMODICONS_{77:016X}_0 as expected: {ic_ok}')
    # 4. real PC mods (optional, --mods a.MOD b.MOD ...): both converters, byte for byte, and their skill-tree tables
    for i, m in enumerate(mods):
        T.convert(m, os.path.join(work, 'py'), 10 + i)
        print(subprocess.check_output([jb('java'), '-cp', cls, 'com.xiaoboluo.d2r.Tl2ModConvMain', m, os.path.join(work, 'java'), str(10 + i)], text=True).strip())
        mb = [f for f in os.listdir(os.path.join(work, 'py', 'PAKS')) if f.startswith(f'ZMOD_{10 + i:02d}_') and f.endswith('.PAK')][0][:-4]
        for ext in ('.PAK', '.PAK.MAN'):
            a = open(os.path.join(work, 'py', 'PAKS', mb + ext), 'rb').read(); b = open(os.path.join(work, 'java', 'PAKS', mb + ext), 'rb').read()
            same = a == b; ok &= same; print(f'{mb}{ext}: python {len(a)} bytes, java {len(b)} bytes, identical: {same}')
        t = pack_file(os.path.join(work, 'py', 'PAKS'), mb, ''.join(T.ST_PATH)); lines = t.decode('utf-8').split('\n') if t else []
        print(f'  {os.path.basename(m)}: skill-tree table {len(t) if t else 0} bytes, layouts {sum(l.startswith("L") for l in lines)}, '
              f'classes {sum(l.startswith("C") for l in lines)}, dropped {sum(l.startswith("X") for l in lines)}, '
              f'icon atlas pages {sum(1 for f in T.man_read(open(os.path.join(work, "py", "PAKS", mb + ".PAK.MAN"), "rb").read())[1] if f[1].startswith("ZMODICONS_") and f[1].endswith(".ATLAS"))}')
    print('PASS' if ok else 'FAIL'); return ok

if __name__ == '__main__':
    a = sys.argv[1:]; mods = a[a.index('--mods') + 1:] if '--mods' in a else []; a = a[:a.index('--mods')] if '--mods' in a else a
    sys.exit(0 if main(a[0], a[1] if len(a) > 1 else os.path.join(os.environ.get('TEMP', '/tmp'), 'tl2_mod_java_test'), mods) else 1)
