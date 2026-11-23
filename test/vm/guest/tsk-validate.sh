#!/bin/bash
# Guest-side helper of the validate command (test/vm/README.md, "Gate 3:
# Strict checks"). The host copies it, with the static guest build, to
# /root/tsk-validate in the Validation VM and runs one step per SSH call:
#
#   tsk-validate.sh tools                      run each static tool once (no glibc errors)
#   tsk-validate.sh check-mount DEV MNT        DEV is mounted on MNT
#   tsk-validate.sh corpus MNT                 (re)create the starter corpus in MNT/corpus
#   tsk-validate.sh freeze MNT SECONDS         sync, arm the unfreeze timer, fsfreeze -f
#   tsk-validate.sh unfreeze MNT               fsfreeze -u if frozen, disarm the timer
#   tsk-validate.sh capture MNT OUT            Ground truth of MNT into OUT
#   tsk-validate.sh flush DEV                  flush DEV's buffer cache, drop page caches
#   tsk-validate.sh run DEV OUT                Guest-side run: the harness on DEV into OUT
#   tsk-validate.sh break-locate MNT DEV PATH  test hook: where PATH's first byte is on DEV
#   tsk-validate.sh break                      test hook: flip that byte on DEV
#   tsk-validate.sh restore                    test hook: put the byte back
#
# The exit status is the step's: 0 on success. capture and run pass through
# the exit status of capture-ground-truth and tsk-harness (1: incomplete).
set -uo pipefail
PATH=/usr/sbin:/usr/bin:/sbin:/bin

HERE=$(cd "$(dirname "$0")" && pwd)
BIN=$HERE/bin
BREAK_STATE=$HERE/break.state

die() {
    echo "tsk-validate.sh: $*" >&2
    exit 1
}

# One systemd timer per mount point unfreezes it if the host never does
# (the host was killed, or lost the connection).
timer_unit() {
    local m=${1#/}
    echo "tsk-validate-unfreeze-${m//\//-}"
}

cmd_tools() {
    local t rc=0 out st
    for t in "$BIN"/*; do
        # -V for the TSK tools, --help for ours: either way the binary must
        # start, which it can't if it needs a newer glibc than the guest's.
        # 126 and above: not executable, not found, or killed by a signal.
        case ${t##*/} in
        tsk-harness | capture-ground-truth) out=$("$t" --help 2>&1) ;;
        *) out=$("$t" -V 2>&1) ;;
        esac
        st=$?
        if ((st >= 126)) || grep -q 'GLIBC_\|not found\|cannot execute' <<<"$out"; then
            echo "FAIL ${t##*/} (exit $st): $out"
            rc=1
        else
            echo "OK   ${t##*/}: $(head -n1 <<<"$out")"
        fi
    done
    return "$rc"
}

cmd_check_mount() {
    local dev=$1 mnt=$2 src
    src=$(findmnt -no SOURCE --mountpoint "$mnt") || die "nothing is mounted on $mnt"
    [[ $src == "$dev" ]] || die "$mnt is mounted from $src, not $dev"
}

# The starter corpus: a small, fixed tree that proves the path. The full Test
# corpus replaces it (#19). Times are fixed; ctime and crtime can't be set.
cmd_corpus() {
    local mnt=$1 c
    c=$mnt/corpus
    rm -rf "$c"
    mkdir -p "$c/sub/deeper" "$c/empty-dir"
    printf 'hello, world\n' >"$c/hello.txt"
    : >"$c/empty"
    # 200 KiB: more than one block, and not a whole number of them.
    yes 'The Sleuth Kit starter corpus, line of text 0123456789' | head -c 204801 >"$c/blocks.bin"
    printf 'nested\n' >"$c/sub/deeper/nested.txt"
    printf 'a name with spaces\n' >"$c/name with spaces.txt"
    printf 'UTF-8 name\n' >"$c/caf$(printf '\303\251').txt"
    ln "$c/hello.txt" "$c/sub/hard-link"
    ln -s hello.txt "$c/symlink"
    ln -s ../hello.txt "$c/sub/relative-symlink"
    printf 'private\n' >"$c/private"
    chmod 0600 "$c/private"
    chown 1000:1000 "$c/private"
    chmod 0750 "$c/sub"
    # Nanosecond mtimes, set children first so the directories keep theirs.
    find "$c" -depth -exec touch -h -d '2021-02-03 04:05:06.123456789 UTC' {} +
    touch -h -d '2001-09-09 01:46:40.5 UTC' "$c/symlink"
    touch -d '2030-01-01 00:00:00.000000001 UTC' "$c/blocks.bin"
    sync -f "$c"
}

cmd_freeze() {
    local mnt=$1 secs=$2 unit
    unit=$(timer_unit "$mnt")
    sync
    systemctl stop "$unit.timer" "$unit.service" >/dev/null 2>&1
    systemctl reset-failed "$unit.timer" "$unit.service" >/dev/null 2>&1
    systemd-run --quiet --unit="$unit" --on-active="$secs" \
        /usr/sbin/fsfreeze --unfreeze "$mnt" ||
        die "could not arm the unfreeze timer for $mnt"
    if ! fsfreeze --freeze "$mnt"; then
        systemctl stop "$unit.timer" >/dev/null 2>&1
        die "fsfreeze --freeze $mnt failed"
    fi
}

cmd_unfreeze() {
    local mnt=$1 unit out
    unit=$(timer_unit "$mnt")
    # EINVAL means it wasn't frozen: fine, the goal is "not frozen".
    if ! out=$(fsfreeze --unfreeze "$mnt" 2>&1) && [[ $out != *'Invalid argument'* ]]; then
        echo "$out" >&2
        die "fsfreeze --unfreeze $mnt failed"
    fi
    systemctl stop "$unit.timer" >/dev/null 2>&1
    systemctl reset-failed "$unit.timer" "$unit.service" >/dev/null 2>&1
    return 0
}

cmd_capture() { "$BIN/capture-ground-truth" -o "$2" "$1"; }

cmd_flush() {
    blockdev --flushbufs "$1" || die "blockdev --flushbufs $1 failed"
    echo 3 >/proc/sys/vm/drop_caches || die "could not drop the page caches"
}

cmd_run() { "$BIN/tsk-harness" -o "$2" "$1"; }

# Test hook (TSK_VALIDATE_BREAK): the byte offset on DEV of the first byte of
# MNT/PATH, found while the file system is not frozen yet.
cmd_break_locate() {
    local mnt=$1 dev=$2 path=$3 file out bs phys
    file=$mnt$path
    [[ -f $file && -s $file ]] || die "break: $file is not a non-empty regular file"
    out=$(filefrag -v "$file") || die "break: filefrag failed on $file"
    bs=$(sed -nE 's/.*blocks? of ([0-9]+) bytes.*/\1/p' <<<"$out" | head -n1)
    phys=$(awk -F: '$1 ~ /^ *0$/ { split($3, a, "."); gsub(/ /, "", a[1]); print a[1]; exit }' <<<"$out")
    [[ $bs =~ ^[0-9]+$ && $phys =~ ^[0-9]+$ ]] || die "break: cannot parse filefrag output: $out"
    printf '%s %s\n' "$dev" $((phys * bs)) >"$BREAK_STATE"
}

write_byte() { # dev offset value
    printf "\\$(printf %03o "$3")" | dd of="$1" bs=1 seek="$2" count=1 conv=notrunc status=none &&
        blockdev --flushbufs "$1"
}

cmd_break() {
    local dev off orig
    read -r dev off <"$BREAK_STATE" || die "break: no location (break-locate first)"
    orig=$(dd if="$dev" bs=1 skip="$off" count=1 status=none | od -An -tu1 | tr -d ' ')
    [[ $orig =~ ^[0-9]+$ ]] || die "break: cannot read the byte at $off on $dev"
    write_byte "$dev" "$off" $((orig ^ 0x20)) || die "break: cannot write $dev"
    printf '%s %s %s\n' "$dev" "$off" "$orig" >"$BREAK_STATE"
    echo "break: flipped the byte at offset $off of $dev ($orig -> $((orig ^ 0x20)))"
}

cmd_restore() {
    local dev off orig
    [[ -f $BREAK_STATE ]] || return 0
    read -r dev off orig <"$BREAK_STATE"
    if [[ -n ${orig:-} ]]; then
        write_byte "$dev" "$off" "$orig" || die "restore: cannot write $dev"
        echo 3 >/proc/sys/vm/drop_caches
        echo "restore: put back the byte at offset $off of $dev"
    fi
    rm -f "$BREAK_STATE"
}

[[ $# -ge 1 ]] || die "no command"
cmd=$1
shift
case $cmd in
tools) cmd_tools ;;
check-mount) cmd_check_mount "$@" ;;
corpus) cmd_corpus "$@" ;;
freeze) cmd_freeze "$@" ;;
unfreeze) cmd_unfreeze "$@" ;;
capture) cmd_capture "$@" ;;
flush) cmd_flush "$@" ;;
run) cmd_run "$@" ;;
break-locate) cmd_break_locate "$@" ;;
break) cmd_break ;;
restore) cmd_restore ;;
*) die "unknown command: $cmd" ;;
esac
