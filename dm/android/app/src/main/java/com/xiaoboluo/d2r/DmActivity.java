// 安卓外壳：一个全屏横屏的 SurfaceView，原生层（libdm.so，dm/android/jni_android.c）跑宿主与游戏。
// 对应 iOS 的 main.m（启动链）与 front_ios.m（视图、触屏、生命周期）；本类只做系统对接，游戏逻辑与界面都在原生层。
// Android shell: a full-screen landscape SurfaceView; the native layer (libdm.so, dm/android/jni_android.c) runs the host and the game.
// Counterpart of iOS's main.m (startup chain) and front_ios.m (view, touch, lifecycle); this class only does system integration, game logic and UI are all in the native layer.
package com.xiaoboluo.d2r;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.hardware.display.DisplayManager;
import android.hardware.input.InputManager;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.system.ErrnoException;
import android.system.Os;
import android.text.Editable;
import android.text.InputType;
import android.text.SpanWatcher;
import android.text.Spannable;
import android.text.Spanned;
import android.text.TextWatcher;
import android.view.inputmethod.BaseInputConnection;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.TextView;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.content.SharedPreferences;
import android.widget.EditText;
import android.widget.FrameLayout;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.ref.WeakReference;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Locale;

public class DmActivity extends Activity implements SurfaceHolder.Callback, DisplayManager.DisplayListener, InputManager.InputDeviceListener, FrontMenu.Host, FrontMenu.Owner, GearHost {
    static final boolean ETERNAL = "eternal".equals(BuildConfig.DM_GAME);   // [ETERNAL] DOOM Eternal: the DOOM front end, own data folder
    static final boolean XC2 = "xc2".equals(BuildConfig.DM_GAME);   // [XC2] Xenoblade Chronicles 2: the DOOM front end, own data folder
    static final boolean TL2 = "tl2".equals(BuildConfig.DM_GAME);   // [TL2] Torchlight II: the D3 front end (same ARPG controls), own data folder
    static final boolean D3 = "d3".equals(BuildConfig.DM_GAME) || TL2;   // [D3] Diablo III: Eternal Collection: the DOOM front end, own data folder
    static final boolean DOOM = "doom".equals(BuildConfig.DM_GAME) || ETERNAL || XC2 || D3;
    static final String DATA_DIR = TL2 ? "Torchlight2Mobile" : D3 ? "Diablo3Mobile" : XC2 ? "Xenoblade2Mobile" : ETERNAL ? "DOOM Eternal Mobile" : "DOOM Mobile";   // [DOOM] Download/<DATA_DIR>/{romfs,dm,drivers}
    private DoomPadView doomPad;
    private D3PadView d3Pad;   // [D3] its own on-screen controls (touch / gamepad layouts) instead of doomPad
    static native int nativeStart(String filesDir, String extDir, String romfsSrc, String lang);
    static native void nativeSurface(Object surface, int w, int h);
    static native void nativeRefresh(float hz);
    static native void nativeFrameStats(float[] out);   // [DOOM] {fps, mean ms, max ms, GPU ms} over the last 0.5 s
    static native void nativeTouch(int n, int[] ids, float[] xs, float[] ys);
    static native boolean nativeTl2UiTap(float x, float y);   // [TL2] hle_tl2_ui.c: tap on a console menu item (picture fractions); false = not taken
    static native boolean nativeTl2OpenPanel(int type);   // [TL2] hle_tl2_ui.c: open an in-game panel (0 inventory, 7 skills, 9 stats, 12 quests) the game's own way
    static native boolean nativeTl2UiTab(int delta);      // [TL2] hle_tl2_ui.c: the radial menu's tab -1 / +1 (its own L / R code)
    static native boolean nativeTl2MenuOpen();                // [TL2] a console menu / panel is up (published by the game's input thread)
    static native boolean nativeTl2MapHit(float x, float y);  // [TL2] hle_tl2_map.c: the point (picture fractions) is on the shown minimap / big map
    static native boolean nativeTl2ToggleMap();               // [TL2] hle_tl2_map.c: minimap <-> big map (the game's D-pad Right mode setting)
    static native void nativeSetDrs(boolean on);   // [XC2] dynamic resolution on / off, live (hle_xc2.c)
    static native void nativeFocus(int ev);
    static native void nativeTextResult(long id, boolean accepted, String text);
    // Platform services (jni_android.c): rumble needs the application Context; the motion sensors the display rotation; game controllers
    // (gamepad / joystick / d-pad sources) go to the native side, which answers whether the game took a key.
    // [front] The unified front end (frontend-common, docs/FRONTEND_UNIFIED.md; the same names and signatures as nx2android's front_android.c): the
    // pause menu holds the game (nativeHold), the graceful exit (nativeQuit), the Switch operation mode live (nativeOpmode / nativeSetOpmode)
    static native void nativeHold(boolean on);
    static native boolean nativeLookDelta(float dxDp, float dyDp, float tiltSecPerDp);   // [front] the shared DoomPadView asks; D3 uses D3PadView (not in this library)
    static native void nativeQuit();
    static native int nativeOpmode();
    static native void nativeSetOpmode(int mode);
    static final String FRONT_DATA_DIR = TL2 ? "Torchlight2Mobile" : "Diablo3Mobile";   // Download/<this>: the player's data folder; save exports go to its saves/
    private FrontMenu front;
    private SaveTransfer saves;
    static native void nativeContext(Object context);
    static native void nativeRotation(int rotation);
    static native boolean nativePadKey(int device, int keyCode, boolean down);
    static native void nativePadAxes(int device, float x, float y, float z, float rz, float lt, float rt, float hatX, float hatY);
    static native void nativePadRemoved(int device);
    static native String nativeDriverStatus();          // [D3] "<requested>\t<active>\t<reason>\t<details>" (vk_driver.c), "" before the driver loads
    static native void nativeLog(String text);          // [DOOM] a "[java] ..." breadcrumb line into boot.log (jni_android.c)
    static native String nativeLogPath();               // [DOOM] this run's boot.log ("" before nativeStart)
    // [D3] (from the DOOM 2016 port, its BUGS_AND_FIXES 2.22) crash logs (CrashLog.java): breadcrumbs from before the native log exists
    // wait in pendingCrumbs
    private static volatile boolean nativeLogReady;
    private static boolean crashHandlerInstalled;
    private static String lastStartError;
    private final java.util.ArrayList<String> pendingCrumbs = new java.util.ArrayList<>();

    private static WeakReference<DmActivity> current = new WeakReference<>(null);
    private static volatile int bootFailure;
    private AlertDialog inputDialog;

    private SurfaceView view;
    private FrameLayout gameFrame;
    private TouchOverlay touchOverlay;
    private boolean started;
    private boolean importing;
    private boolean libraryLoaded;
    private boolean resumed;
    private DisplayManager displays;
    private InputManager inputs;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        // 调试包：am start … --ez dmArtDump true 只把触屏控件底图写到 files/dm/artdump/ 就退出（dm/android/tools/art_compare.sh 用）
        // Debug builds: am start … --ez dmArtDump true only writes the touch control art to files/dm/artdump/ and exits (used by dm/android/tools/art_compare.sh)
        if (BuildConfig.DEBUG && getIntent().getBooleanExtra("dmArtDump", false)) {
            try { new TouchOverlay(this, null, null).dumpArt(new File(getFilesDir(), "dm/artdump")); } catch (IOException e) { android.util.Log.e("dm", "art dump: " + e); }
            finish(); return;
        }
        current = new WeakReference<>(this);
        drivers = new VkDrivers(this);   // [front] the shared gear's driver choice (GearHost.drivers)
        moveUiLang();
        if (DOOM) checkLastRun();   // [DOOM] crash report of the last run, before anything of this one is written
        displays = getSystemService(DisplayManager.class);
        if (displays != null) displays.registerDisplayListener(this, null);
        inputs = getSystemService(InputManager.class);
        if (inputs != null) inputs.registerInputDeviceListener(this, null);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        // [DOOM] Android 10 (the minSdk 29 build) has no ALWAYS: SHORT_EDGES is the same in landscape
        getWindow().getAttributes().layoutInDisplayCutoutMode = android.os.Build.VERSION.SDK_INT >= 30
            ? WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS : WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        view = new SurfaceView(this);
        view.getHolder().addCallback(this);
        gameFrame = new FrameLayout(this);
        gameFrame.addView(view, new FrameLayout.LayoutParams(-1, -1));
        if (XC2 || D3) { gameFrame.setBackgroundColor(0xff000000); gameFrame.addOnLayoutChangeListener((lv, l0, t0, r0, b0, ol, ot, or0, ob) -> { if (r0 - l0 != or0 - ol || b0 - t0 != ob - ot) applyAspectLayout(); }); }
        setContentView(gameFrame);
        immersiveBars();
        // [front] Back (the key, the gesture) opens / closes the pause menu; targetSdk 36 delivers it only through this callback on Android 13+
        if (DOOM && android.os.Build.VERSION.SDK_INT >= 33) getOnBackInvokedDispatcher().registerOnBackInvokedCallback(android.window.OnBackInvokedDispatcher.PRIORITY_DEFAULT, this::backPressed);
    }

    // ---- [front] FrontMenu.Host: what the pause menu asks of this port (frontend-common, docs/FRONTEND_UNIFIED.md)
    private void backPressed() { if (front != null && started) front.back(); else finish(); }
    @Override public void onBackPressed() { if (DOOM) backPressed(); else super.onBackPressed(); }
    @Override public FrontMenu frontMenu() { return front; }
    @Override public boolean ready() { return started; }
    @Override public void hold(boolean on) { if (!libraryLoaded) return; try { nativeHold(on); } catch (UnsatisfiedLinkError e) { android.util.Log.e("dm", "nativeHold missing: the game is not paused (" + e.getMessage() + ")"); } }
    @Override public void gameSettings() { new GearSettings(this).show(); }   // [front] the shared gear (Tl2Gear.java)
    @Override public FrontMenu.Saves saves() { if (saves == null) saves = SaveTransfer.forGame(this, DATA_DIR); return saves; }
    @Override public void quit() {
        android.util.Log.i("dm", "[java] quit from the pause menu");
        if (DOOM) CrashLog.sessionEnd(this);   // [DOOM] a clean end: no crash report at the next start
        CrashLog.stopLiveLogcat();
        try { if (libraryLoaded) nativeQuit(); } catch (UnsatisfiedLinkError e) { /* an older library: just end */ }
        if (DOOM && libraryLoaded) CrashLog.mirrorBootLogs(this);   // [DOOM] boot.log next to crash.txt (System.exit skips onStop)
        finishAndRemoveTask(); System.exit(0);
    }
    @Override public void immersive() { immersiveBars(); }
    // a save import asked for in the pause menu: done at the start, before the library loads and the game runs (SaveTransfer.applyPending)
    private void applyPendingImport() {
        String imp = ((SaveTransfer) saves()).applyPending();
        if (imp != null) android.widget.Toast.makeText(this, imp.startsWith("!") ? imp.substring(1) : imp, android.widget.Toast.LENGTH_LONG).show();
    }
    @Override public String gameName() { return TL2 ? "Torchlight II" : "Diablo III"; }
    // [D3] the mode row: the Switch operation mode, live (nativeSetOpmode -> hle_oe.c dm_set_opmode, notifications 30 / 31; the widescreen patch
    // covers both window sizes, hle_d3.c), and kept in the gear dialog's "handheld" pref (DM_OPMODE at the next start, as before); docked = the default
    @Override public int modeKind() { return FrontMenu.MODE_OPMODE; }
    @Override public boolean docked() {
        try { if (libraryLoaded && started) return nativeOpmode() != 0; } catch (UnsatisfiedLinkError e) { /* the library has no nativeOpmode */ }
        return !doomPrefs().getBoolean("handheld", false);
    }
    @Override public boolean setDocked(boolean on) {
        try { nativeSetOpmode(on ? 1 : 0); } catch (UnsatisfiedLinkError e) { android.util.Log.e("dm", "nativeSetOpmode missing: " + e.getMessage()); return false; }
        doomPrefs().edit().putBoolean("handheld", !on).apply(); startedHandheld = !on;
        android.util.Log.i("dm", "[java] Switch mode -> " + (on ? "docked" : "handheld"));
        return true;
    }

    // ---- [D3] (from the DOOM 2016 port, its BUGS_AND_FIXES 2.22) crash logs and breadcrumbs (CrashLog.java)
    // A "[java] ..." line in boot.log (and logcat); kept until the native log exists (nativeStart opens it).
    @Override public void crumb(String s) {
        if (nativeLogReady) { try { nativeLog(s); return; } catch (Throwable t) { /* an older library */ } }
        android.util.Log.i("dm", "[java] " + s);
        if (!nativeLogReady && pendingCrumbs.size() < 64) pendingCrumbs.add(s);
    }
    private void flushCrumbs() { java.util.ArrayList<String> l = new java.util.ArrayList<>(pendingCrumbs); pendingCrumbs.clear(); for (String s : l) crumb(s); }
    private static File currentLog(android.content.Context c) {
        if (nativeLogReady) try { String p = nativeLogPath(); if (p != null && !p.isEmpty()) return new File(p); } catch (Throwable t) { /* below */ }
        return CrashLog.bootLog(c);
    }
    // Once per process: the Java exception handler (report at once, then the system's handler) and the check of the last run.
    private void checkLastRun() {
        final android.content.Context app = getApplicationContext();
        if (!crashHandlerInstalled) {
            crashHandlerInstalled = true;
            final Thread.UncaughtExceptionHandler prev = Thread.getDefaultUncaughtExceptionHandler();
            Thread.setDefaultUncaughtExceptionHandler((t, e) -> {
                try {
                    String what = "Java exception on thread \"" + t.getName() + "\": " + e;
                    if (nativeLogReady) try { nativeLog(what); } catch (Throwable x) { /* still write the report */ }
                    CrashLog.writeNow(app, what, CrashLog.stackTrace(e), currentLog(app));
                    CrashLog.sessionEnd(app);
                } catch (Throwable x) { android.util.Log.e("dm", "crash log: " + x); }
                if (prev != null) prev.uncaughtException(t, e); else { android.os.Process.killProcess(android.os.Process.myPid()); System.exit(10); }
            });
            String last = CrashLog.checkPrevious(app, (f, why) -> runOnUiThread(() -> {
                if (f == null) return;
                crumb("crash report of the last run written: " + f.getAbsolutePath());
                android.widget.Toast.makeText(app, d3Ui().getString(R.string.d3_toast_last_crash, f.getAbsolutePath()), android.widget.Toast.LENGTH_LONG).show();
            }));
            if (last != null) crumb(last);
            CrashLog.startLiveLogcat(app);   // [D3] logs/logcat.txt written live (survives a kill); after checkPrevious read the old run
        }
    }
    // A start / boot failure screen: the report now (the screen names it), once per message and process.
    private File startErrorReport(String message) {
        File f = CrashLog.report(this);
        if (message.equals(lastStartError)) return f;
        lastStartError = message;
        crumb("start error shown: " + message.replace('\n', ' '));
        final android.content.Context app = getApplicationContext(); final File log = currentLog(this);
        CrashLog.sessionEnd(this);   // reported now, not again at the next start
        new Thread(() -> {
            try { Thread.sleep(300); } catch (InterruptedException e) { /* write now */ }   // the last native lines reach boot.log
            CrashLog.writeNow(app, "start error: " + message, null, log);
        }, "dm-crashlog").start();
        return f;
    }
    @Override public void onTrimMemory(int level) {
        super.onTrimMemory(level);
        // 5 / 10 / 15 running moderate / low / critical, 20 UI hidden, 40+ background (the constants below 20 are deprecated in API 34)
        if (DOOM && level >= 5) crumb("trim memory level " + level + (started ? "" : " (game not started)"));
    }

    @SuppressWarnings("deprecation")
    private void immersiveBars() {
        if (android.os.Build.VERSION.SDK_INT < 30) {   // [DOOM] Android 10 (the minSdk 29 build): the flags the insets controller replaced
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
            return;
        }
        WindowInsetsController c = getWindow().getInsetsController();
        if (c != null) { c.hide(WindowInsets.Type.systemBars()); c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE); }
    }

    // 资源在玩家放的位置读（缺省 Download/S2X/D2R Mobile/data），要「所有文件访问」权限；没有就引导去系统设置页，回来再启动
    // Resources are read where the player put them (default Download/S2X/D2R Mobile/data), which needs "all files access"; without it, guide the player to the system settings page and start after returning
    // [DOOM] Android 10 (the minSdk 29 build) has no all files access: the storage permission with requestLegacyExternalStorage reads the same
    // folders. It is asked for once by itself; a tap asks again, or opens the app's settings page once the player chose "don't ask again"
    private boolean legacyStorageAsked;
    private static final String[] LEGACY_STORAGE = {android.Manifest.permission.READ_EXTERNAL_STORAGE, android.Manifest.permission.WRITE_EXTERNAL_STORAGE};
    private boolean ensureFileAccess() {
        if (SaveTransfer.allFilesAccess(this)) return true;
        boolean legacy = android.os.Build.VERSION.SDK_INT < 30;
        if (legacy && !legacyStorageAsked) { legacyStorageAsked = true; requestPermissions(LEGACY_STORAGE, 1); }
        TextView t = new TextView(this);
        t.setText(D3 ? d3Ui().getString(R.string.d3_start_permission) : getString(R.string.dmAndroidPermission));   // [D3] the shared text names d2r's Download/S2X/D2R Mobile
        t.setTextSize(20); t.setPadding(64, 64, 64, 64);
        t.setOnClickListener(v -> {
            if (!legacy) startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName())));
            else if (shouldShowRequestPermissionRationale(android.Manifest.permission.WRITE_EXTERNAL_STORAGE)) requestPermissions(LEGACY_STORAGE, 1);
            else startActivity(new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:" + getPackageName())));
        });
        setContentView(t);
        return false;
    }

    // 导入包：玩家在电脑上用 dm/tools/d2rm_android.py 和自己的游戏文件做出 android 文件夹，拷到 Download/S2X/D2R Mobile/ 下；
    // 这里把它拷进内部目录 files/dm/（out、shaders、overlay、derived.tsv、pack_info.txt）。pack_info.txt 与已导入的逐字相同就不再拷。
    // 先拷到 files/dm/import.part/，齐了再逐项换上，pack_info.txt 最后放：中途被杀或空间不够，下次打开从头再导，不会用到半份。
    // 原生库加载之前做：不含游戏内容的发布包，几张表在库加载时就从 derived.tsv 读（dm/host/dm_derived_rt.h）。
    // Import pack: the player makes the android folder on a computer with dm/tools/d2rm_android.py and their own game files and copies it to
    // Download/S2X/D2R Mobile/; this copies it into the internal files/dm/ (out, shaders, overlay, derived.tsv, pack_info.txt). Nothing is copied
    // when pack_info.txt is the same as the imported one. It goes to files/dm/import.part/ first and replaces the items one by one once complete,
    // pack_info.txt last: if the app is killed or runs out of space, the next start imports again and half a copy is never used.
    // Done before the native library loads: in the release APK without game content, a few tables are read from derived.tsv at load time.
    private static final String[] IMPORT_ITEMS = {"out", "shaders", "overlay", "derived.tsv", "pack_info.txt"};

    private static String readText(File f) {
        try { return new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8); } catch (IOException e) { return null; }
    }

    private static String packProduct(String info) {
        if (info != null) for (String line : info.split("\n")) if (line.startsWith("PRODUCT=")) return line.substring(8).trim();
        return null;
    }

    // true = 可以接着启动；false = 正在导入或已显示错误
    // true = startup can continue; false = importing, or an error is shown
    private boolean ensureImported(File source) {
        if (importing) return false;
        File runtime = new File(getFilesDir(), "dm");
        String want = readText(new File(source, "pack_info.txt")), have = readText(new File(runtime, "pack_info.txt"));
        if (want != null && !want.equals(have)) {
            String product = packProduct(want);
            if (!BuildConfig.DM_PRODUCT.equals(product)) { showStartError(getString(R.string.dmAndroidImportVersion, product)); return false; }
            importing = true;
            TextView t = new TextView(this);
            t.setTextSize(20); t.setPadding(64, 64, 64, 64); t.setText(getString(R.string.dmAndroidImporting, 0));
            setContentView(t);
            new Thread(() -> {
                String error = null;
                try { importPack(source, runtime, percent -> runOnUiThread(() -> t.setText(getString(R.string.dmAndroidImporting, percent)))); }
                catch (IOException e) { error = e.getMessage() == null ? e.toString() : e.getMessage(); }
                String failure = error;
                runOnUiThread(() -> {
                    importing = false;
                    if (failure != null) showStartError(getString(R.string.dmAndroidImportFailed, failure));
                    else startHost();
                });
            }, "dm-import").start();
            return false;
        }
        // 已导入的是给别的版本做的（App 升级了、导入包没重做）：同样要重做导入包。开发时直接推进来的没有 pack_info.txt，不核
        // The imported pack was made for another version (the app was updated, the pack was not remade): it has to be remade too.
        // Files pushed directly during development have no pack_info.txt and are not checked
        String product = packProduct(have);
        if (have != null && !BuildConfig.DM_PRODUCT.equals(product)) { showStartError(getString(R.string.dmAndroidImportVersion, product)); return false; }
        return true;
    }

    private interface Progress { void percent(int p); }

    private static void importPack(File source, File runtime, Progress progress) throws IOException {
        for (String item : IMPORT_ITEMS) if (!new File(source, item).exists()) throw new IOException("missing " + item);
        File part = new File(runtime, "import.part");
        deleteTree(part);
        long sum = 0;
        for (String item : IMPORT_ITEMS) sum += treeSize(new File(source, item));
        final long total = sum;
        long[] done = {0}; int[] shown = {-1};
        byte[] buffer = new byte[1 << 20];
        for (String item : IMPORT_ITEMS) copyTree(new File(source, item), new File(part, item), buffer, bytes -> {
            done[0] += bytes;
            int p = (int) (done[0] * 100 / Math.max(1, total));
            if (p != shown[0]) { shown[0] = p; progress.percent(p); }
        });
        for (String item : IMPORT_ITEMS) {
            File from = new File(part, item), to = new File(runtime, item);
            deleteTree(to);
            if (!from.renameTo(to)) throw new IOException("rename " + item);
        }
        deleteTree(part);
    }

    private interface Copied { void bytes(long n); }

    private static long treeSize(File f) {
        if (!f.isDirectory()) return f.length();
        long n = 0; File[] list = f.listFiles();
        if (list != null) for (File c : list) n += treeSize(c);
        return n;
    }

    private static void copyTree(File from, File to, byte[] buffer, Copied copied) throws IOException {
        if (from.isDirectory()) {
            if (!to.isDirectory() && !to.mkdirs()) throw new IOException("mkdir " + to.getName());
            File[] list = from.listFiles();
            if (list == null) throw new IOException("list " + from.getName());
            for (File c : list) copyTree(c, new File(to, c.getName()), buffer, copied);
            return;
        }
        File parent = to.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("mkdir " + parent.getName());
        try (InputStream in = new FileInputStream(from); OutputStream out = new FileOutputStream(to)) {
            for (int n; (n = in.read(buffer)) > 0; ) { out.write(buffer, 0, n); copied.bytes(n); }
        }
    }

    private static void deleteTree(File f) throws IOException {
        File[] list = f.isDirectory() ? f.listFiles() : null;
        if (list != null) for (File c : list) deleteTree(c);
        if (f.exists() && !f.delete()) throw new IOException("delete " + f.getName());
    }


    // ---- [front] The gear (⚙ settings): the shared dialog of every port (frontend-common GearSettings / GearPages / VkDrivers / DriverManager,
    // docs/FRONTEND_UNIFIED.md "Gear"); TL2's own rows and their order are Tl2Gear.java (parity with the old showD3Settings:
    // frontend-common/migration/tl2-parity.md). Prefs as before: "doom_graphics" (next launch: the environment before the library loads,
    // GearSettings.applyEnv) and the pad's "doom_controls" (immediately). The pause menu's "Game settings…" and the pad's ⚙ open it.
    static final int ASPECT_DEFAULT = 2;   // [XC2] its picture shape default (applyAspectLayout); TL2 / D3: D3_ASPECT_DEFAULT
    int startedAspect = -1;   // the picture shape the running game was started with (widescreen needs a restart; Tl2Gear's aspect env sets it)
    boolean startedHandheld;  // [D3] the Switch mode the running game was started with (DM_OPMODE 0; Tl2Gear's mode env sets it)
    SharedPreferences doomPrefs() { return getSharedPreferences("doom_graphics", MODE_PRIVATE); }
    private VkDrivers drivers;   // made in onCreate: the start's driver choice, the crash guard, the gear's driver list and manager
    private final float[] gearStats = new float[4];
    @Override public android.app.Activity activity() { return this; }
    @Override public String dataDirName() { return FRONT_DATA_DIR; }
    @Override public SharedPreferences gamePrefs() { return doomPrefs(); }
    @Override public FrontMenu.Pad pad() { return d3Pad != null ? d3Pad : doomPad; }
    @Override public FrontMenu front() { return front; }
    @Override public boolean libraryLoaded() { return libraryLoaded; }
    @Override public boolean started() { return started; }
    @Override public VkDrivers drivers() { return drivers; }
    @Override public java.util.List<GearSettings.Section> gearSections(GearSettings g) { return Tl2Gear.sections(this, g); }
    @Override public String driverStatus() { return nativeDriverStatus(); }
    @Override public float[] frameStats() { try { nativeFrameStats(gearStats); } catch (Throwable t) { gearStats[0] = 0; } return gearStats; }
    void showDoomSettings() { new GearSettings(this).show(); }   // the pad's ⚙ when there is no pause menu (D3PadView.openSettings)
    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request == DriverManager.REQ_ZIP) DriverManager.picked(this, result == RESULT_OK && data != null ? data.getData() : null);   // the driver manager's "Install from a file"
    }
    // [D3] the menu language was the gear's own pref (doom_graphics ui_lang); the shared one is the pause menu's (front ui_lang, Ui.PREFS): moved once
    private void moveUiLang() {
        SharedPreferences old = doomPrefs(); if (!old.contains("ui_lang")) return;
        SharedPreferences f = getSharedPreferences(Ui.PREFS, MODE_PRIVATE);
        if (!f.contains("ui_lang")) f.edit().putString("ui_lang", old.getString("ui_lang", "")).apply();
        old.edit().remove("ui_lang").apply();
    }
    // CrashLog.java's helpers, now VkDrivers'
    static String hardwareSummary() { return VkDrivers.hardwareSummary(); }
    static String readSmall(File f) throws IOException { return VkDrivers.readSmall(f); }

    // [D3] Diablo III's game languages (Tl2Gear: GearPages.gameLanguage); the game picks one only when its archives exist (hle_oe.c)
    static final String[] D3_LANG_CODES = {"", "ru", "en-US", "en-GB"};
    static final String[] D3_LANG_LABELS = {null /* gear_lang_system */, "Русский", "English (US)", "English (UK)"};   // game languages keep their own names
    // [D3] The game's render size, as the native side picks it: docked 1600x900, handheld 1280x720; widescreen (DM_D3_WIDE=1,
    // hle_d3.c dm_d3_wide_install) W x 900 with W = front_game_res(1) (front_android.c: 900 x screen aspect, rounded to 8, max 4096)
    // rounded to 16 (max 3200; not wider than 1600 = no widescreen), handheld (W x 4/5 rounded to 16) x 720. 2400x1080 -> 2000x900 / 1600x720.
    // [TL2] picture shape default: widescreen (hle_tl2_wide.c DM_TL2_WIDE); D3 keeps 16:9 with bars
    static final int D3_ASPECT_DEFAULT = TL2 ? 2 : 0;
    // Widescreen loading screens: D3 stretches them by default (DM_D3_WIDE_STRETCH_LOADING), TL2 keeps them 16:9 with bars (DM_TL2_WIDE_LOADSTRETCH)
    static final String STRETCH_LOADING_KEY = TL2 ? "tl2_stretch_loading" : "d3_stretch_loading";
    static final boolean STRETCH_LOADING_DEFAULT = !TL2;
    static int[] d3RenderSize(boolean wide, boolean handheld, int sw, int sh) {
        if (TL2) {   // [TL2] docked 1920x1080, handheld 1280x720; widescreen W x 1080 (1080 x screen aspect, rounded to 16, max 3840), handheld W*2/3 x 720
            int w = 1920;
            if (wide && sw > 0 && sh > 0 && (long) sw * 9 > (long) sh * 16) w = (int) Math.min(3840, Math.round(1080.0 * sw / sh / 16) * 16);
            if (w <= 1920) return handheld ? new int[] {1280, 720} : new int[] {1920, 1080};
            return handheld ? new int[] {(w * 2 / 3 + 8) / 16 * 16, 720} : new int[] {w, 1080};
        }
        int w = 1600;
        if (wide && sw > 0 && sh > 0 && (long) sw * 9 > (long) sh * 16) {
            long bw = Math.round(900.0 * sw / sh / 8) * 8; if (bw > 4096) bw = 4096;
            w = (int) Math.min(3200, (bw + 8) / 16 * 16);
        }
        if (w <= 1600) return handheld ? new int[] {1280, 720} : new int[] {1600, 900};
        return handheld ? new int[] {(w * 4 / 5 + 8) / 16 * 16, 720} : new int[] {w, 900};
    }
    int[] screenSize() { return GearDisplay.screenSize(this); }   // landscape: the long side first (the size the game's surface had at start)
    // The running game's render size (D3SkillIcons reads the game picture back at this size); null before the game starts.
    int[] d3RenderSize() {
        if (startedAspect < 0) return null;
        int[] sc = screenSize();
        return d3RenderSize(startedAspect == 2, startedHandheld, sc[0], sc[1]);
    }
    SurfaceView gameSurface() { return view; }
    // Resources in the menu language: the phone's, or the one picked in the pause menu / the gear (front ui_lang; also D3PadView's labels).
    android.content.Context d3Ui() {
        String l = getSharedPreferences(Ui.PREFS, MODE_PRIVATE).getString("ui_lang", "");
        if (l.isEmpty()) return this;
        android.content.res.Configuration c = new android.content.res.Configuration(getResources().getConfiguration());
        c.setLocale(Locale.forLanguageTag(l));
        return createConfigurationContext(c);
    }

    // [TL2] Before the library starts the game: the enabled mods become packs (Tl2Mods.prepare, a background thread; a progress dialog only
    // when something has to be converted), then startHost goes on. Once per process (0 not yet, 1 running, 2 done).
    private static volatile int tl2ModsState;
    private void prepareTl2Mods() {
        tl2ModsState = 1;
        final android.content.Context ui = d3Ui(), app = getApplicationContext(); final SharedPreferences p = doomPrefs();
        final int todo = Tl2Mods.pending(this, p);
        final AlertDialog dlg = todo == 0 ? null : new AlertDialog.Builder(this).setTitle(ui.getString(R.string.tl2_mods_title))
            .setMessage(ui.getString(R.string.tl2_mods_preparing, 1, todo, "")).setCancelable(false).show();
        new Thread(() -> {
            Tl2Mods.Outcome o = Tl2Mods.prepare(app, p, (done, total, name) -> { if (dlg != null) runOnUiThread(() -> dlg.setMessage(ui.getString(R.string.tl2_mods_preparing, Math.min(done + 1, Math.max(1, total)), total, name))); });
            runOnUiThread(() -> {
                if (dlg != null) try { dlg.dismiss(); } catch (RuntimeException ignored) { /* the activity went away */ }
                for (int i = 0; i < Math.min(40, o.log.size()); i++) crumb("[tl2 mods] " + o.log.get(i));
                if (!o.failed.isEmpty()) android.widget.Toast.makeText(app, ui.getString(R.string.tl2_mods_failed, android.text.TextUtils.join("; ", o.failed)), android.widget.Toast.LENGTH_LONG).show();
                tl2ModsState = 2;
                DmActivity a = current.get(); if (a != null) a.startHost();
            });
        }, "tl2-mods").start();
    }

    // [XC2] 16:9 with black side bars = the SurfaceView itself sized 16:9 and centred (GearDisplay.fitSurface; the compositor still scales the
    // game's buffer); stretched and widened = the whole screen. A game started widened renders at the screen's aspect, so it fills the screen
    // whatever the saved choice until the next launch.
    void applyAspectLayout() {
        if (!(XC2 || D3) || view == null || gameFrame == null) return;   // [D3] 0 = 16:9 with bars (default), 1 = stretched, 2 = widescreen (W x 900 at the screen's aspect)
        runOnUiThread(() -> {
            int mode = XC2 && startedAspect == 2 ? 2 : doomPrefs().getInt("aspect", XC2 ? ASPECT_DEFAULT : D3_ASPECT_DEFAULT);
            if (D3) mode = startedAspect == 2 ? 2 : (TL2 && startedAspect < 0 && mode == 2) ? 2 : mode == 2 ? 0 : mode;   // [D3] widescreen = whole screen only when the game was started with it;
            // [TL2] before the host starts the saved choice is what it will start with (a 16:9 view here made the first surface 16:9)
            GearDisplay.fitSurface(view, gameFrame, mode == 0);
        });
    }

    private void startHost() {
        if (bootFailure != 0) { showBootFailure(); return; }
        if (started || importing || !resumed || !ensureFileAccess()) return;
        File downloads = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
        if (!DOOM && !libraryLoaded && !ensureImported(new File(downloads, "S2X/D2R Mobile/android"))) return;
        if (gameFrame.getParent() == null) setContentView(gameFrame);
        if (TL2 && !libraryLoaded && tl2ModsState != 2) { if (tl2ModsState == 0) prepareTl2Mods(); return; }   // [TL2] mods first (Tl2Mods)
        if (!libraryLoaded) {
            // 与 jni_android.c 选运行目录同一条规则：内部目录有主程序就用内部目录，否则用外部私有目录
            // Same rule as jni_android.c's choice of runtime directory: internal storage when it has the main module, otherwise external private storage
            File ext = getExternalFilesDir(null), internal = new File(getFilesDir(), "dm");
            File runtime = new File(internal, "out/main.patched.bin").canRead() || ext == null ? internal : new File(ext, "dm");
            try { Os.setenv("DM_DERIVED", new File(runtime, "derived.tsv").getAbsolutePath(), true); } catch (ErrnoException e) { showStartError(getString(R.string.dmAndroidStorage)); return; }
            if (DOOM) { applyPendingImport(); GearSettings.applyEnv(this); }   // [front] a requested save import first; then every gear section's environment (Tl2Gear)
            if (DOOM) try {   // [DOOM] the app version for boot.log's first lines (jni_android.c log_header)
                android.content.pm.PackageInfo pi = getPackageManager().getPackageInfo(getPackageName(), 0);
                Os.setenv("DM_APP_VERSION", getPackageName() + " " + pi.versionName + " (" + pi.getLongVersionCode() + ")" + (BuildConfig.DEBUG ? " debug" : ""), true);
            } catch (Exception e) { /* only a log line */ }
            try { System.loadLibrary("dm"); libraryLoaded = true; }
            catch (UnsatisfiedLinkError error) { showStartError(getString(R.string.dmAndroidLibrary)); return; }
            nativeContext(getApplicationContext());
        }
        if (D3 && d3Pad == null) { d3Pad = new D3PadView(this); gameFrame.addView(d3Pad, new FrameLayout.LayoutParams(-1, -1)); updatePadVisibility(); }   // [D3] touch / gamepad layouts
        else if (DOOM && !D3 && doomPad == null) { doomPad = new DoomPadView(this); gameFrame.addView(doomPad, new FrameLayout.LayoutParams(-1, -1)); updatePadVisibility(); }   // [DOOM] on-screen controller
        if (front == null && (d3Pad != null || doomPad != null)) front = new FrontMenu(this, this, d3Pad != null ? d3Pad : doomPad, gameFrame);   // [front] the pause menu over the pad
        if (touchOverlay == null && !DOOM) {   // [DOOM] the D2R touch layout does not apply; a DOOM layout comes later
            int[] gameIds = {R.string.dmAndroidAttack, R.string.dmAndroidSkill1, R.string.dmAndroidSkill2, R.string.dmAndroidSkill3, R.string.dmAndroidSkill4,
                R.string.dmAndroidPortal, R.string.dmAndroidPotion1, R.string.dmAndroidPotion2, R.string.dmAndroidPotion3, R.string.dmAndroidPotion4,
                R.string.dmAndroidMenu, R.string.dmAndroidMap, R.string.dmAndroidMove, R.string.dmAndroidSwap, R.string.dmAndroidDifficulty};
            int[] navIds = {R.string.dmConfirm, R.string.dmAndroidBack, R.string.dmAndroidAction, R.string.dmAndroidBind, R.string.dmAndroidNext};
            String[] gameLabels = new String[gameIds.length], navLabels = new String[navIds.length];
            for (int i = 0; i < gameIds.length; i++) gameLabels[i] = getString(gameIds[i]);
            for (int i = 0; i < navIds.length; i++) navLabels[i] = getString(navIds[i]);
            touchOverlay = new TouchOverlay(this, gameLabels, navLabels);
            touchOverlay.setOnApplyWindowInsetsListener((v, insets) -> {
                if (android.os.Build.VERSION.SDK_INT >= 30) {
                    android.graphics.Insets safe = insets.getInsets(WindowInsets.Type.displayCutout());
                    touchOverlay.setSafeInsets(safe.left, safe.right, safe.bottom);
                } else {   // [DOOM] Android 10 (the minSdk 29 build)
                    android.view.DisplayCutout c = insets.getDisplayCutout();
                    if (c == null) touchOverlay.setSafeInsets(0, 0, 0); else touchOverlay.setSafeInsets(c.getSafeInsetLeft(), c.getSafeInsetRight(), c.getSafeInsetBottom());
                }
                return insets;
            });
            gameFrame.addView(touchOverlay, new FrameLayout.LayoutParams(-1, -1));
            touchOverlay.requestApplyInsets();
        }
        if (!view.getHolder().getSurface().isValid() || view.getWidth() <= 0 || view.getHeight() <= 0) return;
        File ext = getExternalFilesDir(null);
        if (ext == null) { showStartError(getString(R.string.dmAndroidStorage)); return; }
        nativeSurface(view.getHolder().getSurface(), view.getWidth(), view.getHeight());
        updateRefresh();
        // 资源目录缺省 Download/S2X/D2R Mobile/data；没有这个目录、而早先的 Download/data 在时沿用早先的位置，已装好的手机不必搬数据。
        // 按目录判断而不按 .build.config：它也可以由导入的覆盖层提供，资源目录里就没有。
        // The resource directory defaults to Download/S2X/D2R Mobile/data; when that directory does not exist but the earlier
        // Download/data does, the earlier one is kept, so existing installs need not move their data. The test is the directory,
        // not .build.config: the imported overlay can provide that file, and then the resource directory has none.
        File data = new File(downloads, "S2X/D2R Mobile/data"), earlier = new File(downloads, "data");
        if (!data.isDirectory() && earlier.isDirectory()) data = earlier;
        String src = DOOM ? new File(downloads, DATA_DIR).getAbsolutePath() : data.getAbsolutePath();   // [DOOM] Download/DOOM Mobile/{romfs,dm}
        if (DOOM) CrashLog.session(this, "fg");   // [DOOM] the game runs from here: a leftover marker at the next start = it did not end cleanly
        int result = nativeStart(getFilesDir().getAbsolutePath(), ext.getAbsolutePath(), src, Locale.getDefault().toLanguageTag());
        if (DOOM) { nativeLogReady = true; flushCrumbs(); crumb("nativeStart " + result); }
        if (result < 0) {
            int resource = switch (result) {
                // [D3] its own texts (from the DOOM 2016 port, d89997e): the shared ones name d2r's Download/S2X/D2R Mobile and d2rm_android.py
                case -2 -> D3 ? R.string.d3_start_missing_data : R.string.dmAndroidMissingData;
                case -3 -> D3 ? R.string.d3_start_missing_modules : R.string.dmAndroidMissingModules;
                case -4 -> D3 ? R.string.d3_start_missing_shaders : R.string.dmAndroidMissingShaders;
                case -5 -> R.string.dmAndroidMissingConfig;
                case -9 -> R.string.dmAndroidMissingOverlay;
                case -10 -> R.string.dmAndroidInvalidOverlay;
                default -> 0;
            };
            showStartError(resource == 0 ? getString(R.string.dmAndroidStartFailed, result) : (D3 ? d3Ui() : this).getString(resource)); return;
        }
        started = true;
        if (touchOverlay != null) touchOverlay.setInputEnabled(true);
        drivers.gameStarted(view);   // [front] VkDrivers: the notice from this launch's driver check, then the crash guard / fallback watch
        nativeFocus(3);
    }

    private void showStartError(String message) {
        // [DOOM] every error screen writes a report at once and names it (CrashLog.java)
        if (DOOM) message += "\n\n" + d3Ui().getString(R.string.d3_start_report) + startErrorReport(message).getAbsolutePath();
        TextView t = new TextView(this);
        t.setText(message); t.setTextSize(20); t.setPadding(64, 64, 64, 64);
        setContentView(t);
    }

    // 原生线程只投递主线程工作，不持有 Activity，也不直接操作视图。
    // Native threads only post UI work; they neither retain the Activity nor touch views.
    private static void showNativeFailure(int code) {
        bootFailure = code;
        DmActivity activity = current.get();
        if (activity != null) activity.runOnUiThread(() -> {
            if (!activity.isDestroyed()) activity.showBootFailure();
        });
    }

    private void showBootFailure() {
        releaseTouches();
        showStartError(getString(bootFailure == -6 ? R.string.dmAndroidGraphicsFailed : R.string.dmAndroidHostStopped));
    }

    private static void showNativeInput(long id, String title, String sub, String guide, String initial,
                                        int maxChars, int nameRule, int numericMax) {
        DmActivity activity = current.get();
        if (activity == null) { nativeTextResult(id, false, ""); return; }
        activity.runOnUiThread(() -> activity.openInput(id, title, sub, guide, initial, maxChars, nameRule != 0, numericMax));
    }

    private void openInput(long id, String title, String sub, String guide, String initial,
                           int maxChars, boolean nameRule, int numericMax) {
        if (!resumed || isFinishing() || isDestroyed() || inputDialog != null) { nativeTextResult(id, false, ""); return; }
        releaseTouches();
        EditText input = new EditText(this);
        input.setSingleLine(true);
        input.setInputType(numericMax >= 0 ? InputType.TYPE_CLASS_NUMBER : InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        input.setHint(nameRule ? getString(R.string.dmNameBlankRandom) : guide);
        input.setText(TextInputRules.normalize(initial, maxChars, nameRule, numericMax));
        input.setSelection(input.length());
        String message = sub + (sub.isEmpty() || guide.isEmpty() ? "" : "\n") + guide;
        if (nameRule) message += (message.isEmpty() ? "" : "\n") + getString(R.string.dmNameRule);
        if (numericMax >= 0) message = getString(R.string.dmAndroidQuantityRange, numericMax);
        if (title.isEmpty()) title = getString(nameRule ? R.string.dmNameTitle : numericMax >= 0 ? R.string.dmQtyTitle : R.string.dmInputTitle);
        final boolean[] finished = {false};
        AlertDialog dialog = new AlertDialog.Builder(this).setTitle(title).setMessage(message).setView(input)
            .setPositiveButton(R.string.dmConfirm, null).setNegativeButton(R.string.dmCancel, (d, which) -> {})
            .create();
        inputDialog = dialog;
        dialog.setOnDismissListener(d -> {
            if (inputDialog == dialog) inputDialog = null;
            if (!finished[0]) { finished[0] = true; nativeTextResult(id, false, ""); }
            if (resumed) immersiveBars();
        });
        dialog.setOnShowListener(d -> {
            Runnable check = () -> {
                boolean composing = BaseInputConnection.getComposingSpanStart(input.getText()) >= 0;
                int error = nameRule ? TextInputRules.nameError(input.getText().toString()) : 0;
                input.setError(composing ? getString(R.string.dmNamePickIme) : error == 1 ? getString(R.string.dmNameTooShort) : error == 2 ? getString(R.string.dmNameEdge) : null);
                dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(!composing && error == 0);
            };
            Runnable settle = () -> {
                Editable text = input.getText();
                if (BaseInputConnection.getComposingSpanStart(text) < 0) {
                    String normalized = TextInputRules.normalize(text.toString(), maxChars, nameRule, numericMax);
                    if (!normalized.contentEquals(text)) { input.setText(normalized); input.setSelection(input.length()); return; }
                }
                check.run();
            };
            // 输入法结束组字（点候选、按完成）时常常只去掉组字标记、文字不变：TextWatcher 收不到，「确定」一直灰、提示停在「先选完字」。
            // 组字状态一变就和文字变化一样处理（规整 + 重判）：SpanWatcher 挂在文字上，Editable.Factory 让每次 setText 换上的新文字也挂着它。
            // When the IME finishes composing (picking a candidate, Done) it often only removes the composing span and keeps the text: TextWatcher is not
            // called, so OK stays disabled with "finish choosing characters". A change of composing state is handled like a text change (normalize + check):
            // a SpanWatcher sits on the text, and an Editable.Factory attaches it to the new text after every setText.
            SpanWatcher composingWatch = new SpanWatcher() {
                boolean composing = BaseInputConnection.getComposingSpanStart(input.getText()) >= 0;
                private void changed(Spannable text) {
                    boolean now = BaseInputConnection.getComposingSpanStart(text) >= 0;
                    if (now != composing) { composing = now; input.post(settle); }
                }
                public void onSpanAdded(Spannable text, Object what, int start, int end) { changed(text); }
                public void onSpanRemoved(Spannable text, Object what, int start, int end) { changed(text); }
                public void onSpanChanged(Spannable text, Object what, int ostart, int oend, int nstart, int nend) { changed(text); }
            };
            input.setEditableFactory(new Editable.Factory() {
                @Override public Editable newEditable(CharSequence source) {
                    Editable e = super.newEditable(source);
                    e.setSpan(composingWatch, 0, e.length(), Spanned.SPAN_INCLUSIVE_INCLUSIVE);
                    return e;
                }
            });
            input.getText().setSpan(composingWatch, 0, input.length(), Spanned.SPAN_INCLUSIVE_INCLUSIVE);
            input.addTextChangedListener(new TextWatcher() {
                public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
                public void onTextChanged(CharSequence s, int start, int before, int count) {}
                public void afterTextChanged(Editable text) { settle.run(); }
            });
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
                finished[0] = true;
                nativeTextResult(id, true, input.getText().toString());
                dialog.dismiss();
            });
            check.run();
            input.requestFocus();
            if (dialog.getWindow() != null) dialog.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
        });
        dialog.show();
    }

    private void releaseTouches() {
        if (touchOverlay != null) touchOverlay.cancelTouches();
        if (d3Pad != null) d3Pad.releaseAll();   // [D3] nothing stays held while the window is away (dialogs, pause)
        if (started) nativeTouch(0, new int[0], new float[0], new float[0]);
    }

    private void updateRefresh() {
        if (!libraryLoaded) return;
        android.view.Display d = view.getDisplay();
        nativeRefresh(d == null ? 0 : d.getRefreshRate());
        if (d != null) nativeRotation(d.getRotation());
    }

    // 游戏手柄：手柄 / 摇杆 / 十字键来源（非虚拟设备）的按键与摇杆交原生层；原生层不认的键照常交给系统。
    // Game controllers: keys and sticks from gamepad / joystick / d-pad sources of a real (not virtual) device go to the native layer; a key
    // the native layer does not map goes on to the system as usual. The phone's own Back (a virtual keyboard source) is never taken.
    private static boolean controller(InputDevice d, int source) {
        if (d == null || d.isVirtual()) return false;
        return (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD || (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
            || (source & InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD;
    }
    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        if (front != null && front.key(e)) return true;   // [front] the pause menu: Back / Home / Menu, its navigation; nothing reaches a held game
        if (DOOM && e.getAction() == KeyEvent.ACTION_DOWN) android.util.Log.i("dm", "[input] java key " + e.getKeyCode() + " source 0x" + Integer.toHexString(e.getSource()) + " device " + e.getDeviceId() + " started " + started + " dialog " + (inputDialog != null));
        if (started && inputDialog == null && (e.getAction() == KeyEvent.ACTION_DOWN || e.getAction() == KeyEvent.ACTION_UP)
            && (controller(e.getDevice(), e.getSource())
                || (DOOM && ((e.getSource() & InputDevice.SOURCE_KEYBOARD) == InputDevice.SOURCE_KEYBOARD || e.getSource() == 0) && e.getKeyCode() != KeyEvent.KEYCODE_BACK
                    && e.getKeyCode() != KeyEvent.KEYCODE_SCROLL_LOCK))   // [ETERNAL] Scroll Lock = tools/keep_awake.sh's screen keep-alive, not game input   // [DOOM] keyboard too
            && nativePadKey(e.getDeviceId(), e.getKeyCode(), e.getAction() == KeyEvent.ACTION_DOWN)) {
            if (DOOM && controller(e.getDevice(), e.getSource()) && (e.getSource() & (InputDevice.SOURCE_GAMEPAD | InputDevice.SOURCE_JOYSTICK)) != 0) padUsed(e.getDeviceId());
            return true;
        }
        return super.dispatchKeyEvent(e);
    }
    @Override public boolean dispatchGenericMotionEvent(MotionEvent e) {
        if (front != null && front.motion(e)) return true;   // [front] the menu's navigation by hat / stick; a held game gets no axes
        int src = e.getSource();
        if (started && inputDialog == null && e.getAction() == MotionEvent.ACTION_MOVE && e.getDevice() != null && !e.getDevice().isVirtual()
            && (src & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) {
            nativePadAxes(e.getDeviceId(), e.getAxisValue(MotionEvent.AXIS_X), e.getAxisValue(MotionEvent.AXIS_Y),
                e.getAxisValue(MotionEvent.AXIS_Z), e.getAxisValue(MotionEvent.AXIS_RZ),
                Math.max(e.getAxisValue(MotionEvent.AXIS_LTRIGGER), e.getAxisValue(MotionEvent.AXIS_BRAKE)),
                Math.max(e.getAxisValue(MotionEvent.AXIS_RTRIGGER), e.getAxisValue(MotionEvent.AXIS_GAS)),
                e.getAxisValue(MotionEvent.AXIS_HAT_X), e.getAxisValue(MotionEvent.AXIS_HAT_Y));
            if (DOOM && (Math.abs(e.getAxisValue(MotionEvent.AXIS_X)) > 0.5f || Math.abs(e.getAxisValue(MotionEvent.AXIS_Y)) > 0.5f
                || Math.abs(e.getAxisValue(MotionEvent.AXIS_Z)) > 0.5f || Math.abs(e.getAxisValue(MotionEvent.AXIS_RZ)) > 0.5f
                || e.getAxisValue(MotionEvent.AXIS_HAT_X) != 0 || e.getAxisValue(MotionEvent.AXIS_HAT_Y) != 0)) padUsed(e.getDeviceId());
            return true;
        }
        return super.dispatchGenericMotionEvent(e);
    }
    @Override public void onInputDeviceAdded(int id) { updatePadVisibility(); }
    @Override public void onInputDeviceChanged(int id) { updatePadVisibility(); }
    @Override public void onInputDeviceRemoved(int id) { if (libraryLoaded) nativePadRemoved(id); usedPads.remove(id); updatePadVisibility(); }

    // [DOOM] A real controller connected (external gamepad / joystick): the on-screen pad hides all but the settings button,
    // and comes back when the last one is disconnected.
    // A device counts only once it has sent controller input: emulators (LDPlayer's "joystick") and some phones register
    // always-present gamepad-class devices for key mapping, which would hide the pad with no controller in hand.
    // [D3] (from the DOOM 2016 port, its issue #5) Only a device that looks like a game controller counts, besides having sent input: an
    // external one (USB / Bluetooth) with face buttons or a stick, or a built-in one (handheld consoles) with both. HyperOS phones list
    // built-in input devices that call themselves gamepad / joystick without being one (POCO F6), and some send a key now and then.
    private final java.util.Set<Integer> usedPads = new java.util.HashSet<>();
    private void padUsed(int id) { if (usedPads.add(id)) updatePadVisibility(); }
    private static boolean looksLikeController(InputDevice d) {
        if (d == null || d.isVirtual()) return false;
        int s = d.getSources();
        if ((s & InputDevice.SOURCE_GAMEPAD) != InputDevice.SOURCE_GAMEPAD && (s & InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK) return false;
        boolean[] k = d.hasKeys(KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BUTTON_X, KeyEvent.KEYCODE_BUTTON_Y);
        boolean buttons = k[0] && k[1] && (k[2] || k[3]);
        boolean stick = d.getMotionRange(MotionEvent.AXIS_X, InputDevice.SOURCE_JOYSTICK) != null && d.getMotionRange(MotionEvent.AXIS_Y, InputDevice.SOURCE_JOYSTICK) != null;
        return d.isExternal() ? buttons || stick : buttons && stick;
    }
    private boolean physicalController(InputDevice d) { return d != null && usedPads.contains(d.getId()) && looksLikeController(d); }
    private String lastInputDevices;
    void updatePadVisibility() {
        if (doomPad == null && d3Pad == null) return;
        boolean any = false; StringBuilder list = new StringBuilder();
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id); if (d == null) continue;
            boolean c = physicalController(d); any |= c;
            list.append("\n  ").append(id).append(" \"").append(d.getName()).append("\" sources 0x").append(Integer.toHexString(d.getSources()))
                .append(d.isVirtual() ? " virtual" : "").append(d.isExternal() ? " external" : " built-in").append(looksLikeController(d) ? " controller-like" : "")
                .append(usedPads.contains(id) ? " used" : "").append(c ? " -> controller" : "");
        }
        String s = list.toString();   // one log per change: which device hides the on-screen pad is visible in boot.log
        if (!s.equals(lastInputDevices)) { lastInputDevices = s; android.util.Log.i("dm", "[input] devices (on-screen pad " + (any ? "hidden: controller" : "shown") + "):" + s); }
        if (doomPad != null) doomPad.setControllerMode(any);
        if (d3Pad != null) d3Pad.setControllerMode(any);
    }
    @Override public void onDisplayAdded(int id) { updateRefresh(); }
    @Override public void onDisplayRemoved(int id) { updateRefresh(); }
    @Override public void onDisplayChanged(int id) { updateRefresh(); }
    @Override protected void onResume() { super.onResume(); resumed = true; immersiveBars(); startHost(); updateRefresh(); if (d3Pad != null) d3Pad.setHostResumed(true); if (started) { nativeFocus(3); if (touchOverlay != null && bootFailure == 0) touchOverlay.setInputEnabled(true); } }
    @Override protected void onPause() { resumed = false; if (d3Pad != null) d3Pad.setHostResumed(false); if (inputDialog != null) inputDialog.dismiss(); releaseTouches(); if (touchOverlay != null) touchOverlay.setInputEnabled(false); if (started) nativeFocus(0); super.onPause(); }
    @Override protected void onStart() { super.onStart(); if (started) { nativeFocus(2); if (DOOM && bootFailure == 0) CrashLog.session(this, "fg"); } }   // [DOOM] session state for CrashLog
    @Override protected void onStop() { if (started) { nativeFocus(1); if (DOOM && bootFailure == 0) CrashLog.session(this, "bg"); }
        if (DOOM && libraryLoaded) { android.content.Context app = getApplicationContext(); new Thread(() -> CrashLog.mirrorBootLogs(app), "dm-logcopy").start(); }   // [DOOM] boot.log next to crash.txt
        com.xiaoboluo.d2r.widget.HeroWidgetProvider.requestUpdate(this); super.onStop(); }
    @Override protected void onDestroy() { if (DOOM && isFinishing()) { crumb("activity finishing"); CrashLog.sessionEnd(this); } if (displays != null) displays.unregisterDisplayListener(this); if (inputs != null) inputs.unregisterInputDeviceListener(this); if (inputDialog != null) inputDialog.dismiss(); if (current.get() == this) current.clear(); super.onDestroy(); }
    @Override public void onWindowFocusChanged(boolean f) { super.onWindowFocusChanged(f); if (f) immersiveBars(); else releaseTouches(); }

    @Override public void surfaceCreated(SurfaceHolder h) {}
    @Override public void surfaceChanged(SurfaceHolder h, int format, int w, int ht) {
        if (libraryLoaded) nativeSurface(h.getSurface(), w, ht);
        updateRefresh();
        startHost();
    }
    @Override public void surfaceDestroyed(SurfaceHolder h) { releaseTouches(); if (libraryLoaded) nativeSurface(null, 0, 0); }

}
