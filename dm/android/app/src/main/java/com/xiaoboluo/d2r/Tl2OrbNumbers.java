package com.xiaoboluo.d2r;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import java.util.Locale;

// [TL2] Health and mana as numbers ("current / max") beside the HUD orb (GitHub issue #31; the console game only fills the orb, PC TL2 shows
// them when the mouse is over it). The numbers, the orb's place and whether the HUD is on screen come from hle_tl2_orbs.c (JNI nativeTl2Orbs,
// published by the game thread every frame from the HUD's own health update); this view only draws two lines right of the action-bar ring,
// vertically centred on the orb: red "HP / max" over blue "MP / max", outlined like the game's HUD text, the size of the HUD's small text
// (30 of 1080 canvas units, at least 10 dp). Positions are picture fractions, mapped onto the game SurfaceView's own rectangle, so every
// picture shape (16:9 with bars, stretched, widescreen) and render size fits.
// Hidden: menus / panels / dialogs / loading screens / the title (native "shown" = 0, or no update for 400 ms), the gear row off (game pref
// tl2_orb_numbers, default on, applies at once: read on every poll), or the native side off (DM_TL2_ORB_NUMBERS=0 / not TL2 v5 main: polling
// stops). Polled on the UI thread every 100 ms (one JNI call, no allocation); redrawn only when a text or its place changes. Takes no touches.
final class Tl2OrbNumbers extends View {
    static final String PREF = "tl2_orb_numbers";
    static final boolean PREF_DEFAULT = true;
    static native int nativeTl2Orbs(float[] out);   // jni_android.c -> hle_tl2_orbs.c dm_tl2_orbs; 1 shown, 0 hidden, -1 off

    private static final long TICK_MS = 100;
    private static final int HP_COLOR = 0xFFFF8F80, MP_COLOR = 0xFF8CC4FF, OUTLINE = 0xE6000000;
    private final View game; private final SharedPreferences prefs; private final float dp;
    private final Handler h = new Handler(Looper.getMainLooper());
    private final float[] v = new float[12];
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private boolean running, off;
    // what is drawn now
    private boolean shown; private String hp = "", mp = ""; private float x, cy, size;

    Tl2OrbNumbers(Context c, View game, SharedPreferences prefs) {
        super(c);
        this.game = game; this.prefs = prefs; dp = c.getResources().getDisplayMetrics().density;
        setClickable(false); setFocusable(false); setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
        Typeface tf = Typeface.create("sans-serif-medium", Typeface.BOLD);
        fill.setTypeface(tf); stroke.setTypeface(tf);
        fill.setFontFeatureSettings("tnum"); stroke.setFontFeatureSettings("tnum");   // tabular digits: the line does not wobble
        stroke.setStyle(Paint.Style.STROKE); stroke.setStrokeJoin(Paint.Join.ROUND); stroke.setColor(OUTLINE);
    }

    @Override protected void onAttachedToWindow() { super.onAttachedToWindow(); if (!running && !off) { running = true; h.post(tick); } }
    @Override protected void onDetachedFromWindow() { running = false; h.removeCallbacks(tick); super.onDetachedFromWindow(); }

    private final Runnable tick = new Runnable() { @Override public void run() {
        if (!running) return;
        poll();
        if (running) h.postDelayed(this, TICK_MS);
    } };

    private static String num(float cur, float max) {   // the game's numbers are floats; show whole points, never 0 while there is some left
        int m = Math.max(0, Math.round(max)), c = cur <= 0 ? 0 : (int) Math.ceil(cur - 1e-3f);
        if (m > 0 && c > m) c = m;
        return String.format(Locale.ROOT, "%d / %d", c, m);
    }

    private void poll() {
        int r;
        try { r = prefs.getBoolean(PREF, PREF_DEFAULT) ? nativeTl2Orbs(v) : 0; }
        catch (UnsatisfiedLinkError e) { r = -1; }
        if (r < 0) { off = true; running = false; }   // no hook in this run: nothing will ever show
        boolean on = r > 0 && v[2] > 0 && game.getWidth() > 0 && game.getHeight() > 0;
        if (!on) { if (shown) { shown = false; invalidate(); } return; }
        String nh = num(v[1], v[2]), nm = num(v[3], v[4]);
        float gw = game.getWidth(), gh = game.getHeight();
        float nx = game.getX() + v[9] * gw, ny = game.getY() + (v[6] + v[8]) / 2 * gh;
        float ns = Math.max(10 * dp, Math.min(28 * dp, v[10] * gh));
        if (shown && nh.equals(hp) && nm.equals(mp) && Math.abs(nx - x) < 0.5f && Math.abs(ny - cy) < 0.5f && Math.abs(ns - size) < 0.25f) return;
        shown = true; hp = nh; mp = nm; x = nx; cy = ny; size = ns;
        fill.setTextSize(ns); stroke.setTextSize(ns); stroke.setStrokeWidth(Math.max(1.5f * dp, ns * 0.16f));
        invalidate();
    }

    @Override protected void onDraw(Canvas c) {
        if (!shown) return;
        float s = size;
        line(c, hp, x, cy - 0.25f * s, HP_COLOR);   // the line above the orb's middle
        line(c, mp, x, cy + 0.85f * s, MP_COLOR);   // and the one below it
    }
    private void line(Canvas c, String t, float x, float base, int color) {
        c.drawText(t, x, base, stroke);
        fill.setColor(color); c.drawText(t, x, base, fill);
    }
}
