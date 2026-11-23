#!/usr/bin/env python3
"""Mirror the working tree's source files into a build copy.

    sync_tree.py REPO DEST

Copies every tracked file and every untracked file that .gitignore doesn't
exclude (git ls-files --cached --others --exclude-standard) from REPO to DEST.
A file is copied only when its content differs, and then gets the current time
as its modification time (the source's may be older than the objects built from
the previous version), so an incremental build in DEST rebuilds exactly what
changed. Symlinks are copied as symlinks. A file copied by the previous run that is no longer in the list is
removed from DEST; everything else in DEST (configure output, objects) is kept.

The list of copied files is kept in DEST/.synced-files. Prints a one-line
summary. Exit status 0, or 1 with a message on error.
"""

import os
import shutil
import subprocess
import sys

LIST = ".synced-files"


def same_file(src, dst):
    if os.path.islink(src) or os.path.islink(dst):
        return (os.path.islink(src) and os.path.islink(dst)
                and os.readlink(src) == os.readlink(dst))
    try:
        if os.path.getsize(src) != os.path.getsize(dst):
            return False
    except OSError:
        return False
    with open(src, "rb") as a, open(dst, "rb") as b:
        while True:
            x = a.read(1 << 20)
            if x != b.read(1 << 20):
                return False
            if not x:
                return True


def main():
    if len(sys.argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    repo, dest = (os.fsencode(p) for p in sys.argv[1:])
    out = subprocess.run(
        ["git", "-C", repo, "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
        check=True, stdout=subprocess.PIPE).stdout
    # A tracked file deleted in the working tree is still listed by --cached.
    files = sorted({f for f in out.split(b"\0")
                    if f and os.path.lexists(os.path.join(repo, f))})

    os.makedirs(dest, exist_ok=True)
    list_path = os.path.join(dest, LIST.encode())
    old = set()
    if os.path.exists(list_path):
        with open(list_path, "rb") as f:
            old = {p for p in f.read().split(b"\0") if p}

    copied = 0
    for rel in files:
        src = os.path.join(repo, rel)
        dst = os.path.join(dest, rel)
        if os.path.isdir(src) and not os.path.islink(src):
            continue  # a submodule or a nested repository: not part of the build
        if os.path.lexists(dst) and same_file(src, dst):
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if os.path.lexists(dst) or os.path.isdir(dst):
            if os.path.isdir(dst) and not os.path.islink(dst):
                shutil.rmtree(dst)
            else:
                os.unlink(dst)
        if os.path.islink(src):
            os.symlink(os.readlink(src), dst)
        else:
            shutil.copyfile(src, dst)
            shutil.copymode(src, dst)
        copied += 1

    removed = 0
    for rel in sorted(old - set(files)):
        dst = os.path.join(dest, rel)
        if os.path.lexists(dst) and not os.path.isdir(dst):
            os.unlink(dst)
            removed += 1

    tmp = list_path + b".tmp"
    with open(tmp, "wb") as f:
        f.write(b"\0".join(files))
    os.replace(tmp, list_path)
    print(f"synced {len(files)} files into {os.fsdecode(dest)}: "
          f"{copied} copied, {removed} removed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, subprocess.CalledProcessError) as e:
        print(f"sync_tree.py: {e}", file=sys.stderr)
        sys.exit(1)
