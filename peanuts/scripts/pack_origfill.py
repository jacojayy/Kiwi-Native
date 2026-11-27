#!/usr/bin/env python3
# <module>.orig.bin for a pack made before the rewriter wrote it (dm/tools/patch_nso.py since 2026-11-14), without a rebuild.
#   pack_origfill.py <pack> <game>                     main.orig.bin, when the pack has main.changed.tsv and no main.orig.bin
#   pack_origfill.py <pack> <game> --module sdk        another module's file (--module all: every module of pack.tsv's modules row)
# <pack> is the pack folder (<titleID>-work/android-pack); <game> the .nup, the dump folder, an exefs folder, or the module's own file.
# The file holds the bytes the game's own module has at the runs of <module>.changed.tsv (offset and length, in the table's order), one run after the other: what a
# companion package's code reads in place of the rewritten words (dm/peanuts/peanuts.c rt_orig_load), and what a package's load plan needs before it is installed
# (rt_code_rewritten: without the file every patch of main is refused, and the companion steers the game with button presses instead of calling it). The rewriter
# writes exactly these bytes with a rewrite; here they are taken from the game's file again, so the result is the file a rebuild of the same pack would write.
# Nothing is written unless the pack's code is made from this very file: the table's build ID is the module's, and outside the table's runs every byte of
# <module>.patched.bin's code is the module's own (another game version, a mod's module, a damaged file: refused with the reason). The file is written beside
# the others as <module>.orig.bin.part and renamed when it is complete; a file that is there is never written again, and no other file of the pack is touched.
# The app does the same by itself at a game's start (dev.peanuts.pack.OrigFill, the same checks in Java; peanuts/tests/test_origfill.py compares the two).
# Needs lz4 for a compressed module, as the rewriter does (android/environment.yml). Exit code 0 when every asked file is there afterwards, 1 otherwise.
import argparse, os, sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(REPO / 'dm' / 'tools'), str(REPO / 'scripts')]
import nso  # noqa: E402

PART = '.orig.bin.part'   # the unfinished file's suffix (the runtime waits for a fresh one: peanuts.c rt_orig_pending)


class Refused(Exception):
    """Why a pack does not get the file; nothing was written"""


def read_table(text, name='main'):
    """(build ID, [(offset, length)]) of a <module>.changed.tsv: a `# BUILD_ID` line and rows of two hexadecimal numbers. Refused when it is not a table the
    rewriter writes: no build ID, a row that is not whole instructions, rows out of order or overlapping"""
    bid, runs, end = None, [], 0
    for i, line in enumerate(text.splitlines(), 1):
        if line.startswith('# BUILD_ID'):
            cells = line.split('\t')
            bid = cells[1].strip().lower() if len(cells) > 1 else ''
            continue
        if not line.strip() or line.startswith('#'): continue
        try: off, n = (int(x, 16) for x in line.split('\t')[:2])
        except ValueError: raise Refused(f'{name}.changed.tsv line {i} is not an offset and a length ({line!r})') from None
        if n <= 0 or off % 4 or n % 4: raise Refused(f'{name}.changed.tsv line {i} is not whole instructions ({off:#x}, {n:#x})')
        if off < end: raise Refused(f'{name}.changed.tsv line {i} starts at {off:#x}, before the end of the run above it ({end:#x})')
        runs.append((off, n)); end = off + n
    if not bid: raise Refused(f'{name}.changed.tsv has no BUILD_ID line: the build its runs belong to is unknown')
    return bid, runs


def orig_bytes(mod, table, patched, name='main'):
    """The content of <name>.orig.bin: mod (nso.Module, the game's own module) at the runs of table (the text of <name>.changed.tsv), after checking that
    patched (the bytes of <name>.patched.bin, at least its code) is made from this module. Refused otherwise"""
    bid, runs = read_table(table, name)
    have = mod.build_id[:0x10].hex()
    if bid != have: raise Refused(f'the pack\'s {name} is of build {bid}, the game\'s file is {have}: another version of the game than the pack was built from')
    text_end = mod.text_end
    if runs and runs[-1][0] + runs[-1][1] > text_end:
        raise Refused(f'{name}.changed.tsv names {runs[-1][0]:#x}+{runs[-1][1]:#x}, past the end of the code ({text_end:#x})')
    if len(patched) < text_end: raise Refused(f'{name}.patched.bin has {len(patched):#x} bytes, less than the code ({text_end:#x}): the pack is damaged')
    text, at, out = bytes(mod.image[:text_end]), 0, bytearray()
    for off, n in runs + [(text_end, 0)]:
        if patched[at:off] != text[at:off]:
            bad = next(o for o in range(at, off) if patched[o] != text[o])
            raise Refused(f'{name}.patched.bin differs from the game\'s file at {bad & ~3:#x}, outside the runs of {name}.changed.tsv: '
                          'the pack\'s code is not made from this file (a mod\'s module, or a damaged pack)')
        out += text[off:off + n]; at = off + n
    return bytes(out)


def fill(pack, name, module, label=None):
    """Writes <pack>/<name>.orig.bin from module (the bytes of the game's NSO) and returns its length; None when the file is there already (nothing is
    written). Refused, with nothing written, when the pack has no <name>.changed.tsv or <name>.patched.bin, the module does not load, or orig_bytes refuses"""
    pack = Path(pack); dst = pack / f'{name}.orig.bin'
    if dst.is_file(): return None
    table, patched = pack / f'{name}.changed.tsv', pack / f'{name}.patched.bin'
    if not table.is_file(): raise Refused(f'the pack has no {name}.changed.tsv (made before the rewriter listed what it changed): only a rebuild makes {dst.name}')
    if not patched.is_file(): raise Refused(f'the pack has no {name}.patched.bin')
    try: mod = nso.load(module, label or name)
    except nso.Error as e: raise Refused(str(e)) from None
    with open(patched, 'rb') as f: code = f.read(mod.text_end)
    buf = orig_bytes(mod, table.read_text(encoding='utf-8', errors='replace'), code, name)
    tmp = pack / (name + PART)
    try:
        with open(tmp, 'wb') as f:
            f.write(buf); f.flush(); os.fsync(f.fileno())
        os.replace(tmp, dst)
    except OSError as e:
        tmp.unlink(missing_ok=True)
        raise Refused(f'{dst.name} cannot be written ({e})') from None
    return len(buf)


def pack_rows(pack):
    """pack.tsv's rows as (key, value) pairs; none when it has no pack.tsv"""
    try: lines = (Path(pack) / 'pack.tsv').read_text(encoding='utf-8', errors='replace').splitlines()
    except OSError: return []
    return [tuple(c.strip() for c in line.partition('\t')[::2]) for line in lines if '\t' in line]


def candidates(game, name, version):
    """The files that may be the game's module <name>, as (label, read) pairs with read() → its bytes: game itself when it is a file that is not a NUP file,
    <game>/<name> for an exefs folder, else each version of a dump folder or a NUP file (scripts/pack_dump.py scan), the pack's own first, then the newest"""
    import pack_dump
    game = Path(game)
    if game.is_file() and not pack_dump.nup.is_nup(game): return [(str(game), game.read_bytes)]
    if (game / name).is_file() and not (game / 'exefs').is_dir(): return [(str(game / name), (game / name).read_bytes)]
    try: _, vers, _ = pack_dump.scan(game)
    except (pack_dump.Error, OSError, ValueError) as e: raise Refused(f'{game} is not a game this tool reads ({e})') from None
    out = []
    for v in sorted({0, *vers}, key=lambda v: (v != version, -v)):
        try: f = pack_dump.version_part(game, vers, v, 'exefs') / name
        except (pack_dump.Error, OSError, ValueError): continue
        if not any(str(f) == l for l, _ in out): out.append((str(f), f.read_bytes))
    return out


def find_module(game, name, table_bid, version):
    """(the bytes, the label) of the game's module <name> of build table_bid (32 hex digits); when no version of the game is that build, the first one that
    reads (fill then names both builds). Refused when the game has no such file"""
    first, why = None, None
    for label, read in candidates(game, name, version):
        try: d = read()
        except (OSError, ValueError) as e: why = f'{label}: {e}'; continue
        if d[:4] != b'NSO0' or len(d) < 0x60: why = f'{label} is not an NSO'; continue
        if bytes(d[0x40:0x50]).hex() == table_bid: return d, label
        first = first or (d, label)
    if first: return first
    raise Refused(why or f'{game} has no exefs/{name}')


def fill_pack(pack, game, names=('main',), say=print):
    """fill for each named module of the pack from the game (a .nup, a dump folder, an exefs folder or the module's file), saying one line each.
    Returns the number of modules whose file is not there afterwards"""
    pack, missing = Path(pack), 0
    rows = pack_rows(pack)
    version = next((int(v) for k, v in rows if k == 'version' and v.isdigit()), 0)
    if 'all' in names: names = next((v.split() for k, v in rows if k == 'modules'), ['main'])
    for name in names:
        dst = pack / f'{name}.orig.bin'
        if dst.is_file(): say(f'{name}: {dst.name} is there ({dst.stat().st_size} bytes): nothing to do'); continue
        try:
            table = pack / f'{name}.changed.tsv'
            if not table.is_file(): raise Refused(f'the pack has no {name}.changed.tsv (made before the rewriter listed what it changed): only a rebuild makes {dst.name}')
            bid, runs = read_table(table.read_text(encoding='utf-8', errors='replace'), name)
            module, label = find_module(game, name, bid, version)
            n = fill(pack, name, module, label)
            say(f'{name}: {dst.name} made from {label} ({len(runs)} rewritten runs, {n} bytes)')
        except Refused as e:
            missing += 1
            say(f'[RED] {name}: {dst.name} not made: {e}')
    return missing


def main():
    ap = argparse.ArgumentParser(description='Adds <module>.orig.bin (the game\'s own bytes of the code the rewriter changed) to a pack made before the rewriter wrote it, '
                                             'from the game\'s file and the pack\'s <module>.changed.tsv, without a rebuild.')
    ap.add_argument('pack', type=Path, help='the pack folder (<titleID>-work/android-pack)')
    ap.add_argument('game', type=Path, help='the game: a .nup file, a dump folder, an exefs folder, or the module\'s own file')
    ap.add_argument('--module', action='append', default=[], metavar='NAME', help='the module (default main; all: every module of the pack); repeatable')
    a = ap.parse_args()
    if not a.pack.is_dir(): sys.exit(f'[RED] {a.pack} is not a folder')
    sys.exit(1 if fill_pack(a.pack, a.game, a.module or ['main']) else 0)


if __name__ == '__main__':
    main()
