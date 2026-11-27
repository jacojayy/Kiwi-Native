package dev.nxnative.android;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.TreeSet;

/**
 * The shader libraries of a pack folder (shaders/v-&lt;V&gt;/dmsh_vk.lib) and their delta update (android/SHADERS.md#delta-updates), as the launcher decides them:
 * <ul>
 * <li>{@link #playable}: a library of another version the host plays while this version's library is being made: its header names the game and carries this APK's
 * DMSH_VK_ABI, or else one of the older ABIs the host also plays (DMSH_VK_ABI_COMPAT, vka_abi_compat.h), the rule of the replay's own choice (vka_shader.c lib_load,
 * vka_shlib_check); the newest such one, as the replay takes it.</li>
 * <li>{@link #toTranslate}: how many shaders the update translates from the older libraries the pack has, from the profile's delta manifest "from" rows (Profile.delta),
 * and {@link #minutes} the time that takes on the phone.</li>
 * <li>{@link #restamp}: an older library that is this version's byte for byte but for the header's version and abi (the manifest's "stamp" rows, Profile.stamp) becomes
 * this version's library by rewriting those fields: no update job at all (android/SHADERS.md#restamp).</li>
 * <li>{@link #selection}: the shaders Prepare builds for a profile with a selected set (shader_seen.tsv), not every shader of its map.</li>
 * </ul>
 * No Android classes: the JVM test (android/app/src/jvmtest ShaderLibsJvmTest, dm/tools/tests/test_android_shader_delta.py) runs it on the Mac.
 */
final class ShaderLibs {
    private ShaderLibs() {}

    static final int MAGIC = 0x4B56534D;   // dmsh_abi_vk.h DMSH_VK_LIB_MAGIC, DMSH_VK_LIB_VERSION
    static final int VERSION = 1;

    /** What the launcher reads of a library's header (dmsh_vk_header: chain at 8, vk_chain at 24, game at 40, inputs at 56, abi at 116, 12 characters) */
    static final class Header {
        String chain, vkChain, game, inputs, abi;
    }

    private static String text(byte[] h, int off, int n) {
        int e = off;
        while (e < off + n && h[e] != 0) e++;
        return new String(h, off, e - off, StandardCharsets.US_ASCII);
    }

    /** The header of a library file; null when it is not one this app reads (short, another magic or format) */
    static Header header(File lib) {
        byte[] h = new byte[128];
        try (RandomAccessFile f = new RandomAccessFile(lib, "r")) {
            f.readFully(h);
        } catch (IOException e) {
            return null;
        }
        int magic = (h[0] & 0xff) | (h[1] & 0xff) << 8 | (h[2] & 0xff) << 16 | (h[3] & 0xff) << 24, version = (h[4] & 0xff) | (h[5] & 0xff) << 8 | (h[6] & 0xff) << 16 | (h[7] & 0xff) << 24;
        if (magic != MAGIC || version != VERSION) return null;
        Header r = new Header();
        r.chain = text(h, 8, 16);
        r.vkChain = text(h, 24, 16);
        r.game = text(h, 40, 16);
        r.inputs = text(h, 56, 16);
        String a = text(h, 116, 12);
        r.abi = a.matches("[0-9a-f]{12}") ? a : "";   // zero in a library made before the field: played by its own version only
        return r;
    }

    /** The v-&lt;V&gt; folders of a pack folder that hold a library */
    private static File[] libraries(File packDir) {
        File[] l = new File(packDir, "shaders").listFiles(f -> f.getName().startsWith("v-") && new File(f, "dmsh_vk.lib").isFile());
        return l == null ? new File[0] : l;
    }

    /**
     * The library of another version than current the host plays (the newest one whose header names titleId and carries abi; when there is none, the newest that carries
     * one of compat, the older ABIs this APK's host also plays), null when none. current: this APK's DMSH_VK_CHAIN (its own library is the launcher's usual check, not this
     * one); abi: its DMSH_VK_ABI; compat: its DMSH_VK_ABI_COMPAT (vka_abi_compat.h: space-separated, "" or null = none), the order of vka_shader.c lib_load
     */
    static File playable(File packDir, String titleId, String current, String abi, String compat) {
        if (abi == null || !abi.matches("[0-9a-f]{12}")) return null;
        Set<String> older = new HashSet<>();
        if (compat != null) for (String c : compat.trim().split("\\s+")) if (c.matches("[0-9a-f]{12}") && !c.equals(abi)) older.add(c);
        File best = null, bestOlder = null;
        for (File d : libraries(packDir)) {
            if (d.getName().equals("v-" + current)) continue;
            File lib = new File(d, "dmsh_vk.lib");
            Header h = header(lib);
            if (h == null || !titleId.equals(h.game)) continue;
            if (abi.equals(h.abi)) { if (best == null || lib.lastModified() > best.lastModified()) best = lib; }
            else if (older.contains(h.abi) && (bestOlder == null || lib.lastModified() > bestOlder.lastModified())) bestOlder = lib;
        }
        return best != null ? best : bestOlder;
    }

    /** As above with no older ABIs */
    static File playable(File packDir, String titleId, String current, String abi) { return playable(packDir, titleId, current, abi, null); }

    /**
     * A library of another version made before the header's abi field (no ABI: whether the host reads it the same is not known), the newest; null when none. The host plays
     * it as a last resort, after any library of its ABI (vka_shader.c lib_load), with a warning instead of drawing nothing; the launcher does not block Play on it and
     * suggests one more shader update, after which updates are deltas of the same ABI
     */
    static File legacy(File packDir, String titleId, String current) {
        File best = null;
        for (File d : libraries(packDir)) {
            if (d.getName().equals("v-" + current)) continue;
            File lib = new File(d, "dmsh_vk.lib");
            Header h = header(lib);
            if (h == null || !titleId.equals(h.game) || !h.abi.isEmpty()) continue;
            if (best == null || lib.lastModified() > best.lastModified()) best = lib;
        }
        return best;
    }

    /** The warning for a game played on such a library */
    static final String LEGACY_WARNING = "This game's shaders were made before versioned shader updates, so this app version cannot update them in part. "
            + "You can keep playing on them, but updating the shaders once more with Prepare shaders is suggested: after that, shader fixes arrive as small incremental updates.";

    /** The versions of the libraries of titleId's game a pack folder has, other than current (what a delta update can start from) */
    static List<String> older(File packDir, String titleId, String current) {
        List<String> out = new java.util.ArrayList<>();
        for (File d : libraries(packDir)) {
            String v = d.getName().substring(2);
            Header h = header(new File(d, "dmsh_vk.lib"));
            if (!v.equals(current) && h != null && titleId.equals(h.game)) out.add(v);
        }
        return out;
    }

    /**
     * How many shaders a delta update translates when the pack has the older versions given: the fewest of the profile's "from" rows (delta: "version=count,…", from the
     * manifest's from rows) for those versions; -1 when the manifest has none of them (the update then translates as many as a full build, or more as the tool finds)
     */
    static int toTranslate(String delta, List<String> versions) {
        int best = -1;
        if (delta == null) return -1;
        for (String row : delta.split(",")) {
            int eq = row.indexOf('=');
            if (eq <= 0 || !versions.contains(row.substring(0, eq).trim())) continue;
            try {
                int n = Integer.parseInt(row.substring(eq + 1).trim());
                if (n >= 0 && (best < 0 || n < best)) best = n;
            } catch (NumberFormatException e) {
                // a malformed row is not a measurement
            }
        }
        return best;
    }

    /**
     * The phone's time for a shaders step that translates n shaders, as "a-b" minutes: the split of the dump (4-8 min, android/ONDEVICE.md "Time and power"), each shader
     * translated (45-80 ms: the step's 20-35 min for DKB's 22,852 less the split), and the library written and checked (about 1 min)
     */
    static int[] minutes(int n) {
        int lo = 4 + 1 + (int) Math.round(n * 0.045 / 60), hi = 8 + 1 + (int) Math.ceil(n * 0.080 / 60);
        return new int[] {lo, Math.max(hi, lo + 1)};
    }

    // ---- Restamp: the manifest's "stamp" rows (vk_android_delta.py STAMP_FIELDS, restamped_header) ----

    static final int STAMP_VERSION_AT = 24, STAMP_VERSION_LEN = 16, STAMP_ABI_AT = 116, STAMP_ABI_LEN = 12;
    private static final Object RESTAMP = new Object();   // one restamp at a time (two cards may name the same pack)

    /**
     * Writes this version's library (shaders/v-&lt;current&gt;/) from an older one of the pack that a "stamp" row names, when the pack has none of its own: the older file's
     * SHA-256 must be the row's first hash; it is copied with its header's version set to current and its abi to abi, into shaders/.restamp-&lt;current&gt;/ with the
     * older folder's inputs.tsv, and moved in by rename only when the copy's SHA-256 is the row's second hash (the Mac's library of this version, byte for byte). The
     * older library stays. stamps: "version:old SHA-256:new SHA-256", ';' between them. → the older version taken over, null when none was (nothing changed then)
     */
    static String restamp(File packDir, String titleId, String current, String abi, String stamps) {
        if (stamps == null || stamps.isEmpty() || current == null || !current.matches("[0-9a-f]{12}") || abi == null || !abi.matches("[0-9a-f]{12}")) return null;
        File sh = new File(packDir, "shaders"), want = new File(sh, "v-" + current), tmp = new File(sh, ".restamp-" + current);
        synchronized (RESTAMP) {
            if (new File(want, "dmsh_vk.lib").isFile()) return null;
            for (String row : stamps.split(";")) {
                String[] c = row.trim().split(":");
                if (c.length != 3 || !c[0].matches("[0-9a-f]{12}") || c[0].equals(current) || !c[1].matches("[0-9a-f]{64}") || !c[2].matches("[0-9a-f]{64}")) continue;
                File from = new File(sh, "v-" + c[0]), lib = new File(from, "dmsh_vk.lib");
                Header h = header(lib);
                if (h == null || !titleId.equals(h.game)) continue;
                try {
                    if (!c[1].equals(sha256(lib))) continue;   // not the library the Mac compared (damaged, or made by other tools)
                    if (sh.getUsableSpace() < lib.length() + (64L << 20)) return null;
                    deleteTree(tmp);
                    if (!tmp.mkdirs()) continue;
                    if (!c[2].equals(copyStamped(lib, new File(tmp, "dmsh_vk.lib"), current, abi))) { deleteTree(tmp); continue; }
                    File in = new File(from, "inputs.tsv");
                    if (in.isFile()) Files.copy(in.toPath(), new File(tmp, "inputs.tsv").toPath());
                    if (want.isDirectory()) want.delete();   // an empty folder of this version (a stopped job's): only an empty one goes
                    if (!tmp.renameTo(want)) { deleteTree(tmp); continue; }
                    return c[0];
                } catch (IOException | NoSuchAlgorithmException e) {
                    deleteTree(tmp);
                }
            }
        }
        return null;
    }

    private static String hex(byte[] d) {
        StringBuilder b = new StringBuilder();
        for (byte x : d) b.append(String.format(Locale.ROOT, "%02x", x & 0xff));
        return b.toString();
    }

    static String sha256(File f) throws IOException, NoSuchAlgorithmException {
        MessageDigest md = MessageDigest.getInstance("SHA-256");
        byte[] buf = new byte[1 << 20];
        try (InputStream in = new FileInputStream(f)) {
            for (int n; (n = in.read(buf)) > 0; ) md.update(buf, 0, n);
        }
        return hex(md.digest());
    }

    /** Copies a library with its version and abi fields set (as vk_android_delta.py restamped_header), synced; → the copy's SHA-256 */
    private static String copyStamped(File src, File dst, String version, String abi) throws IOException, NoSuchAlgorithmException {
        MessageDigest md = MessageDigest.getInstance("SHA-256");
        byte[] buf = new byte[1 << 20];
        try (InputStream in = new FileInputStream(src); FileOutputStream out = new FileOutputStream(dst)) {
            byte[] h = new byte[128];
            for (int got = 0, n; got < h.length; got += n) if ((n = in.read(h, got, h.length - got)) < 0) throw new IOException("short library");
            java.util.Arrays.fill(h, STAMP_VERSION_AT, STAMP_VERSION_AT + STAMP_VERSION_LEN, (byte) 0);
            byte[] v = version.getBytes(StandardCharsets.US_ASCII);
            System.arraycopy(v, 0, h, STAMP_VERSION_AT, Math.min(v.length, STAMP_VERSION_LEN));
            java.util.Arrays.fill(h, STAMP_ABI_AT, STAMP_ABI_AT + STAMP_ABI_LEN, (byte) 0);
            byte[] a = abi.getBytes(StandardCharsets.US_ASCII);
            System.arraycopy(a, 0, h, STAMP_ABI_AT, Math.min(a.length, STAMP_ABI_LEN));
            out.write(h); md.update(h);
            for (int n; (n = in.read(buf)) > 0; ) { out.write(buf, 0, n); md.update(buf, 0, n); }
            out.getFD().sync();
        }
        return hex(md.digest());
    }

    private static void deleteTree(File f) {
        File[] l = f.listFiles();
        if (l != null) for (File x : l) deleteTree(x);
        f.delete();
    }

    /**
     * A game card's state while a background shader update runs (MainActivity's card, from the update's job): "Ready · updating shaders 1,234 / 22,852 · about 12 min
     * left", "Ready · shader update paused 1,234 / 22,852", or without the count while the job has none yet. eta: Job.etaText ("" while unknown)
     */
    static String updatingText(Locale l, long done, long total, String eta, boolean paused) {
        String count = total > 0 ? String.format(l, " %,d / %,d", Math.min(done, total), total) : "";
        return (paused ? "Ready · shader update paused" : "Ready · updating shaders") + count + (!paused && eta != null && !eta.isEmpty() ? " · " + eta : "");
    }

    static String minutesText(int n) {
        int[] m = minutes(n);
        return String.format(Locale.ROOT, "about %d-%d minutes", m[0], m[1]);
    }

    /**
     * A whole Prepare (the code rewritten, then every shader of the game's map, n: Profile.shaderTotal) as "a-b" minutes: the rewrite's 2-5 minutes on the phone
     * (Donkey Kong Bananza 74 s, ONDEVICE.md "Phone test status") and the shaders step as {@link #minutes}. DKB (22,852 shaders) 24-45, Mario Kart World (11,604) 16-30
     */
    static String jobMinutesText(int n) {
        int[] m = minutes(n);
        return String.format(Locale.ROOT, "about %d-%d minutes", m[0] + 2, m[1] + 5);
    }

    /** The profile's selected shader set: its file in dm/games/&lt;title ID&gt;/ (rows "key&lt;TAB&gt;stage"; android/SHADERS.md "Selected shaders") */
    static final String SELECTED = "shader_seen.tsv";

    /**
     * The shader keys the shaders step passes to vk_android_build.py for a profile with a selected set ({@link #SELECTED}): its keys that the profile's shader_map.tsv
     * has (column 4), sorted, each once; the tool adds the pixel shaders paired with them (dmsh_build.py build_keys). null when the profile has no selected set: the
     * step then builds every shader of the map. The same keys the Mac's selected build takes (docs/development.md, Building selected Mac shaders), so the phone's
     * library is the Mac's.
     */
    static List<String> selection(File profile) throws IOException {
        File seen = new File(profile, SELECTED);
        if (!seen.isFile()) return null;
        Set<String> map = new HashSet<>();
        for (String l : Files.readAllLines(new File(profile, "shader_map.tsv").toPath(), StandardCharsets.UTF_8)) {
            if (l.isEmpty() || l.startsWith("#") || l.startsWith("package\t")) continue;
            String[] c = l.split("\t", 5);
            if (c.length > 3) map.add(c[3]);
        }
        TreeSet<String> keys = new TreeSet<>();
        for (String l : Files.readAllLines(seen.toPath(), StandardCharsets.UTF_8)) {
            if (l.length() < 16 || !l.substring(0, 16).matches("[0-9a-f]{16}")) continue;
            String k = l.substring(0, 16);
            if (map.contains(k)) keys.add(k);
        }
        return new ArrayList<>(keys);
    }

    /** The profile's keys that a dump of one of its registered builds may lack: column 1 of dm/games/&lt;title ID&gt;/shader_optional.tsv (scripts/pack_android.py optional_keys) */
    static final String OPTIONAL = "shader_optional.tsv";

    /** The keys of a shader_map.tsv (column 4 of its package rows, as pack_android.py map_keys reads them) */
    static Set<String> mapKeys(File map) throws IOException {
        Set<String> keys = new HashSet<>();
        for (String l : Files.readAllLines(map.toPath(), StandardCharsets.UTF_8)) {
            String[] c = l.split("\t", 6);
            if (c.length > 4 && c[0].endsWith(".bin")) keys.add(c[3]);
        }
        return keys;
    }

    /**
     * The keys of the profile's shader_map.tsv that a split's map (splitMap) lacks, sorted, when the split is the profile's but for keys its {@link #OPTIONAL} names
     * (a registered build of the game without a few of its shaders: Mario Kart World's 1.8.0 as an update, docs/reference/packing.md "A game with several registered
     * builds"); null when the split has a key the profile's map does not, lacks one that is not optional, or lacks none. The shaders step then builds the map's other keys.
     */
    static List<String> absentOptional(File profile, File splitMap) throws IOException { return absentOptional(profile, splitMap, -1); }   // PEANUTS: the form without a version

    // PEANUTS begin: a profile whose shader tables are the union of several versions of the game (game.tsv shader_versions), prepared from a dump of one of them.
    // shader_split.py prefixes the source of a shader that first appears in a later version with that version (/v1507328/Common/..., main@v1507328; the first
    // version's rows carry none), so a dump of one listed version may lack exactly the rows tagged with the other listed versions: they count as optional
    // (absentOptional below), with no failure and no Try anyway. Mario Kart 8 Deluxe's tables are 4.0.0's and 4.0.1's: 135 of 29,785 rows are 4.0.1's own, and
    // a dump of 4.0.0 was refused for them (2026-11-15). The tags do not say which untagged rows a later version dropped: such a dump is refused as before,
    // unless shader_optional.tsv names them. scripts/pack_android.py may_lack is the same rule on the PC
    /** game.tsv's shader_versions ("v0", "v327680"): the versions the profile's shader tables are the union of; empty when it names none or cannot be read */
    static List<String> shaderVersions(File profile) {
        List<String> out = new ArrayList<>();
        try {
            for (String l : Files.readAllLines(new File(profile, "game.tsv").toPath(), StandardCharsets.UTF_8)) {
                String[] c = l.split("\t", 2);
                if (c.length == 2 && c[0].trim().equals("shader_versions")) for (String v : c[1].trim().split("\\s+")) if (v.matches("v[0-9]+")) out.add(v);
            }
        } catch (IOException e) {
            out.clear();
        }
        return out;
    }

    /** The listed versions other than the dump's, when the dump's version is listed; else none (a single-version profile, a version the tables were not made from) */
    static List<String> otherVersions(File profile, long version) {
        List<String> listed = shaderVersions(profile);
        if (!listed.contains("v" + version)) return new ArrayList<>();
        listed.removeIf(v -> v.equals("v" + version));
        return listed;
    }

    /** The keys of the profile's map whose source is tagged with a listed version other than the dump's: shaders only those versions have */
    static Set<String> otherVersionKeys(File profile, long version) throws IOException {
        Set<String> keys = new HashSet<>();
        List<String> others = otherVersions(profile, version);
        if (others.isEmpty()) return keys;
        for (String l : Files.readAllLines(new File(profile, "shader_map.tsv").toPath(), StandardCharsets.UTF_8)) {
            String[] c = l.split("\t", 6);
            if (c.length <= 4 || !c[0].endsWith(".bin")) continue;
            for (String v : others) {
                int at = c[4].indexOf("@" + v), end = at + v.length() + 1;
                if (c[4].startsWith("/" + v + "/") || at >= 0 && (end >= c[4].length() || !Character.isDigit(c[4].charAt(end)))) { keys.add(c[3]); break; }
            }
        }
        return keys;
    }

    /** The one line for the log when a dump lacks only what other listed versions have (PackService), as scripts/pack_android.py prints it */
    static String otherVersionsLine(int theirs, int total, List<String> others, long version) {
        return String.format(java.util.Locale.US, "%,d of %,d shaders in this app's table belong to other versions of the game (%s): not needed for v%d", theirs, total,
                String.join(", ", others), version);
    }
    // PEANUTS end

    /** PEANUTS: version, the dump's (-1: not given): the keys only another listed version has (otherVersionKeys) may be lacking too, with or without the file */
    static List<String> absentOptional(File profile, File splitMap, long version) throws IOException {
        File opt = new File(profile, OPTIONAL);
        Set<String> optional = otherVersionKeys(profile, version);   // PEANUTS (was: no shader_optional.tsv, nothing optional)
        if (!opt.isFile() && optional.isEmpty()) return null;   // PEANUTS: && optional.isEmpty()
        Set<String> want = mapKeys(new File(profile, "shader_map.tsv")), have = mapKeys(splitMap);
        if (!want.containsAll(have)) return null;
        if (opt.isFile())   // PEANUTS: the file is no longer the only source
        for (String l : Files.readAllLines(opt.toPath(), StandardCharsets.UTF_8)) if (!l.isEmpty() && !l.startsWith("#")) optional.add(l.split("\t", 2)[0]);
        TreeSet<String> absent = new TreeSet<>(want);
        absent.removeAll(have);
        return !absent.isEmpty() && optional.containsAll(absent) ? new ArrayList<>(absent) : null;
    }

    // PEANUTS begin: the three rules of scripts/pack_android.py that the phone's Prepare keeps too (PackService): may_lack, lacking_shaders' one case that names
    // add-on content, and no_nvn_shaders
    /** The keys a dump of this version may lack with no failure: shader_optional.tsv's, and those only another listed version has (pack_android.py may_lack) */
    static Set<String> mayLack(File profile, long version) throws IOException {
        Set<String> keys = otherVersionKeys(profile, version);
        File opt = new File(profile, OPTIONAL);
        if (opt.isFile()) for (String l : Files.readAllLines(opt.toPath(), StandardCharsets.UTF_8)) if (!l.isEmpty() && !l.startsWith("#")) keys.add(l.split("\t", 2)[0]);
        return keys;
    }

    /**
     * Whether add-on content (DLC) that was not imported is named as a possible reason for the shaders a split lacks: only when nothing else is known to explain
     * them. missing: the shaders it lacks beyond mayLack; absent: those of them whose file the dump does not have (every one of them, or the dump differs for
     * another reason); addons: the add-on packages the dump holds (none); listed: game.tsv's shader_versions, which must be the dump's version alone (a union
     * of versions, a profile that names none, or a dump of another version: a missing file is then more likely another version's)
     */
    static boolean addonCase(int missing, int absent, int addons, List<String> listed, long version) {
        return missing > 0 && absent == missing && addons == 0 && listed.size() == 1 && listed.get(0).equals("v" + version);
    }

    /** Whether the profile says its game has no NVN shaders for a split to find: game.tsv's shader_note says "no NVN shaders", or the profile lists the shaders
     *  captured from a run instead (vk_capture.tsv, gl_capture.tsv). A split that reads no shader of any other game is a failure (PackService.emptySplit) */
    static boolean noNvnShaders(File profile) {
        if (new File(profile, "vk_capture.tsv").isFile() || new File(profile, "gl_capture.tsv").isFile()) return true;
        try {
            for (String l : Files.readAllLines(new File(profile, "game.tsv").toPath(), StandardCharsets.UTF_8)) {
                String[] c = l.split("\t", 2);
                if (c.length == 2 && c[0].trim().equals("shader_note") && c[1].toLowerCase(java.util.Locale.ROOT).contains("no nvn shaders")) return true;
            }
        } catch (IOException e) { /* no game.tsv to say so */ }
        return false;
    }
    // PEANUTS end

    // PEANUTS begin: where the shaders a split lacks come from, so a failed comparison can tell files the import does not have (add-on content that was not
    // imported) from files that are there with other shaders (PackService.splitMatchesProfile, PrepareText.addonNotImported)
    /**
     * The keys of the profile's map (profileMap) that a split's map lacks → the RomFS path its row's source names, sorted by key: the source up to its first '#'
     * ("/Course/Gu_Cloud/course.ptcl" of "/Course/Gu_Cloud/course.ptcl#BNSH@0xab000#var0"; a file inside a container keeps the container's path before its own),
     * "" for a source that is no RomFS path (main, a run-time save)
     */
    static java.util.SortedMap<String, String> missingSources(File profileMap, File splitMap) throws IOException {
        Set<String> have = mapKeys(splitMap);
        java.util.SortedMap<String, String> out = new java.util.TreeMap<>();
        for (String l : Files.readAllLines(profileMap.toPath(), StandardCharsets.UTF_8)) {
            String[] c = l.split("\t", 6);
            if (c.length <= 4 || !c[0].endsWith(".bin") || have.contains(c[3]) || out.containsKey(c[3])) continue;
            int h = c[4].indexOf('#');
            String src = h < 0 ? c[4] : c[4].substring(0, h);
            out.put(c[3], src.startsWith("/") ? src : "");
        }
        return out;
    }
    // PEANUTS end
}
