// PEANUTS: a test of the fork's own classes (which companion package belongs to a title, the return-to-game decision, the job command's words); upstream has no dev.peanuts tests
package dev.peanuts;

import dev.peanuts.build.Reimport;
import dev.peanuts.ctl.Request;
import dev.peanuts.settings.CompanionSection;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * Three rules of the fork's own code on the JVM (run with the app's classes and the Android API jar on the class path; no argument; prints "ok" or throws):
 * <ul>
 * <li>CompanionSection.packageFor: in a folder that holds several games' companion packages, the package of a title is the one whose manifest names it, as
 *     the runtime picks it (dm/peanuts/peanuts.c rt_scan_root), and none when no manifest does;</li>
 * <li>the control interface's job command takes stop as the build card's name for pause;</li>
 * <li>an import again whose free space is not known (Long.MAX_VALUE) still answers whether the new file fits without the old one;</li>
 * <li>GameReturn.runningPid: the app's icon goes back to the game only when the game's run record names the title started last.</li>
 * </ul>
 */
public final class CompanionPickJvmTest {
    private static void check(boolean ok, String what) { if (!ok) throw new AssertionError(what); }

    private static void pkg(File root, String name, String manifest) throws IOException {
        File d = new File(new File(root, name), "dualscreen");
        Files.createDirectories(d.toPath());
        if (manifest != null) Files.write(new File(d, "manifest.json").toPath(), manifest.getBytes(StandardCharsets.UTF_8));
    }

    private static Request request(String... kv) {
        Map<String, String> m = new LinkedHashMap<>();
        for (int i = 0; i < kv.length; i += 2) m.put(kv[i], kv[i + 1]);
        return new Request(m, 1L);
    }

    public static void main(String[] a) throws Exception {
        File root = Files.createTempDirectory("companion-pick").toFile();
        try {
            // the first folder by name is another game's: it was the one reported for every game
            pkg(root, "AlphaDS", "{\n  \"name\": \"Alpha\",\n  \"title_id\": \"010055D009F78000\"\n}");
            pkg(root, "EchoesDS", "{\"title_id\":\"01008CF01BAAC000\",\"module\":{}}");
            pkg(root, "LowerDS", "{\"title_id\": \"0100f2c0115b6000\"}");
            pkg(root, "NoTitleDS", "{\"name\": \"no title\"}");
            pkg(root, "NoManifestDS", null);
            pkg(root, "EchoesDS.new", "{\"title_id\":\"0100E95004038000\"}");   // an install under way
            pkg(root, "EchoesDS.old", "{\"title_id\":\"0100E95004038000\"}");   // a swap's parked folder
            pkg(root, ".peanuts-versions", "{\"title_id\":\"0100E95004038000\"}");
            check("EchoesDS".equals(CompanionSection.packageFor(root, "01008CF01BAAC000")), "the package whose manifest names the title");
            check("EchoesDS".equals(CompanionSection.packageFor(root, "01008cf01baac000")), "the title in lower case");
            check("AlphaDS".equals(CompanionSection.packageFor(root, "010055D009F78000")), "the first folder for its own title");
            check("LowerDS".equals(CompanionSection.packageFor(root, "0100F2C0115B6000")), "a manifest in lower case");
            check(CompanionSection.packageFor(root, "0100152000022000") == null, "a title no package names has none");
            check(CompanionSection.packageFor(root, "0100E95004038000") == null, "a hidden folder, .new and .old are passed over");
            check(CompanionSection.packageFor(root, "") == null && CompanionSection.packageFor(root, null) == null && CompanionSection.packageFor(root, "not a title") == null, "no title");
            check(CompanionSection.packageFor(null, "01008CF01BAAC000") == null && CompanionSection.packageFor(new File(root, "absent"), "01008CF01BAAC000") == null, "no folder");
        } finally {
            Files.walk(root.toPath()).sorted(java.util.Comparator.reverseOrder()).forEach(p -> p.toFile().delete());
        }

        Request stop = request("cmd", "job", "do", "stop"), pause = request("cmd", "job", "do", "PAUSE"), resume = request("cmd", "job", "do", "resume");
        stop.check(); pause.check(); resume.check();
        check("pause".equals(stop.action()) && "pause".equals(pause.action()) && "resume".equals(resume.action()), "job: stop is pause");
        check(!stop.needsScreen() && resume.needsScreen(), "job stop needs no screen, resume does");
        try { request("cmd", "job", "do", "halt").check(); check(false, "job: an unknown word is refused"); } catch (Request.Bad e) { check("bad_args".equals(e.code), e.code); }

        Reimport.Plan p = new Reimport.Plan();
        p.oldBytes = 6L << 30; p.needBytes = 7L << 30;
        p.freeBytes = Long.MAX_VALUE;
        check(!p.lowSpace() && p.fitsWithoutOld(), "free space not known: it fits");
        p.freeBytes = 2L << 30;
        check(p.lowSpace() && p.fitsWithoutOld(), "2 GB free and 6 GB given back hold 7 GB");
        p.freeBytes = 512L << 20;
        check(p.lowSpace() && !p.fitsWithoutOld(), "0.5 GB free and 6 GB given back do not hold 7 GB");

        // the app's icon goes back to a game only when the run record names the remembered title and a pid
        Map<String, String> rec = dev.peanuts.ctl.ExitRecord.parse("v=1\ntitle=01008CF01BAAC000\nname=Echoes\npid=4321\nstart=5\nstate=running\n");
        check(GameReturn.runningPid(rec, "01008CF01BAAC000") == 4321 && GameReturn.runningPid(rec, "01008cf01baac000") == 4321, "the remembered title runs");
        check(GameReturn.runningPid(rec, "0100152000022000") == -1, "another title runs: nothing is started");
        check(GameReturn.runningPid(rec, null) == -1 && GameReturn.runningPid(dev.peanuts.ctl.ExitRecord.parse(null), "01008CF01BAAC000") == -1, "nothing remembered, or no record");
        check(GameReturn.runningPid(dev.peanuts.ctl.ExitRecord.parse("title=01008CF01BAAC000\npid=x\n"), "01008CF01BAAC000") == -1, "a record without a pid");
        System.out.println("ok");
    }
}
