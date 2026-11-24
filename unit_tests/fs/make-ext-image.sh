#!/bin/sh
# Builds the small ext4 image that the Manifest-comparison helper's tests walk
# (manifest_check_test.cpp, which holds its expected Manifest). Every field the
# comparison step compares is fixed, so the image reads the same on every run
# and every machine. Needs mkfs.ext4 and debugfs (e2fsprogs); no root and no
# mounting.
#
# Usage: make-ext-image.sh OUTPUT
set -eu

out=$1
work=$out.tmp.d
rm -rf "$work" "$out.tmp"
mkdir -p "$work/tree/sub"
printf 'hello\n' >"$work/tree/sub/hello.txt"

# mkfs.ext4 stamps the root and /lost+found with E2FSPROGS_FAKE_TIME. It copies
# the rest of the tree from the source, ctime and owner included, so debugfs
# sets those fields afterwards.
E2FSPROGS_FAKE_TIME=1700000000 mkfs.ext4 -q -F -b 1024 -I 256 -N 16 \
    -O ^has_journal -E root_owner=0:0 -d "$work/tree" "$out.tmp" 1024

debugfs -w -f - "$out.tmp" >/dev/null 2>"$work/debugfs.err" <<'CMDS'
sif /sub mode 040750
sif /sub uid 1000
sif /sub gid 100
sif /sub atime @1500000000
sif /sub mtime @1500000001
sif /sub ctime @1500000002
sif /sub crtime @1500000003
sif /sub/hello.txt mode 0100640
sif /sub/hello.txt uid 1001
sif /sub/hello.txt gid 100
sif /sub/hello.txt atime @1600000000
sif /sub/hello.txt mtime @1600000001
sif /sub/hello.txt ctime @1600000002
sif /sub/hello.txt crtime @1600000003
CMDS
# debugfs exits 0 even when a command fails; anything on standard error but
# its version banner is a failure.
if grep -v '^debugfs [0-9]' "$work/debugfs.err" >&2; then
    echo "make-ext-image.sh: debugfs failed" >&2
    exit 1
fi

mv "$out.tmp" "$out"
rm -rf "$work"
