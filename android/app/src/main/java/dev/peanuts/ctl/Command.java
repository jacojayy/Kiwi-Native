package dev.peanuts.ctl;

import android.app.Activity;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.util.Log;

import java.util.LinkedHashMap;
import java.util.Map;

/**
 * The control interface's one door (peanuts/docs/CONTROL.md):
 * <pre>adb shell am start -n &lt;package&gt;/dev.peanuts.ctl.Command -e cmd &lt;name&gt; [-e &lt;argument&gt; &lt;value&gt; ...] [-e id &lt;token&gt;]</pre>
 * The manifest guards it with android.permission.DUMP: Android checks that permission against whoever starts the activity, adb's shell holds it and an
 * ordinary app cannot. The activity draws nothing and finishes at once; the command's outcome goes to &lt;app files&gt;/ctl/status.json and one log line
 * (tag peanuts-ctl). Nothing else in this package is reachable from outside the app.
 */
public final class Command extends Activity {
    static final String GATE = "android.permission.DUMP";

    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        try {
            if (saved == null && guarded()) Ctl.receive(this, extras());
        } catch (RuntimeException e) {
            Log.w(Ctl.TAG, "command not taken: " + e);
        }
        finish();   // before the first frame: the theme shows nothing
    }

    /** The door is only used while the manifest really guards it: a build whose manifest lost the permission line takes no command at all */
    private boolean guarded() {
        try {
            ActivityInfo me = getPackageManager().getActivityInfo(getComponentName(), 0);
            if (GATE.equals(me.permission)) return true;
            Log.w(Ctl.TAG, "command refused: this activity is not guarded by " + GATE + " in the manifest");
        } catch (PackageManager.NameNotFoundException e) {
            Log.w(Ctl.TAG, "command refused: " + e);
        }
        return false;
    }

    /** Every extra as text, whatever its type (am's -e and --es give strings, --ei numbers, --ez booleans) */
    private Map<String, String> extras() {
        Map<String, String> out = new LinkedHashMap<>();
        Bundle b = getIntent() == null ? null : getIntent().getExtras();
        if (b != null) for (String k : b.keySet()) {
            Object v = b.get(k);
            if (v instanceof CharSequence || v instanceof Number || v instanceof Boolean) out.put(k, v.toString());
        }
        return out;
    }
}
