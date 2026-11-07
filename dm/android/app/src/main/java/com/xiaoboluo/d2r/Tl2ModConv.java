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
    static final class Result { String pack; int files, textures, converted, deleted; final List<String> failed = new ArrayList<>(); }

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
            List<Entry> out = new ArrayList<>(); long pos = 8, largest = 0;
            try (OutputStream os = new BufferedOutputStream(new FileOutputStream(pak), 1 << 16)) {
                os.write(new byte[8]);
                byte[] hdr = new byte[8]; int[] kind = new int[1];
                for (Entry e : ents) {
                    if (e.type == 7 || (e.off == 0 && e.size == 0)) { out.add(new Entry(e.dir, e.name, e.type, 0, 0, e.type == 7 ? e.crc : 0, e.ft)); continue; }
                    in.seek(info.offData + e.off); in.readFully(hdr);
                    long dl = le32(hdr, 0) & 0xffffffffL, cl = le32(hdr, 4) & 0xffffffffL; int crc = e.crc;
                    byte[] raw = new byte[(int) (cl != 0 ? cl : dl)]; in.readFully(raw); byte[] full = null;
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
