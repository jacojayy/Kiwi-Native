package hernandez.kiwinative.kiwi.ui;

import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;

/**
 * The app's kiwi (the slice of res/drawable-nodpi/kiwi_sprite.png, drawn in code: a peel ring, green flesh, a pale core and a ring of seeds, 108 units across)
 * split along its seam: drawn by PrepareCover while shaders are prepared, by CrackedKiwi on About, and by Crack when a kiwi of the background is tapped.
 * A crack runs down the seam, the halves swing apart about the foot (54, 90) and show the juice between them. Coordinates are the drawable's 108 units;
 * the caller scales.
 */
final class KiwiArt {
    static final float[] ZIG = {54, 18, 57, 27, 51, 36, 57, 45, 52, 54, 57, 63, 51, 72, 56, 81, 54, 92};   // the seam, top to foot
    private static final float CX = 54, CY = 54, R = 36;   // the slice
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path shell = new Path(), left = new Path(), right = new Path(), crack = new Path();

    KiwiArt() {
        shell.addCircle(CX, CY, R, Path.Direction.CW);
        left.moveTo(0, 0); right.moveTo(108, 0);
        for (int i = 0; i < ZIG.length; i += 2) { left.lineTo(ZIG[i], ZIG[i + 1]); right.lineTo(ZIG[i], ZIG[i + 1]); }
        left.lineTo(0, 108); left.close(); right.lineTo(108, 108); right.close();
        line.setStyle(Paint.Style.STROKE); line.setStrokeJoin(Paint.Join.ROUND); line.setStrokeCap(Paint.Cap.ROUND);
    }

    /**
     * crackT: how far the seam has run (0 to 1); swing: how far the halves have opened (0 to 1, the kernels show and the seam's line fades with it); deg:
     * each half's angle about the foot; alpha: the whole kiwi's (255: opaque, drawn without a layer)
     */
    void draw(Canvas c, float crackT, float swing, float deg, int alpha) {
        int layer = alpha < 255 ? c.saveLayerAlpha(0, 0, 108, 108, Math.max(0, alpha)) : -1;
        if (swing > 0) {   // the juice, under the halves
            fill.setColor(0xFF9CD13A); fill.setAlpha((int) (255 * Math.min(1f, swing * 2)));
            c.drawOval(44, 22, 64, 56, fill); c.drawOval(43, 52, 65, 86, fill);
            fill.setColor(0xFFF4EDCB); fill.setAlpha((int) (255 * Math.min(1f, swing * 2)));
            c.drawOval(49, 30, 59, 46, fill); c.drawOval(48, 60, 60, 78, fill);
            fill.setAlpha(255);
        }
        half(c, left, -deg);
        half(c, right, deg);
        if (crackT > 0 && swing < 1) {   // the seam, as far as it has run
            crack.rewind();
            float end = 18 + (92 - 18) * crackT;
            crack.moveTo(ZIG[0], ZIG[1]);
            for (int i = 2; i < ZIG.length; i += 2) {
                if (ZIG[i + 1] <= end) { crack.lineTo(ZIG[i], ZIG[i + 1]); continue; }
                float f = (end - ZIG[i - 1]) / (ZIG[i + 1] - ZIG[i - 1]);
                crack.lineTo(ZIG[i - 2] + (ZIG[i] - ZIG[i - 2]) * f, end);
                break;
            }
            line.setColor(0xFF5E3A17); line.setStrokeWidth(1.8f); line.setAlpha((int) (255 * (1 - swing)));
            c.save(); c.clipPath(shell); c.drawPath(crack, line); c.restore();
            line.setAlpha(255);
        }
        if (layer >= 0) c.restoreToCount(layer);
    }

    private void half(Canvas c, Path side, float deg) {
        c.save();
        c.rotate(deg, 54, 90);
        c.clipPath(side);
        fill.setColor(0xFF8A5A2E); c.drawCircle(CX, CY, R, fill);                 // the peel
        fill.setColor(0xFF2E6B1E); c.drawCircle(CX, CY, R - 3.4f, fill);          // the flesh's dark rim
        fill.setColor(0xFF6DB02A); c.drawCircle(CX, CY, R - 5.2f, fill);          // the flesh
        fill.setColor(0xFF9CD13A); c.drawCircle(CX, CY, 21, fill);                // its lighter heart
        line.setColor(0xFFD4F06A); line.setStrokeWidth(1.4f);
        for (int i = 0; i < 8; i++) {                                             // the light rays
            double a = i * Math.PI / 4;
            c.drawLine(CX + (float) Math.cos(a) * 14, CY + (float) Math.sin(a) * 14, CX + (float) Math.cos(a) * 27, CY + (float) Math.sin(a) * 27, line);
        }
        fill.setColor(0xFFF4EDCB); c.drawCircle(CX, CY, 11, fill);                // the core
        fill.setColor(0xFF15150F);
        for (int i = 0; i < 8; i++) {                                             // the seeds, between the rays
            double a = (i + 0.5) * Math.PI / 4;
            float x = CX + (float) Math.cos(a) * 17.5f, y = CY + (float) Math.sin(a) * 17.5f;
            c.save(); c.rotate((float) Math.toDegrees(a), x, y); c.drawOval(x - 2.4f, y - 1.2f, x + 2.4f, y + 1.2f, fill); c.restore();
        }
        line.setColor(0xFF15150F); line.setStrokeWidth(2f); c.drawCircle(CX, CY, R, line);   // the outline
        c.restore();
    }
}
