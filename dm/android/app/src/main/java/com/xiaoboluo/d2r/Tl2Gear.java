package com.xiaoboluo.d2r;

import android.app.AlertDialog;
import android.content.SharedPreferences;
import android.system.ErrnoException;
import android.system.Os;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.List;

// [TL2] Torchlight II's own gear sections and their order, on the shared gear (frontend-common GearSettings / GearPages, docs/FRONTEND_UNIFIED.md
// "Gear"). This file belongs to the TL2 port (dm/android/app/src/main/java/com/xiaoboluo/d2r/Tl2Gear.java; a reference copy sits in
// frontend-common/migration/tl2/), not to frontend-common's java/: the sync never touches it. DmActivity.gearSections returns sections(this, g).
// The dialog has the rows DmActivity.showD3Settings had before the move (parity table frontend-common/migration/tl2-parity.md: same prefs,
// environment, defaults; the generic rows are GearPages' with GearText's 13 languages, the TL2 rows keep their string resources d3_* / tl2_*
// in the menu language). TL2 rows here:
//   mode            Switch mode = render size (pref handheld, DM_OPMODE 1 / 0; labels show the size for the picked picture shape)   next launch
//   picture shape   GearPages.pictureShape: 16:9 bars / stretched / widescreen (pref aspect, default 2; DM_TL2_WIDE=<W> from the display)
//   loading stretch widescreen loading screens stretched (pref tl2_stretch_loading, default off; DM_TL2_WIDE_LOADSTRETCH)            next launch
//   mods            Tl2Mods: the .MOD list, its own dialog with its own Save (pref tl2_mods; converted at the next start)
//   layout          D3PadView / Tl2Touch layouts tl2 / tl2_side / gamepad (pad d3_layout, applies when picked) + skill icons (tl2_icons)
//   touch           Tl2Touch: diagonal correction, second-set modifier, the optional extras (pad tl2_iso / tl2_mod / tl2_tap / tl2_hold_walk /
//                   tl2_hero_y / tl2_aim)
//   start env       no row: DM_TL2_POINTER=1 (always on), the DOOM cvars unset, the non-Adreno text diagnostics
// A Diablo III build of this tree (DM_GAME=d3) gets D3's rows instead of the TL2 ones where they differ (touch / gamepad, D3 skill icons,
// hide sticks, walking to the finger, menu swipes).
final class Tl2Gear {
    private Tl2Gear() {}
    static final boolean TL2 = DmActivity.TL2;
    static final int[] FPS_LIMITS = {30, 60};   // above 60 is not offered: the game runs its frames at 60 like on the Switch (present interval 1)

    // The dialog, top to bottom (and the start environment in the same order)
    static List<GearSettings.Section> sections(DmActivity a, GearSettings g) {
        Mode mode = new Mode(a);
        List<GearSettings.Section> l = new ArrayList<>();
        l.add(startEnv(a));
        l.add(GearPages.menuLanguage());
        l.add(GearPages.gameLanguage(DmActivity.D3_LANG_CODES, DmActivity.D3_LANG_LABELS, true));
        l.add(note(R.string.d3_game_lang_note));
        l.add(mode);
        l.add(GearPages.pictureShape(new String[] {g.str(R.string.d3_aspect_bars), g.str(R.string.d3_aspect_stretch), g.str(R.string.d3_aspect_wide)},
            DmActivity.D3_ASPECT_DEFAULT, g.str(R.string.d3_aspect_wide_note), a::applyAspectLayout, (s, v) -> aspectEnv(a, v), mode::relabel));
        l.add(stretchLoading());
        if (TL2) l.add(mods(a));
        l.add(GearPages.sharpen());
        l.add(GearPages.fpsLimit(FPS_LIMITS, 60));
        l.add(note(R.string.d3_fps_note));
        if (a.drivers() != null) { l.add(GearPages.driver(a.drivers())); l.add(note(R.string.d3_driver_note)); }
        l.add(GearPages.rumble());
        l.add(GearPages.fpsCounter());
        l.add(new Layout());
        l.add(GearPages.padControls());
        l.add(GearPages.deadzone());
        l.add(TL2 ? new TouchRows() : new D3Touch());
        l.add(GearPages.gearButton());
        return l;
    }

    // ---- a row-less note under the section before it
    static GearSettings.Section note(int res) {
        return new GearSettings.Section() { public void build(GearSettings g, LinearLayout box) { g.note(box, g.str(res)); } };
    }

    // ---- no row: the start environment that has no option
    static GearSettings.Section startEnv(DmActivity a) {
        return new GearSettings.Section() {
            public void build(GearSettings g, LinearLayout box) {}
            public void env(GearSettings g) throws ErrnoException {
                Os.unsetenv("DM_DOOM_DRS"); Os.unsetenv("DM_DOOM_RES");   // the DOOM cvars (rs_enable / rs_forceFraction*) do not apply
                if (TL2) Os.setenv("DM_TL2_POINTER", "1", true);   // hle_tl2_ui.c menu taps: always on (owner), no gear option; DM_TL2_UIDUMP only from dm/env
                // GPUs we could not test (no Adreno kernel driver: Mali, PowerVR, Xclipse): the existing text diagnostics, so one report shows what
                // the renderer did - the NVN calls of the first 120 frames, a per-pass GPU profile every 600 frames for the first ~2 minutes. Text
                // only; dm/env still overrides each value. Not DM_NVN_TRACE (its wrapper crashed the host's slot calls at boot, Mali 0.2.1).
                if (!VkDrivers.kgsl()) {
                    Os.setenv("DM_NVN_FRAMES", "120", false);
                    Os.setenv("DM_GFX_PASS_PROF", "60 600 7200", false);
                    a.crumb("diagnostics on (not an Adreno GPU: " + VkDrivers.hardwareSummary() + "): NVN calls of 120 frames, pass profile for ~2 min");
                }
            }
        };
    }

    // ---- the Switch operation mode = the game's render size (hle_oe.c GetOperationMode / GetPerformanceMode): docked 1920x1080 (D3 1600x900),
    // handheld 1280x720; the labels show the size the next launch renders at for the picked picture shape (widescreen: the screen's aspect).
    // Next launch here (DM_OPMODE); the pause menu's mode row switches it live (DmActivity.setDocked, the same pref).
    static final class Mode implements GearSettings.Section {
        final DmActivity a; RadioGroup r; GearSettings g;
        Mode(DmActivity a) { this.a = a; }
        public void build(GearSettings g, LinearLayout box) {
            this.g = g; SharedPreferences p = g.gamePrefs();
            g.heading(box, g.when(g.str(R.string.d3_mode), false));
            r = g.radios(box, new String[] {"", ""}, p.getBoolean("handheld", false) ? 1 : 0, false);
            relabel(p.getInt("aspect", DmActivity.D3_ASPECT_DEFAULT));
            g.note(box, g.str(R.string.d3_mode_note));
        }
        void relabel(int aspect) {
            if (r == null || g == null) return;
            int[] sc = a.screenSize(), dk = DmActivity.d3RenderSize(aspect == 2, false, sc[0], sc[1]), hh = DmActivity.d3RenderSize(aspect == 2, true, sc[0], sc[1]);
            ((RadioButton) r.getChildAt(0)).setText(g.str(R.string.d3_mode_docked, dk[0] + "×" + dk[1]));
            ((RadioButton) r.getChildAt(1)).setText(g.str(R.string.d3_mode_handheld, hh[0] + "×" + hh[1]));
        }
        public void save(GearSettings g, SharedPreferences.Editor game, SharedPreferences.Editor pad) { game.putBoolean("handheld", GearSettings.picked(r) == 1); }
        public void env(GearSettings g) throws ErrnoException {
            a.startedHandheld = g.gamePrefs().getBoolean("handheld", false);
            Os.setenv("DM_OPMODE", a.startedHandheld ? "0" : "1", true);   // dm/env still wins (it is applied after this)
        }
    }
    // ---- picture shape 2 = widescreen (next launch): TL2 hle_tl2_wide.c W x 1080 Hor+, the width from the display, not the SurfaceView (the view
    // may still be laid out 16:9 before startedAspect is known, or portrait when started with the screen off); D3 hle_d3.c DM_D3_WIDE
    static void aspectEnv(DmActivity a, int v) throws ErrnoException {
        a.startedAspect = v;
        if (TL2) {
            int[] sc = a.screenSize(); long ww = sc[1] > 0 ? Math.round(1080.0 * sc[0] / sc[1] / 16) * 16 : 0;
            Os.setenv("DM_TL2_WIDE", v != 2 ? "0" : ww > 1920 ? Long.toString(ww) : "1", true);
        } else if (v == 2) Os.setenv("DM_D3_WIDE", "1", true); else Os.unsetenv("DM_D3_WIDE");
    }
    // ---- widescreen loading screens: TL2 pillarboxed 16:9 by default, stretched when asked (hle_tl2_wide.c); D3 the other way round (ls_present_crop)
    static GearSettings.Section stretchLoading() {
        return new GearSettings.Section() {
            CheckBox c;
            public void build(GearSettings g, LinearLayout box) { c = g.check(box, g.when(g.str(R.string.d3_stretch_loading), false), g.gamePrefs().getBoolean(DmActivity.STRETCH_LOADING_KEY, DmActivity.STRETCH_LOADING_DEFAULT)); }
            public void save(GearSettings g, SharedPreferences.Editor game, SharedPreferences.Editor pad) { game.putBoolean(DmActivity.STRETCH_LOADING_KEY, c.isChecked()); }
            public void env(GearSettings g) throws ErrnoException {
                String on = g.gamePrefs().getBoolean(DmActivity.STRETCH_LOADING_KEY, DmActivity.STRETCH_LOADING_DEFAULT) ? "1" : "0";
                Os.setenv(TL2 ? "DM_TL2_WIDE_LOADSTRETCH" : "DM_D3_WIDE_STRETCH_LOADING", on, true);
            }
        };
    }

    // ---- the mod manager (Tl2Mods): every .MOD in Download/Torchlight2Mobile/mods with title, author, version from its header, a tick to enable
    // it and up / down for the load order (lower = loaded later, wins). Its own dialog, saved with its own "Save"; the next start converts and
    // loads them (DmActivity.prepareTl2Mods).
    static GearSettings.Section mods(DmActivity a) {
        return new GearSettings.Section() {
            public void build(GearSettings g, LinearLayout box) {
                g.heading(box, g.str(R.string.tl2_mods_title));
                Button b = g.button(box, g.str(R.string.tl2_mods_button, Tl2Mods.enabledCount(g.gamePrefs())));
                b.setOnClickListener(v -> showMods(g, b));
                g.note(box, g.str(R.string.tl2_mods_restart));
            }
        };
    }
    static void showMods(GearSettings g, Button from) {
        SharedPreferences p = g.gamePrefs(); final List<Tl2Mods.Mod> mods = Tl2Mods.list(p);
        LinearLayout box = g.vbox(null); box.setPadding(48, 24, 48, 0);
        g.note(box, g.str(R.string.tl2_mods_folder, Tl2Mods.modsDir().getAbsolutePath()));
        final LinearLayout rows = g.vbox(box);
        final Runnable[] fill = new Runnable[1];
        fill[0] = () -> {
            rows.removeAllViews();
            if (mods.isEmpty()) { TextView t = g.text(rows, g.str(R.string.tl2_mods_none)); t.setPadding(0, 24, 0, 24); }
            for (int i = 0; i < mods.size(); i++) {
                final int at = i; final Tl2Mods.Mod m = mods.get(i);
                LinearLayout row = new LinearLayout(g.a); row.setOrientation(LinearLayout.HORIZONTAL); row.setPadding(0, 12, 0, 12);
                CheckBox c = new CheckBox(g.a);
                String sub = m.info == null ? g.str(R.string.tl2_mods_bad, m.error)
                    : g.str(R.string.tl2_mods_line, (m.info.author.isEmpty() ? "" : m.info.author + " · ") + m.file.getName(), m.info.modver);
                c.setText(Tl2Mods.describe(m) + "\n" + sub); c.setEnabled(m.info != null); c.setChecked(m.on && m.info != null);
                c.setOnCheckedChangeListener((b, on) -> m.on = on);
                row.addView(c, new LinearLayout.LayoutParams(0, -2, 1f));
                Button up = new Button(g.a), down = new Button(g.a);
                up.setText("▲"); down.setText("▼"); up.setEnabled(i > 0); down.setEnabled(i < mods.size() - 1);
                up.setOnClickListener(v -> { java.util.Collections.swap(mods, at, at - 1); fill[0].run(); });
                down.setOnClickListener(v -> { java.util.Collections.swap(mods, at, at + 1); fill[0].run(); });
                row.addView(up, new LinearLayout.LayoutParams(-2, -2)); row.addView(down, new LinearLayout.LayoutParams(-2, -2));
                rows.addView(row);
            }
        };
        fill[0].run();
        g.note(box, g.str(R.string.tl2_mods_restart));
        android.widget.ScrollView scroll = new android.widget.ScrollView(g.a); scroll.addView(box);
        new AlertDialog.Builder(g.a).setTitle(g.str(R.string.tl2_mods_title)).setView(scroll)
            .setPositiveButton(g.s("gear_save"), (d, w) -> { Tl2Mods.save(p, mods); from.setText(g.str(R.string.tl2_mods_button, Tl2Mods.enabledCount(p))); })
            .setNegativeButton(g.s("gear_cancel"), null).show();
    }

    // ---- the on-screen control layout (D3PadView "d3_layout"; applies when picked: the pad is visible behind the dialog). TL2: the D2R Mobile-style
    // layouts (Tl2Touch) "tl2" sticks + skill fan (default), "tl2_side" quick-access cells on the sides, or the gamepad; D3: touch / gamepad.
    // Below it the skill icons on the buttons (TL2: Tl2Touch "tl2_icons", hle_tl2_icons.c; D3: D3SkillIcons "d3_icons" + "d3_hide_sticks").
    static final class Layout implements GearSettings.Section {
        CheckBox icons, hide;
        public void build(GearSettings g, LinearLayout box) {
            SharedPreferences cp = g.padPrefs();
            g.heading(box, g.when(g.str(R.string.d3_layout), true));
            final String[] ids = TL2 ? new String[] {D3PadView.LAYOUT_TL2, D3PadView.LAYOUT_TL2_SIDE, D3PadView.LAYOUT_GAMEPAD} : new String[] {D3PadView.LAYOUT_TOUCH, D3PadView.LAYOUT_GAMEPAD};
            String now = cp.getString("d3_layout", D3PadView.LAYOUT_DEFAULT);
            int at = TL2 ? (D3PadView.LAYOUT_GAMEPAD.equals(now) ? 2 : D3PadView.LAYOUT_TL2_SIDE.equals(now) ? 1 : 0) : (D3PadView.LAYOUT_TOUCH.equals(now) ? 0 : 1);
            String[] labels = TL2 ? new String[] {g.str(R.string.tl2_layout_touch), g.str(R.string.tl2_layout_side), g.str(R.string.d3_layout_gamepad)}
                                  : new String[] {g.str(R.string.d3_layout_touch), g.str(R.string.d3_layout_gamepad)};
            RadioGroup r = g.radios(box, labels, at, false);
            r.setOnCheckedChangeListener((rg, id) -> { cp.edit().putString("d3_layout", ids[GearSettings.picked(rg)]).apply(); g.reloadPad(); });
            g.note(box, g.str(TL2 ? R.string.tl2_layout_note : R.string.d3_layout_note));
            if (TL2) icons = g.check(box, g.when(g.str(R.string.d3_skill_icons), true), cp.getBoolean("tl2_icons", true));
            else {
                icons = g.check(box, g.when(g.str(R.string.d3_skill_icons), true), cp.getBoolean("d3_icons", true));
                hide = g.check(box, g.when(g.str(R.string.d3_hide_sticks), true), cp.getBoolean("d3_hide_sticks", true));
                g.note(box, g.str(R.string.d3_skill_icons_note));
            }
        }
        public void save(GearSettings g, SharedPreferences.Editor game, SharedPreferences.Editor pad) {
            pad.putBoolean(TL2 ? "tl2_icons" : "d3_icons", icons.isChecked());
            if (hide != null) pad.putBoolean("d3_hide_sticks", hide.isChecked());
        }
    }

    // ---- [TL2] the D2R-style touch layout (Tl2Touch, pad prefs tl2_*): sliders live, the rest on Save; the extras are off by default
    static final class TouchRows implements GearSettings.Section {
        RadioGroup mod, tap, aim; CheckBox walk;
        public void build(GearSettings g, LinearLayout box) {
            SharedPreferences cp = g.padPrefs();
            g.heading(box, g.when(g.str(R.string.tl2_touch), true));
            g.slider(box, cp, g.str(R.string.tl2_iso), "tl2_iso", Tl2Touch.ISO_MIN, Tl2Touch.ISO_MAX, Tl2Touch.ISO_DEFAULT, g::reloadPad);
            g.note(box, g.str(R.string.tl2_iso_note));
            g.heading(box, g.str(R.string.tl2_mod));
            mod = g.radios(box, Tl2Touch.MOD_NAMES, cp.getInt("tl2_mod", 0), true);
            g.note(box, g.str(R.string.tl2_mod_note));
            g.heading(box, g.str(R.string.tl2_extras));
            g.heading(box, g.str(R.string.tl2_tap));
            tap = g.radios(box, new String[] {g.str(R.string.d3_off), g.str(R.string.tl2_tap_a), g.str(R.string.tl2_tap_face)}, cp.getInt("tl2_tap", Tl2Touch.TAP_OFF), false);
            walk = g.check(box, g.str(R.string.tl2_hold_walk), cp.getBoolean("tl2_hold_walk", false));
            g.slider(box, cp, g.str(R.string.d3_hero_y), "tl2_hero_y", Tl2Touch.HERO_Y_MIN, Tl2Touch.HERO_Y_MAX, Tl2Touch.HERO_Y_DEFAULT, g::reloadPad);
            g.heading(box, g.str(R.string.tl2_aim));
            aim = g.radios(box, new String[] {g.str(R.string.d3_off), g.str(R.string.tl2_aim_right), g.str(R.string.tl2_aim_left)}, cp.getInt("tl2_aim", Tl2Touch.AIM_OFF), false);
        }
        public void save(GearSettings g, SharedPreferences.Editor game, SharedPreferences.Editor pad) {
            pad.putInt("tl2_tap", GearSettings.picked(tap)).putBoolean("tl2_hold_walk", walk.isChecked()).putInt("tl2_aim", GearSettings.picked(aim))
               .putInt("tl2_mod", GearSettings.picked(mod));
        }
    }
    // ---- [D3] its touch layout: walking toward a held finger (D3PadView.HERO_* / DEPTH_*, live sliders) and what menu-mode swipes press
    static final class D3Touch implements GearSettings.Section {
        RadioGroup swipe;
        public void build(GearSettings g, LinearLayout box) {
            SharedPreferences cp = g.padPrefs();
            g.heading(box, g.when(g.str(R.string.d3_touch_walk), true));
            g.slider(box, cp, g.str(R.string.d3_hero_y), "d3_hero_y", D3PadView.HERO_Y_MIN, D3PadView.HERO_Y_MAX, D3PadView.HERO_Y_DEFAULT, g::reloadPad);
            g.slider(box, cp, g.str(R.string.d3_depth), "d3_depth", D3PadView.DEPTH_MIN, D3PadView.DEPTH_MAX, D3PadView.DEPTH_DEFAULT, g::reloadPad);
            g.note(box, g.str(R.string.d3_touch_walk_note));
            g.heading(box, g.when(g.str(R.string.d3_menu_swipe), true));
            swipe = g.radios(box, new String[] {g.str(R.string.d3_menu_swipe_stick), g.str(R.string.d3_menu_swipe_dpad)}, cp.getInt("d3_menu_swipe", D3PadView.MENU_SWIPE_STICK), true);
        }
        public void save(GearSettings g, SharedPreferences.Editor game, SharedPreferences.Editor pad) { pad.putInt("d3_menu_swipe", GearSettings.picked(swipe)); }
    }
}
