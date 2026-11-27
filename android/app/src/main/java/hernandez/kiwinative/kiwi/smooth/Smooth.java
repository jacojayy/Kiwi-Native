package hernandez.kiwinative.kiwi.smooth;

import android.content.Context;

import hernandez.kiwinative.kiwi.KiwiPrefs;

/**
 * Smooth Experience's switches (SmoothItems; KiwiPrefs "smooth", "smooth_&lt;key&gt;"), for the places it changes: each asks {@link #hides}, which is
 * false while the switch is off, so with it off nothing changes. The game's process reads the switches at its start.
 */
public final class Smooth {
    private Smooth() {}

    public static boolean on(Context c) { return c != null && KiwiPrefs.smooth(c); }

    /** Whether Smooth Experience is on and takes this item away (SmoothItems' keys) */
    public static boolean hides(Context c, String key) { return on(c) && KiwiPrefs.smoothItem(c, key); }

    /** How many items are picked (the settings row's line) */
    public static int picked(Context c) {
        int n = 0;
        for (SmoothItems.Item it : SmoothItems.ALL) if (KiwiPrefs.smoothItem(c, it.key)) n++;
        return n;
    }
}
