"""Device converter (Tl2ModConv.java) == reference converter (tl2_mod.py), byte for byte, on a synthetic PC .MOD built from the game.

Usage: python tools/tl2_mod_java_test.py <romfs> [workdir]      (needs a JDK: $JAVA_HOME/bin or PATH, numpy, Pillow)
The .MOD holds the selftest's texture sample (the same per-kind sample as `tl2_mod.py selftest`, 461 textures) turned back into PC
files -- BC1/2/3 as DXT1/3/5 DDS with all mip levels, RGBA8 as 32-bit DDS, 24-bit DDS (opaque ones), PNG and TGA (raw / RLE, both
origins) -- plus some data files (packed as GUTS packs them) and a deleted path. Checks:
  1. both converters write identical PAK and PAK.MAN;
  2. every converted texture equals the game's own XTX (layout, info block, pixels; the RGBA8 source-format block aside);
  3. the TGA decoder agrees with Pillow on every TGA written.
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

def main(romfs, work):
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
    print('PASS' if ok else 'FAIL'); return ok

if __name__ == '__main__':
    sys.exit(0 if main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.environ.get('TEMP', '/tmp'), 'tl2_mod_java_test')) else 1)
