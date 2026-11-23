# shellcheck shell=bash
# Reading the Validation VM's collected results in gates 3 and 4
# (test/vm/gates/gate3.sh, gate4.sh): the parts both gates share. They run in
# the devcontainer and report with the validate command's item_* functions
# (lib/report.sh). The data they read is described in lib/vm_checks.sh.

# vm_require_collected: fails the check unless the host collected the VM's
# results before the gates (validate.sh run inside the devcontainer can't).
vm_require_collected() {
    [[ -f $RUN_DIR/vm/status ]] && return 0
    item_fail "no results from the Validation VM: this gate needs the host, which collects them before entering the devcontainer. Run test/vm/validate.sh on the host."
    return 1
}

# vm_known_failure_notes <comparison output>: one note per Known failure the
# comparison tolerated (its KNOWN lines). They don't appear in the items, so
# this is how the report shows what a check tolerated.
vm_known_failure_notes() {
    local line
    while IFS= read -r line; do
        item_note "Known failure: ${line#KNOWN }"
    done < <(grep '^KNOWN ' "$1")
}

# vm_compare <output> <check> <partition> <compare-manifests arguments...>:
# compares a run's Manifest with Ground truth (the arguments give the
# Manifests and the mode), tolerating the Known failures of this check and
# partition. The full report goes to the log and <output>, the findings to
# the items.
vm_compare() {
    local out=$1 check=$2 part=$3 rc
    shift 3
    if ! make -s -j"$JOBS" -f test/vm/manifest/manifest.mk build/vm-tools/compare-manifests; then
        item_fail "could not build compare-manifests; see the log"
        return 1
    fi
    build/vm-tools/compare-manifests "$@" \
        --known-failures "$KNOWN_FAILURES" --check "$check" --partition "$part" \
        --items "$VALIDATE_ITEMS" | tee "$out"
    rc=${PIPESTATUS[0]}
    vm_known_failure_notes "$out"
    ((rc == 0))
}

# vm_host_run <dir> <label>: the outcome of a Host-side run in its one-off
# container (test/vm/host-run.sh), from <dir>/steps.tsv and host-run.log:
# each refused write as a note, each writable path as a failure. Returns 0
# if there is a Manifest to compare (the run passed, or failed in a way the
# comparison reports), 1 otherwise.
vm_host_run() {
    local dir=$1 label=$2 line rc
    echo "== host-run (exit status, seconds)"
    cat "$dir/steps.tsv"
    echo "== host-run.log"
    cat "$dir/host-run.log"
    # The write attempts in the one-off container: each must have failed.
    while IFS= read -r line; do
        case $line in
        read-only:*) item_note "one-off container: write refused, ${line#read-only: }" ;;
        WRITABLE:*) item_fail "one-off container: ${line#WRITABLE: } is writable; it must be mounted read-only" ;;
        esac
    done <"$dir/host-run.log"
    IFS=$'\t' read -r _ rc _ <"$dir/steps.tsv"
    case $rc in
    0 | 1) ;; # 1: an incomplete or failed run; the comparison reports it
    3) return 1 ;; # writable paths, reported above; no Manifest
    *)
        item_fail "$label: the Host-side run failed (exit $rc): $(tail -n1 "$dir/host-run.log")"
        return 1
        ;;
    esac
}
