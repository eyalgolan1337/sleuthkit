# shellcheck shell=bash
# Gate 3: Strict checks (see GLOSSARY.md, test/vm/README.md "Gate 3: Strict
# checks"): the Guest-side run and the Host-side run of each partition, each
# compared with the same Ground truth.
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
    if [[ -f $vm/host.txt ]]; then
        while IFS= read -r line; do item_note "Host side: $line"; done <"$vm/host.txt"
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

# The Test corpus generator's record (test/vm/corpus/): one note listing what
# it made, one per feature it couldn't make on this file system, with the
# reason. The full record is in the check's log.
gate3_corpus_record() {
    local mnt=$1 part=$2 log=$3 rc=$4 kind feature detail made= ok=1
    while IFS=$'\t' read -r kind feature detail; do
        case $kind in
        made) made+=${made:+, }$feature ;;
        not-made) item_note "$mnt ($part): Test corpus: not made here: $feature: $detail" ;;
        failed) item_fail "$mnt ($part): the Test corpus generator failed: $detail"; ok= ;;
        esac
    done <"$log"
    [[ -n $made ]] && item_note "$mnt ($part): Test corpus: made $made"
    if ((rc != 0)) && [[ -n $ok ]]; then
        item_fail "$mnt ($part): step corpus failed (exit $rc): $(tail -n1 "$log")"
        ok=
    fi
    [[ -n $ok ]]
}

# The corpus's XFS formats, checked with xfs_db in the frozen window: one
# note per path with its format, a failure for a path without the format it
# was made for. Nothing for a file system that isn't XFS.
gate3_corpus_formats() {
    local mnt=$1 part=$2 log=$3 rc=$4 line failed=
    while IFS= read -r line; do
        case $line in
        'OK   '*) item_note "$mnt ($part): XFS format: ${line#OK   }" ;;
        'FAIL '*) item_fail "$mnt ($part): XFS format: ${line#FAIL }"; failed=1 ;;
        esac
    done <"$log"
    if ((rc != 0)) && [[ -z $failed ]]; then
        item_fail "$mnt ($part): step corpus-formats failed (exit $rc): $(tail -n1 "$log")"
        failed=1
    fi
    [[ -z $failed ]]
}

# Strict check, Guest side, of one partition: the steps of the Strict check
# sequence, then the comparison of the Guest-side run with Ground truth.
gate3_strict_guest() {
    local part=$1 mnt=$2 dir=$RUN_DIR/vm/strict-guest/$1 name rc secs ok=1
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
        corpus:*) gate3_corpus_record "$mnt" "$part" "$dir/$name.log" "$rc" || ok= ;;
        corpus-formats:*) gate3_corpus_formats "$mnt" "$part" "$dir/$name.log" "$rc" || ok= ;;
        *:0) ;;
        run:1) ;; # an incomplete or failed run: the comparison reports it
        capture:1) item_fail "$mnt ($part): Ground truth is incomplete (capture-ground-truth exit 1): $(head -n1 "$dir/$name.log")"; ok= ;;
        *) item_fail "$mnt ($part): step $name failed (exit $rc): $(tail -n1 "$dir/$name.log")"; ok= ;;
        esac
    done <"$dir/steps.tsv"
    for name in check-mount corpus freeze capture flush corpus-formats run unfreeze; do
        grep -q "^$name"$'\t' "$dir/steps.tsv" && continue
        item_fail "$mnt ($part): step $name did not run"
        ok=
    done
    if [[ ! -s $dir/ground-truth.manifest || ! -s $dir/guest.manifest ]]; then
        item_fail "$mnt ($part): no Manifest to compare (Ground truth or the Guest-side run is missing)"
        return 1
    fi
    gate3_compare "$dir/ground-truth.manifest" "$dir/guest.manifest" strict-guest "$part" \
        "$dir/compare.txt" || ok=
    [[ -n $ok ]]
}

# gate3_compare <ground truth> <run> <check> <partition> <output>: the Strict
# comparison of a run's Manifest with Ground truth, its findings as items.
gate3_compare() {
    local gt=$1 run=$2 check=$3 part=$4 out=$5 rc line
    echo "== comparison"
    if ! make -s -j"$JOBS" -f test/vm/manifest/manifest.mk build/vm-tools/compare-manifests; then
        item_fail "could not build compare-manifests; see the log"
        return 1
    fi
    build/vm-tools/compare-manifests --ground-truth "$gt" --run "$run" --mode strict \
        --known-failures "$KNOWN_FAILURES" --check "$check" --partition "$part" \
        --items "$VALIDATE_ITEMS" | tee "$out"
    rc=${PIPESTATUS[0]}
    # Known failures don't appear in the items; list them, so the report
    # shows what this check tolerated.
    while IFS= read -r line; do
        item_note "Known failure: ${line#KNOWN }"
    done < <(grep '^KNOWN ' "$out")
    ((rc == 0))
}

# Strict check, Host side, of one partition: the Host-side run on the .vmdk
# in its one-off container (test/vm/host-run.sh), compared with the Ground
# truth of the same frozen window. The Guest side's check reports the steps
# of the sequence they share.
gate3_strict_host() {
    local part=$1 mnt=$2 dir=$RUN_DIR/vm/strict-host/$1 gt=$RUN_DIR/vm/strict-guest/$1/ground-truth.manifest
    local name rc secs ok=1 line
    if [[ ! -f $dir/steps.tsv ]]; then
        item_fail "$mnt ($part) was not checked on the Host side: the Strict check sequence stopped before the Host-side run (see the Guest side check)"
        return 1
    fi
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
    IFS=$'\t' read -r name rc secs <"$dir/steps.tsv"
    case $rc in
    0 | 1) ;; # 1: an incomplete or failed run; the comparison reports it
    3) return 1 ;; # writable paths, reported above; no Manifest
    *)
        item_fail "$mnt ($part): the Host-side run failed (exit $rc): $(tail -n1 "$dir/host-run.log")"
        return 1
        ;;
    esac
    if [[ ! -s $gt || ! -s $dir/host.manifest ]]; then
        item_fail "$mnt ($part): no Manifest to compare (Ground truth or the Host-side run is missing)"
        return 1
    fi
    gate3_compare "$gt" "$dir/host.manifest" strict-host "$part" "$dir/compare.txt" || ok=
    [[ -n $ok ]]
}

# CLI tool check of one tool on one side of a partition's Strict check
# (test/vm/README.md, "Gate 3: CLI tool checks"): the tool's output, run in
# the same frozen window as the harness, compared with Ground truth by
# check-tool. Check name in the Known failure list: <tool>-<side>.
gate3_tool() {
    local tool=$1 side=$2 part=$3 mnt=$4 gt=$RUN_DIR/vm/strict-guest/$3
    local dir=$RUN_DIR/vm/strict-$2/$3 step=run-tools name rc secs line f
    local override=()
    [[ $side == host ]] && step=host-tools
    # The tool steps' exit statuses (collect-vm.sh): capture-tools and
    # run-tools in the guest's tool-steps/, host-tools in the host's.
    if ! IFS=$'\t' read -r name rc secs < <(grep "^capture-tools"$'\t' "$gt/tool-steps/steps.tsv" 2>/dev/null); then
        item_fail "$mnt ($part): the tool checks' Ground truth was not captured: the Strict check sequence stopped before it (see the Strict check, Guest side)"
        return 1
    fi
    if [[ $rc != 0 ]]; then
        item_fail "$mnt ($part): capturing the tool checks' Ground truth failed (capture-tools, exit $rc): $(tail -n1 "$gt/tool-steps/capture-tools.log")"
        return 1
    fi
    if ! IFS=$'\t' read -r name rc secs < <(grep "^$step"$'\t' "$dir/tool-steps/steps.tsv" 2>/dev/null); then
        item_fail "$mnt ($part): fls, icat, fsstat and istat did not run on the $side side (step $step; see the Strict check of this side)"
        return 1
    fi
    echo "== $step (exit status $rc, ${secs}s)"
    cat "$dir/tool-steps/$step.log"
    if [[ $side == host ]]; then
        while IFS= read -r line; do
            item_fail "one-off container: ${line#WRITABLE: } is writable; it must be mounted read-only"
        done < <(grep '^WRITABLE: ' "$dir/tool-steps/$step.log")
    fi
    if [[ $rc != 0 ]]; then
        item_fail "$mnt ($part): the $side side's tool run failed (step $step, exit $rc): $(tail -n1 "$dir/tool-steps/$step.log")"
        return 1
    fi
    if [[ $tool == fsstat && -f $gt/summary-override ]]; then
        override=(--override "$(<"$gt/summary-override")")
    fi
    echo "== $tool: exit statuses and standard error"
    for f in "$dir/tools/$tool"*.status "$dir/tools/$tool"/*.status; do
        [[ -f $f ]] || continue
        printf '%s: exit %s %s\n' "${f#"$dir/tools/"}" "$(<"$f")" "$(head -n1 "${f%.status}.err")"
    done
    echo "== comparison"
    if ! make -s -j"$JOBS" -f test/vm/manifest/manifest.mk build/vm-tools/check-tool; then
        item_fail "could not build check-tool; see the log"
        return 1
    fi
    build/vm-tools/check-tool "$tool" --ground-truth-dir "$gt" --run-dir "$dir/tools" \
        --known-failures "$KNOWN_FAILURES" --check "$tool-$side" --partition "$part" \
        --items "$VALIDATE_ITEMS" "${override[@]}" | tee "$dir/$tool-compare.txt"
    rc=${PIPESTATUS[0]}
    while IFS= read -r line; do
        item_note "Known failure: ${line#KNOWN }"
    done < <(grep '^KNOWN ' "$dir/$tool-compare.txt")
    ((rc == 0))
}

gate_3() {
    gate_begin 3 "Strict checks"
    run_check vm "Validation VM and the static guest tools" gate3_vm
    local entry part mnt
    for entry in "${STRICT_PARTITIONS[@]}"; do
        read -r part mnt <<<"$entry"
        run_check "strict-guest-$part" "Strict check, Guest side: $mnt ($part)" \
            gate3_strict_guest "$part" "$mnt"
        run_check "strict-host-$part" "Strict check, Host side: $mnt ($part), .vmdk" \
            gate3_strict_host "$part" "$mnt"
    done
    local tool side
    for entry in "${STRICT_PARTITIONS[@]}"; do
        read -r part mnt <<<"$entry"
        for side in guest host; do
            for tool in "${TOOL_CHECKS[@]}"; do
                run_check "$tool-$side-$part" \
                    "CLI tool check, ${side^} side: $tool, $mnt ($part)$([[ $side == host ]] && echo ', .vmdk')" \
                    gate3_tool "$tool" "$side" "$part" "$mnt"
            done
        done
    done
    gate_end
}
