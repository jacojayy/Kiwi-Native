"""KIWI: Share build log (android/app/src/main/java/hernandez/kiwinative/kiwi/build/BuildLog*.java, hernandez/kiwinative/kiwi/ui/BuildLogUi.java; kiwi/docs/FAQ.md "Share build
log"): the pure half on the JVM (BuildLogJvmTest: the build log, the bundle's names, cutting, the jobs it finds, what never goes in), and the marked lines
that reach it from upstream's files. The JVM part is skipped without a JDK."""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
APP = REPO / 'android/app/src'
SRC = [APP / 'main/java/hernandez/kiwinative/kiwi/build/BuildLog.java', APP / 'main/java/hernandez/kiwinative/kiwi/build/BuildLogBundle.java']
TEST = APP / 'jvmtest/java/hernandez/kiwinative/kiwi/build/BuildLogJvmTest.java'


class Wiring(unittest.TestCase):
    def test_entry_points(self):
        main = (APP / 'main/java/hernandez/kiwinative/MainActivity.java').read_text(encoding='utf-8')
        self.assertIn('v -> hernandez.kiwinative.kiwi.ui.BuildLogUi.shareLog(this, this::shareLog)', main)
        self.assertIn('hernandez.kiwinative.kiwi.ui.BuildLogUi.shareLog(this, g.titleId, g.name, null, () -> shareLog(g.titleId))', main)   # KIWI Round 42: one Share log row asks which (kiwi/tests/test_share_log_choice.py)
        ond = (APP / 'ondevice/java/hernandez/kiwinative/Ondevice.java').read_text(encoding='utf-8')
        self.assertIn('hernandez.kiwinative.kiwi.ui.BuildLogUi.card(a, card, ', ond)
        self.assertIn('hernandez.kiwinative.kiwi.ui.BuildLogUi.shareLog(a, j.titleId(), j.name(), null, null)', ond)
        lib = (APP / 'main/java/hernandez/kiwinative/kiwi/ui/Library.java').read_text(encoding='utf-8')
        self.assertIn('BuildLogUi.shareLog(a, b.titleId, name, null, null)', lib)
        ew = (APP / 'main/java/hernandez/kiwinative/ExitWatch.java').read_text(encoding='utf-8')
        for f, text in (('MainActivity', main), ('Ondevice', ond), ('ExitWatch', ew)):
            for line in text.splitlines():
                if 'BuildLogUi' in line:
                    self.assertIn('KIWI', line, f + ': an upstream line without its marker: ' + line.strip())

    def test_provider(self):
        m = (APP / 'main/AndroidManifest.xml').read_text(encoding='utf-8')
        block = re.search(r'<!-- KIWI begin: Share build log.*?<!-- KIWI end -->', m, re.S)
        self.assertIsNotNone(block, 'the provider block')
        self.assertIn('android:name="hernandez.kiwinative.kiwi.build.BuildLogProvider"', block.group(0))
        self.assertIn('android:authorities="${applicationId}.buildlog"', block.group(0))
        self.assertIn('android:exported="false"', block.group(0))
        p = (APP / 'main/java/hernandez/kiwinative/kiwi/build/BuildLogProvider.java').read_text(encoding='utf-8')
        self.assertIn('getPackageName() + ".buildlog"', p)
        self.assertIn('if (!"r".equals(mode))', p)

    def test_ctl(self):
        req = (APP / 'main/java/hernandez/kiwinative/kiwi/ctl/Request.java').read_text(encoding='utf-8')
        self.assertIn('"share_build_log"', req)
        doc = (REPO / 'kiwi/docs/CONTROL.md').read_text(encoding='utf-8')
        self.assertIn('`share_build_log`', doc)


@unittest.skipUnless(shutil.which('javac') and shutil.which('java'), 'no JDK')
class BuildLogJvm(unittest.TestCase):
    def test_rules(self):
        with tempfile.TemporaryDirectory() as d:
            c = subprocess.run(['javac', '-Xlint:all', '-Werror', '-encoding', 'UTF-8', '-d', d] + [str(s) for s in SRC] + [str(TEST)], capture_output=True, text=True)
            self.assertEqual(c.returncode, 0, c.stderr)
            r = subprocess.run(['java', '-cp', d, 'hernandez.kiwinative.kiwi.build.BuildLogJvmTest'], capture_output=True, text=True, timeout=300)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), 'ok')


if __name__ == '__main__':
    unittest.main()
