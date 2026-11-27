# Pack upgrade: what an older pack gets without a rebuild

A pack is the built form of a game: its rewritten code, the rewriter's tables and the shader library. When the
app starts to need a file that older packs do not have, and that file can be made from what the pack and the
game already hold, the app makes it at the game's next start. Nothing is rebuilt and no other file of the pack
is changed.

There is one such file now. It was checked on a PC (the tests below, and a real pack of Link's Awakening from
2026-11-11: the same bytes from the app's Java and from the PC command); it has not been run on a device yet.

## main.orig.bin: the game's own bytes of the code the rewriter changed

The game runs with its code rewritten, so some words in memory differ from the game's file.
`main.changed.tsv` lists them as runs (offset and length). `main.orig.bin` holds the file's own bytes of those
runs, one run after the other in the table's order. The companion runtime needs it for two things
(`dm/peanuts/peanuts.c`, `rt_orig_load`):

- A companion that reads the game's code (to know the build, or to take an address from an instruction) gets
  the file's bytes in place of the rewritten ones.
- A companion with a load plan (a small helper put into the running game, so the companion can call the game's
  own functions) is only given one when the runtime can tell which code the rewriter changed. Without
  `main.orig.bin` every patch of the plan is refused, and the companion falls back to pressing buttons.

The rewriter has written the file since 2026-11-14 (`dm/tools/patch_nso.py`). A pack built before that has
`main.changed.tsv` and `main.patched.bin`, but no `main.orig.bin`.

### What the app does

At a game's start (`dev.peanuts.pack.PackUpgrade`, called from `dev.peanuts.Companion.start`, before the host
starts), when the pack has no `main.orig.bin`:

1. It makes `main.orig.bin.part`, empty, at once. While that file is there and less than a minute old, the
   companion runtime does not start (`rt_orig_pending`). The game itself is not held.
2. On a thread of its own it reads the game's `main` of the pack's version (the `.nup`'s, or the dump folder's
   `exefs/main`), decompresses its code, and checks that the pack's code is made from it:
   - the build ID in `main.changed.tsv` is the file's;
   - the code matches its SHA-256 in the file's header, where the header asks for that check;
   - outside the runs of `main.changed.tsv`, every byte of `main.patched.bin`'s code is the file's own.
3. When all three hold it writes the runs' bytes to `main.orig.bin.part`, renames it to `main.orig.bin`, and
   the runtime starts and reads it. The file is the one a rebuild of the same pack would write.

On a later start the file is there and nothing is done. The code is decompressed through a 64 KiB window and
never held whole; a `.nup`'s `main` is held as the `.nup` stores it while it is read, a dump folder's is read
as a stream. On a PC the same Java takes 0.13 s for Link's Awakening's `main` (30 MiB of rewritten code) in a
64 MiB heap. It has not been timed on a device yet.

When a check fails, nothing is written, `main.orig.bin.part` is removed, and the log says why. The usual
causes: the game's file is now another version than the pack was built from, the pack was built with a mod
that replaces or patches `main`, the pack is from before `main.changed.tsv`, or the game's file is gone. A
`main` stored in the newer compression (ZBIC) is not read here either. The game then runs as before, and the
game's Companion section shows a row that says what the companion lost and that a rebuild fixes it.

### The log (logcat tag `peanuts`)

First start of an older pack:

```
companion: main.orig.bin is being made for this pack: the runtime starts when it is there
pack 01008CF01BAAC000: main.orig.bin made from the game's file update/131072/exefs/main (41236 bytes of the code the rewriter changed, 840 ms): the pack was built before the app kept them; the companion runtime reads it at this start
companion: main's code is read as its file has it (7411 rewritten runs, 41236 bytes from main.orig.bin)
```

(The numbers are examples.) Later starts have only the last line. When the file cannot be made:

```
pack 01008CF01BAAC000: main.orig.bin not made: the pack's main is of build …, the game's file is …: another version of the game than the pack was built from. Companions read main's code as rewritten and a companion's direct actions (its load plan) stay off until the game is built again
companion: no main.orig.bin in the pack (7411 changed runs of main, 41236 bytes): the runtime reads main's code as rewritten; build the game again for the file's own bytes
```

A file made after the runtime had started already (the work took more than a minute, or the app could not
make the `.part` file) says "it is used from the next start".

A load plan can still be refused after the file is there: when the plan patches a place the rewriter itself
changed, the log says `the load plan patches main+0x…, which this host's rewriter changed or moved`. That is
a limit of the companion with this game's rewrite, not of the pack, and a rebuild does not change it.

### What it does not change

`main.orig.bin` is not part of what the app compares to decide whether a pack is up to date: that is each
module's `.android.key`, `pack.tsv` and the shader library's version (`PackCheck`, `Pack.libraryProblem`).
Adding the file changes none of them, so the pack keeps its state and no rebuild is offered because of it.
Other modules (`sdk.orig.bin`, `subsdk0.orig.bin`) are written by the rewriter too, but the runtime reads only
`main`'s, so the app adds only that one.

## On a computer

The same for a pack on a PC, before it is pushed to the device:

```
python3 peanuts/scripts/pack_origfill.py <pack folder> <game .nup or dump folder>
python3 peanuts/scripts/pack_origfill.py <pack folder> <game> --module all
```

It prints one line per module and exits with 1 when a file could not be made. `--module all` also makes the
other modules' files, as the rewriter writes them. `scripts/pack_android.py` does the same during a build that
keeps a module's code (`write_orig`), so this command is only for a pack that is not built again.

## Tests

`peanuts/tests/test_origfill.py`: a pack made by the rewriter itself gets the same `main.orig.bin` back after
it is taken away, from the Python and from the Java; both refuse the same cases with the same sentence and
leave nothing behind; the file is written through `.part` and never a second time.
