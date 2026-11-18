package com.xiaoboluo.d2r;

import android.view.View;

// [TL2] Touch on the game's own menus and map (hle_tl2_ui.c, hle_tl2_map.c), shared by the touch layouts (Tl2Touch) and the gamepad
// layout (D3PadView). Points are the view's pixels: the view is the whole game picture (TL2 widescreen and 16:9 both fill it; menus are
// never on the pillarboxed loading screen). A finger off the overlay buttons while a console menu is up, or on the shown minimap / big
// map, waits: a short still tap (< TAP_MS, <= MOVE_DP) goes to tap(); a mostly vertical move from a scrollable list drags it (drag phases
// 0 / 1 / 2, no tap); any other move makes it the finger it would have been without the menu, from where it went down.
final class Tl2Pointer {
    static final long TAP_MS = Tl2Touch.TAP_MS;
    static final float MOVE_DP = Tl2Touch.MOVE_DP;
    private Tl2Pointer() {}

    static boolean menuOpen() { try { return DmActivity.nativeTl2MenuOpen(); } catch (UnsatisfiedLinkError e) { return false; } }
    // the finger should wait for a tap: a console menu is up or it is on the shown minimap / big map
    static boolean waits(View v, float x, float y) { return menuOpen() || mapHit(v, x, y); }
    static boolean mapHit(View v, float x, float y) {
        int w = v.getWidth(), h = v.getHeight();
        try { return w > 0 && h > 0 && DmActivity.nativeTl2MapHit(x / w, y / h); } catch (UnsatisfiedLinkError e) { return false; }
    }
    static boolean listHit(View v, float x, float y) {
        int w = v.getWidth(), h = v.getHeight();
        try { return w > 0 && h > 0 && DmActivity.nativeTl2UiListHit(x / w, y / h); } catch (UnsatisfiedLinkError e) { return false; }
    }
    // a moved waiting finger scrolls a list: mostly vertical and it went down on a scrollable one
    static boolean startsDrag(View v, float x0, float y0, float x, float y) { return Math.abs(y - y0) > Math.abs(x - x0) && listHit(v, x0, y0); }
    static void drag(View v, float x0, float y0, float y, int phase) {
        int w = v.getWidth(), h = v.getHeight();
        try { if (w > 0 && h > 0) DmActivity.nativeTl2UiDrag(x0 / w, y0 / h, (y - y0) / h, phase); } catch (UnsatisfiedLinkError e) { /* old library */ }
    }
    // true = the game's UI took the tap (a menu item, or with no menu up the minimap <-> big map switch)
    static boolean tap(View v, float x, float y) {
        int w = v.getWidth(), h = v.getHeight();
        try { return w > 0 && h > 0 && DmActivity.nativeTl2UiTap(x / w, y / h); } catch (UnsatisfiedLinkError e) { return false; }
    }
}
