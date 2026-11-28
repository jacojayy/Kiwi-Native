package hernandez.kiwinative.kiwi.ui;

import android.app.Activity;
import android.content.Context;
import android.content.res.Configuration;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

/**
 * A page held in landscape fills the whole screen: the status and navigation bars are hidden (a swipe from the edge shows them for a moment), and the
 * page draws under the camera cutout instead of leaving a black bar beside it (the page's own insets keep its content clear of the cutout). Held upright
 * it is as before, with its bars. Ui.page and PUi.page ask on every build, which every turn of the phone makes again; a window that gets the focus back
 * (after a dialog, the keyboard or a swipe) asks again, since the system shows the bars then.
 */
public final class Immersive {
    private Immersive() {}

    /**
     * A phone held in landscape: the window is wide and short (under 520 dp high). Its pages then spend less height on their header and padding and
     * show more, smaller covers in a row, so there is less to scroll.
     */
    public static boolean compact(Context c) {
        Configuration cfg = c.getResources().getConfiguration();
        return cfg.orientation == Configuration.ORIENTATION_LANDSCAPE && cfg.screenHeightDp < 520;
    }

    public static void apply(Activity a) {
        Window w = a.getWindow();
        boolean landscape = a.getResources().getConfiguration().orientation == Configuration.ORIENTATION_LANDSCAPE;
        WindowManager.LayoutParams lp = w.getAttributes();
        int cutout = landscape ? WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES : WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_DEFAULT;
        if (lp.layoutInDisplayCutoutMode != cutout) { lp.layoutInDisplayCutoutMode = cutout; w.setAttributes(lp); }
        WindowInsetsController bars = w.getInsetsController();
        if (bars == null) return;   // the window has no content yet: the page asks again when it builds
        if (landscape) {
            bars.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            bars.hide(WindowInsets.Type.systemBars());
        } else {
            bars.show(WindowInsets.Type.systemBars());
        }
    }
}
