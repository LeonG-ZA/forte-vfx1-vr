#!/usr/bin/env python3
"""
pack-game.py -- zip a DOS game folder for js-dos.

  python web/pack-game.py C:/path/to/game-folder build/web/games/name.zip

js-dos mounts the zip's root as drive C:, so the folder's contents go at the
root of the archive with their directory structure kept.

Output belongs under build/, which git ignores. Game files are supplied by the
user and are never committed to this repository, even freely distributable
shareware: that is a rule of the project, not a judgement about any title.
"""

import os
import sys
import zipfile


def main():
    if len(sys.argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    src, out = sys.argv[1], sys.argv[2]
    if not os.path.isdir(src):
        print(f"not a folder: {src}", file=sys.stderr)
        return 2

    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    files = 0
    total = 0
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for root, _dirs, names in os.walk(src):
            for name in sorted(names):
                full = os.path.join(root, name)
                rel = os.path.relpath(full, src).replace(os.sep, "/")
                z.write(full, rel)
                files += 1
                total += os.path.getsize(full)
    print(f"{files} files, {total / 1e6:.1f} MB -> {out} "
          f"({os.path.getsize(out) / 1e6:.1f} MB compressed)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
