#!/bin/sh
# Kiwi Native's builder on macOS: .nsp / .xci (or a .nup, or a dump folder) -> .nup -> pack -> (with --push) the phone.
# Runs kiwi_build.py with the packer's Python (scripts/kiwinative.command python: set up on first use, see that file).
# Usage: kiwi/scripts/kiwi-build.command <input>... [options] [-- pack_android.py options]   (see kiwi/docs/BUILDER.md)
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
exec "$REPO/scripts/kiwinative.command" python "$REPO/kiwi/scripts/kiwi_build.py" "$@"
