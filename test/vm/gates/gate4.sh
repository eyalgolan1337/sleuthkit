# shellcheck shell=bash
# Gate 4: Best-effort checks (see GLOSSARY.md, test/vm/README.md "Gate 4:
# Best-effort checks"): TSK compared with Ground truth on / and /boot while
# they are in use, with no freeze and no cache flush, on the Guest side and
# the Host side. A difference is tolerated, as a warning, only under a
# Volatile path or on an entry whose ctime is in the 10 minutes before the
# run.
#
# Like gate 3, it checks results the host collected before the gates
# (test/vm/collect-vm.sh, $RUN_DIR/vm/best-effort-*/). Gate 3's "Validation
# VM" check reports how that collection went.

# shellcheck source=../lib/vm_checks.sh
source "$VALIDATE_LIB/vm_checks.sh"

# The steps of the Strict check sequence that a Best-effort check must not
# take: freezing and flushing caches.
GATE4_FORBIDDEN_STEPS=(freeze unfreeze flush)

# gate4_collected <dir> <what>: the collection reached this check.
gate4_collected() {
    local dir=$1 what=$2
    if [[ ! -f $RUN_DIR/vm/status ]]; then
        item_fail "no results from the Validation VM: this gate needs the host, which collects them before entering the devcontainer. Run test/vm/validate.sh on the host."
        return 1
    fi
    if [[ ! -f $dir/steps.tsv ]]; then
        item_fail "$what was not checked: the collection of the VM's results stopped before it (see gate 3's Validation VM check and logs/vm-collect.log)"
        return 1
    fi
}

# gate4_run_time <file> <what>: prints the run's timestamp (nanoseconds since
# the epoch, from the guest's clock), or reports why there is none.
gate4_run_time() {
    local t
    t=$(cat "$1" 2>/dev/null)
    if [[ ! $t =~ ^[0-9]+$ ]]; then
        item_fail "$2: no timestamp of the run from the guest's clock (${t:-empty})"
        return 1
    fi
    printf '%s\n' "$t"
}

# gate4_window <run time> <Ground truth start-time file> <what>: a warning if
# Ground truth capture started more than the recent-ctime window (10 minutes)
# before the run ended. The window ends when the run ends, so it then no
# longer reaches back to the capture: a difference on an entry changed just
# before the capture is a failure, not a warning.
GATE4_WINDOW_SECONDS=600
gate4_window() {
    local run_time=$1 start span
    start=$(cat "$2" 2>/dev/null)
    if [[ ! $start =~ ^[0-9]+$ ]]; then
        item_warn "$3: no timestamp of the start of Ground truth capture; could not check that the run ended within $((GATE4_WINDOW_SECONDS / 60)) minutes of it"
        return 0
    fi
    span=$(((run_time - start) / 1000000000))
    if ((span > GATE4_WINDOW_SECONDS)); then
        item_warn "$3: the run ended ${span}s after Ground truth capture started, more than the $((GATE4_WINDOW_SECONDS / 60))-minute recent-ctime window, so entries changed just before the capture are not tolerated"
    fi
}

# gate4_compare <ground truth> <run> <run time> <check> <partition> <output>:
# the Best-effort comparison of a run's Manifest with Ground truth, its
# findings as items: failures, then each tolerated difference as a warning.
gate4_compare() {
    local gt=$1 run=$2 run_time=$3 check=$4 part=$5 out=$6 rc line
    echo "== comparison (best-effort, run time $run_time, Volatile paths $VOLATILE_PATHS_DIR/$part.txt)"
    if ! make -s -j"$JOBS" -f test/vm/manifest/manifest.mk build/vm-tools/compare-manifests; then
        item_fail "could not build compare-manifests; see the log"
        return 1
    fi
    build/vm-tools/compare-manifests --ground-truth "$gt" --run "$run" --mode best-effort \
        --run-time "$run_time" --volatile "$VOLATILE_PATHS_DIR/$part.txt" \
        --known-failures "$KNOWN_FAILURES" --check "$check" --partition "$part" \
        --items "$VALIDATE_ITEMS" | tee "$out"
    rc=${PIPESTATUS[0]}
    while IFS= read -r line; do
        item_note "Known failure: ${line#KNOWN }"
    done < <(grep '^KNOWN ' "$out")
    ((rc == 0))
}

# Best-effort check, Guest side, of one partition: its steps, then the
# comparison of the Guest-side run with Ground truth.
gate4_guest() {
    local part=$1 mnt=$2 dir=$RUN_DIR/vm/best-effort-guest/$1 name rc secs ok=1 calm=1 run_time
    gate4_collected "$dir" "$mnt ($part)" || return 1
    echo "== steps (name, exit status, seconds)"
    cat "$dir/steps.tsv"
    while IFS=$'\t' read -r name rc secs; do
        echo "== $name.log"
        cat "$dir/$name.log"
        case $name:$rc in
        *:0) ;;
        run:1) ;; # an incomplete or failed run: the comparison reports it
        capture-fs:1)
            # On a file system in use, an entry can vanish between the
            # directory listing and its stat; the comparison then sees it as
            # extra in the run, and applies the Best-effort tolerances.
            item_warn "$mnt ($part): Ground truth is incomplete ($(grep -c '^capture-ground-truth: ' "$dir/$name.log") problem(s) while capturing a file system in use; see the log): $(head -n1 "$dir/$name.log")"
            ;;
        *) item_fail "$mnt ($part): step $name failed (exit $rc): $(tail -n1 "$dir/$name.log")"; ok= ;;
        esac
    done <"$dir/steps.tsv"
    for name in check-mount capture-fs run; do
        grep -q "^$name"$'\t' "$dir/steps.tsv" && continue
        item_fail "$mnt ($part): step $name did not run"
        ok=
    done
    for name in "${GATE4_FORBIDDEN_STEPS[@]}"; do
        if grep -q "^$name"$'\t' "$dir/steps.tsv"; then
            item_fail "$mnt ($part): the Best-effort check ran the step $name; it must neither freeze nor flush caches"
            ok= calm=
        fi
    done
    [[ -n $calm ]] && item_note "$mnt ($part): no freeze and no cache flush (steps: $(cut -f1 "$dir/steps.tsv" | paste -sd, -))"
    if [[ ! -s $dir/ground-truth.manifest || ! -s $dir/guest.manifest ]]; then
        item_fail "$mnt ($part): no Manifest to compare (Ground truth or the Guest-side run is missing)"
        return 1
    fi
    run_time=$(gate4_run_time "$dir/run-time" "$mnt ($part)") || return 1
    gate4_window "$run_time" "$dir/gt-start-time" "$mnt ($part)"
    gate4_compare "$dir/ground-truth.manifest" "$dir/guest.manifest" "$run_time" \
        best-effort-guest "$part" "$dir/compare.txt" || ok=
    [[ -n $ok ]]
}

# Best-effort check, Host side, of one partition: the Host-side run on the
# .vmdk while the VM runs and the file system is in use, compared with the
# Guest side's Ground truth.
gate4_host() {
    local part=$1 mnt=$2 dir=$RUN_DIR/vm/best-effort-host/$1 guest_dir=$RUN_DIR/vm/best-effort-guest/$1
    local gt=$guest_dir/ground-truth.manifest name rc secs line run_time
    if [[ -f $guest_dir/steps.tsv && ! -f $dir/steps.tsv ]]; then
        item_fail "$mnt ($part) was not checked on the Host side: the Best-effort check stopped before the Host-side run (see the Guest side check)"
        return 1
    fi
    gate4_collected "$dir" "$mnt ($part) on the Host side" || return 1
    echo "== host-run (exit status, seconds)"
    cat "$dir/steps.tsv"
    echo "== host-run.log"
    cat "$dir/host-run.log"
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
    run_time=$(gate4_run_time "$dir/run-time" "$mnt ($part)") || return 1
    gate4_window "$run_time" "$guest_dir/gt-start-time" "$mnt ($part)"
    gate4_compare "$gt" "$dir/host.manifest" "$run_time" best-effort-host "$part" "$dir/compare.txt"
}

gate_4() {
    gate_begin 4 "Best-effort checks"
    local entry part mnt
    for entry in "${BEST_EFFORT_PARTITIONS[@]}"; do
        read -r part mnt <<<"$entry"
        run_check "best-effort-guest-$part" "Best-effort check, Guest side: $mnt ($part)" \
            gate4_guest "$part" "$mnt"
        run_check "best-effort-host-$part" "Best-effort check, Host side: $mnt ($part), .vmdk" \
            gate4_host "$part" "$mnt"
    done
    gate_end
}
