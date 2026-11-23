#!/bin/bash
# Guest-side helper of the validate command (test/vm/README.md, "Gate 3:
# Strict checks"). The host copies it, with the static guest build, to
# /root/tsk-validate in the Validation VM and runs one step per SSH call:
#
#   tsk-validate.sh tools                      run each static tool once (no glibc errors)
#   tsk-validate.sh check-mount DEV MNT        DEV is mounted on MNT
#   tsk-validate.sh corpus MNT SIZE            (re)generate the Test corpus in MNT/corpus
#   tsk-validate.sh corpus-formats MNT DEV     XFS: check the corpus's on-disk formats
#   tsk-validate.sh freeze MNT SECONDS         sync, arm the unfreeze timer, fsfreeze -f
#   tsk-validate.sh unfreeze MNT               fsfreeze -u if frozen, disarm the timer
#   tsk-validate.sh capture MNT OUT            Ground truth of MNT into OUT
#   tsk-validate.sh flush DEV                  flush DEV's buffer cache, drop page caches
#   tsk-validate.sh run DEV OUT                Guest-side run: the harness on DEV into OUT
#   tsk-validate.sh capture-tools MNT DEV DIR PATH...
#                                              Ground truth of the CLI tool checks into DIR:
#                                              the file-system summary, stat of each PATH
#   tsk-validate.sh run-tools DEV DIR          CLI tool checks: fls, icat, fsstat, istat on
#                                              DEV into DIR/tools (DIR: capture, capture-tools)
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
        tsk-harness | capture-ground-truth | tsk-corpus) out=$("$t" --help 2>&1) ;;
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

# The Test corpus (#19): generated afresh in MNT/corpus in each run, from the
# generator's fixed seed. Its record (made and not-made features) goes to
# standard output. SIZE is the size of /files/large.bin.
cmd_corpus() {
    local mnt=$1 size=$2 c=$1/corpus
    rm -rf "$c" || die "could not remove the old corpus $c"
    "$BIN/tsk-corpus" --large-size "$size" "$c"
}

# The magic of the directory block xfs_db printed, or nothing if the block is
# unmapped (xfs_db then stays on the inode, whose core.magic doesn't count).
xfs_block_magic() {
    grep -v '^core\.' | sed -nE 's/^[^ ]*magic = (0x[0-9a-f]+)$/\1/p' | head -n1
}

# Checks with xfs_db that the corpus's directories and its extent B+tree file
# really have the on-disk format each was made for. Run in the frozen window,
# after flush, so the device holds the current metadata. One line per path:
# "OK   /path format: detail" or "FAIL /path: ...". Not XFS: one SKIP line.
cmd_corpus_formats() {
    local mnt=$1 dev=$2 fstype info bs dirbs leafblk spec path want ino out
    local fmt size next first leaf got detail rc=0
    fstype=$(findmnt -no FSTYPE --mountpoint "$mnt") || die "nothing is mounted on $mnt"
    if [[ $fstype != xfs ]]; then
        echo "SKIP $mnt is $fstype: the formats are XFS formats"
        return 0
    fi
    info=$(xfs_info "$mnt") || die "xfs_info $mnt failed"
    bs=$(sed -nE 's/^data +=.* bsize=([0-9]+).*/\1/p' <<<"$info")
    dirbs=$(sed -nE 's/^naming +=version 2 +bsize=([0-9]+).*/\1/p' <<<"$info")
    [[ $bs =~ ^[0-9]+$ && $dirbs =~ ^[0-9]+$ ]] || die "cannot parse xfs_info $mnt: $info"
    leafblk=$(((32 << 30) / bs)) # XFS_DIR2_LEAF_OFFSET, in file system blocks
    for spec in "dirs/shortform shortform" "dirs/block block" "dirs/leaf leaf" \
        "dirs/node node" "dirs/btree btree" "fragmented/extents.bin btree"; do
        read -r path want <<<"$spec"
        if ! ino=$(stat -c %i "$mnt/corpus/$path"); then
            echo "FAIL /corpus/$path: missing"
            rc=1
            continue
        fi
        out=$(xfs_db -r -c "inode $ino" -c "print core.format core.size core.nextents" "$dev" 2>&1)
        fmt=$(sed -nE 's/^core\.format = ([0-9]+).*/\1/p' <<<"$out")
        size=$(sed -nE 's/^core\.size = ([0-9]+)$/\1/p' <<<"$out")
        next=$(sed -nE 's/^core\.nextents = ([0-9]+)$/\1/p' <<<"$out")
        first= leaf=
        if [[ $path == dirs/* && $fmt == 2 ]]; then
            first=$(xfs_db -r -c "inode $ino" -c "dblock 0" -c print "$dev" 2>/dev/null | xfs_block_magic)
            leaf=$(xfs_db -r -c "inode $ino" -c "dblock $leafblk" -c print "$dev" 2>/dev/null | xfs_block_magic)
            case $leaf in
            0x3df1 | 0xd2f1) leaf=leaf ;; # XFS_DIR3_LEAF1_MAGIC, XFS_DIR2_LEAF1_MAGIC
            0x3ebe | 0xfebe) leaf=node ;; # XFS_DA3_NODE_MAGIC, XFS_DA_NODE_MAGIC
            '') ;;
            *) leaf="magic $leaf" ;;
            esac
        fi
        case $fmt in
        1) got=shortform ;;
        3) got=btree ;;
        2)
            # XDB3 (v5) or XD2B (v4): a block directory's single block.
            if [[ $path == dirs/* && -z $leaf && $size == "$dirbs" &&
                ($first == 0x58444233 || $first == 0x58443242) ]]; then
                got=block
            elif [[ -n $leaf ]]; then
                got=$leaf
            else
                got=extents
            fi
            ;;
        *) got="unknown format '${fmt}': $out" ;;
        esac
        detail="inode $ino, data fork format $fmt, size $size, $next extents${leaf:+, leaf offset: $leaf}"
        if [[ $got == "$want" ]]; then
            echo "OK   /corpus/$path $want: $detail"
        else
            echo "FAIL /corpus/$path: $got, not $want ($detail)"
            rc=1
        fi
    done
    return "$rc"
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

# Ground truth of the CLI tool checks (test/vm/README.md, "Gate 3: CLI tool
# checks"), in the frozen window: the guest's own file-system summary for
# fsstat (DIR/fs-summary.txt) and the stat records of the istat spot-check
# paths (DIR/stat.txt). The formats are check-tool's (test/vm/toolcheck/).
cmd_capture_tools() {
    local mnt=$1 dev=$2 dir=$3 type p f rec target
    shift 3
    type=$(blkid -p -s TYPE -o value "$dev") || die "blkid -p $dev failed"
    {
        echo "== blkid"
        blkid -p -o export "$dev"
        case $type in
        ext2 | ext3 | ext4)
            echo "== dumpe2fs"
            dumpe2fs -h "$dev" 2>/dev/null
            ;;
        xfs)
            echo "== xfs_info"
            xfs_info "$mnt"
            ;;
        esac
    } >"$dir/fs-summary.txt" || die "could not write the file-system summary of $dev"
    : >"$dir/stat.txt"
    for p in "$@"; do
        f=$mnt$p
        if rec=$(TZ=UTC stat --printf '%i\t%f\t%u\t%g\t%s\t%h\t%y\t%z\t%w' -- "$f" 2>&1); then
            target=-
            [[ -L $f ]] && target=$(readlink -- "$f")
            printf '%s\t%s\t%s\n' "$p" "$rec" "$target"
        else
            printf '%s\t!missing\t%s\n' "$p" "${rec//$'\n'/ }"
        fi
    done >>"$dir/stat.txt"
}

cmd_run_tools() {
    local dev=$1 dir=$2
    PATH=$BIN:$PATH "$HERE/run-tools.sh" "$dev" "$dir/tools" \
        "$dir/ground-truth.manifest" "$dir/stat.txt"
}

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
corpus-formats) cmd_corpus_formats "$@" ;;
freeze) cmd_freeze "$@" ;;
unfreeze) cmd_unfreeze "$@" ;;
capture) cmd_capture "$@" ;;
flush) cmd_flush "$@" ;;
run) cmd_run "$@" ;;
capture-tools) cmd_capture_tools "$@" ;;
run-tools) cmd_run_tools "$@" ;;
break-locate) cmd_break_locate "$@" ;;
break) cmd_break ;;
restore) cmd_restore ;;
*) die "unknown command: $cmd" ;;
esac
