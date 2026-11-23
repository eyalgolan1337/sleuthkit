#!/usr/bin/env bash
# The Host-side run of a Strict check (test/vm/README.md, "Gate 3: Strict
# checks"), inside the one-off container that test/vm/collect-vm.sh starts
# from the devcontainer image with the repository and the VM directory
# mounted read-only:
#
#   test/vm/host-run.sh OUT PARTITION IMAGE [READ-ONLY-DIR...]
#
# First it tries to write to IMAGE (an open for appending, which writes no
# data) and to create a file in each READ-ONLY-DIR. Each attempt must fail:
# it prints "read-only: PATH: <error>" for each one that does, and
# "WRITABLE: PATH" for each one that doesn't. If anything is writable, it
# stops there. Otherwise it runs the harness of the normal devcontainer build
# (build/vm-tools/tsk-harness) on PARTITION of IMAGE, opened read-only, with
# the Manifest in OUT.
#
# Exit status: the harness's (0 complete, 1 incomplete, 2 error), 3 if
# something was writable, 127 if the harness isn't built.
set -uo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
HARNESS=$REPO/build/vm-tools/tsk-harness

if (($# < 3)); then
    sed -n '2,18p' "$0" >&2
    exit 2
fi
out=$1 partition=$2 image=$3
shift 3

writable=0
# probe PATH ACTION...: ACTION is the write attempt; it must fail.
probe() {
    local path=$1 err
    shift
    if err=$("$@" 2>&1); then
        echo "WRITABLE: $path"
        writable=1
    else
        # The shell's message ends with the error, e.g. "Read-only file system".
        echo "read-only: $path: ${err##*: }"
    fi
}
append_open() { LC_ALL=C bash -c ': >>"$1"' _ "$1"; }
create_in() {
    local f
    f=$(LC_ALL=C mktemp -p "$1" .tsk-validate-write-probe.XXXXXX) || return 1
    rm -f "$f"
}

# A path that isn't there (a missing mount) would fail its write probe too.
[[ -f $image ]] || { echo "host-run.sh: no image at $image" >&2; exit 2; }
for d in "$@"; do
    [[ -d $d ]] || { echo "host-run.sh: no directory $d (not mounted?)" >&2; exit 2; }
done
probe "$image" append_open "$image"
for d in "$@"; do
    probe "$d" create_in "$d"
done
if ((writable)); then
    echo "host-run.sh: refusing to run: the paths above must be read-only (mounted :ro)" >&2
    exit 3
fi

[[ -x $HARNESS ]] || {
    echo "host-run.sh: $HARNESS is not built (test/vm/host-build.sh)" >&2
    exit 127
}
exec "$HARNESS" --partition "$partition" -o "$out" "$image"
