"""peanuts/scripts/pack_origfill.py and dev.peanuts.pack.OrigFill (the app's side, the same checks in Java): <module>.orig.bin added to a pack made before the
rewriter wrote it, from the game's own module and the pack's <module>.changed.tsv. The file is byte for byte the one the rewriter writes (a pack is made here with
dm/tools/patch_nso.py, its main.orig.bin taken away and made again), and nothing is written when the pack's code is not made from the game's file: another build,
a byte changed outside the table's runs, a table past the code, a damaged module. Written through a .part file and never a second time. The Java half is
skipped without a JDK, the rewriter's without Capstone 5."""
import os, shutil, struct, subprocess, sys, tempfile, unittest
from pathlib import Path
from unittest import mock

import lz4.block

REPO = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(REPO / 'peanuts/scripts'), str(REPO / 'dm/tools'), str(REPO / 'dm/tools/tests'), str(REPO / 'scripts')]
import pack_origfill
import test_nso
import test_xmod

JAVA_SRC = REPO / 'android/app/src/main/java/dev/peanuts/pack/OrigFill.java'
RET, BID = 0xD65F03C0, '11' * 16
# Code the rewriter changes: x18 as a register, the thread pointer, a supervisor call
CODE = {0x100: 0xAA0003F2, 0x104: 0xF9400240, 0x108: RET, 0x200: 0xD53BD040, 0x204: RET, 0x300: 0xD40000E1, 0x304: RET}
try:
    import capstone
    HAVE_REWRITER = capstone.cs_version()[0] == 5
except ImportError: HAVE_REWRITER = False


def compressed(nso_bytes):
    """The same NSO with its code stored as an LZ4 block and its hash checked, as retail modules are"""
    import hashlib
    d = bytearray(nso_bytes)
    fo, _, sz = struct.unpack_from('<III', d, 0x10)
    text, rest = bytes(d[fo:fo + sz]), bytes(d[fo + sz:])
    z = lz4.block.compress(text, store_size=False)
    for i in (1, 2):
        o, mo, s = struct.unpack_from('<III', d, 0x10 + i * 0x10); struct.pack_into('<III', d, 0x10 + i * 0x10, o - sz + len(z), mo, s)
    struct.pack_into('<I', d, 0x60, len(z)); struct.pack_into('<I', d, 0xc, struct.unpack_from('<I', d, 0xc)[0] | 1 | 8)
    d[0xa0:0xc0] = hashlib.sha256(text).digest()
    return bytes(d[:fo]) + z + rest


def big_module():
    """A module with 3 MiB of code that compresses to long matches and literal runs: more than the Java reader's 2 MiB buffer, so its window moves"""
    import random
    r = random.Random(7)
    text = bytearray()
    while len(text) < 3 << 20:
        text += r.randbytes(r.randrange(1, 600)) if r.random() < 0.5 else bytes([r.randrange(256)]) * r.randrange(4, 70000)
        if r.random() < 0.3 and len(text) > 70000: o = r.randrange(1, 65535); n = r.randrange(4, 5000); text += text[-o:][:n]
    del text[3 << 20:]
    return test_nso.make_nso(segs=(bytes(text), test_nso.RO, test_nso.DATA), compress=(1, 0, 1)), bytes(text)


class Pack:
    """A pack folder with a module's table and code, made by hand: code is the module's own text with the runs' words replaced"""

    def __init__(self, d, text, runs, bid, name='main', tail=b'\xee' * 0x40):
        self.dir = Path(d); self.dir.mkdir(parents=True, exist_ok=True); self.name = name
        code = bytearray(text)
        for off, n in runs: code[off:off + n] = b'\xff' * n
        (self.dir / f'{name}.patched.bin').write_bytes(bytes(code) + tail)
        (self.dir / f'{name}.changed.tsv').write_text(f'# BUILD_ID\t{bid}\n' + ''.join(f'{o:#x}\t{n:#x}\n' for o, n in runs), encoding='utf-8', newline='\n')
        self.want = b''.join(text[o:o + n] for o, n in runs)

    @property
    def orig(self): return self.dir / f'{self.name}.orig.bin'

    def files(self): return sorted(f.name for f in self.dir.iterdir())


class Core(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='origfill_'); self.d = Path(self.tmp.name)
        self.nso = bytes(test_nso.make_nso()); self.text = test_nso.TEXT; self.bid = bytes(range(0x10)).hex()
        self.runs = [(0x10, 4), (0x40, 8), (0x100, 0x20), (len(self.text) - 4, 4)]

    def tearDown(self): self.tmp.cleanup()

    def test_runs_in_the_tables_order(self):
        p = Pack(self.d / 'p', self.text, self.runs, self.bid)
        self.assertEqual(pack_origfill.fill(p.dir, 'main', self.nso), 48)
        self.assertEqual(p.orig.read_bytes(), p.want)
        self.assertEqual(p.files(), ['main.changed.tsv', 'main.orig.bin', 'main.patched.bin'])   # nothing else, and no .part left

    def test_written_once(self):
        p = Pack(self.d / 'p', self.text, self.runs, self.bid)
        pack_origfill.fill(p.dir, 'main', self.nso)
        p.orig.write_bytes(b'kept')
        self.assertIsNone(pack_origfill.fill(p.dir, 'main', self.nso))
        self.assertIsNone(pack_origfill.fill(p.dir, 'main', b'not a module at all'))   # the file is the marker: the game is not even read
        self.assertEqual(p.orig.read_bytes(), b'kept')

    def test_no_changed_runs(self):
        p = Pack(self.d / 'p', self.text, [], self.bid)
        self.assertEqual(pack_origfill.fill(p.dir, 'main', self.nso), 0)
        self.assertEqual(p.orig.read_bytes(), b'')   # as the rewriter writes it: an empty file

    def refused(self, p, module, *words):
        before = {f: (p.dir / f).read_bytes() for f in p.files()}
        with self.assertRaises(pack_origfill.Refused) as c: pack_origfill.fill(p.dir, p.name, module)
        for w in words: self.assertIn(w, str(c.exception))
        self.assertEqual({f: (p.dir / f).read_bytes() for f in p.files()}, before)   # nothing written, nothing changed
        return str(c.exception)

    def test_another_build_refused(self):
        p = Pack(self.d / 'p', self.text, self.runs, 'ff' * 16)
        self.refused(p, self.nso, 'of build ' + 'ff' * 16, self.bid, 'another version of the game')

    def test_code_not_of_this_file_refused(self):
        p = Pack(self.d / 'p', self.text, self.runs, self.bid)
        code = bytearray((p.dir / 'main.patched.bin').read_bytes()); code[0x23] ^= 1   # one byte outside every run
        (p.dir / 'main.patched.bin').write_bytes(code)
        self.refused(p, self.nso, 'differs from the game\'s file at 0x20', 'outside the runs')

    def test_a_change_inside_a_run_is_the_rewriters(self):
        p = Pack(self.d / 'p', self.text, self.runs, self.bid)
        code = bytearray((p.dir / 'main.patched.bin').read_bytes()); code[0x44] ^= 1
        (p.dir / 'main.patched.bin').write_bytes(code)
        pack_origfill.fill(p.dir, 'main', self.nso)
        self.assertEqual(p.orig.read_bytes(), p.want)

    def test_table_past_the_code_refused(self):
        p = Pack(self.d / 'p', self.text + bytes(8), self.runs + [(len(self.text), 4)], self.bid)
        self.refused(p, self.nso, 'past the end of the code')

    def test_short_code_refused(self):
        p = Pack(self.d / 'p', self.text, self.runs, self.bid, tail=b'')
        (p.dir / 'main.patched.bin').write_bytes(self.text[:-8])
        self.refused(p, self.nso, 'less than the code')

    def test_bad_tables_refused(self):
        for rows, word in (('0x10\t0x4\n0x10\t0x4\n', 'before the end of the run above'), ('0x12\t0x4\n', 'not whole instructions'), ('0x10\t0x0\n', 'not whole instructions'),
                           ('sixteen\tfour\n', 'not an offset and a length')):
            with self.subTest(rows):
                p = Pack(self.d / word.replace(' ', '_'), self.text, [], self.bid)
                (p.dir / 'main.changed.tsv').write_text(f'# BUILD_ID\t{self.bid}\n{rows}')
                self.refused(p, self.nso, word)
        p = Pack(self.d / 'nobid', self.text, [], self.bid)
        (p.dir / 'main.changed.tsv').write_text('0x10\t0x4\n')
        self.refused(p, self.nso, 'no BUILD_ID line')

    def test_damaged_module_refused(self):
        p = Pack(self.d / 'p', self.text, self.runs, self.bid)
        bad = bytearray(test_nso.make_nso(compress=(0, 0, 0))); bad[0x180] ^= 1   # the code's hash no longer matches
        self.refused(p, bytes(bad), 'SHA-256')
        self.refused(p, b'NSO0' + bytes(8), 'cut short')

    def test_no_table_or_no_code(self):
        p = Pack(self.d / 'p', self.text, self.runs, self.bid); (p.dir / 'main.changed.tsv').unlink()
        self.refused(p, self.nso, 'no main.changed.tsv', 'only a rebuild')
        q = Pack(self.d / 'q', self.text, self.runs, self.bid); (q.dir / 'main.patched.bin').unlink()
        self.refused(q, self.nso, 'no main.patched.bin')

    def test_written_through_a_part_file(self):
        p = Pack(self.d / 'p', self.text, self.runs, self.bid)
        seen, real = [], os.replace

        def replace(src, dst):
            seen.append((Path(src).name, Path(dst).name, Path(src).read_bytes(), Path(dst).exists())); real(src, dst)
        with mock.patch.object(pack_origfill.os, 'replace', replace): pack_origfill.fill(p.dir, 'main', self.nso)
        self.assertEqual(seen, [('main.orig.bin.part', 'main.orig.bin', p.want, False)])   # complete before it takes the name

    def test_a_failed_write_leaves_nothing(self):
        p = Pack(self.d / 'p', self.text, self.runs, self.bid)
        with mock.patch.object(pack_origfill.os, 'replace', side_effect=OSError('no space')): self.refused(p, self.nso, 'cannot be written', 'no space')
        self.assertEqual(p.files(), ['main.changed.tsv', 'main.patched.bin'])

    def test_another_module(self):
        p = Pack(self.d / 'p', self.text, self.runs, self.bid, name='sdk')
        self.assertEqual(pack_origfill.fill(p.dir, 'sdk', self.nso), 48)
        self.assertEqual((p.dir / 'sdk.orig.bin').read_bytes(), p.want)
        self.assertFalse((p.dir / 'main.orig.bin').exists())


class Game(unittest.TestCase):
    """The game's module found in a dump folder, a .nup, an exefs folder or as the file itself; the command's lines and exit code"""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='origfill_game_'); self.d = Path(self.tmp.name)
        self.nso = bytes(test_nso.make_nso()); self.text = test_nso.TEXT; self.bid = bytes(range(0x10)).hex()
        other = bytearray(test_nso.make_nso()); other[0x40] ^= 0xff; self.other = bytes(other)   # another build of the same code
        tid = 0x0100AAAA0000B000
        npdm = bytearray(0x100); npdm[:4] = b'META'; npdm[0x70] = 0x80; npdm[0x74] = 0x20; npdm[0x80:0x84] = b'ACI0'; npdm[0x90:0x98] = tid.to_bytes(8, 'little')
        self.dump = self.d / 'Game'
        for sub, main in (('base', self.other), ('updates/65536', self.nso)):
            e = self.dump / sub / 'exefs'; e.mkdir(parents=True)
            (e / 'main').write_bytes(main); (e / 'sdk').write_bytes(self.nso); (e / 'main.npdm').write_bytes(bytes(npdm))
            (self.dump / sub / 'romfs').mkdir(); (self.dump / sub / 'romfs/a.txt').write_text(sub)

    def tearDown(self): self.tmp.cleanup()

    def pack(self, name, version='65536', modules=('main',)):
        p = Pack(self.d / name, self.text, [(0x40, 8)], self.bid)
        for m in modules[1:]: Pack(p.dir, self.text, [(0x80, 4)], self.bid, name=m)
        (p.dir / 'pack.tsv').write_text(f'format\t1\nversion\t{version}\nmodules\t{" ".join(modules)}\n')
        return p

    def run_cli(self, *args):
        return subprocess.run([sys.executable, str(REPO / 'peanuts/scripts/pack_origfill.py'), *map(str, args)], capture_output=True, text=True)

    def test_dump_folder(self):
        p = self.pack('p')
        r = self.run_cli(p.dir, self.dump)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('main: main.orig.bin made from', r.stdout); self.assertIn('updates/65536/exefs/main (1 rewritten runs, 8 bytes)', r.stdout.replace(os.sep, '/'))
        self.assertEqual(p.orig.read_bytes(), p.want)
        r = self.run_cli(p.dir, self.dump)
        self.assertEqual((r.returncode, r.stdout.strip()), (0, 'main: main.orig.bin is there (8 bytes): nothing to do'))

    def test_the_build_is_found_under_another_version(self):
        p = self.pack('p', version='0')   # the pack says the base, whose main is another build: the update's is the pack's
        self.assertEqual(pack_origfill.fill_pack(p.dir, self.dump, say=lambda s: None), 0)
        self.assertEqual(p.orig.read_bytes(), p.want)

    def test_nup(self):
        nup = self.d / 'Game.nup'
        r = subprocess.run([sys.executable, '-I', str(REPO / 'dm/tools/nup_pack.py'), str(self.dump), str(nup), '--raw'], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        p = self.pack('p')
        r = self.run_cli(p.dir, nup)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('Game.nup:update/65536/exefs/main', r.stdout)
        self.assertEqual(p.orig.read_bytes(), p.want)

    def test_exefs_folder_and_file(self):
        for game in (self.dump / 'updates/65536/exefs', self.dump / 'updates/65536/exefs/main'):
            p = self.pack('p-' + game.name)
            self.assertEqual(pack_origfill.fill_pack(p.dir, game, say=lambda s: None), 0)
            self.assertEqual(p.orig.read_bytes(), p.want)

    def test_all_modules(self):
        p = self.pack('p', modules=('main', 'sdk'))
        r = self.run_cli(p.dir, self.dump, '--module', 'all')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual((p.dir / 'sdk.orig.bin').read_bytes(), self.text[0x80:0x84])
        self.assertEqual(p.orig.read_bytes(), p.want)

    def test_wrong_game_says_why_and_fails(self):
        p = self.pack('p')
        r = self.run_cli(p.dir, self.dump / 'base/exefs/main')
        self.assertEqual(r.returncode, 1)
        self.assertIn('[RED] main: main.orig.bin not made: the pack\'s main is of build', r.stdout)
        self.assertFalse(p.orig.exists())
        r = self.run_cli(p.dir, self.d / 'nothing-here')
        self.assertEqual(r.returncode, 1); self.assertIn('[RED] main: main.orig.bin not made', r.stdout)


@unittest.skipUnless(HAVE_REWRITER, 'the rewriter needs Capstone 5')
class Rewriter(unittest.TestCase):
    """A pack made by the rewriter itself: its main.orig.bin taken away and made again is the same file"""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='origfill_rw_'); cls.d = Path(cls.tmp.name)
        cls.plain = test_xmod.module(CODE); cls.lz4 = compressed(cls.plain)
        cls.src = cls.d / 'main'; cls.src.write_bytes(cls.lz4)
        r = subprocess.run([sys.executable, str(REPO / 'dm/tools/patch_nso.py'), str(cls.src), str(cls.d / 'pack'), 'main', '--target', 'android'],
                           capture_output=True, text=True, env=dict(os.environ, DM_REWRITE_J='1', DM_GAME=''))
        assert r.returncode == 0, r.stdout + r.stderr
        cls.built = {f.name: f.read_bytes() for f in (cls.d / 'pack').iterdir()}

    @classmethod
    def tearDownClass(cls): cls.tmp.cleanup()

    def old_pack(self, name):
        p = self.d / name; shutil.copytree(self.d / 'pack', p); (p / 'main.orig.bin').unlink(); return p

    def test_the_rewriter_changed_something(self):
        self.assertEqual(self.built['main.changed.tsv'].decode(), f'# BUILD_ID\t{BID}\n0x100\t0x8\n0x200\t0x4\n0x300\t0x4\n')
        self.assertEqual(self.built['main.orig.bin'], struct.pack('<4I', CODE[0x100], CODE[0x104], CODE[0x200], CODE[0x300]))

    def test_same_file_as_the_rewriters(self):
        for name, module in (('lz4', self.lz4), ('plain', self.plain)):
            with self.subTest(name):
                p = self.old_pack('py-' + name)
                self.assertEqual(pack_origfill.fill(p, 'main', module), 16)
                self.assertEqual({f.name: f.read_bytes() for f in p.iterdir()}, self.built)   # the whole pack is as the rewriter left it

    def test_a_patched_main_is_not_this_packs(self):
        p = self.old_pack('mod')
        mod = bytearray(self.plain); struct.pack_into('<I', mod, 0x100 + 0x400, 0xD503201F ^ 0x20)   # a mod's main: one other instruction outside the runs
        with self.assertRaises(pack_origfill.Refused): pack_origfill.fill(p, 'main', bytes(mod))
        self.assertFalse((p / 'main.orig.bin').exists())


@unittest.skipUnless(shutil.which('javac') and shutil.which('java'), 'no JDK')
class Java(unittest.TestCase):
    """dev.peanuts.pack.OrigFill, what the app runs at a game's start: the same file and the same refusals as the Python"""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='origfill_java_'); cls.d = Path(cls.tmp.name)
        runner = cls.d / 'OrigFillTest.java'
        runner.write_text('''package dev.peanuts.pack;
import java.io.*;
public final class OrigFillTest {
    public static void main(String[] a) throws Exception {
        File module = new File(a[2]);
        try {
            System.out.println("ok " + OrigFill.fill(new File(a[0]), a[1], () -> new FileInputStream(module), module.getName()));
        } catch (OrigFill.Refused e) {
            System.out.println("refused " + e.getMessage());
        }
    }
}
''')
        subprocess.run(['javac', '-nowarn', '-d', str(cls.d / 'classes'), str(JAVA_SRC), str(runner)], check=True)
        cls.nso = bytes(test_nso.make_nso()); cls.text = test_nso.TEXT; cls.bid = bytes(range(0x10)).hex()
        cls.runs = [(0x10, 4), (0x40, 8), (0x100, 0x20), (len(cls.text) - 4, 4)]

    @classmethod
    def tearDownClass(cls): cls.tmp.cleanup()

    def java(self, pack, module, name='main'):
        f = self.d / f'module-{self._testMethodName}-{len(list(self.d.iterdir()))}'; f.write_bytes(module)
        r = subprocess.run(['java', '-cp', str(self.d / 'classes'), 'dev.peanuts.pack.OrigFillTest', str(pack), name, str(f)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return r.stdout.strip()

    def both(self, name, text, runs, bid, module, **kw):
        """The Python's answer and the Java's on two copies of one pack: (the Python's bytes or its refusal, the Java's line, the Java pack)"""
        a, b = Pack(self.d / (name + '-py'), text, runs, bid, **kw), Pack(self.d / (name + '-java'), text, runs, bid, **kw)
        return a, b

    def test_same_file_as_the_python(self):
        big, big_text = big_module()
        r = [(o * 4, 4 * (1 + o % 5)) for o in range(16, len(big_text) // 4 - 8, 9973)]
        cases = {'lz4': (self.text, self.runs, self.nso), 'plain': (self.text, self.runs, bytes(test_nso.make_nso(compress=(0, 0, 0)))), 'none': (self.text, [], self.nso),
                 'big': (big_text, r, bytes(big)), 'first-and-last': (self.text, [(0, 4), (len(self.text) - 8, 8)], self.nso)}
        for name, (text, runs, module) in cases.items():
            with self.subTest(name):
                a, b = self.both(name, text, runs, self.bid, module)
                n = pack_origfill.fill(a.dir, 'main', module)
                self.assertEqual(self.java(b.dir, module), f'ok {n}')
                self.assertEqual(b.orig.read_bytes(), a.orig.read_bytes()); self.assertEqual(b.orig.read_bytes(), a.want)
                self.assertEqual(b.files(), ['main.changed.tsv', 'main.orig.bin', 'main.patched.bin'])
                self.assertEqual(self.java(b.dir, b'no module'), 'ok -1')   # written once: the file is the marker

    def test_same_refusals_as_the_python(self):
        def flip(p, at):
            code = bytearray((p.dir / 'main.patched.bin').read_bytes()); code[at] ^= 1; (p.dir / 'main.patched.bin').write_bytes(code)
        damaged = bytearray(test_nso.make_nso(compress=(0, 0, 0))); damaged[0x180] ^= 1
        short = bytearray(self.nso); del short[0x100 + 40:]
        cases = {'build': (self.runs, 'ff' * 16, self.nso, None), 'outside': (self.runs, self.bid, self.nso, lambda p: flip(p, 0x23)),
                 'past': (self.runs + [(len(self.text), 4)], self.bid, self.nso, None), 'damaged': (self.runs, self.bid, bytes(damaged), None),
                 'notable': (self.runs, self.bid, self.nso, lambda p: (p.dir / 'main.changed.tsv').unlink()),
                 'nocode': (self.runs, self.bid, self.nso, lambda p: (p.dir / 'main.patched.bin').unlink()),
                 'shortcode': (self.runs, self.bid, self.nso, lambda p: (p.dir / 'main.patched.bin').write_bytes(self.text[:-8])),
                 'order': (self.runs, self.bid, self.nso, lambda p: (p.dir / 'main.changed.tsv').write_text(f'# BUILD_ID\t{self.bid}\n0x40\t0x8\n0x10\t0x4\n')),
                 'nobid': (self.runs, self.bid, self.nso, lambda p: (p.dir / 'main.changed.tsv').write_text('0x40\t0x8\n'))}
        for name, (runs, bid, module, spoil) in cases.items():
            with self.subTest(name):
                a, b = self.both(name, self.text + (bytes(8) if name == 'past' else b''), runs, bid, module)
                for p in (a, b):
                    if spoil: spoil(p)
                before = b.files()
                with self.assertRaises(pack_origfill.Refused) as c: pack_origfill.fill(a.dir, 'main', module)
                out = self.java(b.dir, module)
                self.assertTrue(out.startswith('refused '), out)
                self.assertEqual(b.files(), before)   # nothing written, no .part left
                if name != 'damaged': self.assertEqual(out[8:], str(c.exception))   # the same sentence (a module's own damage is told with each reader's name for it)
        # a module cut short inside its compressed code, and one that is no NSO
        for name, module in (('cut', bytes(short)), ('notnso', b'ELF' + bytes(0x200))):
            with self.subTest(name):
                b = Pack(self.d / (name + '-java'), self.text, self.runs, self.bid)
                out = self.java(b.dir, module)
                self.assertTrue(out.startswith('refused '), out); self.assertFalse(b.orig.exists()); self.assertNotIn('main.orig.bin.part', b.files())

    def test_a_stale_part_file_is_replaced(self):
        b = Pack(self.d / 'stale-java', self.text, self.runs, self.bid)
        (b.dir / 'main.orig.bin.part').write_bytes(b'left by a start that was killed')
        self.assertEqual(self.java(b.dir, self.nso), 'ok 48')
        self.assertEqual(b.orig.read_bytes(), b.want); self.assertNotIn('main.orig.bin.part', b.files())

    @unittest.skipUnless(HAVE_REWRITER, 'the rewriter needs Capstone 5')
    def test_same_file_as_the_rewriters(self):
        plain = test_xmod.module(CODE)
        for name, module in (('lz4', compressed(plain)), ('plain', plain)):
            with self.subTest(name):
                src = self.d / f'rw-{name}-main'; src.write_bytes(module); p = self.d / f'rw-{name}'
                r = subprocess.run([sys.executable, str(REPO / 'dm/tools/patch_nso.py'), str(src), str(p), 'main', '--target', 'android'],
                                   capture_output=True, text=True, env=dict(os.environ, DM_REWRITE_J='1', DM_GAME=''))
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                built = {f.name: f.read_bytes() for f in p.iterdir()}
                (p / 'main.orig.bin').unlink()
                self.assertEqual(self.java(p, module), 'ok 16')
                self.assertEqual({f.name: f.read_bytes() for f in p.iterdir()}, built)


if __name__ == '__main__':
    unittest.main()
