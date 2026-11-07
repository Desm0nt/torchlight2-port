package com.xiaoboluo.d2r;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.SystemClock;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Locale;
import java.util.Map;

// [TL2] Torchlight II touch layout ("d3_layout" = "tl2", the TL2 default), modelled on D2R Mobile's touch controls
// (dm/ios/Sources/dm_touch_controls.c: dm_tc_layout_make / update / update_aim / dm_tc_end_at), not on the D3 touch layout.
// D3PadView hands touches and drawing to this class while that layout is picked; the gear dialog, FrontMenu, opacity / size sliders
// stay D3PadView's. Output: the on-screen pad's controller channel (DmActivity.nativePadKey / nativePadAxes, device PAD_DEVICE).
//
// Why front-end only: TL2's Switch build reads the touch screen once, at hid init (main 0x13ea04, the only call of
// nn::hid::GetTouchScreenState<16>), and never polls it; its CEGUI 0.6.2 has no mouse event names left (no "MouseClick" /
// "MouseButtonDown" / "Clicked" strings); its menus are console focus / radial menus (CConsoleUIMenu, CConsoleUIButtonMenu,
// CConsoleUIRadialMenu). So, unlike D2R, there is no game mouse to click with; every touch becomes npad buttons and sticks.
//
// Game mode (as D2R's game state):
//   * floating move stick: a touch in the lower left (x < 40% of the width, y > 45% of the height) puts the base under the finger,
//     axes from the finger relative to the touch point, full tilt at 60 dp, dead zone 6 dp; idle it is drawn only as a faint ring at its
//     rest spot. Diagonal pre-correction (D2R g_iso_fix): the vertical part is multiplied by "tl2_iso" % and the vector is scaled back,
//     so the screen angle the hero walks matches the finger's (calibrate: push 45 degrees, measure the walk angle t: iso = 1 / tan(t));
//   * right-thumb skill fan (D2R's geometry): A = primary (large; key down at once), B X Y R around it, ZR above R (D2R's portal spot),
//     the set-swap button left of ZR. A skill key goes down after 80 ms or as soon as the finger drags past 10 dp; while held, a drag
//     aims ("tl2_aim": right stick (default) / left stick / off; 60 dp = full tilt); a tap shorter than the key's decision time is made
//     up as a 100 ms pulse carrying the drag direction; dragging onto the "-" button cancels the skill (no cast);
//   * set II (swap button): the modifier ("tl2_mod": ZL / ZR / L / R) goes down 170 ms before any skill key and is held only while a
//     skill is in use (D2R DM_TC_ZL_LEAD_S), so the other buttons keep their own meaning;
//   * D2R's belt row at the bottom centre: d-pad left / up / down / right and "+"; "-" top right (D2R's map spot), ZL / L / L3 top left,
//     R3 top right, gear and the MENU toggle on top;
//   * world: a quick tap anywhere else (or a quick tap in place inside the stick zone, as D2R) = "tl2_tap" (off / A pulse (default) /
//     face the point then A); touch and hold the world ("tl2_hold_walk", default on) = walk toward the finger from the hero's spot
//     (screen centre, "tl2_hero_y" % from the top), the stick wins over it.
// Menu mode (D2R's panel state: the fan, belt and stick go away): tap = A, long press = hold A, a two-finger tap = B, a swipe =
//   d-pad steps (one per 48 dp; "tl2_nav_swipe" 0) or the left stick toward the swipe (1, radial menus); B (in A's place), X, Y, R, ZR,
//   the top row, gear and the toggle stay. The game starts in menu mode; "+" / "-" in game mode switch to it ("tl2_auto_nav", default on);
//   the toggle (MENU / GAME) switches by hand.
// Edit mode: a control drags its group (fan, belt, top left, top right, top centre); offsets "tl2_off_<group>" = "fx,fy" of the view.
final class Tl2Touch {
    static final int K_A = KeyEvent.KEYCODE_ENTER, K_B = KeyEvent.KEYCODE_ESCAPE, K_X = KeyEvent.KEYCODE_R, K_Y = KeyEvent.KEYCODE_F,
        K_L = KeyEvent.KEYCODE_Q, K_R = KeyEvent.KEYCODE_E, K_ZL = KeyEvent.KEYCODE_SHIFT_LEFT, K_ZR = KeyEvent.KEYCODE_CTRL_LEFT,
        K_PLUS = KeyEvent.KEYCODE_TAB, K_MINUS = KeyEvent.KEYCODE_M, K_UP = KeyEvent.KEYCODE_DPAD_UP, K_DOWN = KeyEvent.KEYCODE_DPAD_DOWN,
        K_LEFT = KeyEvent.KEYCODE_DPAD_LEFT, K_RIGHT = KeyEvent.KEYCODE_DPAD_RIGHT, K_L3 = KeyEvent.KEYCODE_BUTTON_THUMBL, K_R3 = KeyEvent.KEYCODE_BUTTON_THUMBR;
    static final int[] MOD_KEYS = {K_ZL, K_ZR, K_L, K_R};
    static final int TAP_OFF = 0, TAP_A = 1, TAP_FACE_A = 2, AIM_OFF = 0, AIM_RIGHT = 1, AIM_LEFT = 2;
    static final int ISO_MIN = 100, ISO_MAX = 250, ISO_DEFAULT = 130, HERO_Y_MIN = 35, HERO_Y_MAX = 65, HERO_Y_DEFAULT = 50;

    // D2R timings (dm_touch_controls.h)
    static final long AIM_SETTLE_MS = 80, KEY_MIN_MS = 35, MOD_LEAD_MS = 170, TAP_MS = 350, HOLD_MS = 500, PULSE_MS = 100;
    static final float MOVE_DP = 10, AIM_DEAD_DP = 10, AIM_FULL_DP = 60, JOY_FULL_DP = 60, JOY_DEAD_DP = 6;
    // TL2-only gestures
    static final long FACE_MS = 70, NAV_LONG_MS = 450, NAV_MIN_STICK_MS = 90, STEP_GAP_MS = 40;
    static final float NAV_SWIPE_DP = 20, NAV_STEP_DP = 48, NAV_STICK_DP = 70, WALK_DEAD_DP = 30, WALK_FULL_DP = 140, WALK_MIN_TILT = 0.4f;

    static final int KIND_MAIN = 0, KIND_SKILL = 1, KIND_PLAIN = 2, KIND_SWAP = 3, KIND_GEAR = 4, KIND_MODE = 5;
    private static final class Btn {
        final String label, group; final int key, kind; final boolean game, nav; final float bx, by, br;   // base spot / radius in k units, set by layout()
        float x, y, r; int pointer = -1;
        Btn(String label, int key, int kind, String group, boolean game, boolean nav, float bx, float by, float br) {
            this.label = label; this.key = key; this.kind = kind; this.group = group; this.game = game; this.nav = nav; this.bx = bx; this.by = by; this.br = br;
        }
    }
    private static final int T_NONE = -1, T_JOY = -2, T_WORLD = -3, T_NAV = -4;
    private static final class Finger {
        int target = T_NONE; float x, y, x0, y0; long t0, keyT; boolean keyDown, cancelled, moved, swiping, holdA, walking; long seq;
        float stepX, stepY;   // nav d-pad steps: where the last step was counted from
    }
    private static final class Pulse { final int key; final long start, end; final float ax, ay; final boolean mod;
        Pulse(int key, long start, long end, float ax, float ay, boolean mod) { this.key = key; this.start = start; this.end = end; this.ax = ax; this.ay = ay; this.mod = mod; } }

    private final View view; private final Runnable openSettings;
    private final float dp;
    private final List<Btn> btns = new ArrayList<>();
    private final Map<Integer, Finger> fingers = new HashMap<>();
    private final List<Pulse> pulses = new ArrayList<>();
    private final Map<String, float[]> off = new HashMap<>();
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private float k = 1, op = 1, iso = ISO_DEFAULT / 100f, heroFy = HERO_Y_DEFAULT / 100f;
    private int tapMode = TAP_A, aimMode = AIM_RIGHT, modKey = K_ZL, navSwipe = 0; private boolean holdWalk = true, autoNav = true;
    private String menuLabel = "MENU", gameLabel = "GAME", editHint = "", resetLabel = "Reset", doneLabel = "Done";
    private boolean nav = true, setII, editMode, controllerMode;
    private int lw, lh; private long seqN;
    // joystick
    private int joyPtr = -1; private float joyTx, joyTy, joyBx, joyBy, joyRx, joyRy, joyR, joyMinX, joyMaxY;
    private final java.util.HashSet<Integer> sent = new java.util.HashSet<>();
    private float sLx, sLy, sRx, sRy; private boolean axesSent;
    private long faceUntil; private float faceX, faceY;

    Tl2Touch(View v, Runnable openSettings) {
        view = v; this.openSettings = openSettings; dp = v.getResources().getDisplayMetrics().density;
        line.setStyle(Paint.Style.STROKE); line.setStrokeWidth(2 * dp); text.setTextAlign(Paint.Align.CENTER);
        // D2R dm_tc_layout_make, in k (dp x size) units from the bottom right / centres; see layout()
        final float mx = -74, my = -84;   // main: W - 86 + 12, H - 88 - 24 + 28
        add("A", K_A, KIND_MAIN, "fan", true, false, mx, my, 40);
        add("B", K_B, KIND_MAIN, "fan", false, true, mx, my, 40);   // menu mode: back in A's place
        float[] dx = {-96, -84, -34, 34}, dy = {18, -50, -92, -92}; String[] l = {"B", "X", "Y", "R"}; int[] kk = {K_B, K_X, K_Y, K_R};
        for (int i = 0; i < 4; i++) add(l[i], kk[i], KIND_SKILL, "fan", true, i > 0, mx + dx[i], my + dy[i], 26);
        float zx = mx + 34, zy = my - 92 - 26 - 12 - 26;   // D2R's portal: straight above skill 4
        add("ZR", K_ZR, KIND_SKILL, "fan", true, true, zx, zy, 26);
        float px = 34, py = zy - my, cs = (float) Math.cos(.46f), sn = (float) Math.sin(.46f);
        add("II", 0, KIND_SWAP, "fan", true, false, mx + px * cs + py * sn, my - px * sn + py * cs, 22);
        add("-", K_MINUS, KIND_PLAIN, "tr", true, true, -30, 30, 22);   // D2R's map spot (top right)
        add("R3", K_R3, KIND_PLAIN, "tr", true, true, -80, 30, 19);
        add("ZL", K_ZL, KIND_PLAIN, "tl", true, true, 32, 30, 22);
        add("L", K_L, KIND_PLAIN, "tl", true, true, 82, 30, 22);
        add("L3", K_L3, KIND_PLAIN, "tl", true, true, 128, 30, 19);
        add("⚙", 0, KIND_GEAR, "top", true, true, -40, 30, 20);   // x from the centre
        add(menuLabel, 0, KIND_MODE, "top", true, true, 30, 30, 24);
        // D2R's belt (bottom UI v4 cells 1..5, in "u" = height / 1320 units from the centre; see layout())
        String[] bl = {"←", "↑", "↓", "→", "+"}; int[] bk = {K_LEFT, K_UP, K_DOWN, K_RIGHT, K_PLUS};
        for (int i = 0; i < 5; i++) add(bl[i], bk[i], KIND_PLAIN, "belt", true, i == 4, (120 * (i + 1) - 344) / 120f, 0, 19);   // x in cell pitches
    }
    private void add(String label, int key, int kind, String group, boolean game, boolean nav, float bx, float by, float br) { btns.add(new Btn(label, key, kind, group, game, nav, bx, by, br)); }

    void reload(Context ui, SharedPreferences p, float opacity, float scale) {
        op = opacity; k = dp * scale;
        iso = clamp(p.getInt("tl2_iso", ISO_DEFAULT), ISO_MIN, ISO_MAX) / 100f;
        heroFy = clamp(p.getInt("tl2_hero_y", HERO_Y_DEFAULT), HERO_Y_MIN, HERO_Y_MAX) / 100f;
        tapMode = clamp(p.getInt("tl2_tap", TAP_A), 0, 2);
        aimMode = clamp(p.getInt("tl2_aim", AIM_RIGHT), 0, 2);
        modKey = MOD_KEYS[clamp(p.getInt("tl2_mod", 0), 0, MOD_KEYS.length - 1)];
        navSwipe = clamp(p.getInt("tl2_nav_swipe", 0), 0, 1);
        holdWalk = p.getBoolean("tl2_hold_walk", true); autoNav = p.getBoolean("tl2_auto_nav", true);
        menuLabel = ui.getString(R.string.d3_pad_menu); gameLabel = ui.getString(R.string.tl2_pad_game);
        editHint = ui.getString(R.string.d3_pad_edit_hint); resetLabel = ui.getString(R.string.d3_pad_reset); doneLabel = ui.getString(R.string.d3_pad_done);
        off.clear();
        for (String g : new String[] {"fan", "belt", "tl", "tr", "top"}) {
            String v = p.getString("tl2_off_" + g, null); if (v == null) continue;
            String[] f = v.split(",");
            try { if (f.length == 2) off.put(g, new float[] {Float.parseFloat(f[0]), Float.parseFloat(f[1])}); } catch (NumberFormatException e) { /* ignore */ }
        }
        lw = -1; view.invalidate();
    }
    private static int clamp(int v, int lo, int hi) { return Math.max(lo, Math.min(hi, v)); }

    // ---------------- layout (D2R dm_tc_layout_make) ----------------
    private void layout() {
        int w = view.getWidth(), h = view.getHeight();
        if (w == lw && h == lh) return;
        lw = w; lh = h;
        for (Btn b : btns) {
            float x, y, r;
            switch (b.group) {
            case "fan": x = w + b.bx * k; y = h + b.by * k; r = b.br * k; break;
            case "tr": x = w + b.bx * k; y = b.by * k; r = b.br * k; break;
            case "top": x = w / 2f + b.bx * k; y = b.by * k; r = b.br * k; break;
            case "belt": x = w / 2f + b.bx * 40 * k; y = h - 22 * k; r = b.br * k; break;   // D2R: pitch 120 / 1320 of the height, centre 78.5 / 1320 above the bottom (~40 / 22 dp on a phone)
            default: x = b.bx * k; y = b.by * k; r = b.br * k; break;
            }
            float[] o = off.get(b.group);
            if (o != null) { x += o[0] * w; y += o[1] * h; }
            b.x = x; b.y = y; b.r = r;
        }
        joyR = 60 * k; joyRx = Math.max(90 * k, 60 * k) + 30 * k; joyRy = h - 115 * k;
        joyMinX = joyR + 8 * k; joyMaxY = h - joyR - 8 * k;
    }
    private boolean shown(Btn b) {
        if (editMode) return b.game || b.nav;
        if (controllerMode) return b.kind == KIND_GEAR;
        return nav ? b.nav : b.game;
    }
    private Btn hit(float x, float y) {
        Btn best = null; float bd = Float.MAX_VALUE;
        for (Btn b : btns) {
            if (!shown(b)) continue;
            float dx = x - b.x, dy = y - b.y, r = b.r * 1.15f, d = dx * dx + dy * dy;
            if (d <= r * r && d < bd) { best = b; bd = d; }
        }
        return best;
    }
    private Btn btnOf(Finger f) { return f.target >= 0 && f.target < btns.size() ? btns.get(f.target) : null; }
    private boolean inMinus(float x, float y) { for (Btn b : btns) if (b.key == K_MINUS && shown(b)) return Math.abs(x - b.x) <= b.r && Math.abs(y - b.y) <= b.r; return false; }

    // ---------------- mode ----------------
    boolean menuMode() { return nav; }
    void setMenuMode(boolean on) { if (on == nav) return; releaseAll(); nav = on; view.invalidate(); }
    void setControllerMode(boolean on) { controllerMode = on; if (on) releaseAll(); view.invalidate(); }

    // ---------------- output ----------------
    private static void key(int k, boolean down) { DmActivity.nativePadKey(D3PadView.PAD_DEVICE, k, down); }
    private final Runnable tick = new Runnable() { public void run() { update(); } };
    private long now() { return SystemClock.uptimeMillis(); }
    private void pulse(int key, long delay, float ax, float ay, boolean mod) {
        long t = now() + delay;
        for (Pulse p : pulses) if (p.key == key) t = Math.max(t, p.end + STEP_GAP_MS);   // the same key again: after a release the game sees
        pulses.add(new Pulse(key, t, t + PULSE_MS, ax, ay, mod));
    }

    // Recomputes every held key and both sticks from the fingers and pulses, sends what changed, and keeps ticking while anything is timed.
    private void update() {
        view.removeCallbacks(tick);
        if (controllerMode || editMode) return;
        long t = now();
        java.util.HashSet<Integer> keys = new java.util.HashSet<>();
        boolean timed = false, modUse = false;
        float lx = 0, ly = 0, rx = 0, ry = 0; boolean lSet = false;
        Finger aim = null;
        for (Finger f : fingers.values()) {
            Btn b = btnOf(f);
            if (f.target == T_NAV && f.holdA) keys.add(K_A);
            if (b == null || f.cancelled) continue;
            if (b.kind == KIND_PLAIN) { keys.add(b.key); continue; }
            if (b.kind != KIND_MAIN && b.kind != KIND_SKILL) continue;
            if (nav) { keys.add(b.key); continue; }   // menu mode: B / X / Y / R / ZR are plain buttons
            if (b.kind == KIND_SKILL && !f.keyDown) {
                if (dist(f) > AIM_DEAD_DP * dp || t - f.t0 >= AIM_SETTLE_MS) { f.keyDown = true; f.keyT = t; } else timed = true;
            }
            if (setII) { modUse = true; if (t - f.t0 < MOD_LEAD_MS) { timed = true; continue; } }
            if (f.keyDown || b.kind == KIND_MAIN) keys.add(b.key);
            if (dist(f) > AIM_DEAD_DP * dp && (aim == null || f.seq > aim.seq)) aim = f;
        }
        for (Iterator<Pulse> it = pulses.iterator(); it.hasNext();) {
            Pulse p = it.next();
            if (t >= p.end) { it.remove(); continue; }
            timed = true;
            if (p.mod) modUse = true;
            if (t >= p.start) { keys.add(p.key); if (p.ax != 0 || p.ay != 0) { if (aimMode == AIM_LEFT) { lx = p.ax; ly = p.ay; lSet = true; } else { rx = p.ax; ry = p.ay; } } }
        }
        if (modUse && setII && !nav) keys.add(modKey);
        if (aim != null && aimMode != AIM_OFF) {
            float ax = Math.max(-1, Math.min(1, (aim.x - aim.x0) / (AIM_FULL_DP * dp))), ay = Math.max(-1, Math.min(1, (aim.y - aim.y0) / (AIM_FULL_DP * dp)));
            if (aimMode == AIM_LEFT) { lx = ax; ly = ay; lSet = true; } else { rx = ax; ry = ay; }
        }
        // left stick: the joystick wins, then a nav stick swipe, then walking toward a held world finger, then a "face the point" nudge
        if (joyPtr >= 0) { Finger f = fingers.get(joyPtr); if (f != null) { float[] v = joyAxes(f); lx = v[0]; ly = v[1]; lSet = true; } }
        if (!lSet) for (Finger f : fingers.values()) {
            if (f.target == T_NAV && f.swiping && navSwipe == 1) { float d = dist(f); if (d > 0) { float m = Math.min(1f, d / (NAV_STICK_DP * dp)); lx = (f.x - f.x0) / d * m; ly = (f.y - f.y0) / d * m; lSet = true; } }
            else if (f.target == T_WORLD && f.walking) { float[] v = walkAxes(f.x, f.y); lx = v[0]; ly = v[1]; lSet = true; }
            else if (f.target == T_WORLD && holdWalk && !f.walking) timed = true;   // becomes a walk at HOLD
            else if (f.target == T_NAV && !f.swiping && !f.holdA) timed = true;      // becomes a held A at NAV_LONG
        }
        if (!lSet && t < faceUntil) { lx = faceX; ly = faceY; timed = true; }
        for (Finger f : fingers.values()) if (f.target == T_WORLD && !f.walking && holdWalk && t - f.t0 >= HOLD_MS) { f.walking = true; float[] v = walkAxes(f.x, f.y); if (!lSet) { lx = v[0]; ly = v[1]; } }
        for (Finger f : fingers.values()) if (f.target == T_NAV && !f.swiping && !f.holdA && !twoFinger && t - f.t0 >= NAV_LONG_MS) { f.holdA = true; keys.add(K_A); }
        // send
        for (Integer kc : new ArrayList<>(sent)) if (!keys.contains(kc)) { key(kc, false); sent.remove(kc); }
        for (Integer kc : keys) if (!sent.contains(kc)) { key(kc, true); sent.add(kc); }
        if (!axesSent || lx != sLx || ly != sLy || rx != sRx || ry != sRy) {
            sLx = lx; sLy = ly; sRx = rx; sRy = ry; axesSent = true;
            DmActivity.nativePadAxes(D3PadView.PAD_DEVICE, lx, ly, rx, ry, 0, 0, 0, 0);
        }
        if (timed || !fingers.isEmpty()) view.postDelayed(tick, 16);
        view.invalidate();
    }
    private float dist(Finger f) { return (float) Math.hypot(f.x - f.x0, f.y - f.y0); }
    // D2R update(): relative to the touch point, dead 6 dp, full at 60 dp, diagonal pre-correction on the vertical part
    private float[] joyAxes(Finger f) {
        float dx = f.x - joyTx, dy = f.y - joyTy, d = (float) Math.hypot(dx, dy), dead = JOY_DEAD_DP * dp, full = JOY_FULL_DP * dp;
        if (d <= dead) return new float[] {0, 0};
        float s = Math.min(1, (d - dead) / (full - dead));
        return isoFix(dx / d, dy / d, s);
    }
    private float[] isoFix(float ex, float ey, float s) { float n = (float) Math.hypot(ex, iso * ey); return new float[] {ex / n * s, iso * ey / n * s}; }
    private float[] walkAxes(float x, float y) {
        float hx = view.getWidth() * 0.5f, hy = view.getHeight() * heroFy, dx = x - hx, dy = y - hy, d = (float) Math.hypot(dx, dy) / dp;
        if (d < WALK_DEAD_DP) return new float[] {0, 0};
        float m = WALK_MIN_TILT + (1 - WALK_MIN_TILT) * Math.min(1f, (d - WALK_DEAD_DP) / (WALK_FULL_DP - WALK_DEAD_DP)), dd = d * dp;
        return isoFix(dx / dd, dy / dd, m);
    }
    private void worldTap(float x, float y) {
        if (tapMode == TAP_OFF) return;
        long delay = 0;
        if (tapMode == TAP_FACE_A) {
            float[] v = walkAxes(x, y);
            float m = (float) Math.hypot(v[0], v[1]);
            if (m > 0) { faceX = v[0] / m * 0.35f; faceY = v[1] / m * 0.35f; faceUntil = now() + FACE_MS; delay = FACE_MS - 20; }
        }
        pulse(K_A, delay, 0, 0, false);
    }

    // ---------------- touches ----------------
    private boolean twoFinger;
    boolean onTouch(MotionEvent e) {
        layout();
        int a = e.getActionMasked(), i = e.getActionIndex(), id = e.getPointerId(i);
        float x = e.getX(i), y = e.getY(i);
        if (editMode) { editTouch(e, a, id, x, y); return true; }
        if (controllerMode) {
            if (a == MotionEvent.ACTION_DOWN || a == MotionEvent.ACTION_POINTER_DOWN) { Btn b = hit(x, y); if (b != null && b.kind == KIND_GEAR) openSettings.run(); }
            return true;
        }
        switch (a) {
        case MotionEvent.ACTION_DOWN: case MotionEvent.ACTION_POINTER_DOWN: begin(id, x, y); break;
        case MotionEvent.ACTION_MOVE:
            for (int n = 0; n < e.getPointerCount(); n++) move(e.getPointerId(n), e.getX(n), e.getY(n));
            break;
        case MotionEvent.ACTION_POINTER_UP: end(id, false); break;
        case MotionEvent.ACTION_UP: for (Integer p : new ArrayList<>(fingers.keySet())) end(p, false); break;
        case MotionEvent.ACTION_CANCEL: for (Integer p : new ArrayList<>(fingers.keySet())) end(p, true); break;
        }
        update();
        return true;
    }
    private void begin(int id, float x, float y) {
        if (fingers.containsKey(id)) return;
        Finger f = new Finger(); f.x = f.x0 = f.stepX = x; f.y = f.y0 = f.stepY = y; f.t0 = now();
        Btn b = hit(x, y);
        if (b != null && b.kind == KIND_GEAR) { openSettings.run(); return; }
        if (b != null && b.kind == KIND_MODE) { setMenuMode(!nav); return; }
        if (b != null && b.kind == KIND_SWAP) { setII = !setII; return; }
        if (b != null) f.target = btns.indexOf(b);
        else if (nav) {
            f.target = T_NAV;
            for (Finger o : fingers.values()) if (o.target == T_NAV && !o.swiping && !o.holdA) twoFinger = true;   // a second finger on the free area: B
        } else if (joyPtr < 0 && x < view.getWidth() * 0.40f && y > view.getHeight() * 0.45f) {
            f.target = T_JOY; joyPtr = id; joyTx = x; joyTy = y; joyBx = Math.max(x, joyMinX); joyBy = Math.min(y, joyMaxY);
        } else if (y > 60 * k) f.target = T_WORLD;
        fingers.put(id, f);
    }
    private void move(int id, float x, float y) {
        Finger f = fingers.get(id); if (f == null) return;
        f.x = x; f.y = y;
        float d = dist(f);
        if (d > MOVE_DP * dp) f.moved = true;
        Btn b = btnOf(f);
        if (b != null && !nav && (b.kind == KIND_MAIN || b.kind == KIND_SKILL)) {
            if (inMinus(x, y)) f.cancelled = true;
            if (!f.cancelled) f.seq = ++seqN;
        } else if (f.target == T_WORLD && f.moved && holdWalk) f.walking = true;
        else if (f.target == T_NAV) navMove(f, d);
    }
    private void navMove(Finger f, float d) {
        if (!f.swiping && !f.holdA && !twoFinger && d > NAV_SWIPE_DP * dp) f.swiping = true;
        if (!f.swiping || navSwipe != 0) return;
        float dx = f.x - f.stepX, dy = f.y - f.stepY, step = NAV_STEP_DP * dp;
        if (Math.abs(dx) >= step && Math.abs(dx) >= Math.abs(dy)) { pulse(dx > 0 ? K_RIGHT : K_LEFT, 0, 0, 0, false); f.stepX += Math.signum(dx) * step; f.stepY = f.y; }
        else if (Math.abs(dy) >= step) { pulse(dy > 0 ? K_DOWN : K_UP, 0, 0, 0, false); f.stepY += Math.signum(dy) * step; f.stepX = f.x; }
    }
    private void end(int id, boolean cancel) {
        Finger f = fingers.remove(id); if (f == null) return;
        long t = now(), held = t - f.t0;
        Btn b = btnOf(f);
        if (id == joyPtr) joyPtr = -1;
        if (cancel) { if (f.target == T_NAV && fingers.isEmpty()) twoFinger = false; return; }
        boolean tap = held < TAP_MS && dist(f) <= MOVE_DP * dp;
        if (b != null && !nav && b.kind == KIND_SKILL && !f.cancelled && (!f.keyDown || t - f.keyT < KEY_MIN_MS || (setII && held < MOD_LEAD_MS))) {
            float ax = 0, ay = 0, d = dist(f);   // D2R dm_tc_end_at: a tap too short for the key becomes a pulse with the drag's direction
            if (d > AIM_DEAD_DP * dp && aimMode != AIM_OFF) { ax = (f.x - f.x0) / d; ay = (f.y - f.y0) / d; }
            pulse(b.key, setII ? Math.max(0, MOD_LEAD_MS - held) : 0, ax, ay, setII);
        }
        if (b != null && !nav && b.kind == KIND_MAIN && !f.cancelled && (held < KEY_MIN_MS || (setII && held < MOD_LEAD_MS))) pulse(b.key, setII ? Math.max(0, MOD_LEAD_MS - held) : 0, 0, 0, setII);
        else if (b != null && b.key != 0 && (b.kind == KIND_PLAIN || nav) && held < KEY_MIN_MS) pulse(b.key, 0, 0, 0, false);   // too short for the game to see: make it a pulse
        if (b != null && !nav && autoNav && (b.key == K_PLUS || b.key == K_MINUS))   // the game's pause / inventory menu comes up: menu gestures,
            view.postDelayed(() -> setMenuMode(true), PULSE_MS + 50);              // once the key (or its make-up pulse) has reached the game
        if ((f.target == T_JOY || f.target == T_WORLD) && tap) worldTap(f.x0, f.y0);
        if (f.target == T_NAV) {
            boolean last = true; for (Finger o : fingers.values()) if (o.target == T_NAV) last = false;
            if (twoFinger) { if (last) { if (tap || held < TAP_MS * 2) pulse(K_B, 0, 0, 0, false); twoFinger = false; } }
            else if (f.holdA) { /* released with the finger: update() drops A */ }
            else if (f.swiping && navSwipe == 1) {   // a quick flick: keep the stick out until the game has seen it
                long rest = NAV_MIN_STICK_MS - held; float d = dist(f);
                if (rest > 0 && d > 0) { float m = Math.min(1f, d / (NAV_STICK_DP * dp)); faceX = (f.x - f.x0) / d * m; faceY = (f.y - f.y0) / d * m; faceUntil = t + rest; }
            }
            else if (!f.swiping && held < NAV_LONG_MS) pulse(K_A, 0, 0, 0, false);
        }
    }
    void releaseAll() {
        fingers.clear(); pulses.clear(); joyPtr = -1; twoFinger = false; faceUntil = 0;
        view.removeCallbacks(tick);
        for (Integer kc : sent) key(kc, false);
        sent.clear();
        if (axesSent && (sLx != 0 || sLy != 0 || sRx != 0 || sRy != 0) && !controllerMode) DmActivity.nativePadAxes(D3PadView.PAD_DEVICE, 0, 0, 0, 0, 0, 0, 0, 0);
        sLx = sLy = sRx = sRy = 0;
        view.invalidate();
    }

    // ---------------- edit mode ----------------
    private Runnable editDone; private int editPtr = -1; private String editGroup; private float editX0, editY0; private float[] editO0;
    private final RectF resetRect = new RectF(), doneRect = new RectF(), panelRect = new RectF();
    boolean editing() { return editMode; }
    void startEdit(Runnable done) { releaseAll(); editDone = done; editMode = true; view.invalidate(); }
    void endEdit() { editMode = false; editPtr = -1; Runnable d = editDone; editDone = null; view.invalidate(); if (d != null) d.run(); }
    void resetLayout() {
        SharedPreferences.Editor ed = view.getContext().getSharedPreferences("doom_controls", Context.MODE_PRIVATE).edit();
        for (String g : new String[] {"fan", "belt", "tl", "tr", "top"}) ed.remove("tl2_off_" + g);
        ed.apply(); off.clear(); lw = -1; view.invalidate();
    }
    private void editTouch(MotionEvent e, int a, int id, float x, float y) {
        switch (a) {
        case MotionEvent.ACTION_DOWN: case MotionEvent.ACTION_POINTER_DOWN:
            if (resetRect.contains(x, y)) { resetLayout(); break; }
            if (doneRect.contains(x, y)) { endEdit(); break; }
            if (editPtr < 0) { Btn b = hit(x, y); if (b != null) { editPtr = id; editGroup = b.group; editX0 = x; editY0 = y; float[] o = off.get(b.group); editO0 = o == null ? new float[] {0, 0} : o.clone(); } }
            break;
        case MotionEvent.ACTION_MOVE:
            for (int n = 0; n < e.getPointerCount(); n++) if (e.getPointerId(n) == editPtr && view.getWidth() > 0 && view.getHeight() > 0) {
                off.put(editGroup, new float[] {editO0[0] + (e.getX(n) - editX0) / view.getWidth(), editO0[1] + (e.getY(n) - editY0) / view.getHeight()});
                lw = -1;
            }
            break;
        case MotionEvent.ACTION_UP: case MotionEvent.ACTION_POINTER_UP: case MotionEvent.ACTION_CANCEL:
            if (editPtr >= 0 && (id == editPtr || a != MotionEvent.ACTION_POINTER_UP)) {
                float[] o = off.get(editGroup);
                if (o != null) view.getContext().getSharedPreferences("doom_controls", Context.MODE_PRIVATE).edit()
                    .putString("tl2_off_" + editGroup, String.format(Locale.ROOT, "%.4f,%.4f", o[0], o[1])).apply();
                editPtr = -1;
            }
            break;
        }
        view.invalidate();
    }

    // ---------------- drawing ----------------
    void draw(Canvas c) {
        layout();
        if (editMode) c.drawColor(0x50000000);
        if (!controllerMode && !nav) {
            float kk = op;
            if (joyPtr >= 0 && fingers.get(joyPtr) != null) {   // the base where the thumb landed, the knob = base + axes x radius
                float[] v = joyAxes(fingers.get(joyPtr));
                fill.setColor(0xffffffff); fill.setAlpha(Math.round(0x28 * kk)); c.drawCircle(joyBx, joyBy, joyR, fill);
                line.setColor(0xffffffff); line.setAlpha(Math.round(0x90 * kk)); c.drawCircle(joyBx, joyBy, joyR, line);
                fill.setAlpha(Math.round(0x80 * kk)); c.drawCircle(joyBx + v[0] * joyR, joyBy + v[1] / Math.max(1f, iso) * joyR, joyR * 0.42f, fill);
            } else {   // rest spot: a faint ring only
                line.setColor(0xffffffff); line.setAlpha(Math.round((editMode ? 0xc0 : 0x38) * kk)); c.drawCircle(joyRx, joyRy, joyR, line);
            }
            for (Finger f : fingers.values()) if (f.target == T_WORLD && f.walking) { line.setColor(0xffffffff); line.setAlpha(Math.round(0x90 * kk)); c.drawCircle(f.x, f.y, 22 * dp, line); }
        }
        for (Btn b : btns) if (shown(b)) drawBtn(c, b);
        if (editMode) drawEditPanel(c);
    }
    private boolean held(Btn b) {
        if (b.kind == KIND_SWAP) return setII;
        if (b.kind == KIND_MODE) return nav;
        for (Finger f : fingers.values()) if (f.target >= 0 && btns.get(f.target) == b && !f.cancelled) return true;
        return b.key != 0 && sent.contains(b.key);
    }
    private void drawBtn(Canvas c, Btn b) {
        boolean on = held(b);
        float kk = b.kind == KIND_GEAR ? 1f : (editMode ? Math.max(op, 0.5f) : op);
        int tint = b.kind == KIND_SWAP && setII ? 0xffffb030 : b.kind == KIND_MODE && nav ? 0xff60b0ff : 0xffffffff;
        fill.setColor(tint); fill.setAlpha(Math.round((on ? 0x80 : 0x30) * kk));
        line.setColor(editMode ? 0xffffd040 : tint); line.setAlpha(editMode ? 0xe0 : Math.round(0x90 * kk));
        c.drawCircle(b.x, b.y, b.r, fill); c.drawCircle(b.x, b.y, b.r, line);
        String lbl = b.kind == KIND_MODE ? (nav ? gameLabel : menuLabel) : b.label;
        float ts = Math.max(11 * dp, 0.6f * b.r); text.setTextSize(ts);
        float w = text.measureText(lbl); if (w > 1.7f * b.r) { ts *= 1.7f * b.r / w; text.setTextSize(ts); }
        text.setColor(0xffffffff); text.setAlpha(Math.round(0xd8 * kk));
        c.drawText(lbl, b.x, b.y + ts * 0.35f, text);
    }
    private void drawEditPanel(Canvas c) {
        float w = view.getWidth(), h = view.getHeight(), pw = Math.min(w * 0.8f, 440 * dp), bh = 44 * dp, bw = (pw - 36 * dp) / 2, top = h * 0.30f;
        text.setTextSize(16 * dp); text.setAlpha(0xff);
        panelRect.set((w - pw) / 2, top, (w + pw) / 2, top + 28 * dp + bh + 24 * dp);
        fill.setColor(0xc0202020); c.drawRoundRect(panelRect, 12 * dp, 12 * dp, fill);
        text.setColor(0xffffffff); c.drawText(editHint, w / 2, top + 26 * dp, text);
        float by = top + 40 * dp;
        resetRect.set(panelRect.left + 12 * dp, by, panelRect.left + 12 * dp + bw, by + bh);
        doneRect.set(panelRect.right - 12 * dp - bw, by, panelRect.right - 12 * dp, by + bh);
        fill.setColor(0xff505050); c.drawRoundRect(resetRect, 8 * dp, 8 * dp, fill);
        fill.setColor(0xff2f7d32); c.drawRoundRect(doneRect, 8 * dp, 8 * dp, fill);
        c.drawText(resetLabel, resetRect.centerX(), resetRect.centerY() + 6 * dp, text);
        c.drawText(doneLabel, doneRect.centerX(), doneRect.centerY() + 6 * dp, text);
    }
}
