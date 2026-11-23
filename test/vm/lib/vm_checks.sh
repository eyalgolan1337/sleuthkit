# shellcheck shell=bash
# What the Validation VM's checks cover, shared by the host-side collection
# (test/vm/collect-vm.sh) and gate 3 (test/vm/gates/gate3.sh).

# Strict checks: "partition mount-point", one per file system. The partition
# name is the /dev name in the guest, and the name used in the Known failure
# list. / and /boot get Best-effort checks instead (gate 4, #21).
STRICT_PARTITIONS=(
    "sda4 /data"
    "sda5 /data-compat"
    "sda6 /control"
)

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

# The committed Known failure list, relative to the repository root.
KNOWN_FAILURES=test/vm/known-failures.txt

# The static guest build's binaries (test/vm/guest-build.sh), in build/guest-static/bin.
GUEST_TOOLS=(fls icat fsstat istat tsk-harness capture-ground-truth tsk-corpus)
