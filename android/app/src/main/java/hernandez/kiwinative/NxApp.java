package hernandez.kiwinative;

import android.app.Application;

/** Runs first in every process of the app (the launcher, :game, :pack, :readiness, :selftest): a .nup picked in place opens there as any .nup (PickedNup) */
public final class NxApp extends Application {
    @Override public void onCreate() {
        super.onCreate();
        PickedNup.install(this);
        hernandez.kiwinative.kiwi.ui.AppScreens.install(this);   // KIWI: the app's pages in other processes keep the second display's page (hernandez.kiwinative.kiwi.ui.AppScreens)
    }
}
