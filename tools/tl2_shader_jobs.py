"""Torchlight II (Switch) shader jobs for the offline translator (dmsh_xlate via dm/gfx/tools/dmsh_vk_xc2.py). Notes: docs/tl2/SHADERS.md

TL2 (Ogre 1.x, Switch port by Panic Button) keeps every NVN shader in RomFS PROGRAMS/*.NVN (and skin*_c.nvn): each file is one stage, a bare
GLSLCoutput (as DOOM 2016's .sb without the 8-byte id wrapper; see the DOOM port's doom_shader_jobs.py):
    +0x00 u32 magic 0x19866891   +0x44 total size   +0x48 data offset of the first section   +0x4c section count
    +0x90 + i*0x90: section header (36 x u32): [0] size [1] data offset [2] type (0 GPU code, 1 asm dump, 2 perf, 3 reflection, 4 debug hash)
        GPU code: [11] NVN stage (0 vertex, 1 fragment, 5 compute) [12] control offset (from data) [13] code offset [14] code size [15] control size
    control: magic 0x98761234 (graphics 0x850 bytes, compute 0xe48); code area: graphics 0x12345678 header + SPH + instructions + c1, compute instructions + c1
Program pairing follows CONSOLE_SHADERS.PROGRAM: FIXEDFUNCTIONVS[_perm] <-> FIXEDFUNCTIONPS[_perm], POSTVS <-> POST*PS, POSTFXAAVS <-> POSTFXAAPS;
skin*_c are compute (vertex skinning).

Outputs as d3_shader_jobs.py (rec/, jobs.tsv, programs.tsv, shader_map.tsv, shader_facts.tsv, xfb.tsv, report.txt), same KEY
(xbg_shader_program_key): n = u32 @ control+0x20; L = n if 0x760 <= n < 0x880 else 0x880; key = sha256(control[0:L] + code[0:nc])[:8].hex().
Depth: right after device init the game calls nvnDeviceSetWindowOriginMode(dev, 1) and nvnDeviceSetDepthMode(dev, 1 = NEAR_IS_ZERO)
unconditionally (main+0x199e4f8 / +0x199e50c, proc pointers at main+0x7702940 / +0x7702948), so vertex shaders get {"depth_zero": true}.
Usage: tl2_shader_jobs.py [PROGRAMS dir] [out dir]
"""
import collections, hashlib, json, os, re, shutil, struct, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from d3_shader_jobs import host_key, xfb_layout, CTRL, GFX, SPH_STAGE

ROOT = Path(__file__).resolve().parents[1]
NVN_STAGE = {0: 'vs', 1: 'ps', 2: 'gs', 5: 'cs'}


def glslc_stage(g, where, bad):
    """(stage, control, code) of the GPU-code section of one GLSLCoutput file, or None"""
    if struct.unpack_from('<I', g, 0)[0] != 0x19866891: bad.append(f'{where}: no GLSLCoutput magic'); return None
    tot, _, ns = struct.unpack_from('<3I', g, 0x44)
    if tot != len(g): bad.append(f'{where}: total size {tot:#x} != file {len(g):#x}')
    secs = [struct.unpack_from('<36I', g, 0x90 + i * 0x90) for i in range(ns)]
    code_secs = [h for h in secs if h[2] == 0]
    if len(code_secs) != 1: bad.append(f'{where}: {len(code_secs)} GPU code sections'); return None
    h = code_secs[0]
    st = NVN_STAGE.get(h[11])
    if st is None: bad.append(f'{where}: NVN stage {h[11]}'); return None
    d = h[1]
    ctl, code = g[d + h[12]:d + h[12] + h[15]], g[d + h[13]:d + h[13] + h[14]]
    if ctl[:4] != CTRL: bad.append(f'{where}: no control magic'); return None
    nc, = struct.unpack_from('<I', ctl, 0x704); c1o, c1t = struct.unpack_from('<2I', ctl, 0x700)
    if nc != len(code) or not c1o <= c1t == nc: bad.append(f'{where}: code {len(code):#x}, control says {nc:#x}, c1 {c1o:#x}..{c1t:#x}'); return None
    if st != 'cs':
        if code[:4] != GFX: bad.append(f'{where}: graphics code without 0x12345678 header'); return None
        sph = SPH_STAGE.get(struct.unpack_from('<I', code, 0x30)[0] >> 10 & 0xf)
        if sph != st: bad.append(f'{where}: SPH stage {sph} vs section stage {st}'); return None
    return st, ctl, code


def partner_vs(name):
    """vertex-stage file name for a pixel-stage file name (upper-case stem), or None"""
    if name.startswith('FIXEDFUNCTIONPS'): return 'FIXEDFUNCTIONVS' + name[len('FIXEDFUNCTIONPS'):]
    if name == 'POSTFXAAPS': return 'POSTFXAAVS'
    if name.startswith('POST') and name.endswith('PS'): return 'POSTVS'
    return None


def main():
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT.parent / 'work' / 'romfs' / 'PROGRAMS'
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT.parent / 'work' / 'shaders-jobs'
    if (out / 'rec').exists(): shutil.rmtree(out / 'rec')
    (out / 'rec').mkdir(parents=True)
    files = sorted(p for p in src.iterdir() if p.suffix.lower() == '.nvn')
    meta, where, bad, rep, xfbs, byname = {}, collections.defaultdict(list), [], collections.Counter(), {}, {}
    for p in files:
        r = glslc_stage(p.read_bytes(), p.name, bad)
        if r is None: continue
        st, ctl, code = r
        k = host_key(ctl, code)
        rec = bytes(0x38) + code + ctl
        ci = rec.find(CTRL, 0x38 + len(code))
        if ci != 0x38 + len(code) or host_key(rec[ci:], rec[0x38:ci]) != k: bad.append(f'{p.name}: key recomputation from the record differs')
        byname[p.stem.upper()] = (k, st)
        where[k].append(p.name)
        if k in meta:
            if (out / 'rec' / f'{k}.rec').read_bytes() != rec: bad.append(f'{k}: same key, different bytes ({p.name})')
            continue
        (out / 'rec' / f'{k}.rec').write_bytes(rec)
        c1o, c1t = struct.unpack_from('<2I', ctl, 0x700)
        xfb = st == 'vs' and any(struct.unpack_from('<4H', ctl, 0x6dc))
        if xfb: xfbs[k] = xfb_layout(ctl)
        own, = struct.unpack_from('<I', ctl, 0x20)
        meta[k] = {'stage': st, 'code': len(code), 'ctrl': len(ctl), 'kl': own if 0x760 <= own < 0x880 else 0x880, 'c1': c1t - c1o, 'src': p.name,
                   'xfb': xfb, 'progs': 0, 'next': set()}
        rep[f'{st} control {len(ctl):#x} c1 {c1t - c1o}'] += 1
    progs = []
    for name, (k, st) in sorted(byname.items()):
        if st == 'cs': progs.append((name, '-', '-', '-', k)); continue
        if st != 'ps': continue
        vn = partner_vs(name)
        if vn not in byname: bad.append(f'{name}: no vertex partner {vn}'); continue
        progs.append((name, byname[vn][0], '-', k, '-'))
    for label, vs, gs, ps, cs in progs:
        for x in (vs, ps, cs):
            if x != '-': meta[x]['progs'] += 1
        if vs != '-': meta[vs]['next'].add(ps)
    with open(out / 'programs.tsv', 'w', newline='\n', encoding='utf-8') as fp:
        fp.write('label\tvs\tgs\tps\tcs\n' + ''.join('\t'.join(r) + '\n' for r in progs))
    with open(out / 'jobs.tsv', 'w', newline='\n', encoding='utf-8') as fp:
        fp.write('key\tstage\tcode_bytes\tctrl_bytes\tctl_key_bytes\tc1_bytes\txfb\tprograms\tnext\tsource\n')
        for k in sorted(meta):
            v = meta[k]
            fp.write(f"{k}\t{v['stage']}\t{v['code']}\t{v['ctrl']}\t{v['kl']}\t{v['c1']}\t{int(v['xfb'])}\t{v['progs']}\t{','.join(sorted(v['next'])) or '-'}\t{v['src']}\n")
    with open(out / 'shader_map.tsv', 'w', newline='\n', encoding='utf-8') as fp:
        fp.write('key\tstage\tfiles\n')
        for k in sorted(meta): fp.write(f"{k}\t{meta[k]['stage']}\t{' | '.join(where[k])}\n")
    dz = os.environ.get('TL2_DEPTH_ZERO', '1') != '0'
    with open(out / 'shader_facts.tsv', 'w', newline='\n') as fp:
        fp.write('# TL2 translation-query facts (tools/tl2_shader_jobs.py): key <TAB> facts JSON merged into the translator job;\n'
                 '# depth_zero: the game runs NVN_DEPTH_MODE_NEAR_IS_ZERO (main+0x199e50c), so vertex shaders keep z unchanged\n')
        if dz:
            for k in sorted(meta):
                if meta[k]['stage'] in ('vs', 'gs'): fp.write(k + '\t' + json.dumps({'depth_zero': True}, separators=(',', ':')) + '\n')
    with open(out / 'xfb.tsv', 'w', newline='\n') as fp:
        fp.write('# vertex shaders whose control section declares transform feedback: key, buffer, stride, location:components...\n')
        for k in sorted(xfbs):
            for b, stride, o in xfbs[k]: fp.write(f'{k}\t{b}\t{stride}\t{" ".join(f"{l}:{c}" for l, c in o)}\n')
    sc = collections.Counter(v['stage'] for v in meta.values())
    lines = [f'source: {src} ({len(files)} .nvn files)',
             f'unique keys: {len(meta)} ({", ".join(f"{s} {n}" for s, n in sorted(sc.items()))}); problems: {len(bad)}',
             f'programs: {len(progs)}; keys in no program: {sum(not v["progs"] for v in meta.values())}',
             f'transform feedback: {" ".join(sorted(xfbs)) or "none"}',
             'depth_zero facts: ' + ('on' if dz else 'OFF (TL2_DEPTH_ZERO=0)')] + \
            [f'  {n:5d}  {k}' for k, n in sorted(rep.items())] + [f'  PROBLEM {b}' for b in bad]
    (out / 'report.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print('\n'.join(lines))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
