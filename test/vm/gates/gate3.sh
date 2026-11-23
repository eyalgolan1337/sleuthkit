# shellcheck shell=bash
# Gate 3: Strict checks (see GLOSSARY.md, test/vm/README.md "Gate 3: Strict
# checks"). Guest side so far; the Host-side run comes with #18.
#
# The Validation VM is reachable only from the host, so on the host the
# validate command collects the VM's results first (test/vm/collect-vm.sh,
# into $RUN_DIR/vm/) and this gate, in the devcontainer, checks and compares
# them. Without that collection (validate.sh run inside the devcontainer),
# every check of this gate fails.

# shellcheck source=../lib/vm_checks.sh
source "$VALIDATE_LIB/vm_checks.sh"

# The collection's outcome: the VM, the copy of the static guest build, and
# whether each static tool runs in the guest.
gate3_vm() {
    local vm=$RUN_DIR/vm state reason line
    if [[ ! -f $vm/status ]]; then
        item_fail "no results from the Validation VM: this gate needs the host, which collects them before entering the devcontainer. Run test/vm/validate.sh on the host."
        return 1
    fi
    cat "$RUN_DIR/logs/vm-collect.log" 2>/dev/null
    if [[ -f $vm/vm.txt ]]; then
        while IFS= read -r line; do item_note "VM: $line"; done <"$vm/vm.txt"
    fi
    if [[ -f $vm/tools.txt ]]; then
        while IFS= read -r line; do
            case $line in
            OK*) item_note "static guest tool runs in the guest: ${line#OK }" ;;
            *) item_fail "static guest tool doesn't run in the guest: ${line#FAIL }" ;;
            esac
        done <"$vm/tools.txt"
    fi
    IFS=$'\t' read -r state reason <"$vm/status"
    if [[ $state != ok ]]; then
        item_fail "collecting the Validation VM's results failed: $reason (log: logs/vm-collect.log)"
        return 1
    fi
}

# Strict check, Guest side, of one partition: the steps of the Strict check
# sequence, then the comparison of the Guest-side run with Ground truth.
gate3_strict_guest() {
    local part=$1 mnt=$2 dir=$RUN_DIR/vm/strict-guest/$1 name rc secs ok=1 line
    if [[ ! -f $dir/steps.tsv ]]; then
        item_fail "$mnt ($part) was not checked: the collection of the VM's results stopped before it (see the Validation VM check)"
        return 1
    fi
    echo "== steps of the Strict check sequence (name, exit status, seconds)"
    cat "$dir/steps.tsv"
    while IFS=$'\t' read -r name rc secs; do
        echo "== $name.log"
        cat "$dir/$name.log"
        case $name:$rc in
        *:0) ;;
        run:1) ;; # an incomplete or failed run: the comparison reports it
        capture:1) item_fail "$mnt ($part): Ground truth is incomplete (capture-ground-truth exit 1): $(head -n1 "$dir/$name.log")"; ok= ;;
        *) item_fail "$mnt ($part): step $name failed (exit $rc): $(tail -n1 "$dir/$name.log")"; ok= ;;
        esac
    done <"$dir/steps.tsv"
    for name in check-mount freeze capture flush run unfreeze; do
        grep -q "^$name"$'\t' "$dir/steps.tsv" && continue
        item_fail "$mnt ($part): step $name did not run"
        ok=
    done
    if [[ ! -s $dir/ground-truth.manifest || ! -s $dir/guest.manifest ]]; then
        item_fail "$mnt ($part): no Manifest to compare (Ground truth or the Guest-side run is missing)"
        return 1
    fi
    echo "== comparison"
    if ! make -s -j"$JOBS" -f test/vm/manifest/manifest.mk build/vm-tools/compare-manifests; then
        item_fail "could not build compare-manifests; see the log"
        return 1
    fi
    build/vm-tools/compare-manifests --ground-truth "$dir/ground-truth.manifest" \
        --run "$dir/guest.manifest" --mode strict \
        --known-failures "$KNOWN_FAILURES" --check strict-guest --partition "$part" \
        --items "$VALIDATE_ITEMS" | tee "$dir/compare.txt"
    rc=${PIPESTATUS[0]}
    # Known failures don't appear in the items; list them, so the report
    # shows what this check tolerated.
    while IFS= read -r line; do
        item_note "Known failure: ${line#KNOWN }"
    done < <(grep '^KNOWN ' "$dir/compare.txt")
    [[ -n $ok ]] && ((rc == 0))
}

gate_3() {
    gate_begin 3 "Strict checks"
    run_check vm "Validation VM and the static guest tools" gate3_vm
    local entry part mnt
    for entry in "${STRICT_PARTITIONS[@]}"; do
        read -r part mnt <<<"$entry"
        run_check "strict-guest-$part" "Strict check, Guest side: $mnt ($part)" \
            gate3_strict_guest "$part" "$mnt"
    done
    gate_end
}
