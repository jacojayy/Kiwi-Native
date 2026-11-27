package dev.peanuts.build;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * What the build knows about a pack folder that pack.tsv does not say, in a file of its own beside it ({@link #FILE}, "key&lt;TAB&gt;value" lines; a pack without
 * the file, every pack made before it, reads as "nothing to say"):
 * <ul>
 * <li><b>stale</b>: the game file the pack was made from was imported again since ({@link BuildService}'s import writes it before the new .nup takes the old
 *     one's place). The pack still plays, but its shaders were found in the old file: {@link Prepare} does not answer "Already prepared" for it and makes the
 *     shaders and the pack again. A build that finishes takes the row away.</li>
 * <li><b>without</b> n total: the pack was built by Try anyway without n of the total shaders the app knows for the game, because they come from add-on content
 *     that was not imported. The game sheet says so and offers Import again.</li>
 * </ul>
 * No Android class here: the JVM tests drive it.
 */
public final class PackNote {
    private PackNote() {}

    public static final String FILE = "peanuts-pack.tsv";
    private static final String STALE = "stale", WITHOUT = "without";

    private static Map<String, String> read(File packDir) {
        Map<String, String> rows = new LinkedHashMap<>();
        File f = packDir == null ? null : new File(packDir, FILE);
        if (f == null || !f.isFile()) return rows;
        try {
            for (String l : Files.readAllLines(f.toPath(), StandardCharsets.UTF_8)) {
                int t = l.indexOf('\t');
                if (t > 0) rows.put(l.substring(0, t), l.substring(t + 1));
            }
        } catch (IOException e) {
            rows.clear();   // a file that does not read says nothing
        }
        return rows;
    }

    /** The rows written whole beside the old file, then moved over it; no rows: no file */
    private static void write(File packDir, Map<String, String> rows) throws IOException {
        File f = new File(packDir, FILE), tmp = new File(packDir, FILE + ".part");
        if (rows.isEmpty()) { Files.deleteIfExists(f.toPath()); return; }
        StringBuilder s = new StringBuilder();
        for (Map.Entry<String, String> e : rows.entrySet()) s.append(e.getKey()).append('\t').append(e.getValue()).append('\n');
        Files.write(tmp.toPath(), s.toString().getBytes(StandardCharsets.UTF_8));
        Files.move(tmp.toPath(), f.toPath(), StandardCopyOption.REPLACE_EXISTING);
    }

    /** Whether the game file was imported again since this pack was made */
    public static boolean stale(File packDir) { return read(packDir).containsKey(STALE); }

    /** The game file of this pack is being replaced by a new import. Only a folder that holds a pack (pack.tsv) is marked; false when it holds none */
    public static boolean markStale(File packDir) throws IOException {
        if (packDir == null || !new File(packDir, "pack.tsv").isFile()) return false;
        Map<String, String> rows = read(packDir);
        rows.put(STALE, "the game file was imported again");
        write(packDir, rows);
        return true;
    }

    /** A build finished into this pack: it is of the game file that is there now, and lacks without of total shaders for add-on content that was not imported (0: none) */
    public static void built(File packDir, long without, long total) throws IOException {
        if (packDir == null || !packDir.isDirectory()) return;
        Map<String, String> rows = read(packDir);
        rows.remove(STALE);
        if (without > 0) rows.put(WITHOUT, without + "\t" + total);
        else rows.remove(WITHOUT);
        write(packDir, rows);
    }

    /** {shaders the pack was built without, the shaders the app knows for the game}, null when the pack lacks none for add-on content (or says nothing) */
    public static long[] without(File packDir) {
        String v = read(packDir).get(WITHOUT);
        if (v == null) return null;
        String[] c = v.split("\t");
        try {
            long n = Long.parseLong(c[0].trim()), of = c.length > 1 ? Long.parseLong(c[1].trim()) : 0;
            return n > 0 ? new long[] {n, of} : null;
        } catch (NumberFormatException e) {
            return null;
        }
    }
}
