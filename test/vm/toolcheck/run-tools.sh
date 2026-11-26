#!/bin/bash
# The CLI tool checks' runs (test/vm/README.md, "Gate 3: CLI tool checks"):
# TSK's fls, icat, fsstat, istat, ils, ifind and blkls on one file system,
# their output kept as they print it for check-tool. The same script runs in
# the guest (the static guest build, on /dev/sdN: tsk-validate.sh run-tools)
# and in the Host-side run's one-off container (the normal build, on
# partition N of the .vmdk:
# host-run.sh --tools).
#
#   run-tools.sh IMAGE OUT GROUND-TRUTH STAT-RECORDS IFIND-BLOCKS [PARTITION]
#
# The tools are found through PATH, so put the build's directories first.
# IMAGE is the file system, or with PARTITION, a disk image whose
# PARTITION-th allocated partition (as mmls -a lists them, the same as
# tsk-harness --partition) is the file system. GROUND-TRUTH is the partition's
# Ground truth Manifest: icat reads each of its regular files' inodes once,
# and its root directory's inode first, which shows whether icat can open the
# file system at all. STAT-RECORDS is the guest's stat records of the
# spot-check paths: istat runs on each one's inode. IFIND-BLOCKS has the
# blocks to ask ifind -d about, one per line (check-tool plan-blocks of the
# walk record). The output goes to OUT:
#
#   fls.{out,err,status}        fls -r -p -u
#   fsstat.{out,err,status}     fsstat
#   icat-root.{out,err,status}  icat of the root directory (its output dropped)
#   icat.tsv                    per regular file inode: inode, icat's exit
#                               status, SHA-256 of its output (or -), first
#                               line of its standard error
#   istat/INODE.{out,err,status}
#   ils-a, ils-e, ils.{out,err,status}
#                               ils -a, ils -e and ils with no option
#   ifind/BLOCK.{out,err,status}
#                               ifind -d BLOCK
#   blkls.{out,err,status}, blkls.lines
#                               blkls -e -l prints a line per block: only its
#                               three header lines and its last line are kept
#                               (blkls.out), and how many lines it printed
#                               (blkls.lines)
#
# TZ=UTC: istat prints times in local time. Exit status 0 when every tool
# ran (they may have failed; check-tool reports that), 2 on bad usage or when
# the partition can't be found.
set -uo pipefail
export TZ=UTC LC_ALL=C

if (($# < 5 || $# > 6)); then
    sed -n '2,42p' "$0" >&2
    exit 2
fi
image=$1 out=$2 gt=$3 stat=$4 blocks=$5 part=${6:-}

die() {
    echo "run-tools.sh: $*" >&2
    exit 2
}

[[ -r $gt ]] || die "no Ground truth Manifest $gt"
[[ -r $stat ]] || die "no stat records $stat"
[[ -r $blocks ]] || die "no ifind block list $blocks"
mkdir -p "$out/istat" "$out/ifind" || die "cannot create $out"

# Sector size and offset of the partition, for the tools' -b and -o.
where=()
if [[ -n $part ]]; then
    mm=$(mmls -a "$image" 2>&1) || die "mmls -a $image failed: $mm"
    units=$(sed -nE 's/^Units are in ([0-9]+)-byte sectors.*/\1/p' <<<"$mm")
    start=$(awk -v n="$part" '/^[0-9]+:/ { if (++i == n) { print $3 + 0; exit } }' <<<"$mm")
    [[ $units =~ ^[0-9]+$ && $start =~ ^[0-9]+$ ]] ||
        die "no allocated partition $part in $image: $mm"
    where=(-b "$units" -o "$start")
    echo "partition $part of $image: sector $start, $units-byte sectors"
fi

# tool NAME COMMAND...: NAME.out, NAME.err and NAME.status in OUT.
tool() {
    local name=$1
    shift
    "$@" >"$out/$name.out" 2>"$out/$name.err"
    echo $? >"$out/$name.status"
}

tool fls fls -r -p -u "${where[@]}" "$image"
tool fsstat fsstat "${where[@]}" "$image"

# Manifest lines: path, type, inode, ...; comments and !error lines skipped.
root=$(awk -F'\t' '$1 == "/" { print $3; exit }' "$gt")
tool icat-root icat "${where[@]}" "$image" "${root:-2}"
: >"$out/icat-root.out" # a directory's raw content: not needed

: >"$out/icat.tsv"
n=0
while read -r inode; do
    sum=$(icat "${where[@]}" "$image" "$inode" 2>"$out/icat.err.tmp" | sha256sum
        exit "${PIPESTATUS[0]}")
    st=$?
    sum=${sum%% *}
    ((st == 0)) || sum=-
    printf '%s\t%s\t%s\t%s\n' "$inode" "$st" "$sum" \
        "$(head -n1 "$out/icat.err.tmp" | tr '\t' ' ')" >>"$out/icat.tsv"
    n=$((n + 1))
done < <(awk -F'\t' '$1 !~ /^[#!]/ && $2 == "file" { print $3 }' "$gt" | sort -nu)
rm -f "$out/icat.err.tmp"
echo "icat: $n regular file inode(s)"

m=0
while IFS=$'\t' read -r _ inode _; do
    [[ $inode =~ ^[0-9]+$ ]] || continue # a !missing path
    tool "istat/$inode" istat "${where[@]}" "$image" "$inode"
    m=$((m + 1))
done <"$stat"
echo "istat: $m inode(s)"

# The inode walk: -a allocated inodes, -e every inode, no option the free
# inodes that were once in use.
tool ils-a ils -a "${where[@]}" "$image"
tool ils-e ils -e "${where[@]}" "$image"
tool ils ils "${where[@]}" "$image"

k=0
while read -r block; do
    [[ $block =~ ^[0-9]+$ ]] || continue
    tool "ifind/$block" ifind -d "$block" "${where[@]}" "$image"
    k=$((k + 1))
done <"$blocks"
echo "ifind -d: $k block(s)"

# The block walk: one line per block (a million on /data), so only the header
# lines, the last line and the count are kept.
: >"$out/blkls.out"
blkls -e -l "${where[@]}" "$image" 2>"$out/blkls.err" |
    awk -v kept="$out/blkls.out" -v count="$out/blkls.lines" '
        NR <= 3 { print > kept }
        { last = $0 }
        END { if (NR > 3) print last > kept; print NR > count }'
echo "${PIPESTATUS[0]}" >"$out/blkls.status"
echo "blkls: $(<"$out/blkls.lines") line(s)"
exit 0
