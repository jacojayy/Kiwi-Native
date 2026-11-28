package hernandez.kiwinative.kiwi.ui;

import android.content.Context;
import android.graphics.Color;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.LayerDrawable;
import android.view.View;

import hernandez.kiwinative.R;

/**
 * Liquid glass, the look of every surface of the launcher: a frosted fill that lets the aurora behind it (res/drawable/kiwi_aurora.xml, the window's
 * background) glow through, a sheen falling from the top left, and a bright rim. A dialog is real glass on Android 12 and later (the theme blurs what is
 * behind it, Theme.Kiwi.Dialog). Colours are res/values(-night)/colors.xml glass_*, so both themes have it.
 */
public final class Glass {
    private Glass() {}

    private static int dp(Context c, float v) { return Math.max(1, Math.round(v * c.getResources().getDisplayMetrics().density)); }

    /** A frosted panel: the fill, the sheen, the rim, corners of radiusDp */
    public static Drawable panel(Context c, float radiusDp) { return panel(c, radiusDp, c.getColor(R.color.glass_fill)); }

    public static Drawable panel(Context c, float radiusDp, int fill) {
        float r = dp(c, radiusDp);
        GradientDrawable base = new GradientDrawable();
        base.setColor(fill);
        base.setCornerRadius(r);
        GradientDrawable sheen = new GradientDrawable(GradientDrawable.Orientation.TL_BR, new int[] {c.getColor(R.color.glass_sheen_top), c.getColor(R.color.glass_sheen_bottom)});
        sheen.setCornerRadius(r);
        GradientDrawable rim = new GradientDrawable();
        rim.setColor(Color.TRANSPARENT);
        rim.setStroke(dp(c, 1), c.getColor(R.color.glass_edge));
        rim.setCornerRadius(r);
        return new LayerDrawable(new Drawable[] {base, sheen, rim});
    }

    /** A panel with a soft lift: a card over the aurora */
    public static void card(View v, float radiusDp) {
        v.setBackground(panel(v.getContext(), radiusDp));
        v.setElevation(dp(v.getContext(), 2));
        v.setOutlineAmbientShadowColor(v.getContext().getColor(R.color.glass_shade));
        v.setOutlineSpotShadowColor(v.getContext().getColor(R.color.glass_shade));
    }

    /** A glossy button of one colour: lighter at the top, a bright rim, as a drop of the colour on glass */
    public static GradientDrawable jelly(Context c, int fill, float radiusDp) {
        GradientDrawable g = new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, new int[] {mix(fill, Color.WHITE, 0.28f), fill, mix(fill, Color.BLACK, 0.06f)});
        g.setCornerRadius(dp(c, radiusDp));
        g.setStroke(dp(c, 1), 0x66FFFFFF);
        return g;
    }

    /** A shape under a lime ripple, the shape's corners as the ripple's mask */
    public static Drawable rippled(Context c, Drawable shape, float radiusDp) {
        GradientDrawable mask = new GradientDrawable();
        mask.setColor(Color.BLACK);
        mask.setCornerRadius(dp(c, radiusDp));
        return new android.graphics.drawable.RippleDrawable(android.content.res.ColorStateList.valueOf(0x336DB02A), shape, mask);
    }

    /** A tonal glass button or chip */
    public static Drawable chip(Context c, float radiusDp) { return panel(c, radiusDp, c.getColor(R.color.glass_fill_strong)); }

    private static int mix(int a, int b, float t) {
        return Color.argb(Color.alpha(a), Math.round(Color.red(a) + (Color.red(b) - Color.red(a)) * t), Math.round(Color.green(a) + (Color.green(b) - Color.green(a)) * t),
                Math.round(Color.blue(a) + (Color.blue(b) - Color.blue(a)) * t));
    }
}
