package com.xiaoboluo.d2r;

import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.zip.CRC32;
import java.util.zip.Inflater;

// [TL2] Torchlight II PC mod (.MOD) -> Switch data pack PAKS/ZMOD_<NN>_<NAME>.PAK(.MAN), on the device. A port of tools/tl2_mod.py
// "convert" (the reference; docs/tl2/TL2_PORT.md "Моды PC"): both write the same bytes for the same .MOD (tools/tl2_mod_java_test.py
// checks it against the game's 461-texture selftest and a synthetic .MOD). Pure Java (no android.*): the image decoder for PNG / JPG
// is passed in (BitmapFactory on the device, ImageIO in the desktop test); DDS and TGA are decoded here.
//   .MOD  = header v1..4 (u16 ver, u16 modver, [u16 gamever[4]], u32 offData, u32 offMan, wstr title / author / descr / website /
//           download, u64 modid, u32 flags, u64 reqHash, u16 nreq {wstr, u64, [u16]}, [u16 ndel {wstr}]) + a TL2 PAK at offData
//           ({u32 size, u32 packed, zlib | raw} per file, MAN offsets relative to offData) + a PAK.MAN at offMan. wstr = u16 n + UTF-16LE.
//   Output: the mod's files as they are (packed bytes copied), textures (.DDS .PNG .JPG .TGA) converted to the Switch XTX and stored
//           unpacked, deleted paths as zero entries; PAK header = largest packed size + the TL2 pack hash; MAN v2 root "MEDIA/".
final class Tl2ModConv {
    private Tl2ModConv() {}

    interface ImageDecoder { /** -> {w, h, pixels...} as non-premultiplied ARGB ints, or null */ int[] decode(byte[] data); }

    static final class Info {
        int format, modver; String gamever = "", title = "", author = "", descr = "", website = "", download = "";
        long modid; int flags; long reqHash; final List<String> requires = new ArrayList<>(), deletes = new ArrayList<>();
        long offData, offMan;
    }
    static final class Entry { String dir, name; int type; long off, size; int crc; long ft; Entry(String d, String n, int t, long o, long s, int c, long f) { dir = d; name = n; type = t; off = o; size = s; crc = c; ft = f; } }
    static final class Result { String pack; int files, textures, converted, deleted, skilltrees, icons; String iconError; final List<String> failed = new ArrayList<>(), iconNotes = new ArrayList<>(); }

    // ------------------------------------------------------------ little-endian reader over a byte[]
    private static final class Rd {
        final byte[] b; int o; Rd(byte[] b, int o) { this.b = b; this.o = o; }
        int u8() { return b[o++] & 0xff; }
        int u16() { int v = (b[o] & 0xff) | (b[o + 1] & 0xff) << 8; o += 2; return v; }
        long u32() { long v = (b[o] & 0xffL) | (b[o + 1] & 0xffL) << 8 | (b[o + 2] & 0xffL) << 16 | (b[o + 3] & 0xffL) << 24; o += 4; return v; }
        long u64() { long lo = u32(), hi = u32(); return lo | hi << 32; }
        String ws() { int n = u16(); String s = new String(b, o, 2 * n, StandardCharsets.UTF_16LE); o += 2 * n; return s; }
    }
    static int le32(byte[] b, int o) { return (b[o] & 0xff) | (b[o + 1] & 0xff) << 8 | (b[o + 2] & 0xff) << 16 | (b[o + 3] & 0xff) << 24; }

    /** The header of a .MOD (the first bytes of the file are enough: 64 KiB covers any real header). */
    static Info readInfo(byte[] head) throws IOException {
        try {
            Rd r = new Rd(head, 0); Info i = new Info();
            i.format = r.u16(); if (i.format < 1 || i.format > 4) throw new IOException("not a TL2 .MOD (header version " + i.format + ")");
            i.modver = r.u16();
            if (i.format >= 4) i.gamever = r.u16() + "." + r.u16() + "." + r.u16() + "." + r.u16();
            if (i.format == 1) r.o += 9 * (i.modver + 1);
            i.offData = r.u32(); i.offMan = r.u32();
            i.title = r.ws(); i.author = r.ws(); i.descr = r.ws(); i.website = r.ws(); i.download = r.ws();
            i.modid = r.u64(); i.flags = (int) r.u32(); i.reqHash = r.u64();
            int nreq = r.u16();
            for (int k = 0; k < nreq; k++) { String n = r.ws(); r.u64(); if (i.format != 1) r.u16(); i.requires.add(n); }
            if (i.format >= 3) { int nd = r.u16(); for (int k = 0; k < nd; k++) i.deletes.add(r.ws()); }
            return i;
        } catch (ArrayIndexOutOfBoundsException | StringIndexOutOfBoundsException e) { throw new IOException("truncated .MOD header"); }
    }
    static Info readInfo(File f) throws IOException {
        try (RandomAccessFile raf = new RandomAccessFile(f, "r")) {
            byte[] h = new byte[(int) Math.min(raf.length(), 1 << 16)]; raf.readFully(h); return readInfo(h);
        }
    }

    static List<Entry> manRead(byte[] b) {
        Rd r = new Rd(b, 0); int ver = r.u16(); if (ver >= 2) r.u32();
        r.ws(); r.u32(); long nd = r.u32(); List<Entry> out = new ArrayList<>();
        for (long i = 0; i < nd; i++) {
            String d = r.ws(); long n = r.u32();
            for (long j = 0; j < n; j++) { int crc = (int) r.u32(); int t = r.u8(); String nm = r.ws(); long off = r.u32(), size = r.u32(), ft = r.u64(); out.add(new Entry(d, nm, t, off, size, crc, ft)); }
        }
        return out;
    }

    // ------------------------------------------------------------ little-endian writer
    private static final class Wr {
        byte[] b = new byte[256]; int n;
        void need(int k) { if (n + k > b.length) b = java.util.Arrays.copyOf(b, Math.max(b.length * 2, n + k)); }
        Wr u8(int v) { need(1); b[n++] = (byte) v; return this; }
        Wr u16(int v) { need(2); b[n++] = (byte) v; b[n++] = (byte) (v >> 8); return this; }
        Wr u32(long v) { need(4); for (int i = 0; i < 4; i++) b[n++] = (byte) (v >> (8 * i)); return this; }
        Wr u64(long v) { u32(v); return u32(v >>> 32); }
        Wr raw(byte[] x) { need(x.length); System.arraycopy(x, 0, b, n, x.length); n += x.length; return this; }
        Wr ws(String s) { byte[] e = s.getBytes(StandardCharsets.UTF_16LE); u16(e.length / 2); return raw(e); }
        byte[] bytes() { return java.util.Arrays.copyOf(b, n); }
    }

    static byte[] manWrite(String root, List<Entry> ents) {
        LinkedHashMap<String, List<Entry>> dirs = new LinkedHashMap<>();
        for (Entry e : ents) dirs.computeIfAbsent(e.dir, k -> new ArrayList<>()).add(e);
        Wr w = new Wr(); w.u16(2).u32(0).ws(root).u32(ents.size() + dirs.size()).u32(dirs.size());
        for (Map.Entry<String, List<Entry>> d : dirs.entrySet()) {
            w.ws(d.getKey()).u32(d.getValue().size());
            for (Entry e : d.getValue()) w.u32(e.crc).u8(e.type).ws(e.name).u32(e.off).u32(e.size).u64(e.ft);
        }
        return w.bytes();
    }

    /** TL2 PAK header hash over the whole pack file (sampling starts after the 8-byte header). */
    static int pakHash(RandomAccessFile f) throws IOException {
        long n = f.length(); if (n == 0) return 0;
        long seed = 25 + ((((n >>> 32) + (n & 0xffffffffL) * 0x29777B41L) & 0xffffffffL) % 51);
        long step = Math.max(2, n / Math.min(seed, 75)); long h = n;
        for (long o = 8; o < n; o += step) { f.seek(o); h = (h * 33 + (byte) f.read()) & 0xffffffffL; }
        f.seek(n - 1); return (int) ((h * 33 + (byte) f.read()) & 0xffffffffL);
    }

    // ------------------------------------------------------------ XTX (Switch textures)
    static final int BC1 = 0x42, BC2 = 0x43, BC3 = 0x44, RGBA8 = 0x25;
    static int bpb(int f) { return f == BC1 ? 8 : f == RGBA8 ? 4 : 16; }
    static int bsz(int f) { return f == RGBA8 ? 1 : 4; }
    static int cdiv(int a, int b) { return (a + b - 1) / b; }
    static int bh0(int rows) { int g = cdiv(rows, 8), bh = 1; while (bh < g && bh < 16) bh *= 2; return bh; }
    /** per level {offset, size, widthBlocks, heightBlocks, blockHeight}; [n][0] of the extra row = total */
    static int[][] levels(int w, int h, int f, int mips) {
        int bp = bpb(f), bs = bsz(f), b0 = bh0(cdiv(h, bs)); int[][] out = new int[mips + 1][]; int off = 0;
        for (int i = 0; i < mips; i++) {
            int lw = Math.max(1, w >> i), lh = Math.max(1, h >> i), wb = cdiv(lw, bs), hb = cdiv(lh, bs), bh = b0;
            while (bh > 1 && hb <= (bh / 2) * 8) bh /= 2;
            int size = cdiv(wb * bp, 64) * 64 * cdiv(hb, 8 * bh) * 8 * bh;
            out[i] = new int[] {off, size, wb, hb, bh}; off += size;
        }
        out[mips] = new int[] {off}; return out;
    }
    static void swizzle(byte[] lin, int linOff, byte[] dst, int dstOff, int rowBytes, int rows, int bh) {
        int pitch = cdiv(rowBytes, 64);
        for (int y = 0; y < rows; y++) {
            int rowBase = dstOff + (y / (8 * bh)) * (pitch * bh * 512) + ((y % (8 * bh)) / 8) * 512 + ((y % 8) / 2) * 64 + (y % 2) * 16;
            for (int x = 0; x < rowBytes; x += 16) {
                int o = rowBase + (x / 64) * (bh * 512) + ((x % 64) / 32) * 256 + ((x % 32) / 16) * 32;
                System.arraycopy(lin, linOff + y * rowBytes + x, dst, o, Math.min(16, rowBytes - x));
            }
        }
    }
    private static byte[] blk(int type, int gid, byte[] p) { return new Wr().raw(new byte[] {'H', 'B', 'v', 'N'}).u32(0x24).u64(p.length).u64(0x24).u32(type).u32(gid).u32(0).raw(p).bytes(); }
    /** levels: linear data of each mip level (block rows, tightly packed); pf = the Ogre pixel format block (-1 = default) */
    static byte[] xtxBuild(byte[][] lv, int w, int h, int f, int pf) {
        if (pf < 0) {
            if (f == BC1) pf = 17; else if (f == BC2) pf = 19; else if (f == BC3) pf = 21;
            else { pf = 11; byte[] a = lv[0]; for (int i = 3; i < a.length; i += 4) if ((a[i] & 0xff) != 255) { pf = 12; break; } }
        }
        int[][] L = levels(w, h, f, lv.length); int total = L[lv.length][0]; byte[] img = new byte[total];
        for (int i = 0; i < lv.length; i++) swizzle(lv[i], 0, img, L[i][0], L[i][2] * bpb(f), L[i][3], L[i][4]);
        Wr info = new Wr().u64(total).u32(0x200).u32(w).u32(h).u32(1).u32(1).u32(f).u32(lv.length).u32(total);
        for (int i = 0; i < 17; i++) info.u32(i < lv.length ? L[i][0] : 0);
        info.u32(Integer.numberOfTrailingZeros(L[0][4])).u32(0x10007).u32(0);
        byte[] tail = new Wr().u64(0).u32(1).u32(1).u32(1).u32(0).bytes();
        return new Wr().raw(new byte[] {'D', 'F', 'v', 'N'}).u32(0x10).u32(1).u32(1).raw(blk(2, 0, info.bytes())).raw(blk(3, 1, img))
            .raw(blk(4, 2, new Wr().u32(pf).bytes())).raw(blk(5, 3, tail)).bytes();
    }

    static byte[] ddsToXtx(byte[] b) throws IOException {
        if (b.length < 128 || le32(b, 0) != 0x20534444) throw new IOException("not DDS");
        int flags = le32(b, 8), h = le32(b, 12), w = le32(b, 16), mips = le32(b, 28);
        int pff = le32(b, 80), fourcc = le32(b, 84), bits = le32(b, 88), rm = le32(b, 92), gm = le32(b, 96), bm = le32(b, 100), am = le32(b, 104);
        int caps2 = le32(b, 112);
        if ((caps2 & 0x200) != 0 || (flags & 0x800000) != 0) throw new IOException("cube / volume DDS not supported");
        mips = Math.max(1, (flags & 0x20000) != 0 ? mips : 1);
        int f; byte[] data; int dataOff = 128;
        if ((pff & 4) != 0) {
            String fc = new String(new byte[] {(byte) fourcc, (byte) (fourcc >> 8), (byte) (fourcc >> 16), (byte) (fourcc >> 24)}, StandardCharsets.ISO_8859_1);
            switch (fc) { case "DXT1": f = BC1; break; case "DXT2": case "DXT3": f = BC2; break; case "DXT4": case "DXT5": f = BC3; break; default: throw new IOException("DDS fourcc " + fc + " not supported"); }
            data = b;
        } else if ((pff & 0x40) != 0 && (bits == 24 || bits == 32)) {
            f = RGBA8; int bpp = bits / 8; int ri = mbyte(rm), gi = mbyte(gm), bi = mbyte(bm); boolean al = (pff & 1) != 0 && am != 0; int ai = al ? mbyte(am) : 0;
            if (ri < 0 || gi < 0 || bi < 0 || ri >= bpp || gi >= bpp || bi >= bpp || (al && (ai < 0 || ai >= bpp))) throw new IOException("DDS masks not supported");
            int n = (b.length - 128) / bpp; data = new byte[n * 4]; dataOff = 0;
            for (int i = 0; i < n; i++) { int s = 128 + i * bpp; data[i * 4] = b[s + ri]; data[i * 4 + 1] = b[s + gi]; data[i * 4 + 2] = b[s + bi]; data[i * 4 + 3] = al ? b[s + ai] : (byte) 255; }
        } else throw new IOException("DDS pixel format flags 0x" + Integer.toHexString(pff) + " / " + bits + " bits not supported");
        int bp = bpb(f), bs = bsz(f); List<byte[]> lv = new ArrayList<>(); int o = dataOff;
        for (int i = 0; i < mips; i++) {
            int n = cdiv(Math.max(1, w >> i), bs) * cdiv(Math.max(1, h >> i), bs) * bp;
            if (o + n > data.length) break;
            lv.add(java.util.Arrays.copyOfRange(data, o, o + n)); o += n;
        }
        if (lv.isEmpty()) throw new IOException("DDS data truncated");
        return xtxBuild(lv.toArray(new byte[0][]), w, h, f, -1);
    }
    private static int mbyte(int m) { return m == 0xff ? 0 : m == 0xff00 ? 1 : m == 0xff0000 ? 2 : m == 0xff000000 ? 3 : -1; }

    /** TGA types 1/2/3 and RLE 9/10/11 -> {w, h} + RGBA rows top first (same rules as tools/tl2_mod.py tga_decode) */
    static byte[] tgaDecode(byte[] b, int[] wh) throws IOException {
        try {
            int idl = b[0] & 0xff, cmt = b[1] & 0xff, it = b[2] & 0xff, c0 = (b[3] & 0xff) | (b[4] & 0xff) << 8, clen = (b[5] & 0xff) | (b[6] & 0xff) << 8, csz = b[7] & 0xff;
            int w = (b[12] & 0xff) | (b[13] & 0xff) << 8, h = (b[14] & 0xff) | (b[15] & 0xff) << 8, bpp = b[16] & 0xff, desc = b[17] & 0xff, base = it & 7;
            if ((base != 1 && base != 2 && base != 3) || w == 0 || h == 0) throw new IOException("TGA type " + it + " not supported");
            int o = 18 + idl; int[] pal = null;
            if (cmt == 1) { int es = (csz + 7) / 8; pal = new int[clen]; for (int i = 0; i < clen; i++) pal[i] = tgaPx(b, o + i * es, csz, desc); o += clen * es; }
            int ps = (bpp + 7) / 8, n = w * h; byte[] raw;
            if ((it & 8) != 0) {
                raw = new byte[n * ps]; int k = 0;
                while (k < raw.length) {
                    int hd = b[o++] & 0xff, c = (hd & 0x7f) + 1;
                    if ((hd & 0x80) != 0) { for (int j = 0; j < c && k < raw.length; j++) for (int q = 0; q < ps && k < raw.length; q++) raw[k++] = b[o + q]; o += ps; }
                    else { int m = Math.min(c * ps, raw.length - k); System.arraycopy(b, o, raw, k, m); k += m; o += c * ps; }
                }
            } else { if (o + n * ps > b.length) throw new IOException("TGA data truncated"); raw = java.util.Arrays.copyOfRange(b, o, o + n * ps); }
            byte[] out = new byte[n * 4];
            for (int i = 0; i < n; i++) {
                int s = i * ps, q;
                if (base == 1) { int k = ((raw[s] & 0xff) | (ps > 1 ? (raw[s + 1] & 0xff) << 8 : 0)) - c0; q = pal != null && k >= 0 && k < pal.length ? pal[k] : 0xff000000; }
                else if (base == 3) { int g = raw[s] & 0xff; q = (ps > 1 ? (raw[s + 1] & 0xff) : 255) << 24 | g << 16 | g << 8 | g; }
                else q = tgaPx(raw, s, bpp, desc);
                int y = i / w, x = i % w; if ((desc & 0x10) != 0) x = w - 1 - x; if ((desc & 0x20) == 0) y = h - 1 - y;
                int d = (y * w + x) * 4; out[d] = (byte) (q >> 16); out[d + 1] = (byte) (q >> 8); out[d + 2] = (byte) q; out[d + 3] = (byte) (q >>> 24);
            }
            wh[0] = w; wh[1] = h; return out;
        } catch (ArrayIndexOutOfBoundsException e) { throw new IOException("TGA data truncated"); }
    }
    private static int tgaPx(byte[] c, int o, int sz, int desc) throws IOException {   // -> ARGB
        if (sz == 15 || sz == 16) {
            int v = (c[o] & 0xff) | (c[o + 1] & 0xff) << 8, r = (v >> 10) & 31, g = (v >> 5) & 31, bl = v & 31;
            int a = sz == 15 || (desc & 15) == 0 || (v & 0x8000) != 0 ? 255 : 0;
            return a << 24 | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (bl << 3 | bl >> 2);
        }
        if (sz == 24) return 0xff000000 | (c[o + 2] & 0xff) << 16 | (c[o + 1] & 0xff) << 8 | (c[o] & 0xff);
        if (sz == 32) return (c[o + 3] & 0xff) << 24 | (c[o + 2] & 0xff) << 16 | (c[o + 1] & 0xff) << 8 | (c[o] & 0xff);
        throw new IOException("TGA pixel size " + sz + " not supported");
    }

    static boolean isTexture(String name) { String u = name.toUpperCase(Locale.ROOT); return u.endsWith(".DDS") || u.endsWith(".PNG") || u.endsWith(".JPG") || u.endsWith(".TGA"); }
    /** -> the Switch texture, or null when the bytes already are one (XTX) */
    static byte[] toSwitchTexture(String name, byte[] d, ImageDecoder img, int[] kind) throws IOException {
        if (d.length >= 4 && le32(d, 0) == 0x4e764644) { kind[0] = 0; return null; }   // 'DFvN'
        if (d.length >= 4 && le32(d, 0) == 0x20534444) { kind[0] = 1; return ddsToXtx(d); }
        kind[0] = 2;
        boolean png = d.length >= 8 && (d[0] & 0xff) == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G', jpg = d.length >= 3 && (d[0] & 0xff) == 0xff && (d[1] & 0xff) == 0xd8 && (d[2] & 0xff) == 0xff;
        if (name.toUpperCase(Locale.ROOT).endsWith(".TGA") && !png && !jpg) { int[] wh = new int[2]; byte[] px = tgaDecode(d, wh); return xtxBuild(new byte[][] {px}, wh[0], wh[1], RGBA8, -1); }
        int[] p = img == null ? null : img.decode(d);
        if (p == null) throw new IOException("image not decodable");
        int w = p[0], h = p[1]; byte[] px = new byte[w * h * 4];
        for (int i = 0; i < w * h; i++) { int c = p[2 + i]; px[i * 4] = (byte) (c >> 16); px[i * 4 + 1] = (byte) (c >> 8); px[i * 4 + 2] = (byte) c; px[i * 4 + 3] = (byte) (c >>> 24); }
        return xtxBuild(new byte[][] {px}, w, h, RGBA8, -1);
    }

    static String slug(String s) {
        String u = s.toUpperCase(Locale.ROOT).replaceAll("[^A-Z0-9]+", "_");
        int a = 0, e = u.length(); while (a < e && u.charAt(a) == '_') a++; while (e > a && u.charAt(e - 1) == '_') e--;
        u = u.substring(a, e); if (u.length() > 40) u = u.substring(0, 40);
        return u.isEmpty() ? "MOD" : u;
    }
    static String packName(int slot, Info info, File mod) {
        String t = info.title.isEmpty() ? mod.getName().replaceFirst("\\.[^.]*$", "") : info.title;
        return String.format(Locale.ROOT, "ZMOD_%02d_%s", slot, slug(t));
    }

    // ------------------------------------------------------------ skill trees of PC-mod classes (tools/tl2_mod.py skilltrees_text)
    // The console never reads a class's PC skill tree layout (UISKILLTREE); its trees go into the pack as MEDIA/PORT/SKILLTREES.TXT for
    // dm/host/hle_tl2_skilltree.c. Same rules and bytes as the Python reference (docs/tl2/TL2_PORT.md "Дерево навыков классов из модов").
    static long tl2Hash(String s) { long h = s.length(); for (int i = 0; i < s.length(); i++) h = ((h >>> 27) ^ (h << 5) ^ s.charAt(i)) & 0xffffffffL; return h; }
    static final long K_NAME = tl2Hash("NAME"), K_BASEFILE = tl2Hash("BASEFILE"), K_UISKILLTREE = tl2Hash("UISKILLTREE");
    static final int ST_TREES = 12;   // the console skill table: 120 rows = 12 trees x 10 (tools/tl2_mod.py ST_TREES)
    static final String[][] ST_STOCK = {   // the stock classes' UISKILLTREE
        {"HUM_BERSERKER_BASE", "MEDIA/UI/MENUS/SKILLTREEOVERRIDES/SKILL_TREE_BERSERKER.LAYOUT"}, {"HUM_ARBITER_BASE", "MEDIA/UI/MENUS/SKILLTREEOVERRIDES/SKILL_TREE_ARBITER.LAYOUT"},
        {"HUM_ENGINEER_BASE", "MEDIA/UI/MENUS/SKILLTREEOVERRIDES/SKILL_TREE_RAILMAN.LAYOUT"}, {"HUM_WANDERER_BASE", "MEDIA/UI/MENUS/SKILLTREEOVERRIDES/SKILL_TREE_OUTLANDER.LAYOUT"}};
    static String up(String s) { StringBuilder b = new StringBuilder(s.length()); for (int i = 0; i < s.length(); i++) { char c = s.charAt(i); b.append(c >= 'a' && c <= 'z' ? (char) (c - 32) : c); } return b.toString(); }
    static String lo(String s) { StringBuilder b = new StringBuilder(s.length()); for (int i = 0; i < s.length(); i++) { char c = s.charAt(i); b.append(c >= 'A' && c <= 'Z' ? (char) (c + 32) : c); } return b.toString(); }
    static String clean(String s) {   // tabs / line breaks -> spaces, ASCII spaces trimmed
        String t = s.replace('\t', ' ').replace('\r', ' ').replace('\n', ' '); int a = 0, e = t.length();
        while (a < e && t.charAt(a) == ' ') a++; while (e > a && t.charAt(e - 1) == ' ') e--; return t.substring(a, e);
    }

    /** TL2 binary DAT v2 -> string values of the root block by key hash, or null */
    static Map<Long, String> datTop(byte[] b) {
        try {
            Rd r = new Rd(b, 0); if (r.u32() != 2) return null; long n = r.u32(); Map<Long, String> st = new java.util.HashMap<>();
            for (long i = 0; i < n; i++) {
                long h = r.u32(); int ln = r.u16(); if (r.o + 2L * ln > b.length) return null;
                st.put(h, new String(b, r.o, 2 * ln, StandardCharsets.UTF_16LE)); r.o += 2 * ln;
            }
            r.u32(); long np = r.u32(); Map<Long, String> out = new java.util.HashMap<>();
            for (long i = 0; i < np; i++) {
                long k = r.u32(), t = r.u32();
                if (t == 5 || t == 8) { String v = st.get(r.u32()); out.put(k, v == null ? "" : v); }
                else if (t == 1 || t == 2 || t == 4 || t == 6) r.u32();
                else if (t == 3 || t == 7) r.u64();
                else return null;
            }
            return out;
        } catch (ArrayIndexOutOfBoundsException e) { return null; }
    }

    static final class LObj { final int type, parent; final String name; final Map<Integer, byte[]> props; LObj(int t, String n, Map<Integer, byte[]> p, int par) { type = t; name = n; props = p; parent = par; } }
    /** TL2 binary LAYOUT (version 0x0b) -> objects in file order (parent = index, -1 for roots), or null */
    static List<LObj> layoutObjects(byte[] b) {
        if (b.length < 8 || (b[0] & 0xff) != 0x0b) return null;
        List<LObj> objs = new ArrayList<>();
        try { Rd r = new Rd(b, 6); int n = r.u16(); for (int i = 0; i < n; i++) layoutObj(b, r, -1, objs); }
        catch (ArrayIndexOutOfBoundsException | IOException e) { return null; }
        return objs;
    }
    private static void layoutObj(byte[] b, Rd r, int parent, List<LObj> objs) throws IOException {
        long st = r.o, size = r.u32(); int type = r.u8(); r.o += 8;
        int n = r.u16(); if (r.o + 2L * n > b.length) throw new IOException("string past the end");
        String name = new String(b, r.o, 2 * n, StandardCharsets.UTF_16LE); r.o += 2 * n;
        int np = r.u8(); Map<Integer, byte[]> props = new java.util.HashMap<>();
        for (int i = 0; i < np; i++) {
            int sz = r.u16(), pid = r.u8(); if (sz < 1 || r.o - 3 + 2L + sz > b.length) throw new IOException("bad property");
            props.put(pid, java.util.Arrays.copyOfRange(b, r.o, r.o + sz - 1)); r.o += sz - 1;
        }
        long ex = r.u32(); if (r.o + ex > b.length) throw new IOException("extra block past the end"); r.o += (int) ex;
        int me = objs.size(); objs.add(new LObj(type, name, props, parent));
        int nc = r.u16(); for (int i = 0; i < nc; i++) layoutObj(b, r, me, objs);
        if (r.o != st + size) throw new IOException("object size mismatch");
    }
    private static String pws(byte[] v) {
        if (v == null || v.length < 2) return null; int n = (v[0] & 0xff) | (v[1] & 0xff) << 8;
        return 2 + 2 * n == v.length ? new String(v, 2, 2 * n, StandardCharsets.UTF_16LE) : null;
    }
    private static double pf(byte[] v) { return v != null && v.length == 4 ? Float.intBitsToFloat(le32(v, 0)) : 0.0; }
    private static int num(String name, String pre) {
        String u = up(name); if (!u.startsWith(pre)) return 0; String r = u.substring(pre.length());
        if (r.length() < 1 || r.length() > 3) return 0; for (int i = 0; i < r.length(); i++) if (r.charAt(i) < '0' || r.charAt(i) > '9') return 0;
        return Integer.parseInt(r);
    }
    private static final java.util.regex.Pattern TAB_ICON = java.util.regex.Pattern.compile("IG_SKILLTABS_(.+?)_(OFF|ON|ROLLOVER)");
    private static final int PANE_HIDDEN = 0x29, TAB_LABEL = 0x23, TAB_ICON_OFF = 0x41;
    private static final class Row { final double y, x; final int idx; final String name; Row(double y, double x, int i, String n) { this.y = y; this.x = x; idx = i; name = n; } }
    static final class Trees { final List<String> lines = new ArrayList<>(); int nskills; }
    private static String[] tabLabel(Map<Integer, byte[]> props) {   // tools/tl2_mod.py _tab_label
        String icon = pws(props.getOrDefault(TAB_ICON_OFF, new byte[0])); if (icon == null) icon = "";
        java.util.regex.Matcher m = TAB_ICON.matcher(up(icon)); String lb = pws(props.getOrDefault(TAB_LABEL, new byte[0]));
        return new String[] {clean(lb == null ? "" : lb), m.matches() ? lo(m.group(1)) : "-"};
    }
    /** the skill windows sk (ascending) -> their containers under the ancestor all of them share, in file order; empty = nothing to group
     *  by. Mods that renamed the stock SkillPane-N containers (SYNERGIES) are grouped by this. tools/tl2_mod.py _pane_groups */
    private static java.util.TreeMap<Integer, List<Integer>> paneGroups(List<LObj> objs, List<Integer> sk) {
        java.util.TreeMap<Integer, List<Integer>> g = new java.util.TreeMap<>();
        Map<Integer, List<Integer>> anc = new java.util.HashMap<>(); List<Integer> common = null;
        for (int i : sk) {
            List<Integer> ch = new ArrayList<>();
            for (int p = objs.get(i).parent; p >= 0; p = objs.get(p).parent) ch.add(p);
            anc.put(i, ch);
            if (common == null) common = new ArrayList<>(ch); else common.retainAll(ch);
        }
        if (common == null || common.isEmpty()) return g;
        int lca = common.get(0);
        for (int i : sk) {
            List<Integer> ch = anc.get(i); int k = ch.indexOf(lca);
            if (k <= 0) { g.clear(); return g; }
            g.computeIfAbsent(ch.get(k - 1), x -> new ArrayList<>()).add(i);
        }
        return g;
    }
    private static final class PaneOrder { final List<Integer> order = new ArrayList<>(); boolean byName; }
    /** the pane containers in the display order of the tabs disp; tools/tl2_mod.py _pane_order */
    private static PaneOrder paneOrder(List<LObj> objs, List<Integer> groups, List<Integer> disp) {
        PaneOrder r = new PaneOrder();
        Map<String, Integer> byName = new java.util.HashMap<>(); boolean named = true;
        for (int g : groups) {
            String n = up(objs.get(g).name);
            if (n.isEmpty() || byName.containsKey(n)) { named = false; break; }
            byName.put(n, g);
        }
        List<String> tn = new ArrayList<>();
        for (int i : disp) tn.add(up(objs.get(i).name));
        boolean pair = named && !tn.isEmpty() && tn.size() == groups.size();
        if (pair) for (String n : tn) if (n.isEmpty() || !byName.containsKey(n)) { pair = false; break; }
        if (pair) { for (String n : tn) r.order.add(byName.get(n)); r.byName = true; return r; }
        int shown = 0, nshown = 0;
        for (int k = 0; k < groups.size(); k++) if (!objs.get(groups.get(k)).props.containsKey(PANE_HIDDEN)) { shown = k; nshown++; }
        int from = nshown == 1 ? shown : 0;
        for (int k = 0; k < groups.size(); k++) r.order.add(groups.get((from + k) % groups.size()));
        return r;
    }
    private static List<Integer> tabsByX(List<LObj> objs) {   // the skill tabs left to right (prop 0x3f), file order on a tie
        List<Integer> d = new ArrayList<>();
        for (int i = 0; i < objs.size(); i++) {
            LObj o = objs.get(i);
            if (o.type == 0x58 && (o.props.containsKey(TAB_LABEL) || o.props.containsKey(TAB_ICON_OFF))) d.add(i);
        }
        d.sort((a, c) -> {
            double xa = pf(objs.get(a).props.get(0x3f)), xc = pf(objs.get(c).props.get(0x3f));
            return xa < xc ? -1 : xa > xc ? 1 : Integer.compare(a, c);
        });
        return d;
    }
    /** -> T / S / X lines (without the layout index) or null; tools/tl2_mod.py layout_trees */
    static Trees layoutTrees(byte[] b) {
        List<LObj> objs = layoutObjects(b); if (objs == null) return null;
        java.util.TreeMap<Integer, String[]> tabs = new java.util.TreeMap<>(); java.util.TreeMap<Integer, List<Row>> panes = new java.util.TreeMap<>();
        java.util.TreeMap<Integer, String> loose = new java.util.TreeMap<>();   // every skill window by object index, for the name-free grouping
        for (int i = 0; i < objs.size(); i++) {
            LObj o = objs.get(i);
            int n = num(o.name, "TAB");
            if (o.type == 0x58 && n != 0 && !tabs.containsKey(n)) tabs.put(n, tabLabel(o.props));
            if (o.type == 0x3d && o.props.containsKey(0x40) && o.props.containsKey(0x7b)) {
                String sk = pws(o.props.get(0x40)); int p = o.parent, pane = 0; boolean inside = false;
                while (p >= 0) {
                    LObj q = objs.get(p);
                    if (q.type == 0x3d && q.props.containsKey(0x40) && q.props.containsKey(0x7b)) { inside = true; break; }
                    pane = num(q.name, "SKILLPANE"); if (pane != 0) break;
                    p = q.parent;
                }
                if (sk != null && !sk.isEmpty() && !inside) loose.put(i, clean(sk));
                if (sk != null && !sk.isEmpty() && pane != 0 && !inside) {
                    List<Row> l = panes.computeIfAbsent(pane, k -> new ArrayList<>());
                    l.add(new Row(pf(o.props.get(0x22)), pf(o.props.get(0x3f)), l.size(), clean(sk)));
                }
            }
        }
        String fallbackNote = null;
        if (panes.isEmpty() && !loose.isEmpty()) {
            java.util.TreeMap<Integer, List<Integer>> groups = paneGroups(objs, new ArrayList<>(loose.keySet()));
            if (!groups.isEmpty()) {
                List<Integer> disp = tabsByX(objs);
                PaneOrder po = paneOrder(objs, new ArrayList<>(groups.keySet()), disp);
                tabs = new java.util.TreeMap<>();
                for (int k = 0; k < disp.size(); k++) tabs.put(k + 1, tabLabel(objs.get(disp.get(k)).props));
                for (int k = 0; k < po.order.size(); k++) {
                    List<Integer> ids = groups.get(po.order.get(k)); List<Row> l = new ArrayList<>();
                    for (int j = 0; j < ids.size(); j++) {
                        LObj o = objs.get(ids.get(j));
                        l.add(new Row(pf(o.props.get(0x22)), pf(o.props.get(0x3f)), j, loose.get(ids.get(j))));
                    }
                    panes.put(k + 1, l);
                }
                if (!po.byName && disp.size() > 1)
                    fallbackNote = "X\t%\t" + po.order.size() + " skill pane(s) with no SkillPane<N> name: paired with the tabs in file order from the "
                        + "first shown one, so a tab label may belong to another pane";
            }
        }
        Trees t = new Trees(); java.util.TreeSet<Integer> all = new java.util.TreeSet<>(tabs.keySet()); all.addAll(panes.keySet());
        List<String> notes = new ArrayList<>(), sl = new ArrayList<>(); List<String[]> over = new ArrayList<>(); int nt = 0;   // over: {tree, label, icon, skill}
        if (fallbackNote != null) notes.add(fallbackNote);
        for (int n : all) {
            List<Row> rows = new ArrayList<>(panes.getOrDefault(n, new ArrayList<>()));
            if (nt >= ST_TREES) { String[] tb = tabs.get(n); notes.add("X\t%\ttree " + n + " (" + (tb == null || tb[0].isEmpty() ? "no tab" : tb[0]) + ", " + rows.size() + " skills) dropped: no free tree slot"); continue; }
            String[] tb = tabs.getOrDefault(n, new String[] {"", "-"}); int tr = nt++;
            String label = tb[0].isEmpty() ? "Skills " + n : tb[0], icon = tb[1].isEmpty() ? "-" : tb[1];
            t.lines.add("T\t%\t" + tr + "\t" + label + "\t" + icon);
            if (rows.isEmpty()) continue;
            rows.sort((a, c) -> a.y < c.y ? -1 : a.y > c.y ? 1 : a.x < c.x ? -1 : a.x > c.x ? 1 : Integer.compare(a.idx, c.idx));
            double ymax = rows.get(rows.size() - 1).y; boolean multi = false; for (Row r : rows) if (r.y != ymax) multi = true;
            List<Row> pas = new ArrayList<>(), act = new ArrayList<>();
            if (multi) for (Row r : rows) if (r.y == ymax) pas.add(r);
            if (pas.size() > 3) pas.clear();
            for (Row r : rows) if (!pas.contains(r)) act.add(r);
            if (act.size() + pas.size() > 10) { for (int i = 7; i < act.size(); i++) over.add(new String[] {Integer.toString(tr), label, icon, act.get(i).name}); act = new ArrayList<>(act.subList(0, 7)); }
            for (int s = 0; s < act.size(); s++) sl.add("S\t%\t" + tr + "\t" + s + "\t" + act.get(s).name);
            for (int s = 0; s < pas.size(); s++) sl.add("S\t%\t" + tr + "\t" + (10 - pas.size() + s) + "\t" + pas.get(s).name);
        }
        for (int o = 0; o < over.size(); o += 10) {   // overflow trees: "<tree> II" from one tree, else "More Skills"; 10 each
            if (nt >= ST_TREES) { for (int i = o; i < over.size(); i++) notes.add("X\t%\t" + over.get(i)[3] + " (tree " + over.get(i)[0] + ") dropped: no free tree slot"); break; }
            List<String[]> chunk = over.subList(o, Math.min(over.size(), o + 10)); boolean one = true;
            for (String[] c : chunk) if (!c[0].equals(chunk.get(0)[0])) one = false;
            int tr = nt++;
            t.lines.add("T\t%\t" + tr + "\t" + (one ? chunk.get(0)[1] + " II" : "More Skills") + "\t" + (one ? chunk.get(0)[2] : "-"));
            for (int s = 0; s < chunk.size(); s++) sl.add("S\t%\t" + tr + "\t" + s + "\t" + chunk.get(s)[3]);
        }
        t.nskills = sl.size(); t.lines.addAll(sl); t.lines.addAll(notes); return t;
    }
    private static String norm(String s) { String u = up(s.replace('\\', '/')); int i = 0; while (i < u.length() && u.charAt(i) == '/') i++; return u.substring(i); }
    /** files: UPPER path -> bytes (player DATs, UI layouts) -> MEDIA/PORT/SKILLTREES.TXT, or null */
    static byte[] skilltreesText(Map<String, byte[]> files) {
        java.util.TreeMap<String, Map<Long, String>> dats = new java.util.TreeMap<>();
        for (Map.Entry<String, byte[]> f : files.entrySet())
            if (f.getKey().startsWith("MEDIA/UNITS/PLAYERS/") && f.getKey().endsWith(".DAT")) { Map<Long, String> t = datTop(f.getValue()); if (t != null) dats.put(f.getKey(), t); }
        Map<String, Trees> lays = new java.util.HashMap<>(); List<String[]> units = new ArrayList<>();
        for (Map.Entry<String, Map<Long, String>> d : dats.entrySet()) {
            Map<Long, String> t = d.getValue(); String name = t.getOrDefault(K_NAME, ""), tree = ""; int seen = 0;
            while (t != null && seen < 8) {
                String v = t.get(K_UISKILLTREE); if (v != null && !v.isEmpty()) { tree = norm(v); break; }
                t = dats.get(norm(t.getOrDefault(K_BASEFILE, ""))); seen++;
            }
            if (!name.isEmpty() && !tree.isEmpty() && files.containsKey(tree)) {
                if (!lays.containsKey(tree)) lays.put(tree, layoutTrees(files.get(tree)));
                Trees tr = lays.get(tree); if (tr != null && tr.nskills > 0) units.add(new String[] {up(clean(name)), tree});
            }
        }
        // a mod that replaces a stock class's layout but not its DAT (TL2 ACE: SKILL_TREE_RAILMAN): the stock base unit uses that layout
        java.util.HashSet<String> mine = new java.util.HashSet<>(); for (Map<Long, String> t : dats.values()) mine.add(up(clean(t.getOrDefault(K_NAME, ""))));
        for (String[] s : ST_STOCK) {
            if (mine.contains(s[0]) || !files.containsKey(s[1])) continue;
            if (!lays.containsKey(s[1])) lays.put(s[1], layoutTrees(files.get(s[1])));
            Trees tr = lays.get(s[1]); if (tr != null && tr.nskills > 0) units.add(new String[] {s[0], s[1]});
        }
        java.util.TreeSet<String> usedSet = new java.util.TreeSet<>(); for (String[] u : units) usedSet.add(u[1]);
        if (usedSet.isEmpty()) return null;
        List<String> used = new ArrayList<>(usedSet); StringBuilder sb = new StringBuilder("TL2SKILLTREES\t2\n");
        for (int i = 0; i < used.size(); i++) {
            sb.append("L\t").append(i).append('\t').append(used.get(i)).append('\n');
            for (String l : lays.get(used.get(i)).lines) sb.append(l.replaceFirst("%", Integer.toString(i))).append('\n');
        }
        units.sort((a, c) -> { int k = a[0].compareTo(c[0]); return k != 0 ? k : a[1].compareTo(c[1]); });
        java.util.HashSet<String> done = new java.util.HashSet<>();
        for (String[] u : units) if (done.add(u[0])) sb.append("C\t").append(u[0]).append('\t').append(used.indexOf(u[1])).append('\n');
        return sb.toString().getBytes(StandardCharsets.UTF_8);
    }

    // ------------------------------------------------------------ skill icons of PC mods -> a console UI atlas (tools/tl2_mod.py icon_atlases)
    // The console UI loads every .ATLAS (MAN type 26) under MEDIA/UICONSOLE/ATLASES/ at boot (main+0x141040 / 0x1a00360) and draws a skill's
    // SKILL_ICON / SKILL_ICON_INACTIVE from them; PC mods ship CEGUI imagesets instead. Every icon a mod skill names and a mod imageset
    // defines goes into ZMODICONS_<mod id>_<k>.ATLAS + .DDS (RGBA8 XTX, shelf-packed, padding 2) in the mod's pack.
    static final long[] K_ICONS = {tl2Hash("SKILL_ICON"), tl2Hash("SKILL_ICON_INACTIVE")};
    static final long K_ITEM_ICON = tl2Hash("ICON");
    static final String ATLAS_DIR = "MEDIA/UICONSOLE/ATLASES/"; static final int ATLAS_MAX = 2048;
    interface TexSource { byte[] get(String upperPath) throws IOException; }
    static final class Gen { final String dir, name; final int type; final byte[] data; Gen(String d, String n, int t, byte[] b) { dir = d; name = n; type = t; data = b; } }

    private static String text(byte[] b) {
        if (b.length >= 2 && (b[0] & 0xff) == 0xff && (b[1] & 0xff) == 0xfe) return new String(b, 2, b.length - 2, StandardCharsets.UTF_16LE);
        int o = b.length >= 3 && (b[0] & 0xff) == 0xef && (b[1] & 0xff) == 0xbb && (b[2] & 0xff) == 0xbf ? 3 : 0;
        return new String(b, o, b.length - o, StandardCharsets.UTF_8);
    }
    private static final java.util.regex.Pattern P_SET = java.util.regex.Pattern.compile("<Imageset\\b([^>]*)>", java.util.regex.Pattern.CASE_INSENSITIVE),
        P_IMG = java.util.regex.Pattern.compile("<Image\\b([^>]*)>", java.util.regex.Pattern.CASE_INSENSITIVE),
        P_ATTR = java.util.regex.Pattern.compile("([A-Za-z0-9_]+)\\s*=\\s*\"([^\"]*)\""), P_INT = java.util.regex.Pattern.compile("-?[0-9]{1,6}");
    private static Map<String, String> attrs(String s) {
        Map<String, String> m = new java.util.HashMap<>(); java.util.regex.Matcher a = P_ATTR.matcher(s);
        while (a.find()) m.put(up(a.group(1)), a.group(2));   // a repeated attribute: the last one (as the Python dict)
        return m;
    }
    static final class Img { final String name; final int x, y, w, h; Img(String n, int x, int y, int w, int h) { name = n; this.x = x; this.y = y; this.w = w; this.h = h; } }
    /** -> {Imagefile} + images, or null; tools/tl2_mod.py imageset_parse */
    static Object[] imagesetParse(byte[] b) {
        String t = text(b); java.util.regex.Matcher m = P_SET.matcher(t); if (!m.find()) return null;
        String file = attrs(m.group(1)).getOrDefault("IMAGEFILE", ""); List<Img> out = new ArrayList<>();
        java.util.regex.Matcher im = P_IMG.matcher(t);
        while (im.find()) {
            Map<String, String> a = attrs(im.group(1));
            String xs = a.get("XPOS"), ys = a.get("YPOS"), ws = a.get("WIDTH"), hs = a.get("HEIGHT"), nm = a.get("NAME");
            if (xs == null || ys == null || ws == null || hs == null) continue;
            if (!P_INT.matcher(xs).matches() || !P_INT.matcher(ys).matches() || !P_INT.matcher(ws).matches() || !P_INT.matcher(hs).matches() || nm == null || nm.isEmpty()) continue;
            int w = Integer.parseInt(ws), h = Integer.parseInt(hs);
            if (w >= 1 && w <= 512 && h >= 1 && h <= 512) out.add(new Img(nm, Integer.parseInt(xs), Integer.parseInt(ys), w, h));
        }
        return new Object[] {file, out};
    }

    private static int[][] bcColors(byte[] b, int o, boolean four) {
        int c0 = (b[o] & 0xff) | (b[o + 1] & 0xff) << 8, c1 = (b[o + 2] & 0xff) | (b[o + 3] & 0xff) << 8;
        int[] p0 = rgb565(c0), p1 = rgb565(c1);
        if (four || c0 > c1) return new int[][] {p0, p1, {(2 * p0[0] + p1[0]) / 3, (2 * p0[1] + p1[1]) / 3, (2 * p0[2] + p1[2]) / 3, 255}, {(p0[0] + 2 * p1[0]) / 3, (p0[1] + 2 * p1[1]) / 3, (p0[2] + 2 * p1[2]) / 3, 255}};
        return new int[][] {p0, p1, {(p0[0] + p1[0]) / 2, (p0[1] + p1[1]) / 2, (p0[2] + p1[2]) / 2, 255}, {0, 0, 0, 0}};
    }
    private static int[] rgb565(int c) { int r = c >> 11, g = (c >> 5) & 63, bl = c & 31; return new int[] {(r << 3) | (r >> 2), (g << 2) | (g >> 4), (bl << 3) | (bl >> 2), 255}; }
    /** 16 RGBA pixels (row-major) of the 4x4 block at o; fmt BC1 / BC2 / BC3 (tools/tl2_mod.py bc_block) */
    static int[][] bcBlock(byte[] b, int o, int fmt) {
        int co = o + (fmt == BC1 ? 0 : 8); int[][] cols = bcColors(b, co, fmt != BC1); long idx = le32(b, co + 4) & 0xffffffffL;
        int[][] px = new int[16][];
        for (int i = 0; i < 16; i++) px[i] = cols[(int) ((idx >> (2 * i)) & 3)].clone();
        if (fmt == BC2) {
            long a = (le32(b, o) & 0xffffffffL) | (le32(b, o + 4) & 0xffffffffL) << 32;
            for (int i = 0; i < 16; i++) px[i][3] = (int) ((a >>> (4 * i)) & 15) * 17;
        } else if (fmt == BC3) {
            int a0 = b[o] & 0xff, a1 = b[o + 1] & 0xff; long bits = 0; for (int i = 0; i < 6; i++) bits |= (b[o + 2 + i] & 0xffL) << (8 * i);
            int[] al = new int[8]; al[0] = a0; al[1] = a1;
            if (a0 > a1) for (int k = 1; k <= 6; k++) al[k + 1] = ((7 - k) * a0 + k * a1) / 7;
            else { for (int k = 1; k <= 4; k++) al[k + 1] = ((5 - k) * a0 + k * a1) / 5; al[6] = 0; al[7] = 255; }
            for (int i = 0; i < 16; i++) px[i][3] = al[(int) ((bits >> (3 * i)) & 7)];
        }
        return px;
    }
    /** level 0 of a PC texture read pixel by pixel (tools/tl2_mod.py _Tex) */
    static final class Tex {
        int w, h, bc, bpp; int[] idx; byte[] d, rgba; final Map<Long, int[][]> cache = new java.util.HashMap<>();
        Tex(String name, byte[] d, ImageDecoder img) throws IOException {
            if (d.length >= 4 && le32(d, 0) == 0x20534444) {
                if (d.length < 128) throw new IOException("DDS truncated");
                h = le32(d, 12); w = le32(d, 16); int pff = le32(d, 80), fourcc = le32(d, 84), bits = le32(d, 88); this.d = d;
                if ((pff & 4) != 0) {
                    String fc = new String(new byte[] {(byte) fourcc, (byte) (fourcc >> 8), (byte) (fourcc >> 16), (byte) (fourcc >> 24)}, StandardCharsets.ISO_8859_1);
                    switch (fc) { case "DXT1": bc = BC1; break; case "DXT2": case "DXT3": bc = BC2; break; case "DXT4": case "DXT5": bc = BC3; break; default: throw new IOException("DDS fourcc not supported"); }
                } else if ((pff & 0x40) != 0 && (bits == 24 || bits == 32)) {
                    bpp = bits / 8; int ri = mbyte(le32(d, 92)), gi = mbyte(le32(d, 96)), bi = mbyte(le32(d, 100)), am = le32(d, 104), ai = mbyte(am);
                    if (ri < 0 || gi < 0 || bi < 0 || ri >= bpp || gi >= bpp || bi >= bpp) throw new IOException("DDS masks not supported");
                    idx = new int[] {ri, gi, bi, (pff & 1) != 0 && ai >= 0 && ai < bpp ? ai : -1};
                } else throw new IOException("DDS format not supported");
            } else {
                boolean png = d.length >= 8 && (d[0] & 0xff) == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G', jpg = d.length >= 3 && (d[0] & 0xff) == 0xff && (d[1] & 0xff) == 0xd8 && (d[2] & 0xff) == 0xff;
                if (name.toUpperCase(Locale.ROOT).endsWith(".TGA") && !png && !jpg) { int[] wh = new int[2]; rgba = tgaDecode(d, wh); w = wh[0]; h = wh[1]; }
                else {
                    int[] p = img == null ? null : img.decode(d); if (p == null) throw new IOException("image not decodable");
                    w = p[0]; h = p[1]; rgba = new byte[w * h * 4];
                    for (int i = 0; i < w * h; i++) { int c = p[2 + i]; rgba[i * 4] = (byte) (c >> 16); rgba[i * 4 + 1] = (byte) (c >> 8); rgba[i * 4 + 2] = (byte) c; rgba[i * 4 + 3] = (byte) (c >>> 24); }
                }
            }
        }
        void px(int x, int y, byte[] dst, int o) {
            if (x < 0 || y < 0 || x >= w || y >= h) { dst[o] = dst[o + 1] = dst[o + 2] = dst[o + 3] = 0; return; }
            if (rgba != null) { System.arraycopy(rgba, 4 * (y * w + x), dst, o, 4); return; }
            if (bc == 0) {
                long s = 128L + (long) bpp * ((long) y * w + x);
                if (s + bpp > d.length) { dst[o] = dst[o + 1] = dst[o + 2] = dst[o + 3] = 0; return; }
                int q = (int) s; dst[o] = d[q + idx[0]]; dst[o + 1] = d[q + idx[1]]; dst[o + 2] = d[q + idx[2]]; dst[o + 3] = idx[3] >= 0 ? d[q + idx[3]] : (byte) 255; return;
            }
            int bx = x >> 2, by = y >> 2; long k = (long) by << 32 | bx; int[][] blk = cache.get(k);
            if (blk == null) {
                int bs = bc == BC1 ? 8 : 16; long s = 128L + ((long) by * ((w + 3) >> 2) + bx) * bs;
                if (s + bs <= d.length) blk = bcBlock(d, (int) s, bc);
                else { blk = new int[16][]; for (int i = 0; i < 16; i++) blk[i] = new int[4]; }
                cache.put(k, blk);
            }
            int[] p = blk[(y & 3) * 4 + (x & 3)]; dst[o] = (byte) p[0]; dst[o + 1] = (byte) p[1]; dst[o + 2] = (byte) p[2]; dst[o + 3] = (byte) p[3];
        }
    }
    private static final class Def { final String name, tex; final int x, y, w, h; Def(String n, String t, int x, int y, int w, int h) { name = n; tex = t; this.x = x; this.y = y; this.w = w; this.h = h; } }
    private static boolean iconIsPath(String v) { String u = up(v); return (v.indexOf('/') >= 0 || v.indexOf('\\') >= 0) && (u.endsWith(".DDS") || u.endsWith(".PNG") || u.endsWith(".TGA")); }
    /** files: UPPER path -> bytes (skill DATs, item DATs, imagesets); an item ICON that is a texture path exports the whole texture (up to 512x512); -> generated files (atlas + texture per page); counts icons in n[0] */
    static List<Gen> iconAtlases(Map<String, byte[]> files, TexSource src, long modid, ImageDecoder img, int[] n, List<String> notes) throws IOException {
        java.util.TreeMap<String, java.util.TreeSet<String>> want = new java.util.TreeMap<>();
        for (String p : new java.util.TreeSet<>(files.keySet())) {
            boolean skill = p.startsWith("MEDIA/SKILLS/") && p.endsWith(".DAT");
            if (!skill && !(p.startsWith("MEDIA/UNITS/ITEMS/") && p.endsWith(".DAT"))) continue;
            Map<Long, String> t = datTop(files.get(p)); if (t == null) continue;
            for (long k : skill ? K_ICONS : new long[] {K_ITEM_ICON}) { String v = t.get(k); if (v != null && !v.isEmpty()) want.computeIfAbsent(up(v), q -> new java.util.TreeSet<>()).add(v); }
        }
        Map<String, Def> defs = new java.util.HashMap<>();
        Map<String, Tex> texs = new java.util.HashMap<>(); java.util.HashSet<String> bad = new java.util.HashSet<>();
        for (String p : new java.util.TreeSet<>(files.keySet())) {
            if (!p.endsWith(".IMAGESET")) continue;
            Object[] r = imagesetParse(files.get(p)); if (r == null || ((String) r[0]).isEmpty()) continue;
            String tp = norm((String) r[0]);
            @SuppressWarnings("unchecked") List<Img> ims = (List<Img>) r[1];
            for (Img i : ims) { String u = up(i.name); if (want.containsKey(u) && !defs.containsKey(u)) defs.put(u, new Def(i.name, tp, i.x, i.y, i.w, i.h)); }
        }
        for (Map.Entry<String, java.util.TreeSet<String>> w : want.entrySet()) {
            String first = w.getValue().first();
            if (defs.containsKey(w.getKey()) || !iconIsPath(first)) continue;
            String tp = norm(first); byte[] d = src.get(tp); if (d == null) continue;
            Tex t;
            try { t = new Tex(tp, d, img); } catch (IOException | RuntimeException ex) { bad.add(tp); notes.add(tp + ": " + ex.getMessage()); continue; }
            if (t.w > 512 || t.h > 512) { notes.add(first + ": texture " + t.w + "x" + t.h + " larger than 512x512 skipped"); continue; }
            if (t.w >= 1 && t.h >= 1) defs.put(w.getKey(), new Def(first, tp, 0, 0, t.w, t.h));
        }
        List<Def> items = new ArrayList<>(defs.values());
        items.sort((a, c) -> a.h != c.h ? Integer.compare(c.h, a.h) : up(a.name).compareTo(up(c.name)));
        List<Gen> out = new ArrayList<>(); if (items.isEmpty()) return out;
        List<List<int[]>> pages = new ArrayList<>(); List<int[]> cur = new ArrayList<>(); int x = 0, y = 0, rowh = 0;   // {item, x, y}
        for (int i = 0; i < items.size(); i++) {
            Def e = items.get(i); int cw = e.w + 4, ch = e.h + 4;
            if (x + cw > ATLAS_MAX) { x = 0; y += rowh; rowh = 0; }
            if (y + ch > ATLAS_MAX) { pages.add(cur); cur = new ArrayList<>(); x = 0; y = 0; rowh = 0; }
            cur.add(new int[] {i, x + 2, y + 2}); x += cw; rowh = Math.max(rowh, ch);
        }
        pages.add(cur);
        Map<String, Integer> left = new java.util.HashMap<>(); for (Def e : items) left.merge(e.tex, 1, Integer::sum);
        for (int k = 0; k < pages.size(); k++) {
            int W = 64, H = 64;
            for (int[] c : pages.get(k)) { Def e = items.get(c[0]); W = Math.max(W, c[1] + e.w + 2); H = Math.max(H, c[2] + e.h + 2); }
            W = Integer.highestOneBit(W - 1) << 1; H = Integer.highestOneBit(H - 1) << 1;
            byte[] page = new byte[4 * W * H]; StringBuilder ents = new StringBuilder();
            for (int[] c : pages.get(k)) {
                Def e = items.get(c[0]);
                if (!texs.containsKey(e.tex) && !bad.contains(e.tex)) {
                    byte[] d = src.get(e.tex);
                    try { if (d != null) texs.put(e.tex, new Tex(e.tex, d, img)); else bad.add(e.tex); } catch (IOException | RuntimeException ex) { bad.add(e.tex); notes.add(e.tex + ": " + ex.getMessage()); }
                }
                Tex t = texs.get(e.tex);
                if (left.merge(e.tex, -1, Integer::sum) == 0) texs.remove(e.tex);
                if (t == null) { notes.add(e.name + ": texture " + e.tex + " not in the mod"); continue; }
                for (int yy = 0; yy < e.h; yy++) for (int xx = 0; xx < e.w; xx++) t.px(e.x + xx, e.y + yy, page, 4 * ((c[2] + yy) * W + c[1] + xx));
                java.util.TreeSet<String> sp = new java.util.TreeSet<>(want.get(up(e.name))); sp.add(e.name);
                for (String s : sp) ents.append("        <entry name=\"").append(s.replace("&", "&amp;").replace("\"", "&quot;").replace("<", "&lt;").replace(">", "&gt;"))
                    .append("\" x=\"").append(c[1]).append("\" y=\"").append(c[2]).append("\" w=\"").append(e.w).append("\" h=\"").append(e.h).append("\" />\r\n");
                n[0]++;
            }
            String base = String.format(Locale.ROOT, "ZMODICONS_%016X_%d", modid, k);
            String xml = "\ufeff<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<atlas texture=\"" + base + ".DDS\" padding=\"2\">\r\n    <entries>\r\n" + ents + "    </entries>\r\n</atlas>";
            out.add(new Gen(ATLAS_DIR, base + ".ATLAS", 26, xml.getBytes(StandardCharsets.UTF_8)));
            out.add(new Gen(ATLAS_DIR, base + ".DDS", 4, xtxBuild(new byte[][] {page}, W, H, RGBA8, -1)));
        }
        return out;
    }

    // [TL2] the MAN entries of a .MOD without converting it (the mod overlap report, Tl2Mods.overlaps)
    static List<Entry> readEntries(File mod) throws IOException {
        try (RandomAccessFile in = new RandomAccessFile(mod, "r")) {
            byte[] head = new byte[(int) Math.min(in.length(), 1 << 16)]; in.readFully(head);
            Info info = readInfo(head);
            if (info.offMan <= info.offData || info.offMan > in.length()) throw new IOException("bad .MOD offsets");
            byte[] man = new byte[(int) (in.length() - info.offMan)]; in.seek(info.offMan); in.readFully(man);
            return manRead(man);
        }
    }

    /** Converts one .MOD into <outDir>/PAKS/<pack>.PAK(.MAN); writes to temporary names and renames at the end. */
    static Result convert(File mod, File outDir, int slot, ImageDecoder img) throws IOException {
        Result res = new Result();
        try (RandomAccessFile in = new RandomAccessFile(mod, "r")) {
            byte[] head = new byte[(int) Math.min(in.length(), 1 << 16)]; in.readFully(head);
            Info info = readInfo(head);
            if (info.offMan <= info.offData || info.offMan > in.length()) throw new IOException("bad .MOD offsets");
            byte[] man = new byte[(int) (in.length() - info.offMan)]; in.seek(info.offMan); in.readFully(man);
            List<Entry> ents = manRead(man);
            File pdir = new File(outDir, "PAKS"); if (!pdir.isDirectory() && !pdir.mkdirs()) throw new IOException("cannot create " + pdir);
            res.pack = packName(slot, info, mod);
            File pak = new File(pdir, res.pack + ".PAK.tmp"), manf = new File(pdir, res.pack + ".PAK.MAN.tmp");
            List<Entry> out = new ArrayList<>(); long pos = 8, largest = 0; Map<String, byte[]> stf = new java.util.HashMap<>();
            try (OutputStream os = new BufferedOutputStream(new FileOutputStream(pak), 1 << 16)) {
                os.write(new byte[8]);
                byte[] hdr = new byte[8]; int[] kind = new int[1];
                for (Entry e : ents) {
                    if (e.type == 7 || (e.off == 0 && e.size == 0)) { out.add(new Entry(e.dir, e.name, e.type, 0, 0, e.type == 7 ? e.crc : 0, e.ft)); continue; }
                    in.seek(info.offData + e.off); in.readFully(hdr);
                    long dl = le32(hdr, 0) & 0xffffffffL, cl = le32(hdr, 4) & 0xffffffffL; int crc = e.crc;
                    byte[] raw = new byte[(int) (cl != 0 ? cl : dl)]; in.readFully(raw); byte[] full = null;
                    String up = up(e.dir + e.name);
                    if ((up.startsWith("MEDIA/UNITS/PLAYERS/") && up.endsWith(".DAT")) || (up.startsWith("MEDIA/UI/") && up.endsWith(".LAYOUT")) ||
                        (up.startsWith("MEDIA/SKILLS/") && up.endsWith(".DAT")) || (up.startsWith("MEDIA/UNITS/ITEMS/") && up.endsWith(".DAT")) || up.endsWith(".IMAGESET")) {
                        try { stf.put(up, cl != 0 && cl != dl ? inflate(raw, (int) dl) : raw); } catch (IOException x) { /* not for the skill-tree table */ }
                    }
                    if (isTexture(e.name)) {
                        byte[] plain = cl != 0 && cl != dl ? inflate(raw, (int) dl) : raw;
                        try {
                            byte[] conv = toSwitchTexture(e.name, plain, img, kind); res.textures++;
                            if (conv != null) { full = conv; res.converted++; }
                        } catch (IOException | RuntimeException x) { res.failed.add(e.dir + e.name + ": " + x.getMessage()); }
                    }
                    if (full != null) { raw = full; dl = cl = full.length; CRC32 c = new CRC32(); c.update(full); crc = (int) c.getValue(); }
                    out.add(new Entry(e.dir, e.name, e.type, pos, dl, crc, e.ft));
                    os.write(new Wr().u32(dl).u32(cl).bytes()); os.write(raw); pos += 8 + raw.length;
                    largest = Math.max(largest, cl != 0 ? cl : dl); res.files++;
                }
                byte[] st = skilltreesText(stf);
                if (st != null) {   // stored; a directory record for MEDIA/PORT/ in MEDIA/, then the file (as tools/tl2_mod.py convert)
                    for (String l : new String(st, StandardCharsets.UTF_8).split("\n")) if (l.startsWith("C\t")) res.skilltrees++;
                    CRC32 c = new CRC32(); c.update(st);
                    out.add(new Entry("MEDIA/", "PORT/", 7, 0, 0, 0, 0)); out.add(new Entry("MEDIA/PORT/", "SKILLTREES.TXT", 0, pos, st.length, (int) c.getValue(), 0));
                    os.write(new Wr().u32(st.length).u32(st.length).bytes()); os.write(st); pos += 8 + st.length; largest = Math.max(largest, st.length);
                }
                Map<String, Long> texoff = new java.util.HashMap<>();
                for (Entry e : ents) if (e.type != 7 && e.size != 0 && isTexture(e.name)) texoff.put(up(e.dir + e.name), e.off);
                TexSource srcTex = pth -> {
                    Long off = texoff.get(pth); if (off == null) return null;
                    in.seek(info.offData + off); byte[] h8 = new byte[8]; in.readFully(h8);
                    long dl = le32(h8, 0) & 0xffffffffL, cl = le32(h8, 4) & 0xffffffffL; byte[] raw = new byte[(int) (cl != 0 ? cl : dl)]; in.readFully(raw);
                    return cl != 0 && cl != dl ? inflate(raw, (int) dl) : raw;
                };
                int[] nic = new int[1]; List<Gen> ic = java.util.Collections.emptyList();
                try { ic = iconAtlases(stf, srcTex, info.modid, img, nic, res.iconNotes); res.icons = nic[0]; }
                catch (IOException | RuntimeException | OutOfMemoryError e) { ic = java.util.Collections.emptyList(); res.icons = 0; res.iconError = String.valueOf(e); res.iconNotes.add("icons skipped: " + res.iconError); }
                if (!ic.isEmpty()) {   // directory records MEDIA/UICONSOLE/ and its ATLASES/ unless the mod has them, then the atlas pages (stored)
                    java.util.HashSet<String> have = new java.util.HashSet<>(); for (Entry e : ents) if (e.type == 7) have.add(e.dir + e.name);
                    if (!have.contains("MEDIA/UICONSOLE/")) out.add(new Entry("MEDIA/", "UICONSOLE/", 7, 0, 0, 0, 0));
                    if (!have.contains("MEDIA/UICONSOLE/ATLASES/")) out.add(new Entry("MEDIA/UICONSOLE/", "ATLASES/", 7, 0, 0, 0, 0));
                    for (Gen g : ic) {
                        CRC32 c = new CRC32(); c.update(g.data);
                        out.add(new Entry(g.dir, g.name, g.type, pos, g.data.length, (int) c.getValue(), 0));
                        os.write(new Wr().u32(g.data.length).u32(g.data.length).bytes()); os.write(g.data); pos += 8 + g.data.length; largest = Math.max(largest, g.data.length);
                    }
                }
            }
            for (String p : info.deletes) {
                String u = p.replace('\\', '/').toUpperCase(Locale.ROOT); int k = u.lastIndexOf('/');
                out.add(new Entry((k >= 0 ? u.substring(0, k) : "") + "/", u.substring(k + 1), 0, 0, 0, 0, 0)); res.deleted++;
            }
            try (RandomAccessFile f = new RandomAccessFile(pak, "rw")) {
                f.seek(0); f.write(new Wr().u32(largest).u32(0).bytes());
                int h = pakHash(f); f.seek(4); f.write(new Wr().u32(h).bytes());
            }
            try (OutputStream os = new FileOutputStream(manf)) { os.write(manWrite("MEDIA/", out)); }
            File pakF = new File(pdir, res.pack + ".PAK"), manF = new File(pdir, res.pack + ".PAK.MAN");
            pakF.delete(); manF.delete();
            if (!manf.renameTo(manF) || !pak.renameTo(pakF)) throw new IOException("cannot rename the pack in " + pdir);
        }
        return res;
    }
    static byte[] inflate(byte[] z, int size) throws IOException {
        Inflater inf = new Inflater(); inf.setInput(z); byte[] o = new byte[size]; int n = 0;
        try { while (n < size && !inf.finished()) { int k = inf.inflate(o, n, size - n); if (k == 0 && (inf.needsInput() || inf.needsDictionary())) break; n += k; } }
        catch (java.util.zip.DataFormatException e) { throw new IOException("bad zlib data"); } finally { inf.end(); }
        if (n != size) throw new IOException("zlib data short");
        return o;
    }
}
