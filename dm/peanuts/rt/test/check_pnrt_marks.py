#!/usr/bin/env python3
"""Checks the rule of this port: a lifted file is byte-identical to the emulator's file except
where a comment containing PNRT says what was changed and why.

usage: check_pnrt_marks.py <emulator source tree> [--list] [--show]

<emulator source tree> is the checkout of the release the lift was taken from (the folder that
holds src/core/mods). Every file under rt/mods that has a counterpart in <tree>/src/core/mods is
compared, and so are the headers under rt/shim and the sources under third_party/astc that PORT.md
calls verbatim copies (VERBATIM below, compared with <tree>/src/<same path>).

A difference is one hunk of `diff -U0`. A hunk passes when PNRT appears in one of its own lines
(added or removed) or in the line just above or just below it in this tree's file. Anything else
fails. Files of the emulator's src/core/mods that are not lifted, and files here without a
counterpart, are listed and must be among the expected ones (NOT_LIFTED, OWN).

  --list   print one line per edited file with its hunk count (the table for PORT.md)
  --show   print every hunk that fails

Exit status: 0 when every hunk is marked and the file sets are as expected, 1 otherwise.
"""
import difflib
import pathlib
import sys

MARK = "PNRT"
RT = pathlib.Path(__file__).resolve().parent.parent
# Files of src/core/mods that stay out by design (PORT.md, "What is not lifted").
NOT_LIFTED = {"mod_guest_bridge.cpp", "mod_re_tools.cpp", "mod_console.cpp", "duo_core.cmake",
              "modules/README.md"}
# Files under rt/mods that are this port's own.
OWN = {"pnrt_stubs.cpp"}
# (path in this tree, path under <tree>/src) of the copies kept outside rt/mods.
VERBATIM = [
    (RT / "shim/video_core/dsmod/aux_routing.h", "video_core/dsmod/aux_routing.h"),
    (RT / "shim/video_core/dsmod/texture_readback.h", "video_core/dsmod/texture_readback.h"),
    (RT / "shim/common/dsmod_dev_tools.h", "common/dsmod_dev_tools.h"),
    (RT / "shim/common/device_power_state.h", "common/device_power_state.h"),
    (RT / "shim/hid_core/resources/npad/dsmod_pad_gate.h",
     "hid_core/resources/npad/dsmod_pad_gate.h"),
    (RT.parent / "third_party/astc/video_core/textures/astc.cpp", "video_core/textures/astc.cpp"),
    (RT.parent / "third_party/astc/video_core/textures/astc.h", "video_core/textures/astc.h"),
]


def hunks(theirs, ours):
    """Yields (first line in ours, removed lines, added lines, line above, line below)."""
    sm = difflib.SequenceMatcher(None, theirs, ours, autojunk=False)
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            continue
        above = ours[j1 - 1] if j1 > 0 else ""
        below = ours[j2] if j2 < len(ours) else ""
        yield j1 + 1, theirs[i1:i2], ours[j1:j2], above, below


def check(ours_path, theirs_path, show):
    ours = ours_path.read_text(encoding="utf-8", errors="surrogateescape").splitlines()
    theirs = theirs_path.read_text(encoding="utf-8", errors="surrogateescape").splitlines()
    total = bad = 0
    for line, removed, added, above, below in hunks(theirs, ours):
        total += 1
        if any(MARK in s for s in removed + added + [above, below]):
            continue
        bad += 1
        if show:
            print(f"  UNMARKED {ours_path.name}:{line}")
            for s in removed:
                print("    - " + s)
            for s in added:
                print("    + " + s)
    return total, bad


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 1:
        sys.stderr.write(__doc__)
        return 2
    listing = "--list" in sys.argv
    show = "--show" in sys.argv
    src = pathlib.Path(args[0]) / "src"
    mods_theirs = src / "core/mods"
    mods_ours = RT / "mods"
    if not mods_theirs.is_dir():
        sys.stderr.write(f"check_pnrt_marks: {mods_theirs} is not a folder\n")
        return 2

    failed = False
    pairs = []
    ours_names = {p.relative_to(mods_ours).as_posix() for p in mods_ours.rglob("*") if p.is_file()}
    theirs_names = {p.relative_to(mods_theirs).as_posix()
                    for p in mods_theirs.rglob("*") if p.is_file()}
    for name in sorted(ours_names - theirs_names - OWN):
        print(f"UNEXPECTED: mods/{name} has no counterpart in the emulator's tree")
        failed = True
    for name in sorted(theirs_names - ours_names - NOT_LIFTED):
        print(f"MISSING: src/core/mods/{name} is not lifted and not on the not-lifted list")
        failed = True
    for name in sorted(ours_names & theirs_names):
        pairs.append((mods_ours / name, mods_theirs / name))
    for ours_path, rel in VERBATIM:
        if not (src / rel).is_file() or not ours_path.is_file():
            print(f"MISSING: {rel} (one side of a verbatim copy)")
            failed = True
            continue
        pairs.append((ours_path, src / rel))

    identical = edited = hunks_total = hunks_bad = 0
    for ours_path, theirs_path in pairs:
        if ours_path.read_bytes() == theirs_path.read_bytes():
            identical += 1
            continue
        edited += 1
        total, bad = check(ours_path, theirs_path, show)
        hunks_total += total
        hunks_bad += bad
        if listing or bad:
            print(f"{ours_path.relative_to(RT.parent).as_posix()}: {total} hunks"
                  + (f", {bad} UNMARKED" if bad else ""))
    failed = failed or hunks_bad > 0
    print(f"check_pnrt_marks: {len(pairs)} files compared, {identical} identical, {edited} edited, "
          f"{hunks_total} hunks, {hunks_bad} without a {MARK} mark: "
          + ("FAILED" if failed else "OK"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
