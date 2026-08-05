#!/usr/bin/env python3
"""Zip a staged release directory, deterministically.

Usage: tools/mkzip.py <stage-dir> <out.zip>

Two things this does that `zip -r` does not do for us:

  - Fixed timestamps and sorted entries, so the same source tree always
    produces a byte-identical archive. A release asset that changes on every
    rebuild cannot be checked against the tag it claims to come from.

  - Explicit unix permission bits. The device needs launch.sh and hypr to
    arrive executable; OnionOS will not run them otherwise, and that failure
    looks like the app being broken rather than the archive being wrong.
"""
import os
import stat
import sys
import zipfile

# The zip format cannot store anything earlier than 1980.
FIXED_DATE = (1980, 1, 1, 0, 0, 0)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip())
    stage, out = sys.argv[1], sys.argv[2]

    if not os.path.isdir(stage):
        sys.exit(f"not a directory: {stage}")

    # Sorted so entry order is a function of the tree, not of readdir order.
    paths = []
    for root, dirs, files in os.walk(stage):
        dirs.sort()
        for name in sorted(dirs) + sorted(files):
            paths.append(os.path.join(root, name))
    paths.sort()

    parent = os.path.dirname(os.path.abspath(stage))
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for path in paths:
            arcname = os.path.relpath(path, parent)
            isdir = os.path.isdir(path)
            info = zipfile.ZipInfo(arcname + ("/" if isdir else ""), FIXED_DATE)

            mode = os.stat(path).st_mode
            executable = bool(mode & stat.S_IXUSR)
            if isdir:
                perm = 0o755
            else:
                perm = 0o755 if executable else 0o644
            info.external_attr = (perm << 16) | (0x10 if isdir else 0)
            info.compress_type = zipfile.ZIP_DEFLATED

            if isdir:
                z.writestr(info, b"")
            else:
                with open(path, "rb") as f:
                    z.writestr(info, f.read())

    print(f"wrote {out}")


if __name__ == "__main__":
    main()
