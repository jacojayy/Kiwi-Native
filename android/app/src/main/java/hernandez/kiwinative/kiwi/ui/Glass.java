package hernandez.kiwinative.kiwi.ui;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.ColorFilter;
import android.graphics.LinearGradient;
import android.graphics.Outline;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.view.View;

import hernandez.kiwinative.R;

/**
 * Liquid glass, the look of every surface of the launcher: a frosted fill that lets the aurora behind it (res/drawable/kiwi_aurora.xml, the window's
 * background) glow through, a sheen falling from the top, and a hairline of light along the edge. A dialog is real glass on Android 12 and later (the theme blurs what is
 * behind it, Theme.Kiwi.Dialog). Colours are res/values(-night)/colors.xml glass_*, so both themes have it.
 */
public final class Glass {
    private Glass() {}

    private static int dp(Context c, float v) { return Math.max(1, Math.round(v * c.getResources().getDisplayMetrics().density)); }

    /** A frosted panel: the fill, the sheen falling from the top, a hairline of light along the edge, corners of radiusDp */
    public static Drawable panel(Context c, float radiusDp) { return panel(c, radiusDp, c.getColor(R.color.glass_fill)); }

    public static Drawable panel(Context c, float radiusDp, int fill) { return new Surface(c, dp(c, radiusDp), fill); }

    /** The corner radius in px of a drawable made here, or -1 (the controller's gliding ring takes its corners from it) */
    public static float radiusOf(Drawable d) { return d instanceof Surface ? ((Surface) d).radius : -1f; }

    /**
     * The glass of a card: the fill, then a sheen that is brightest at the top and gone by 60% of the height, then a 1 dp edge light along the outline that is
     * brightest at the top and fainter at the bottom. Drawn inside the bounds (a parent that clips its children cannot cut it); the lift under the card is the
     * view's own elevation shadow (card).
     */
    private static final class Surface extends Drawable {
        final float radius;
        private final Paint fillPaint = new Paint(Paint.ANTI_ALIAS_FLAG), sheenPaint = new Paint(Paint.ANTI_ALIAS_FLAG), edgePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final int sheenTop, sheenBottom, edgeTop, edgeBottom;
        private final float edge;
        private final RectF box = new RectF();
        private final Path outline = new Path();

        Surface(Context c, float radius, int fill) {
            this.radius = radius;
            fillPaint.setColor(fill);
            sheenTop = c.getColor(R.color.glass_sheen_top);
            sheenBottom = c.getColor(R.color.glass_sheen_bottom);
            edgeTop = c.getColor(R.color.glass_edge_top);
            edgeBottom = c.getColor(R.color.glass_edge_bottom);
            edge = dp(c, 1);
            edgePaint.setStyle(Paint.Style.STROKE);
            edgePaint.setStrokeWidth(edge * 2);   // stroked on the outline and clipped to it: the inner 1 dp shows
        }

        @Override protected void onBoundsChange(Rect b) {
            box.set(b);
            outline.rewind();
            outline.addRoundRect(box, radius, radius, Path.Direction.CW);
            float h = Math.max(1f, box.height());
            sheenPaint.setShader(new LinearGradient(0, box.top, 0, box.top + h * 0.6f, sheenTop, sheenBottom, Shader.TileMode.CLAMP));
            edgePaint.setShader(new LinearGradient(0, box.top, 0, box.bottom, edgeTop, edgeBottom, Shader.TileMode.CLAMP));
        }

        @Override public void draw(Canvas canvas) {
            canvas.drawPath(outline, fillPaint);
            int save = canvas.save();
            canvas.clipPath(outline);
            canvas.drawRect(box, sheenPaint);
            canvas.drawPath(outline, edgePaint);
            canvas.restoreToCount(save);
        }

        @Override public void setAlpha(int alpha) { fillPaint.setAlpha(alpha); sheenPaint.setAlpha(alpha); edgePaint.setAlpha(alpha); invalidateSelf(); }
        @Override public void setColorFilter(ColorFilter cf) { fillPaint.setColorFilter(cf); sheenPaint.setColorFilter(cf); edgePaint.setColorFilter(cf); invalidateSelf(); }
        @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
        @Override public void getOutline(Outline o) { o.setRoundRect(getBounds(), radius); }
    }

    /** A panel with a lift: a card over the aurora, its shadow soft and wide, with a tighter one at its foot (the elevation's spot and ambient shadows) */
    public static void card(View v, float radiusDp) {
        v.setBackground(panel(v.getContext(), radiusDp));
        v.setElevation(dp(v.getContext(), 6));
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
