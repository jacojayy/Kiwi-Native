package dev.peanuts.ctl;

import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * The game process's own record of its run (last-exit.txt in the app's files: key=value lines, a later line of a key replaces an earlier one), as the status
 * file's "game" part. Only what the record says is given: a key it lacks is left out. No Android class here: the JVM tests drive it.
 */
public final class ExitRecord {
    private ExitRecord() {}

    public static Map<String, String> parse(String text) {
        Map<String, String> m = new HashMap<>();
        if (text == null) return m;
        for (String l : text.split("\n")) {
            int eq = l.indexOf('=');
            if (eq > 0) m.put(l.substring(0, eq).trim(), l.substring(eq + 1).trim());
        }
        return m;
    }

    public static int pid(Map<String, String> rec) {
        try { return Integer.parseInt(rec.getOrDefault("pid", "")); } catch (NumberFormatException e) { return -1; }
    }

    /** alive: the recorded process still runs (the caller asks Android). A record without a title gives null: nothing is known of a game */
    public static Map<String, Object> game(Map<String, String> rec, boolean alive) {
        String title = rec.get("title");
        if (title == null || title.isEmpty()) return null;
        Map<String, Object> g = new LinkedHashMap<>();
        g.put("running", alive);
        g.put("title_id", title);
        if (rec.get("name") != null) g.put("name", rec.get("name"));
        if (pid(rec) > 0) g.put("pid", pid(rec));
        try { g.put("since", Long.parseLong(rec.getOrDefault("start", ""))); } catch (NumberFormatException e) { /* the record has no start time */ }
        if (rec.get("log") != null) g.put("log", rec.get("log"));
        // the last state the game's process wrote: running (and gone: ended from outside), stopped, java, native, host_exit
        if (!alive && rec.get("state") != null) g.put("ended_as", rec.get("state"));
        return g;
    }
}
