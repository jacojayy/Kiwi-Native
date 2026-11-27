package hernandez.kiwinative.kiwi.ui;

import android.animation.ValueAnimator;
import android.graphics.Canvas;
import android.view.View;

/**
 * A tapped kiwi of the drifting background cracks open instead of bursting (upstream's KiwiBackgroundView draws a burst of flecks; one KIWI line
 * at the start of its drawKiwi hands the first CRACK_MS of a pop to here). The app's own kiwi (KiwiArt) at the tile's size and tilt: the seam
 * runs down it, the halves swing apart about the foot and show the juice, and it fades away, calmly. After that upstream's view goes on as before:
 * the kiwi stays away and grows back. Only in a Kiwi build, whose background is the kiwi, and only while the system's animations are on.
 */
public final class Crack {
    private Crack() {}

    static final long CRACK_MS = 900;
    private static KiwiArt art;

    /**
     * Draws the crack of a kiwi popped age ms ago, the canvas at the tile's centre; tile: the tile's size in px, alpha: the background's, tilt: the
     * tile's turn. False when it is not this one's to draw (upstream then draws as it always did)
     */
    public static boolean draw(View v, Canvas c, float tile, long age, int alpha, float tilt) {
        if (age < 0 || age >= CRACK_MS || !ValueAnimator.areAnimatorsEnabled() || hernandez.kiwinative.BuildConfig.NX_KIWI_ID.isEmpty()) return false;
        if (art == null) art = new KiwiArt();
        float t = age / (float) CRACK_MS;
        float crackT = Math.min(1f, t / 0.3f), swing = Math.max(0f, Math.min(1f, (t - 0.25f) / 0.45f));
        float deg = 28f * swing * swing * (3 - 2 * swing);
        float fade = t < 0.55f ? 1f : 1f - (t - 0.55f) / 0.45f;
        c.save();
        c.rotate(tilt);
        float s = tile / 108f;
        c.scale(s, s);
        c.translate(-54f, -54f);
        art.draw(c, crackT, swing, deg, Math.round(alpha * fade));
        c.restore();
        return true;
    }
}
