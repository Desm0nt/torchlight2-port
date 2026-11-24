package com.xiaoboluo.d2r;

import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.system.ErrnoException;
import android.system.Os;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;

// [TL2] Mod manager (docs/tl2/TL2_PORT.md "Моды PC"): PC .MOD files in Download/Torchlight2Mobile/mods/, picked and ordered in the ⚙
// dialog (Tl2Gear.showMods; pref "tl2_mods" in doom_graphics: one line per known file, "<name>\t<1|0>", in load order). At the
// next launch, before the library starts the game, prepare() turns every enabled .MOD into files/tl2mods/overlay/PAKS/ZMOD_<NN>_<NAME>.PAK
// (+ .MAN; Tl2ModConv, the device port of tools/tl2_mod.py): NN = position among the enabled mods, so the game, which lists ROM:/PAKS in
// RomFS order (hle_fs.c DM_TL2_ROM_ORDER), loads them after DATA5 in the list's order. A pack is converted again only when its .MOD
// changed (size + mtime + converter version in files/tl2mods/cache.tsv); a moved mod's pack is renamed; packs of disabled or removed
// mods are deleted. Then DM_TL2_MODS=1 + DM_TL2_MODS_DIR (hle_fs.c puts the folder over the RomFS and moves the save to <save>-mods).
final class Tl2Mods {
    private Tl2Mods() {}
    static final String PREF = "tl2_mods";
    static final int CONVERTER_VERSION = 4;   // 2: + MEDIA/PORT/SKILLTREES.TXT (issue #6); 3: + skill icon atlases ZMODICONS_*; 4: skill panes found without the SkillPane<N> name (issue #25). Bump when Tl2ModConv's output changes: every pack is converted again

    // [TL2] <game folder>/mods (GameFolder: the internal storage, an SD card or the folder the player chose). The converted packs stay in
    // workDir on internal storage, so no conversion writes reach a card.
    static File modsDir(android.content.Context c) { return new File(GameFolder.dir(c), "mods"); }
    static File workDir(android.content.Context c) { return new File(c.getFilesDir(), "tl2mods"); }

    static final class Mod { final File file; boolean on; Tl2ModConv.Info info; String error; Mod(File f, boolean on) { file = f; this.on = on; } }

    /** The .MOD files in the folder, in the saved order (new files at the end, off), with their headers. */
    static List<Mod> list(android.content.Context c, SharedPreferences p) {
        File[] fs = modsDir(c).listFiles((d, n) -> n.toUpperCase(Locale.ROOT).endsWith(".MOD"));
        Map<String, File> found = new HashMap<>(); if (fs != null) for (File f : fs) if (f.isFile()) found.put(f.getName(), f);
        List<Mod> out = new ArrayList<>(); Set<String> seen = new HashSet<>();
        for (String line : p.getString(PREF, "").split("\n")) {
            int t = line.lastIndexOf('\t'); if (t <= 0) continue;
            String n = line.substring(0, t); File f = found.get(n);
            if (f != null && seen.add(n)) out.add(new Mod(f, line.endsWith("\t1")));
        }
        List<String> rest = new ArrayList<>(found.keySet()); rest.removeAll(seen); java.util.Collections.sort(rest, String.CASE_INSENSITIVE_ORDER);
        for (String n : rest) out.add(new Mod(found.get(n), false));
        for (Mod m : out) { try { m.info = Tl2ModConv.readInfo(m.file); } catch (IOException e) { m.error = e.getMessage(); } }
        return out;
    }
    static void save(SharedPreferences p, List<Mod> mods) {
        StringBuilder s = new StringBuilder(); for (Mod m : mods) s.append(m.file.getName()).append('\t').append(m.on ? '1' : '0').append('\n');
        p.edit().putString(PREF, s.toString()).apply();
    }
    static int enabledCount(android.content.Context c, SharedPreferences p) { int n = 0; for (String l : p.getString(PREF, "").split("\n")) if (l.endsWith("\t1") && new File(modsDir(c), l.substring(0, l.length() - 2)).isFile()) n++; return n; }

    interface Progress { void step(int done, int total, String name); }
    static final class Outcome { int packs; final List<String> failed = new ArrayList<>(), log = new ArrayList<>(); }

    /** PNG / JPG -> unpremultiplied ARGB (the converter's image decoder on the device) */
    static final Tl2ModConv.ImageDecoder BITMAPS = data -> {
        BitmapFactory.Options o = new BitmapFactory.Options(); o.inPremultiplied = false; o.inPreferredConfig = Bitmap.Config.ARGB_8888; o.inScaled = false;
        Bitmap b = BitmapFactory.decodeByteArray(data, 0, data.length, o); if (b == null) return null;
        int w = b.getWidth(), h = b.getHeight(); int[] px = new int[2 + w * h]; px[0] = w; px[1] = h;
        b.getPixels(px, 2, w, 0, 0, w, h); b.recycle(); return px;
    };

    private static String stamp(File f) { return f.length() + "\t" + f.lastModified() + "\t" + CONVERTER_VERSION; }

    /** Number of .MOD files that need converting at this launch (0 = only renames / deletes, no progress dialog needed). */
    static int pending(android.content.Context c, SharedPreferences p) {
        Map<String, String[]> cache = readCache(c); File paks = new File(workDir(c), "overlay/PAKS"); int n = 0;
        for (Mod m : list(c, p)) if (m.on && m.info != null) { String[] e = cache.get(m.file.getName()); if (e == null || !e[1].equals(stamp(m.file)) || !new File(paks, e[0] + ".PAK").isFile()) n++; }
        return n;
    }

    /** Converts / renames / deletes packs to match the enabled list and sets DM_TL2_MODS(_DIR). Runs on a background thread. */
    static Outcome prepare(android.content.Context c, SharedPreferences p, Progress progress) {
        Outcome out = new Outcome(); File work = workDir(c), overlay = new File(work, "overlay"), paks = new File(overlay, "PAKS"); paks.mkdirs();
        Map<String, String[]> cache = readCache(c), next = new HashMap<>();
        List<Mod> on = new ArrayList<>(); for (Mod m : list(c, p)) if (m.on) { if (m.info != null) on.add(m); else out.failed.add(m.file.getName() + ": " + m.error); }
        // 1. packs to keep under their new names: first to temporary names (two mods may swap places), then to the final ones
        Map<String, String> keep = new HashMap<>();   // final base -> temporary base
        int todo = 0;
        for (int i = 0; i < on.size(); i++) {
            Mod m = on.get(i); String want = Tl2ModConv.packName(i, m.info, m.file); String[] e = cache.get(m.file.getName());
            if (e != null && e[1].equals(stamp(m.file)) && new File(paks, e[0] + ".PAK").isFile() && new File(paks, e[0] + ".PAK.MAN").isFile()) {
                String tmp = "KEEP_" + i; if (renamePack(paks, e[0], tmp)) keep.put(want, tmp);
            } else todo++;
        }
        Set<String> wanted = new HashSet<>();
        for (int i = 0; i < on.size(); i++) wanted.add(Tl2ModConv.packName(i, on.get(i).info, on.get(i).file));
        // 2. everything else in PAKS goes (disabled / removed mods, old names, half-written .tmp files)
        File[] old = paks.listFiles();
        if (old != null) for (File f : old) { String n = f.getName(); if (!n.startsWith("KEEP_")) { if (f.delete()) out.log.add("removed " + n); } }
        for (Map.Entry<String, String> k : keep.entrySet()) renamePack(paks, k.getValue(), k.getKey());
        // 3. convert what changed, in load order
        int done = 0;
        for (int i = 0; i < on.size(); i++) {
            Mod m = on.get(i); String want = Tl2ModConv.packName(i, m.info, m.file);
            if (keep.containsKey(want)) { next.put(m.file.getName(), new String[] {want, stamp(m.file)}); out.packs++; out.log.add("kept " + want + " (" + m.file.getName() + ")"); continue; }
            if (progress != null) progress.step(done, todo, m.info.title.isEmpty() ? m.file.getName() : m.info.title);
            long t0 = System.nanoTime();
            try {
                Tl2ModConv.Result r = Tl2ModConv.convert(m.file, overlay, i, BITMAPS);
                next.put(m.file.getName(), new String[] {r.pack, stamp(m.file)}); out.packs++;
                out.log.add(String.format(Locale.ROOT, "converted %s -> %s: %d files, %d textures (%d converted), %d deleted, %d failed, %d skill-tree classes, %d skill icons, %.1f s",
                    m.file.getName(), r.pack, r.files, r.textures, r.converted, r.deleted, r.failed.size(), r.skilltrees, r.icons, (System.nanoTime() - t0) / 1e9));
                for (int k = 0; k < Math.min(20, r.failed.size()); k++) out.log.add("  texture kept as PC bytes: " + r.failed.get(k));
            } catch (IOException | RuntimeException | OutOfMemoryError e) {
                out.failed.add(m.file.getName() + ": " + e.getMessage()); out.log.add("FAILED " + m.file.getName() + ": " + e);
                new File(paks, want + ".PAK.tmp").delete(); new File(paks, want + ".PAK.MAN.tmp").delete();
            }
            done++;
        }
        if (progress != null) progress.step(done, todo, "");
        writeCache(c, next);
        try {
            if (out.packs > 0) { Os.setenv("DM_TL2_MODS", "1", true); Os.setenv("DM_TL2_MODS_DIR", overlay.getAbsolutePath(), true); }
            else Os.setenv("DM_TL2_MODS", "0", true);
        } catch (ErrnoException e) { out.log.add("setenv: " + e); }
        out.log.add(out.packs + " mod pack(s) active" + (wanted.size() != out.packs ? " (" + (wanted.size() - out.packs) + " failed)" : ""));
        return out;
    }

    private static boolean renamePack(File paks, String from, String to) {
        File a = new File(paks, from + ".PAK"), am = new File(paks, from + ".PAK.MAN"), b = new File(paks, to + ".PAK"), bm = new File(paks, to + ".PAK.MAN");
        if (from.equals(to)) return a.isFile() && am.isFile();
        b.delete(); bm.delete(); return a.renameTo(b) && am.renameTo(bm);
    }
    private static Map<String, String[]> readCache(android.content.Context c) {
        Map<String, String[]> m = new HashMap<>();
        try {
            for (String l : new String(Files.readAllBytes(new File(workDir(c), "cache.tsv").toPath()), StandardCharsets.UTF_8).split("\n")) {
                String[] f = l.split("\t", 3); if (f.length == 3) m.put(f[0], new String[] {f[1], f[2]});   // name, pack, stamp (size, mtime, version)
            }
        } catch (IOException ignored) { /* no cache yet */ }
        return m;
    }
    private static void writeCache(android.content.Context c, Map<String, String[]> m) {
        StringBuilder s = new StringBuilder(); for (Map.Entry<String, String[]> e : m.entrySet()) s.append(e.getKey()).append('\t').append(e.getValue()[0]).append('\t').append(e.getValue()[1]).append('\n');
        File f = new File(workDir(c), "cache.tsv"), t = new File(workDir(c), "cache.tsv.tmp");
        try (FileOutputStream o = new FileOutputStream(t)) { o.write(s.toString().getBytes(StandardCharsets.UTF_8)); } catch (IOException e) { return; }
        if (!t.renameTo(f)) { f.delete(); t.renameTo(f); }
    }

    // [TL2] mod overlap report: two enabled mods holding the same game file with different bytes (crc), the later in load order wins.
    // entries() reads a .MOD's MAN (background thread only) into ENTRIES, keyed by name + size + mtime; overlaps() only looks at the cache.
    private static final Map<String, List<Tl2ModConv.Entry>> ENTRIES = new HashMap<>();
    private static String entriesKey(File f) { return f.getName() + "\t" + f.length() + "\t" + f.lastModified(); }
    static List<Tl2ModConv.Entry> cachedEntries(File f) { synchronized (ENTRIES) { return ENTRIES.get(entriesKey(f)); } }
    static List<Tl2ModConv.Entry> entries(File f) throws IOException {
        List<Tl2ModConv.Entry> e = cachedEntries(f); if (e != null) return e;
        e = Tl2ModConv.readEntries(f); synchronized (ENTRIES) { ENTRIES.put(entriesKey(f), e); } return e;
    }

    static final class Overlap { final Map<Mod, Integer> overrides = new java.util.LinkedHashMap<>(), overridden = new java.util.LinkedHashMap<>(); }

    /** Per enabled mod: files it replaces from earlier mods / files later mods replace; mods whose entries are not cached yet are left out. */
    static Map<Mod, Overlap> overlaps(List<Mod> mods) {
        Map<Mod, Overlap> out = new java.util.IdentityHashMap<>(); Map<String, List<Object[]>> paths = new HashMap<>();
        for (Mod m : mods) {
            List<Tl2ModConv.Entry> es = m.on && m.info != null ? cachedEntries(m.file) : null; if (es == null) continue;
            out.put(m, new Overlap());
            for (Tl2ModConv.Entry e : es) {
                if (e.type == 7 || (e.off == 0 && e.size == 0)) continue;
                String key = (e.dir + e.name).toUpperCase(Locale.ROOT); List<Object[]> l = paths.get(key); if (l == null) { l = new ArrayList<>(); paths.put(key, l); }
                if (!l.isEmpty() && l.get(l.size() - 1)[0] == m) l.remove(l.size() - 1);
                l.add(new Object[] {m, e.crc});
            }
        }
        for (List<Object[]> l : paths.values()) {
            for (int b = 1; b < l.size(); b++) for (int a = 0; a < b; a++) {
                if (((Integer) l.get(a)[1]).intValue() == ((Integer) l.get(b)[1]).intValue()) { continue; }
                Mod early = (Mod) l.get(a)[0], late = (Mod) l.get(b)[0];
                out.get(late).overrides.merge(early, 1, Integer::sum); out.get(early).overridden.merge(late, 1, Integer::sum);
            }
        }
        return out;
    }

    static String describe(Mod m) { return m.info == null ? m.file.getName() : (m.info.title.isEmpty() ? m.file.getName() : m.info.title); }
}
