package hernandez.kiwinative;

import android.content.Context;
import android.graphics.Typeface;

/** KIWI: the app's font, Nunito (res/font/nunito.xml): Fonts.face(weight) for code that sets a typeface itself; the theme's fontFamily covers the rest */
public final class Fonts {
    private Fonts() {}

    private static Typeface family;

    /** NxApp.onCreate, in every process: finds the font once; without it (or when it cannot be read) face() gives the system's */
    public static void init(Context c) {
        if (family != null) return;
        try { family = c.getResources().getFont(R.font.nunito); } catch (RuntimeException e) { family = null; }
    }

    /** The font at a weight (100 to 900); the system sans-serif at that weight before init */
    public static Typeface face(int weight) {
        return Typeface.create(family != null ? family : Typeface.SANS_SERIF, weight, false);
    }
}
