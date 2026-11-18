package com.xiaoboluo.d2r;

import android.graphics.Bitmap;
import android.graphics.BitmapShader;
import android.graphics.Canvas;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Shader;
import android.os.SystemClock;
import android.view.View;

// [TL2] Icons of the skills / items bound to the pad buttons (hle_tl2_icons.c: the HUD action bar's slots as the game drew them; pref
// "tl2_icons", default on), shared by the touch layouts (Tl2Touch) and the gamepad layout (D3PadView). One instance per D3PadView: the
// serial is polled twice a second on its own timer while the layout that draws icons is shown; a redraw only when the icons changed.
// Slot button codes: 1 A, 3 B, 2 X, 0 Y, 8 R, 9 ZR, 10 L, 11 ZL. The natives stay Tl2Touch's (their JNI names).
final class Tl2Icons {
    static final int ICON = 64, NSLOT = 8;
    static final int EMPTY = 2;   // state(code): nothing bound to that button in the game
    private final View view;
    private final int[] px = new int[NSLOT * ICON * ICON], meta = new int[NSLOT * 4];
    private final Bitmap[] bmp = new Bitmap[16];
    private final BitmapShader[] sh = new BitmapShader[16];
    private final int[] state = new int[16];
    private int serial = -2; private long nextPoll; private boolean on = true, broken, active;
    private final RectF dst = new RectF(), src = new RectF(0, 0, ICON, ICON);
    private final Matrix m = new Matrix();
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
    private final Runnable tick = new Runnable() { public void run() { if (!on() || !active) return; if (poll()) view.invalidate(); view.postDelayed(this, 500); } };

    Tl2Icons(View v) { view = v; }

    static int codeOf(int key) {   // pad keycode (front_android.c) -> HUD slot button code, -1 = no slot
        switch (key) {
        case Tl2Touch.K_A: return 1; case Tl2Touch.K_B: return 3; case Tl2Touch.K_X: return 2; case Tl2Touch.K_Y: return 0;
        case Tl2Touch.K_R: return 8; case Tl2Touch.K_ZR: return 9; case Tl2Touch.K_L: return 10; case Tl2Touch.K_ZL: return 11; default: return -1;
        }
    }
    boolean on() { return on && !broken; }
    void reset(boolean enabled) { on = enabled; serial = -2; nextPoll = 0; }   // the pref was (re)read: fetch again on the next draw
    Bitmap icon(int code) { return on() && code >= 0 && code < 16 ? bmp[code] : null; }
    int state(int code) { return on() && code >= 0 && code < 16 ? state[code] : 0; }

    boolean poll() {
        long t = SystemClock.uptimeMillis(); if (!on() || t < nextPoll) return false; nextPoll = t + 450;
        int s; try { s = Tl2Touch.nativeTl2IconSerial(); } catch (UnsatisfiedLinkError e) { broken = true; return false; }
        if (s < 0 || s == serial) return false;
        serial = Tl2Touch.nativeTl2Icons(px, meta);
        java.util.Arrays.fill(state, 0);
        for (int i = 0; i < NSLOT; i++) {
            int code = meta[i * 4], st = meta[i * 4 + 1];
            if (code < 0 || code >= 16) continue;
            state[code] = st;
            bmp[code] = st == 1 ? Bitmap.createBitmap(px, i * ICON * ICON, ICON, ICON, ICON, Bitmap.Config.ARGB_8888) : null;
            // drawn as a shader-filled circle / rounded square: no clip path per button per frame
            sh[code] = bmp[code] == null ? null : new BitmapShader(bmp[code], Shader.TileMode.CLAMP, Shader.TileMode.CLAMP);
        }
        return true;
    }
    // Called from the owner's draw: shown = icons are drawn now (play, not the editor); keeps the poll timer running only then.
    void drawn(boolean shown) {
        active = shown;
        if (on() && shown) { view.removeCallbacks(tick); view.postDelayed(tick, 500); }
    }
    // The icon of that slot filling a circle of radius ir (or a rounded square) about (x, y); false = no icon there.
    boolean draw(Canvas c, int code, float x, float y, float ir, boolean square, float corner, int alpha) {
        if (icon(code) == null || sh[code] == null) return false;
        dst.set(x - ir, y - ir, x + ir, y + ir);
        m.setRectToRect(src, dst, Matrix.ScaleToFit.FILL); sh[code].setLocalMatrix(m);
        paint.setShader(sh[code]); paint.setAlpha(alpha);
        if (square) c.drawRoundRect(dst, corner, corner, paint); else c.drawCircle(x, y, ir, paint);
        return true;
    }
}
