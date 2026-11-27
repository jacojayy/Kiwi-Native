package hernandez.kiwinative.kiwi.ingame;

import android.content.Context;
import android.content.res.Resources;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.view.animation.Interpolator;
import android.view.animation.PathInterpolator;

import java.util.HashMap;
import java.util.Map;

/**
 * The in-game menus' look: upstream's grey panel, the banana-yellow focus, the kiwi browns,
 * and the solid icons (res drawable/pis_*.xml from design/kiwi-native/icons, colours per state from res values/kiwi_ingame.xml). Also their motion.
 */
public final class Look {
    private Look() {}

    // The panel (SidePanel, ModsTab): the sheet, a row, a chip, text, quiet text, the yellow and the text on it
    public static final int SHEET = 0xF40E160F, ROW = 0xFF16221A, CHIP = 0xFF20331F, TEXT = 0xFFEEF8DC, QUIET = 0xFFA3BD86, YELLOW = 0xFF9AD64A, ON_YELLOW = 0xFF0F2008;
    // The kiwi: the peel (the drawer's edge), the flesh (the selected block, a switch that is on), the lime of a ray, the core's cream (a thumb)
    public static final int PEEL = 0xFF8A5A2E, FLESH = 0xFF6DB02A, LIME = 0xFFD4F06A, CORE = 0xFFF4EDCB;
    // The rail: its ground, its edge, the selected block (kiwi brown); a section head (kiwi tan); a line that went wrong; the scrim over the game
    public static final int RAIL = 0xFF0A110B, RAIL_EDGE = 0xFF263A2A, KIWI = 0xFF6DB02A, TAN = 0xFFB9D98A, BAD = 0xFFFF9670, SCRIM = 0x66000000, HAIRLINE = 0xFF20331F;

    /** The solid icons' colours: REST (the rail, not selected), SELECTED (the selected tab), MENU (the game menu's rows, the back button) */
    public enum Ink {
        REST("KiwiSolidRest"), SELECTED("KiwiSolidSelected"), MENU("KiwiSolidMenu");
        final String style;
        Ink(String style) { this.style = style; }
    }

    private static final Map<String, Integer> IDS = new HashMap<>();
    private static final Map<Ink, Resources.Theme> THEMES = new HashMap<>();

    private static int id(Context c, String name, String type) {
        String key = type + "/" + name;
        Integer v = IDS.get(key);
        if (v == null) { v = c.getResources().getIdentifier(name, type, c.getPackageName()); IDS.put(key, v); }
        return v;
    }

    /** The solid icon named (bolt, cheats, ...: pis_<name>) in these colours, at its own 24 dp; null when this build has none */
    public static Drawable icon(Context c, String name, Ink ink) {
        int d = id(c, "pis_" + name.replace('-', '_'), "drawable");
        if (d == 0) return null;
        Resources.Theme t = THEMES.get(ink);
        if (t == null) {
            int s = id(c, ink.style, "style");
            if (s == 0) return null;
            t = c.getResources().newTheme();
            t.setTo(c.getTheme());
            t.applyStyle(s, true);
            THEMES.put(ink, t);
        }
        try { return c.getResources().getDrawable(d, t).mutate(); } catch (Resources.NotFoundException e) { return null; }
    }

    /**
     * The drawer's shape (Rail): the sheet's corners on the game's side rounded, 22 dp, its children clipped to them; a bottom screen's menu has its top corners
     * rounded instead. The sheet keeps its colour; the outline reaches past the far edge so only the two corners show
     */
    public static void sheetShape(android.view.View sheet, boolean bottom) {
        Context c = sheet.getContext();
        final float r = dp(c, 22);
        GradientDrawable d = new GradientDrawable();
        d.setColor(SHEET);
        d.setCornerRadii(bottom ? new float[] {r, r, r, r, 0, 0, 0, 0} : new float[] {r, r, 0, 0, 0, 0, r, r});
        sheet.setBackground(d);
        sheet.setClipToOutline(true);
        sheet.setOutlineProvider(new android.view.ViewOutlineProvider() {
            @Override public void getOutline(android.view.View v, android.graphics.Outline o) {
                int w = v.getWidth(), h = v.getHeight(), x = Math.round(r);
                if (bottom) o.setRoundRect(0, 0, w, h + x, r); else o.setRoundRect(0, 0, w + x, h, r);
            }
        });
    }

    public static int dp(Context c, float v) { return Math.round(v * c.getResources().getDisplayMetrics().density); }

    public static GradientDrawable round(int fill, float radiusPx) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(fill);
        d.setCornerRadius(radiusPx);
        return d;
    }

    /** The controller's focus: the panel's own frame (SidePanel.focusMoved), a yellow ring with a light yellow tint, here with the row's corners */
    public static GradientDrawable focus(Context c, float radiusDp) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(0x269AD64A);
        d.setStroke(Math.round(2.5f * c.getResources().getDisplayMetrics().density), YELLOW);
        d.setCornerRadius(radiusDp * c.getResources().getDisplayMetrics().density);
        return d;
    }

    // ---- Motion: calm, no overshoot. In with a long deceleration, out a little quicker, accelerating ----
    public static final int OPEN_MS = 240, CLOSE_MS = 180, TAB_OUT_MS = 60, TAB_IN_MS = 100, BLOCK_MS = 160;
    /** The bottom screen's menu: up from the bottom edge, back down */
    public static final int UP_MS = 280, DOWN_MS = 220;
    public static final Interpolator OUT = new PathInterpolator(0.2f, 0f, 0f, 1f), IN = new PathInterpolator(0.3f, 0f, 1f, 1f);

    /**
     * A global focus listener that stays with v from window to window. One added while v has no window is kept by the first window it joins only: the bottom
     * screen's menu goes into a new window (BottomScreen) at each opening, and from the second opening its focus frame, the rail's tab switching and the
     * focus ticks were gone. Here it is taken off the window v leaves and put on each window v joins
     */
    public static void focusListener(android.view.View v, android.view.ViewTreeObserver.OnGlobalFocusChangeListener l) {
        v.getViewTreeObserver().addOnGlobalFocusChangeListener(l);
        v.addOnAttachStateChangeListener(new android.view.View.OnAttachStateChangeListener() {
            @Override public void onViewAttachedToWindow(android.view.View x) {
                android.view.ViewTreeObserver o = x.getViewTreeObserver();
                o.removeOnGlobalFocusChangeListener(l);   // the first window has it already (merged from the detached view's own)
                o.addOnGlobalFocusChangeListener(l);
            }

            @Override public void onViewDetachedFromWindow(android.view.View x) { x.getViewTreeObserver().removeOnGlobalFocusChangeListener(l); }
        });
    }
}
