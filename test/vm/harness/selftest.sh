#!/usr/bin/env bash
# End-to-end self-test of the harness and Ground truth capture
# (test/vm/README.md, "Harness self-test"). Run in the devcontainer from the
# repository root after `make -f test/vm/manifest/manifest.mk all`:
#
#   test/vm/harness/selftest.sh
#
# It builds a directory tree with awkward entries, captures its Ground truth,
# turns it into an ext4 file system (mkfs.ext4 -d) inside a GPT disk image,
# converts that to .vmdk, and checks that:
#   1. the harness writes the same Manifest for the raw disk with the
#      partition found through the volume system, the raw disk with an
#      explicit byte offset, the .vmdk, and the bare file system image;
#   2. that Manifest matches Ground truth in a Strict comparison, apart from
#      the differences listed in expected-differences.txt;
#   3. a statically linked Ground truth capture writes the same Manifest;
#   4. the harness was compiled against installed libtsk headers only;
#   5. test/vm/host-run.sh (the Host-side run in its one-off container)
#      refuses a writable image or directory and otherwise writes the
#      harness's Manifest.
#
# Each failure is reported as a "fail" item to $VALIDATE_ITEMS when set (the
# validate command's report protocol). Exit status 0 if all checks pass.
set -uo pipefail

REPO=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$REPO" || exit 2
HERE=test/vm/harness
TOOLS=build/vm-tools
WORK=build/harness-selftest
PATH=$PATH:/usr/sbin:/sbin # mkfs.ext4 and sfdisk

failures=0
item() { # item fail|note TEXT
    # One protocol line per item, as test/vm/lib/report.sh writes them: tool
    # output quoted in TEXT may contain newlines and tabs.
    local text=${2//$'\n'/ }
    text=${text//$'\t'/ }
    printf '%s: %s\n' "$1" "$text"
    if [[ -n ${VALIDATE_ITEMS:-} ]]; then
        printf '%s\t%s\n' "$1" "$text" >>"$VALIDATE_ITEMS"
    fi
}
fail() {
    item fail "harness self-test: $1"
    failures=$((failures + 1))
}
note() { item note "harness self-test: $1"; }

for tool in mkfs.ext4 sfdisk qemu-img python3; do
    if ! command -v "$tool" >/dev/null; then
        fail "$tool is missing (it comes with the devcontainer image; rebuild it)"
        exit 1
    fi
done
for prog in tsk-harness capture-ground-truth compare-manifests; do
    if [[ ! -x $TOOLS/$prog ]]; then
        fail "$TOOLS/$prog is missing; run make -f test/vm/manifest/manifest.mk all"
        exit 1
    fi
done

# An interrupted run may have left the read-only directory of the host-run.sh
# checks behind.
[[ -d $WORK ]] && chmod -R u+w "$WORK"
rm -rf "$WORK"
mkdir -p "$WORK"
TREE=$WORK/tree

# The tree: every kind of entry the Manifest format and ext4 (as written by
# mkfs.ext4 -d) can hold, except device nodes (they need root). Content is
# deterministic. mkfs.ext4 -d (e2fsprogs 1.47) stores whole seconds only and
# clamps times after 2038 to 0x7fffffff, so every entry gets a whole-second
# mtime it can store; nanoseconds and times after 2038 are left to the
# Validation VM, whose file systems the kernel writes.
make_tree() {
    local t=$1
    mkdir -p "$t/dir/sub/deeper" "$t/empty-dir" "$t/sticky"
    printf 'abc' >"$t/abc"
    : >"$t/empty"
    # Several blocks, and more than the 4 KiB of one block.
    python3 -c 'import sys; sys.stdout.buffer.write(bytes((i * 131 + 7) % 251 for i in range(300000)))' >"$t/dir/multi-block"
    printf 'deep\n' >"$t/dir/sub/deeper/file"
    ln "$t/abc" "$t/dir/hard-link-to-abc"
    ln "$t/abc" "$t/dir/sub/second-hard-link"
    ln -s abc "$t/short-symlink"
    ln -s "$(printf 'long/%.0s' {1..30})target" "$t/long-symlink" # > 60 bytes: not inline
    ln -s ../../abc "$t/dir/sub/relative-symlink"
    ln -s nowhere "$t/dangling-symlink"
    printf 'space' >"$t/with space"
    printf 'tab' >"$t/tab$(printf '\t')here"
    printf 'newline' >"$t/new
line"
    printf 'latin1' >"$t/latin1-$(printf '\351')"
    printf 'utf8' >"$t/utf8-é"
    printf 'dash' >"$t/-"
    printf 'percent' >"$t/%"
    printf 'name255' >"$t/$(printf 'n%.0s' {1..255})"
    mkfifo "$t/fifo"
    python3 -c 'import socket, sys; socket.socket(socket.AF_UNIX).bind(sys.argv[1])' "$t/socket"
    chmod 4755 "$t/abc"
    chmod 1777 "$t/sticky"
    chmod 0700 "$t/empty-dir"
    chmod 0640 "$t/empty"

    # Touching an entry changes only its own times, so one pass sets them
    # all, then a few get special values.
    find "$t" -depth -exec touch -h -d @1000000000 {} +
    touch -d @1700000000 "$t/dir/multi-block"
    touch -d @-1500000000 "$t/empty"   # before 1970
    touch -d @2147483647 "$t/abc"      # the last second of 32-bit time
    touch -h -d @1600000000 "$t/short-symlink"
}

if ! make_tree "$TREE" 2>"$WORK/make-tree.err"; then
    fail "could not build the test tree: $(head -c 300 "$WORK/make-tree.err")"
    exit 1
fi

# Ground truth of the tree, then the same with a statically linked capture.
gt=$WORK/ground-truth.manifest
if ! "$TOOLS/capture-ground-truth" -o "$gt" "$TREE"; then
    fail "capture-ground-truth failed on the test tree"
    exit 1
fi
note "Ground truth: $(($(wc -l <"$gt") - 1)) entries"
if ! "$TOOLS/compare-manifests" --ground-truth "$gt" --run "$gt" --mode strict >"$WORK/gt-self.txt"; then
    fail "Ground truth doesn't read back or doesn't match itself; see $WORK/gt-self.txt"
fi

if make -s -f test/vm/manifest/manifest.mk capture-static >"$WORK/static-build.log" 2>&1; then
    static=$TOOLS/static/capture-ground-truth
    if ! file "$static" | grep -q 'statically linked'; then
        fail "$static is not statically linked: $(file -b "$static")"
    elif ! "$static" -o "$WORK/ground-truth-static.manifest" "$TREE" ||
        ! "$TOOLS/compare-manifests" --ground-truth "$gt" --mode strict \
            --run "$WORK/ground-truth-static.manifest" >"$WORK/static-compare.txt"; then
        # Strict, not byte for byte: the first capture's readdir updated the
        # directories' atime, which a Manifest records but never compares.
        fail "the statically linked capture-ground-truth writes a different Manifest"
    else
        note "statically linked capture-ground-truth builds and writes the same Manifest"
    fi
else
    fail "could not link capture-ground-truth statically; see $WORK/static-build.log"
fi

# The file system, the disk image and the .vmdk.
fs=$WORK/fs.img
disk=$WORK/disk.img
vmdk=$WORK/disk.vmdk
FS_MIB=16
if ! mkfs.ext4 -q -F -t ext4 -I 256 -L tsk-selftest -E root_owner="$(id -u):$(id -g)" \
    -d "$TREE" "$fs" "${FS_MIB}M" 2>"$WORK/mkfs.err"; then
    fail "mkfs.ext4 -d failed: $(head -c 300 "$WORK/mkfs.err")"
    exit 1
fi
truncate -s $((FS_MIB + 2))M "$disk"
printf 'label: gpt\nstart=2048, size=%d, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4\n' \
    $((FS_MIB * 2048)) | sfdisk -q "$disk" >/dev/null 2>"$WORK/sfdisk.err" ||
    { fail "sfdisk failed: $(head -c 300 "$WORK/sfdisk.err")"; exit 1; }
dd if="$fs" of="$disk" bs=1M seek=1 conv=notrunc status=none
if ! qemu-img convert -f raw -O vmdk "$disk" "$vmdk" 2>"$WORK/qemu-img.err"; then
    fail "qemu-img convert failed: $(head -c 300 "$WORK/qemu-img.err")"
    exit 1
fi
note "$(qemu-img info "$vmdk" | sed -n 's/^ *create type: //p') .vmdk made with qemu-img"

# The harness, four ways.
declare -A runs=(
    [partition]="--partition 1 $disk"
    [offset]="--offset 1048576 $disk"
    [vmdk]="--partition 1 $vmdk"
    [fs-image]="$fs"
)
for name in partition offset vmdk fs-image; do
    # shellcheck disable=SC2086 # the arguments are split on purpose
    if ! "$TOOLS/tsk-harness" -o "$WORK/harness-$name.manifest" ${runs[$name]} \
        2>"$WORK/harness-$name.err"; then
        fail "tsk-harness ${runs[$name]} failed: $(head -c 300 "$WORK/harness-$name.err")"
    fi
done
for name in offset vmdk fs-image; do
    if ! cmp -s "$WORK/harness-partition.manifest" "$WORK/harness-$name.manifest"; then
        fail "the harness Manifest of '${runs[$name]}' differs from '${runs[partition]}'"
    fi
done
note "harness: $(($(wc -l <"$WORK/harness-partition.manifest") - 1)) entries"

# The Host-side run's in-container script (test/vm/host-run.sh). In the real
# run the image and the directories are read-only Docker mounts; here they are
# read-only by permission (the devcontainer runs as an ordinary user).
host_run_selftest() {
    local ro=$WORK/host-run-ro rw=$WORK/host-run-rw out=$WORK/host-run-out rc
    if ((EUID == 0)); then
        note "host-run.sh: skipped as root (permissions don't stop root writing)"
        return
    fi
    rm -rf "$ro" "$rw" "$out"
    mkdir -p "$ro" "$rw" "$out"
    cp "$vmdk" "$ro/disk.vmdk"
    cp "$vmdk" "$rw/disk.vmdk"
    chmod a-w "$ro/disk.vmdk" "$ro"

    # Everything read-only: same Manifest as the harness on the .vmdk directly.
    test/vm/host-run.sh "$out/ro.manifest" 1 "$ro/disk.vmdk" "$ro" >"$out/ro.log" 2>&1
    rc=$?
    if ((rc != 0)); then
        fail "host-run.sh on a read-only image exited $rc: $(tail -n1 "$out/ro.log")"
    elif ! cmp -s "$WORK/harness-vmdk.manifest" "$out/ro.manifest"; then
        fail "host-run.sh writes a different Manifest than tsk-harness --partition 1 on the same .vmdk"
    elif ! grep -q "^read-only: $ro/disk.vmdk: " "$out/ro.log" || ! grep -q "^read-only: $ro: " "$out/ro.log"; then
        fail "host-run.sh doesn't report its read-only probe of the image and the directory: $(head -c 300 "$out/ro.log")"
    else
        note "host-run.sh: read-only image and directory accepted, same Manifest as the harness"
    fi

    # A writable image: refused before the harness runs.
    test/vm/host-run.sh "$out/rw-image.manifest" 1 "$rw/disk.vmdk" "$ro" >"$out/rw-image.log" 2>&1
    rc=$?
    if ((rc != 3)) || [[ -e $out/rw-image.manifest ]] || ! grep -q "^WRITABLE: $rw/disk.vmdk\$" "$out/rw-image.log"; then
        fail "host-run.sh on a writable image: expected exit 3, a WRITABLE line and no Manifest; got exit $rc: $(head -c 300 "$out/rw-image.log")"
    else
        note "host-run.sh: writable image refused"
    fi

    # A writable directory: refused too, and the probe leaves nothing behind.
    test/vm/host-run.sh "$out/rw-dir.manifest" 1 "$ro/disk.vmdk" "$ro" "$rw" >"$out/rw-dir.log" 2>&1
    rc=$?
    if ((rc != 3)) || [[ -e $out/rw-dir.manifest ]] || ! grep -q "^WRITABLE: $rw\$" "$out/rw-dir.log"; then
        fail "host-run.sh with a writable directory: expected exit 3, a WRITABLE line and no Manifest; got exit $rc: $(head -c 300 "$out/rw-dir.log")"
    elif [[ $(find "$rw" -mindepth 1 | wc -l) -ne 1 ]]; then
        fail "host-run.sh's write probe left files in $rw"
    else
        note "host-run.sh: writable directory refused, probe file removed"
    fi
    chmod u+w "$ro"
}
host_run_selftest

# The harness against Ground truth.
if "$TOOLS/compare-manifests" --ground-truth "$gt" --run "$WORK/harness-partition.manifest" \
    --mode strict --known-failures "$HERE/expected-differences.txt" \
    --check harness-selftest --partition ext4 >"$WORK/compare.txt"; then
    note "harness Manifest matches Ground truth (expected differences: $(grep -c '^KNOWN' "$WORK/compare.txt"))"
else
    fail "harness Manifest differs from Ground truth: $(grep -m3 '^FAIL' "$WORK/compare.txt" | tr '\n' ' ')(all in $WORK/compare.txt)"
fi

# Installed headers only: every libtsk header the harness's objects include
# (their dependency files) must come from the staged install.
dep_headers=$(cat "$TOOLS"/.deps/harness*.Po | tr ' \\' '\n\n' | grep -E '\.(h|hpp)$' | sort -u)
stage_count=$(grep -c "/$TOOLS/tsk-stage/" <<<"$dep_headers")
source_headers=$(grep -E "^$REPO/tsk/" <<<"$dep_headers")
if [[ -n $source_headers ]]; then
    fail "the harness includes libtsk headers from the source tree: $(head -3 <<<"$source_headers" | tr '\n' ' ')"
elif ((stage_count == 0)); then
    fail "no installed libtsk header found in the harness's dependency files"
else
    note "harness compiled against installed headers only ($stage_count headers from the staged install, none from tsk/)"
fi

exit $((failures > 0))
