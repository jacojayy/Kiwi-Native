package hernandez.kiwinative.kiwi.ui;

import android.content.Context;
import android.content.res.Resources;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.InsetDrawable;
import android.widget.TextView;

import java.util.HashMap;
import java.util.Map;

/**
 * The Kiwi icons (a shell with one kernel; res drawable/pi_*.xml, written by kiwi/scripts/icons.py, where a new one is drawn). Their two
 * colours are theme attributes, ?attr/pnIconInk for the shell and ?attr/pnIconKernel for the kernel, set by a style per use (res values/kiwi_icons.xml):
 * the text colour with the kernel in the accent, the quieter text colour for a sidebar entry, and the dark brown of text on banana yellow for a selected one.
 * The drawables and styles are in the Kiwi resource folder only, so they are found by name; a build without them gets null and keeps its emoji.
 */
public final class Icons {
    private Icons() {}

    /** The shell and kernel colours: NORMAL (a row's tile), QUIET (a sidebar entry), ON_BANANA (the selected one, on yellow) */
    public enum Ink {
        NORMAL("KiwiIcon"), QUIET("KiwiIconQuiet"), ON_BANANA("KiwiIconOnKiwi");
        final String style;
        Ink(String style) { this.style = style; }
    }

    private static final Map<String, Integer> IDS = new HashMap<>();
    private static final Map<Ink, Resources.Theme> THEMES = new HashMap<>();
    private static int night = -1;

    private static int id(Context c, String name, String type) {
        String key = type + "/" + name;
        Integer v = IDS.get(key);
        if (v == null) { v = c.getResources().getIdentifier(name, type, c.getPackageName()); IDS.put(key, v); }
        return v;
    }

    /** The icon named (library, key, …: pi_<name>) in these colours, at its own 24 dp; null when this build has none */
    public static Drawable get(Context c, String name, Ink ink) {
        if (name == null) return null;
        int d = id(c, "pi_" + name.replace('-', '_'), "drawable");
        if (d == 0) return null;
        int mode = c.getResources().getConfiguration().uiMode & android.content.res.Configuration.UI_MODE_NIGHT_MASK;
        if (mode != night) { THEMES.clear(); night = mode; }   // dark mode changed: the colours resolve again
        Resources.Theme t = THEMES.get(ink);
        if (t == null) {
            int s = id(c, ink.style, "style");
            if (s == 0) return null;
            t = c.getResources().newTheme();
            t.setTo(c.getTheme());
            t.applyStyle(s, true);
            THEMES.put(ink, t);
        }
        try { return c.getResources().getDrawable(d, t); } catch (Resources.NotFoundException e) { return null; }
    }

    /** A row's tile (Ui.Setting: an emoji on the tint, 36 dp) with the icon instead of its emoji, 22 dp in its middle; false when there is no such icon */
    public static boolean tile(TextView tile, String name) {
        Context c = tile.getContext();
        Drawable d = get(c, name, Ink.NORMAL);
        if (d == null) return false;
        int size = tile.getLayoutParams() != null && tile.getLayoutParams().width > 0 ? tile.getLayoutParams().width : PUi.dp(c, 36);
        int inset = Math.max(0, (size - PUi.dp(c, size >= PUi.dp(c, 32) ? 22 : 18)) / 2);
        tile.setText("");
        tile.setForeground(new InsetDrawable(d, inset));
        return true;
    }

    /** The icon that stands for an emoji of a row's tile (Ui.Setting), so no row shows the system's emoji font; the names are pi_<name> drawables */
    private static final Map<String, String> EMOJI = new HashMap<>();
    static {
        String[] m = {"\uD83C\uDFAE", "game", "\uD83E\uDDE9", "mods", "\uD83D\uDD0D", "search", "\uD83D\uDDBC", "picture", "\uD83D\uDDA5", "display", "\uD83D\uDCC1", "folder",
            "\u2728", "quality", "\u26A1", "gauge", "\uD83D\uDCFA", "tv", "\u26A0", "warning", "\uD83E\uDDEA", "terminal", "\uD83D\uDDD1", "trash", "\uD83D\uDD79", "controller",
            "\uD83D\uDD04", "update", "\uD83D\uDCF3", "rumble", "\uD83D\uDCE6", "pack_out", "\u2B06", "upscale", "\uD83E\uDDF9", "undo", "\uD83E\uDDE0", "memory",
            "\uD83D\uDEE0", "build", "\uD83D\uDE80", "fast_forward", "\uD83D\uDDDC", "pack_in", "\uD83D\uDD52", "clock", "\uD83D\uDD2D", "aim", "\uD83D\uDD28", "build",
            "\uD83D\uDD24", "language", "\uD83D\uDD11", "key", "\uD83D\uDCFC", "log", "\uD83D\uDCF1", "screen_landscape", "\uD83D\uDCE8", "share", "\uD83D\uDCE5", "download",
            "\uD83D\uDCBE", "saves", "\uD83C\uDFAF", "aim", "\uD83C\uDFAC", "scene", "\uD83C\uDF9A", "contrast", "\uD83C\uDF3F", "leaf", "\uD83E\uDE9F", "window",
            "\uD83E\uDDFE", "list", "\uD83E\uDDF5", "cores", "\uD83E\uDDF1", "layers", "\uD83E\uDDF0", "settings", "\uD83E\uDD47", "quality", "\uD83D\uDE42", "user",
            "\uD83D\uDDC2", "library", "\uD83D\uDD33", "passes", "\u2699", "settings"};
        for (int i = 0; i < m.length; i += 2) EMOJI.put(m[i], m[i + 1]);
    }

    /** A row's tile with the icon of its emoji instead of it (the variation selector is ignored); false for an emoji that has none, which stays as it is */
    public static boolean tileForEmoji(TextView tile, String emoji) {
        if (emoji == null) return false;
        String name = EMOJI.get(emoji.replace("\uFE0F", ""));
        return name != null && tile(tile, name);
    }
}
