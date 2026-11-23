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

# The Control file system: the starter corpus goes here.
CONTROL_PARTITION=sda6

# The committed Known failure list, relative to the repository root.
KNOWN_FAILURES=test/vm/known-failures.txt

# The static guest build's binaries (test/vm/guest-build.sh), in build/guest-static/bin.
GUEST_TOOLS=(fls icat fsstat istat tsk-harness capture-ground-truth)
