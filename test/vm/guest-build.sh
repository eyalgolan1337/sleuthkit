#!/usr/bin/env bash
# The static guest build (test/vm/README.md, "The static guest build"): the
# library, fls, icat, fsstat, istat, the harness and Ground truth capture,
# fully statically linked so that they run in the Validation VM, whose glibc
# (AlmaLinux 9: 2.34) is older than the devcontainer's. Run in the
# devcontainer, from anywhere:
#
#   test/vm/guest-build.sh            # incremental
#   test/vm/guest-build.sh --fresh    # start over: new copy, bootstrap, configure
#
# It works on a copy of the working tree (tracked files and untracked files
# that .gitignore doesn't exclude) in build/guest-static/src, configured
# static-only and without the optional image-format libraries. The binaries
# end up in build/guest-static/bin. Exit status 0 if all of them were built
# and are statically linked.
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=$REPO/build/guest-static
SRC=$OUT/src
BIN=$OUT/bin
JOBS=${JOBS:-$(nproc)}
# The same flags as the devcontainer build (.devcontainer/build-compile-db.sh):
# without -fno-strict-aliasing, GCC 16.1 breaks file system autodetection.
BUILD_FLAGS="-g -O2 -fno-strict-aliasing"
TOOLS=(fls icat fsstat istat)

case ${1:-} in
--fresh) rm -rf "$OUT" ;;
'') ;;
*)
    sed -n '2,15p' "$0" >&2
    exit 2
    ;;
esac

# A failed build must not leave the previous binaries for the guest to run.
rm -rf "$BIN"
echo "== copy of the working tree"
python3 "$REPO/test/vm/lib/sync_tree.py" "$REPO" "$SRC"
cd "$SRC"

if [[ ! -f config.status ]]; then
    echo "== bootstrap and configure (static only, no image-format libraries)"
    ./bootstrap
    ./configure --disable-shared --enable-static --disable-java \
        --without-afflib --without-libewf --without-libvhdi --without-libvmdk \
        --without-libbfio --without-libvslvm \
        CFLAGS="$BUILD_FLAGS" CXXFLAGS="$BUILD_FLAGS"
fi

echo "== library and tools"
make -j"$JOBS" -C tsk
# -all-static: libtool links everything statically, glibc and libstdc++
# included, not only the libtool libraries (the tools' own LDFLAGS = -static).
make -j"$JOBS" -C tools/fstools LDFLAGS=-all-static "${TOOLS[@]}"

echo "== harness and Ground truth capture"
make -j"$JOBS" -f test/vm/manifest/manifest.mk BUILD_DIR="$OUT/vm-tools" \
    harness-static capture-static corpus-static

mkdir -p "$BIN"
bad=0
for f in "${TOOLS[@]/#/tools/fstools/}" "$OUT/vm-tools/static/tsk-harness" \
    "$OUT/vm-tools/static/capture-ground-truth" "$OUT/vm-tools/static/tsk-corpus"; do
    # Stripped: the guest copy is sent over SSH on every run.
    strip --strip-debug -o "$BIN/${f##*/}" "$f"
    desc=$(file -b "$BIN/${f##*/}")
    if [[ $desc != *'statically linked'* ]]; then
        echo "error: ${f##*/} is not statically linked: $desc" >&2
        bad=1
    fi
done
ls -l "$BIN"
exit "$bad"
