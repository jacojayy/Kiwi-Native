package hernandez.kiwinative.kiwi.ui;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.LayerDrawable;
import android.os.SystemClock;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewTreeObserver;
import android.view.Window;
import android.view.animation.PathInterpolator;
import android.widget.EditText;
import android.widget.ListView;

import java.util.Collections;
import java.util.Set;
import java.util.WeakHashMap;

import hernandez.kiwinative.R;

/**
 * The controller's drag ring: ONE outline per window that glides from the focused item to the next, instead of every item drawing its own frame. A thin
 * (2 dp) accent outline with a faint accent fill, drawn over the item, taking the item's corners. The outline moves on a damped spring (damping ratio 0.8,
 * stiffness 900), so it stretches toward the next item and settles on it; a new target while it still travels replaces the old one and keeps the speed.
 * It appears in place when it was hidden, and it follows an item that only moves because the page scrolls without gliding. Touch hides it (after a short
 * grace, since focus often passes through "nothing" for a moment): it shows again on the first key.
 * <p>
 * {@link #watch} puts it on a window (the activity, each dialog: a dialog is a window of its own). It is a view above the window's content that takes no
 * touch and no focus. Nothing is drawn or measured while the window is at rest: the animation asks for its own frames, and layout and scroll only re-aim it.
 */
public final class DragRing extends View {
    private static final float STROKE_DP = 2f, FILL_ALPHA = 0.10f, DEFAULT_CORNER_DP = 12f;
    private static final float STIFFNESS = 900f, DAMPING = 0.8f;   // mass 1: the damping coefficient is 2 x ratio x sqrt(stiffness)
    private static final float SETTLE_PX = 0.5f, SETTLE_SPEED = 31f;   // a position within half a pixel and a speed under 31 px/s is at rest
    private static final long CORNER_MS = 160, FADE_IN_MS = 120, FADE_OUT_MS = 160, HIDE_GRACE_MS = 140;
    private static final float STEP = 0.004f;   // the spring's integration step, in seconds

    private static final Set<Window> WATCHED = Collections.newSetFromMap(new WeakHashMap<>());   // a set: the ring holds the window's context, so a map to it would keep the window alive

    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final PathInterpolator ease = new PathInterpolator(0.4f, 0f, 0.2f, 1f);
    private final RectF box = new RectF();
    private final Rect seen = new Rect();
    private final int[] at = new int[2], me = new int[2];
    private final float[] cur = new float[4], vel = new float[4], target = new float[4];   // left, top, right, bottom
    private final Runnable fadeOut = this::startFadeOut;

    private View hot;   // the focused item the ring is on, null while none
    private boolean gliding;
    private long last;   // the time of the last animation step, 0 while at rest
    private float corner, cornerFrom, cornerTo;
    private long cornerStart;
    private float alpha, alphaFrom, alphaTo;
    private long alphaStart, alphaMs;

    private DragRing(Context c) {
        super(c);
        setClickable(false);
        setFocusable(false);
        setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
        line.setStyle(Paint.Style.STROKE);
        line.setStrokeWidth(dp(STROKE_DP));
    }

    private float dp(float v) { return v * getResources().getDisplayMetrics().density; }

    /** Puts the ring on this window (once): it follows the focus from now on */
    public static void watch(Window w) {
        if (!WATCHED.add(w)) return;
        DragRing ring = new DragRing(w.getContext());
        ViewTreeObserver o = w.getDecorView().getViewTreeObserver();
        o.addOnGlobalFocusChangeListener((was, now) -> ring.focus(w, now));
        o.addOnTouchModeChangeListener(touch -> { if (touch) ring.lose(); });
        o.addOnScrollChangedListener(ring::reaim);
        o.addOnGlobalLayoutListener(ring::reaim);
    }

    // ---- Following the focus ----

    private void focus(Window w, View now) {
        if (getParent() == null) w.addContentView(this, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));   // again after the page set its content anew
        removeCallbacks(fadeOut);
        if (now == null || now == this || now.isInTouchMode() || now instanceof EditText || now instanceof ListView) { lose(); return; }   // a text box and a list keep their own marks
        hot = now;
        aim(true);
    }

    /** Nothing holds the ring: it waits a moment for the next claim, then fades */
    private void lose() {
        hot = null;
        removeCallbacks(fadeOut);
        postDelayed(fadeOut, HIDE_GRACE_MS);
    }

    private void reaim() { if (hot != null && (alpha > 0.01f || gliding)) aim(false); }

    /**
     * Aims the ring at the hot item. It appears in place when hidden; it glides when the item changed or a glide still runs (the new target replaces it, keeping
     * the speed); the same item that only moved because the page scrolled is followed without a glide.
     */
    private void aim(boolean changed) {
        View v = hot;
        if (v == null) return;
        if (!isLaidOut()) { post(() -> aim(changed)); return; }
        if (!v.isAttachedToWindow() || !v.isShown() || !v.getGlobalVisibleRect(seen)) { removeCallbacks(fadeOut); postDelayed(fadeOut, HIDE_GRACE_MS); return; }   // scrolled out
        v.getLocationInWindow(at);
        getLocationInWindow(me);
        float l = at[0] - me[0], t = at[1] - me[1], r = l + v.getWidth(), b = t + v.getHeight();
        float c = Math.min(cornerOf(v), Math.min(v.getWidth(), v.getHeight()) / 2f);
        boolean wasHidden = alpha < 0.02f;
        boolean same = !changed && l == target[0] && t == target[1] && r == target[2] && b == target[3];
        if (same && !wasHidden) return;
        removeCallbacks(fadeOut);
        target[0] = l; target[1] = t; target[2] = r; target[3] = b;
        long now = SystemClock.uptimeMillis();
        if (wasHidden || !(changed || gliding)) {   // appears in place, or the page scrolled under it
            System.arraycopy(target, 0, cur, 0, 4);
            java.util.Arrays.fill(vel, 0f);
            gliding = false;
            corner = cornerTo = cornerFrom = c;
        } else {
            gliding = true;
            cornerFrom = corner; cornerTo = c; cornerStart = now;
        }
        if (alpha < 1f && alphaTo != 1f) { alphaFrom = alpha; alphaTo = 1f; alphaStart = now; alphaMs = FADE_IN_MS; }
        invalidate();
    }

    private void startFadeOut() {
        alphaFrom = alpha; alphaTo = 0f; alphaStart = SystemClock.uptimeMillis(); alphaMs = FADE_OUT_MS;
        invalidate();
    }

    /** The corner radius of the item's own shape, in px: the glass surface's, or a rounded shape's (also under a ripple), else a default */
    private float cornerOf(View v) {
        float c = cornerOf(v.getBackground(), 0);
        return c > 0 ? c : dp(DEFAULT_CORNER_DP);
    }

    private static float cornerOf(Drawable d, int depth) {
        if (d == null || depth > 3) return -1f;
        float g = Glass.radiusOf(d);
        if (g > 0) return g;
        if (d instanceof GradientDrawable) return ((GradientDrawable) d).getCornerRadius();
        if (d instanceof LayerDrawable) {
            LayerDrawable l = (LayerDrawable) d;
            for (int i = 0; i < l.getNumberOfLayers(); i++) {
                float c = cornerOf(l.getDrawable(i), depth + 1);
                if (c > 0) return c;
            }
        }
        return -1f;
    }

    // ---- Drawing ----

    @Override protected void onDraw(Canvas canvas) {
        long now = SystemClock.uptimeMillis();
        float dt = last == 0 ? 0.016f : Math.min(0.032f, (now - last) / 1000f);
        last = now;
        boolean more = false;
        if (gliding) {
            int n = Math.max(1, Math.round(dt / STEP));
            float h = dt / n, k = 2f * DAMPING * (float) Math.sqrt(STIFFNESS);
            for (int s = 0; s < n; s++)
                for (int i = 0; i < 4; i++) {
                    vel[i] += (-STIFFNESS * (cur[i] - target[i]) - k * vel[i]) * h;
                    cur[i] += vel[i] * h;
                }
            boolean rest = true;
            for (int i = 0; i < 4; i++) if (Math.abs(cur[i] - target[i]) > SETTLE_PX || Math.abs(vel[i]) > SETTLE_SPEED) rest = false;
            if (rest) { System.arraycopy(target, 0, cur, 0, 4); java.util.Arrays.fill(vel, 0f); gliding = false; } else more = true;
        }
        float ct = Math.min(1f, (now - cornerStart) / (float) CORNER_MS);
        if (cornerFrom != cornerTo) { corner = cornerFrom + (cornerTo - cornerFrom) * ease.getInterpolation(ct); if (ct < 1f) more = true; else cornerFrom = cornerTo; }
        if (alphaMs > 0) {
            float ft = Math.min(1f, (now - alphaStart) / (float) alphaMs);
            alpha = alphaFrom + (alphaTo - alphaFrom) * ease.getInterpolation(ft);
            if (ft < 1f) more = true; else { alpha = alphaTo; alphaMs = 0; }
        }
        paintRing(canvas);
        if (more) postInvalidateOnAnimation(); else last = 0;
    }

    private void paintRing(Canvas canvas) {
        if (alpha < 0.01f) return;
        float stroke = line.getStrokeWidth();
        if (cur[2] - cur[0] <= stroke || cur[3] - cur[1] <= stroke) return;
        int accent = getContext().getColor(R.color.bn_accent), rgb = accent & 0x00FFFFFF;
        fill.setColor(rgb | (Math.round(255 * FILL_ALPHA * alpha) << 24));
        line.setColor(rgb | (Math.round(255 * alpha) << 24));
        float half = stroke / 2f, rad = Math.max(0f, corner - half);
        box.set(cur[0] + half, cur[1] + half, cur[2] - half, cur[3] - half);   // the outline's outer edge is on the item's edge: it covers the item's own border
        canvas.drawRoundRect(box, rad, rad, fill);
        canvas.drawRoundRect(box, rad, rad, line);
    }
}
