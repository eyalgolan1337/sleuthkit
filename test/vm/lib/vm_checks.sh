# shellcheck shell=bash
# What the Validation VM's checks cover, shared by the host-side collection
# (test/vm/collect-vm.sh) and gates 3 and 4 (test/vm/gates/gate3.sh,
# gate4.sh).

# Strict checks: "partition mount-point", one per file system. The partition
# name is the /dev name in the guest, and the name used in the Known failure
# list. / and /boot get Best-effort checks instead (gate 4).
STRICT_PARTITIONS=(
    "sda4 /data"
    "sda5 /data-compat"
    "sda6 /control"
)

# Best-effort checks (gate 4): "partition mount-point", for the file systems
# in use that can't be frozen. Each has a committed Volatile path list,
# $VOLATILE_PATHS_DIR/<partition>.txt, relative to the repository root.
BEST_EFFORT_PARTITIONS=(
    "sda2 /boot"
    "sda3 /"
)
VOLATILE_PATHS_DIR=test/vm/volatile-paths

# The Control file system.
CONTROL_PARTITION=sda6

# The Test corpus (test/vm/corpus/, #19) is generated in <mount>/corpus on
# every Strict check partition. corpus_large_size <partition> prints the size
# of its /files/large.bin: about 1 GiB on /data, proportionally smaller on the
# smaller partitions, so each file system stays at most about a third full.
corpus_large_size() {
    case $1 in
    sda4) echo 1G ;;
    sda5) echo 512M ;;
    sda6) echo 256M ;;
    *) return 1 ;;
    esac
}

# corpus_churn_files <partition>: the number of empty files the corpus makes
# in /corpus/churn and deletes every other one of (#58), leaving free inodes
# that were once in use in partly used chunks. On XFS, enough for interior
# levels in the inode B+tree of the directory's AG (a 4 KiB v5 leaf holds 252
# chunk records); none on the Control file system.
corpus_churn_files() {
    case $1 in
    sda4 | sda5) echo 17000 ;;
    sda6) echo 0 ;;
    *) return 1 ;;
    esac
}

# The committed Known failure list, relative to the repository root.
KNOWN_FAILURES=test/vm/known-failures.txt

# The static guest build's binaries (test/vm/guest-build.sh), in build/guest-static/bin.
GUEST_TOOLS=(fls icat fsstat istat ils ifind blkls tsk-harness capture-ground-truth
    tsk-corpus check-tool)

# The CLI tool checks of each Strict check (gate 3): one check per tool and
# side, named <tool>-guest and <tool>-host (also in the Known failure list).
TOOL_CHECKS=(fls icat fsstat istat ils ifind blkls)

# The tool checks of the inode walk and the block walk (#58), whose Ground
# truth is the walk record (tsk-validate.sh capture-walks).
WALK_TOOL_CHECKS=(ils ifind blkls)

# vm_comparisons: every comparison gates 3 and 4 run, as CHECK:PARTITION
# separated by commas (compare-manifests --lint-known-failures --comparisons).
# The check names are the ones the gates pass with --check.
vm_comparisons() {
    local entry part side tool pairs=()
    for entry in "${STRICT_PARTITIONS[@]}"; do
        read -r part _ <<<"$entry"
        for side in guest host; do
            pairs+=("strict-$side:$part")
            for tool in "${TOOL_CHECKS[@]}"; do
                pairs+=("$tool-$side:$part")
            done
        done
    done
    for entry in "${BEST_EFFORT_PARTITIONS[@]}"; do
        read -r part _ <<<"$entry"
        for side in guest host; do
            pairs+=("best-effort-$side:$part")
        done
    done
    local IFS=,
    printf '%s\n' "${pairs[*]}"
}

# spot_check_paths <partition>: the istat spot-check paths of a partition,
# one per line, from the committed list test/vm/istat-spot-checks.txt.
spot_check_paths() {
    awk -v p="$1" '$1 !~ /^#/ && $1 == p { print $2 }' \
        "$(dirname "${BASH_SOURCE[0]}")/../istat-spot-checks.txt"
}
