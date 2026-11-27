"""The in-game overlay's label / value table (Hud, HudSettings, FrameStats, HistogramSpan in android/app/src/main/java/hernandez/kiwinative/kiwi/overlay; the hooks in
Overlay.java, MainActivity.java, SidePanel.java, GameActivity.java): the hooks are there, the settings' label table mirrors Overlay.ITEMS, the preferences file is
Prefs', the maths run on a JVM, and the Android classes compile against android.jar. The JVM parts are skipped without a JDK, the compile check without android.jar."""
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
APP = REPO / 'android/app/src'
PEA = APP / 'main/java/hernandez/kiwinative/kiwi'
OV = PEA / 'overlay'
NX = APP / 'main/java/hernandez/kiwinative'
TEST = APP / 'jvmtest/java/hernandez/kiwinative/kiwi/overlay/FrameStatsJvmTest.java'
ANDROID_JAR = Path.home() / 'Library/Android/sdk/platforms/android-36/android.jar'


def jar():
    j = os.environ.get('ANDROID_JAR')
    if j and Path(j).exists():
        return Path(j)
    if ANDROID_JAR.exists():
        return ANDROID_JAR
    for sdk in (os.environ.get('ANDROID_HOME'), os.environ.get('ANDROID_SDK_ROOT')):   # a machine whose SDK is elsewhere: its newest platform
        jars = sorted(Path(sdk).glob('platforms/android-*/android.jar')) if sdk else []
        if jars:
            return jars[-1]
    return None


class Wiring(unittest.TestCase):
    def test_hooks(self):
        o = (NX / 'Overlay.java').read_text()
        self.assertIn('{"graph", "Frame time graph and 1% low"},', o)
        self.assertIn('{"driver", "GPU driver"},', o)
        self.assertIn('&& !hernandez.kiwinative.kiwi.overlay.Hud.extra(ctx)) return null;', o)
        self.assertIn('return hernandez.kiwinative.kiwi.overlay.Hud.table(ctx, s, rate, fresh, level, mainCap(), batTemp, batLevel, charging, rss, memTotal, memAvail, memLow);', o)
        for f, ctx in (('MainActivity.java', 'this'), ('SidePanel.java', 'act')):
            t = (NX / f).read_text()
            self.assertRegex(t, r'Overlay\.rows\(%s, box, null\);\n\s*hernandez\.kiwinative\.kiwi\.overlay\.HudSettings\.rows\(%s, box\);   // KIWI' % (ctx, ctx), f)
        sp = (NX / 'SidePanel.java').read_text()
        self.assertRegex(sp, r'hernandez\.kiwinative\.kiwi\.ui\.PanelPolish\.apply\(box\);   // KIWI')
        g = (NX / 'GameActivity.java').read_text()
        self.assertRegex(g, r'hernandez\.kiwinative\.kiwi\.overlay\.Hud\.driver\(.*\);\s+// KIWI')
        for f in ('Overlay.java', 'MainActivity.java', 'SidePanel.java', 'GameActivity.java'):
            for line in (NX / f).read_text().splitlines():
                if 'hernandez.kiwinative.kiwi.overlay.Hud' in line:
                    self.assertIn('KIWI', line, f)
        b = (PEA / 'Bridge.java').read_text()
        self.assertIn('public static native int frameTimes(float[] out);', b)
        self.assertIn('public static native void graphRect(int x, int y, int w, int h);', b)
        self.assertIn('public static native void graphBars(int n);', b)
        self.assertIn('Bridge.graphBars(bars)', (OV / 'HistogramSpan.java').read_text())
        self.assertIn('Java_hernandez_kiwinative_kiwi_Bridge_graphBars', (REPO / 'dm' / 'kiwi' / 'kiwi_graph.c').read_text())
        self.assertIn('PILL_BARS = 6, PILL_FRAMES = 20;', (OV / 'Hud.java').read_text())
        self.assertIn('FPS_SIZE = 1f, UNIT_SIZE = 1f, SECONDARY_SIZE = 1f, GRAPH_SIZE = 1f;', (OV / 'Hud.java').read_text())
        self.assertIn('new IconSpan(IconSpan.SEP, SEP)', (OV / 'Hud.java').read_text())
        self.assertIn('view.setBackground(new PillBg());', (OV / 'Hud.java').read_text())
        self.assertIn('RIM = 0x4DD9A066', (OV / 'PillBg.java').read_text())

    def test_graph_hook(self):
        n = (REPO / 'android' / 'app' / 'src' / 'main' / 'cpp' / 'nxlaunch.c').read_text()
        self.assertRegex(n, r'#pragma weak kiwi_ovl_block\s+// KIWI')
        self.assertRegex(n, r'if \(kiwi_ovl_block\) kiwi_ovl_block\(slot, \(void \*\)p, p \? w : 0, p \? h : 0, stride\);\s+// KIWI')
        self.assertLess(n.index('if (kiwi_ovl_block)'), n.index('return nxhost_overlay_set(slot'))
        self.assertRegex((REPO / 'android' / 'host' / 'libnxhost.map').read_text(), r'kiwi_ovl_block;\s+/\* KIWI')
        self.assertIn('kiwi_graph.c', (REPO / 'android' / 'host' / 'CMakeLists.txt').read_text())
        self.assertIn('pn_graph_frame();', (REPO / 'dm' / 'kiwi' / 'kiwi.c').read_text())
        v = (REPO / 'dm' / 'gfx' / 'vk_android' / 'vka_present.c').read_text()
        self.assertRegex(v, r'__attribute__\(\(weak\)\) int kiwi_ovl_corner\(int slot, int dflt\);\s+// KIWI')
        self.assertIn('(kiwi_ovl_corner ? kiwi_ovl_corner(s, g_ocorner[s]) : g_ocorner[s]) /* KIWI', v)
        self.assertIn('public static native void overlayCorner(int corner);', (PEA / 'Bridge.java').read_text())
        h = (OV / 'Hud.java').read_text()
        for k in ('"pn_overlay_corner"', '"pn_overlay_scale"', '{"tl", "tr", "bl", "br", "tc", "bc"}', '{0, 1, 2, 3, 4, 6}', 'Bridge.overlayCorner(c)', 'new RelativeSizeSpan(scale / 100f)'):
            self.assertIn(k, h)
        self.assertIn('Java_hernandez_kiwinative_kiwi_Bridge_overlayCorner', (REPO / 'dm' / 'kiwi' / 'kiwi_graph.c').read_text())
        self.assertIn('VKO_TOP_LEFT = 0, VKO_TOP_RIGHT = 1, VKO_BOTTOM_LEFT = 2, VKO_BOTTOM_RIGHT = 3', (REPO / 'dm' / 'gfx' / 'vk_android' / 'vka_ovl.h').read_text())
        self.assertIn('Java_hernandez_kiwinative_kiwi_Bridge_graphRect', (REPO / 'dm' / 'kiwi' / 'kiwi_graph.c').read_text())
        o = (REPO / 'dm' / 'gfx' / 'vk_android' / 'vka_ovl.h').read_text()
        self.assertRegex(o, r'enum \{ VKO_CENTER = 4, VKO_TOP_CENTER = 4, VKO_BOTTOM_CENTER = 6 \};\s+// KIWI')
        self.assertRegex(o, r'if \(corner & VKO_CENTER\) dx = \(ew - bw\) / 2;\s+// KIWI')
        self.assertIn('corner == 6', (REPO / 'dm' / 'kiwi' / 'kiwi_graph.c').read_text())

    def test_pill_tag_keeps_the_table_tag(self):
        # #538 landing: the pill hides the build tag without writing overlay_tag=false for the table (a false set for the pill is taken back)
        s = (OV / 'HudSettings.java').read_text()
        self.assertIn('if (pill && !on) p.edit().remove("overlay_tag").commit();', s)
        self.assertIn('if ("tag".equals(l[0])) tagBox(p, b, want, pill); else b.setChecked(want);', s)
        self.assertIn('v -> { p.edit().putString(Hud.STYLE, v).apply(); syncTag(c, p, box); }', s)
        self.assertNotIn('((CheckBox) v).setChecked(Hud.tagOn(c))', s)

    def test_pill(self):
        h = (OV / 'Hud.java').read_text()
        self.assertIn('return p.getString(STYLE, "table");', h)
        self.assertIn('return p.getString(CORNER, "pill".equals(style(p)) ? "tc" : "tr");', h)
        self.assertIn('return p.getString(PRESET, "pill".equals(style(p)) ? "normal" : HudSettings.DEFAULT_PRESET);', h)
        self.assertIn('if ("pill".equals(style)) return fit(c, pill(', h)
        self.assertIn('return fit(c, pill(c, p, rate, fresh, level, batTemp, batLevel, charging, memTotal, memAvail, memLow), Math.round(scale * PILL_BASE));', h)
        self.assertIn('PILL_MAX_WIDTH = 0.75f;', h)
        self.assertIn('getCurrentWindowMetrics().getBounds().width()', h)
        self.assertIn('if (pill) fitTo(view, view.getMeasuredWidth());', h)
        self.assertIn('return p.contains("overlay_tag") ? p.getBoolean("overlay_tag", true) : !"pill".equals(style(p));', h)
        g = (NX / 'GameActivity.java').read_text()
        self.assertEqual(g.count('hernandez.kiwinative.kiwi.overlay.Hud.tagOn(this)'), 2)
        self.assertNotIn('Prefs.overlayItem(this, "tag")', g)
        self.assertIn('PILL_BASE = 1.5f;', h)
        o = (NX / 'Overlay.java').read_text()
        self.assertIn('int[] pm = slot == 1 ? hernandez.kiwinative.kiwi.overlay.Hud.margins(view, h, this.inL, this.inT, this.inR) : null;   // KIWI', o)
        self.assertLess(o.index('Hud.margins(view'), o.index('int bw = w + inL + inR, bh = h + inT;'))
        self.assertIn('public static int[] margins(android.view.View view, int h, int l, int t, int r)', h)
        for k in ('icon(d, IconSpan.TEMP, lv)', 'icon(d, IconSpan.BATTERY, lv)', 'icon(d, IconSpan.RAM, lv)', 'GREY = 0xFFA89A8A, ICON = 0xFFE6DCCF, SECONDARY = 0xFFD6CDC2, SEP = 0x73D9A066'):
            self.assertIn(k, h)
        span = (OV / 'HistogramSpan.java').read_text()
        self.assertIn('{0xFF4CAF50, 0xFFFFC107, 0xFFF44336}', span)   # the table keeps green bars on pace
        self.assertIn('PLAIN = 0xFFBDBDBD', span)                       # light grey only for the pill
        self.assertIn('new HistogramSpan(PILL_BARS, true)', (OV / 'Hud.java').read_text())
        self.assertIn('(atomic_load(&gr_plain) ? 0xBDBDBD : 0x4CAF50)', (REPO / 'dm' / 'kiwi' / 'kiwi_graph.c').read_text())
        s = (OV / 'HudSettings.java').read_text()
        self.assertIn('PILL_MINIMAL = {"fps"}', s)
        self.assertIn('PILL_NORMAL = {"fps", "graph", "soc"}', s)
        self.assertIn('PILL_FULL = {"fps", "throttle", "graph", "low", "soc", "battery", "memory"}', s)
        self.assertIn('new String[] {"Pill", "Table", "Classic"}, new String[] {"pill", "table", "classic"}, () -> Hud.style(p)', s)
        self.assertIn('new String[] {"tc", "tr", "tl", "bc", "br", "bl"}, () -> Hud.corner(p)', s)
        labels = set(re.findall(r'\{"(\w+)", "[^"]*"\}', s[s.index('LABELS = {'):s.index('};', s.index('LABELS = {'))]))
        for name in ('PILL_MINIMAL', 'PILL_NORMAL', 'PILL_FULL'):
            block = s[s.index(name + ' = {'):]
            self.assertLessEqual(set(re.findall(r'"(\w+)"', block[:block.index('}')])), labels | {'low'}, name)

    def test_kiwi_md(self):
        t = (REPO / 'KIWI.md').read_text()
        for w in ('Hud.extra', 'Hud.table', 'HudSettings.rows', 'Hud.driver', 'SidePanel.java', 'Bridge.frameTimes', 'kiwi_ovl_block', 'kiwi_graph.c', 'kiwi_ovl_corner'):
            self.assertIn(w, t)

    def test_label_table_matches_items(self):
        o = (NX / 'Overlay.java').read_text()
        block = o[o.index('ITEMS = {'):]
        block = block[:block.index('};')]
        items = re.findall(r'\{"(\w+)", "([^"]*)"\}', block)
        h = (OV / 'HudSettings.java').read_text()
        block = h[h.index('LABELS = {'):]
        block = block[:block.index('};')]
        labels = re.findall(r'\{"(\w+)", "([^"]*)"\}', block)
        self.assertEqual(sorted(items), sorted(labels))
        self.assertEqual(len(items), 10)

    def test_prefs_file_name(self):
        p = (NX / 'Prefs.java').read_text()
        m = re.search(r'static SharedPreferences of\(Context c\) \{ return open\(c, "(\w+)"\); \}', p)
        self.assertIsNotNone(m)
        self.assertIn('PREFS = "%s";' % m.group(1), (OV / 'Hud.java').read_text())
        self.assertIn('return c.getSharedPreferences(name, Context.MODE_MULTI_PROCESS)', p)
        self.assertIn('"overlay_" + key', p)


@unittest.skipUnless(shutil.which('javac') and shutil.which('java'), 'no JDK')
class FrameStatsJvm(unittest.TestCase):
    def test_stats(self):
        with tempfile.TemporaryDirectory() as d:
            c = subprocess.run(['javac', '-Xlint:all', '-Werror', '-encoding', 'UTF-8', '-d', d, str(OV / 'FrameStats.java'), str(TEST)], capture_output=True, text=True)
            self.assertEqual(c.returncode, 0, c.stderr)
            r = subprocess.run(['java', '-cp', d, 'hernandez.kiwinative.kiwi.overlay.FrameStatsJvmTest'], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), 'ok')


@unittest.skipUnless(shutil.which('javac') and jar(), 'no JDK or android.jar')
class Compiles(unittest.TestCase):
    def test_android_classes(self):
        srcs = [OV / n for n in ('Hud.java', 'HudSettings.java', 'HistogramSpan.java', 'IconSpan.java', 'PillBg.java', 'SocTemp.java', 'SocZones.java', 'FrameStats.java')] + [PEA / 'Bridge.java', PEA / 'ui/PanelPolish.java']
        with tempfile.TemporaryDirectory() as d:
            c = subprocess.run(['javac', '-encoding', 'UTF-8', '-Xlint:-deprecation', '-cp', str(jar()), '-d', d] + [str(s) for s in srcs], capture_output=True, text=True)
            self.assertEqual(c.returncode, 0, c.stderr)


if __name__ == '__main__':
    unittest.main()
