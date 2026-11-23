# shellcheck shell=bash
# Report API for the validate command (test/vm/validate.sh).
#
# A gate is a group of checks. A gate function calls:
#
#   gate_begin <n> <title>
#   run_check <slug> <title> <command> [args...]   # once per check
#   gate_end
#
# or, for a gate that does not exist yet:
#
#   gate_not_implemented <n> <title> <text>
#
# run_check runs the command in a subshell with stdout and stderr going to
# logs/gate<n>-<slug>.log in the run directory. The command reports findings
# with item_fail, item_warn and item_note (or by appending "fail|warn|note<TAB>text"
# lines to the file named by $VALIDATE_ITEMS, which is what the Python helpers do).
# A check fails if its command exits non-zero or reports any failure item.
# A gate fails if any of its checks fails.
#
# Requires RUN_DIR (absolute) and REPO to be set before use.

REPORT_GATES=()           # gate numbers in run order
declare -A GATE_TITLE=()  # n -> title
declare -A GATE_STATUS=() # n -> PASS | FAIL | NOT IMPLEMENTED
declare -A GATE_COUNTS=() # n -> "x passed, y failed"
CUR_GATE=
CUR_GATE_PASSED=0
CUR_GATE_FAILED=0

report_init() {
    mkdir -p "$RUN_DIR/logs" "$RUN_DIR/.report"
    : >"$RUN_DIR/.report/failures"
    : >"$RUN_DIR/.report/warnings"
}

# Items: one line per finding, written by the running check.
_item() {
    local kind=$1
    shift
    local text="$*"
    printf '%s\t%s\n' "$kind" "${text//$'\n'/ }" >>"${VALIDATE_ITEMS:?item_$kind used outside run_check}"
}
item_fail() { _item fail "$@"; }
item_warn() { _item warn "$@"; }
item_note() { _item note "$@"; }

_progress() { printf '%s\n' "$*" >&2; }

_duration() {
    local s=$1
    if ((s >= 60)); then printf '%dm %02ds' $((s / 60)) $((s % 60)); else printf '%ds' "$s"; fi
}

gate_begin() {
    CUR_GATE=$1
    REPORT_GATES+=("$1")
    GATE_TITLE[$1]=$2
    CUR_GATE_PASSED=0
    CUR_GATE_FAILED=0
    printf '## Gate %s: %s\n\n' "$1" "$2" >"$RUN_DIR/.report/gate$1.md"
    _progress "== Gate $1: $2"
}

run_check() {
    local slug=$1 title=$2
    shift 2
    local log="$RUN_DIR/logs/gate$CUR_GATE-$slug.log"
    local items="$RUN_DIR/.report/gate$CUR_GATE-$slug.items"
    local body="$RUN_DIR/.report/gate$CUR_GATE.md"
    : >"$items"
    _progress "   $title ... (log: ${log#"$REPO"/})"
    local start=$SECONDS rc
    (
        export VALIDATE_ITEMS="$items" CHECK_LOG="$log"
        cd "$REPO" && "$@"
    ) >"$log" 2>&1
    rc=$?
    local took
    took=$(_duration $((SECONDS - start)))

    local status=PASS
    if ((rc != 0)) || grep -q $'^fail\t' "$items"; then
        status=FAIL
    fi
    if ((rc != 0)) && ! grep -q $'^fail\t' "$items"; then
        printf 'fail\t%s exited with status %d; see the log\n' "$title" "$rc" >>"$items"
    fi
    if [[ $status == PASS ]]; then
        CUR_GATE_PASSED=$((CUR_GATE_PASSED + 1))
    else
        CUR_GATE_FAILED=$((CUR_GATE_FAILED + 1))
    fi
    _progress "   $title: $status ($took)"

    {
        printf -- '- **%s** %s (%s). Log: `%s`\n' "$status" "$title" "$took" "${log#"$REPO"/}"
        local kind text
        while IFS=$'\t' read -r kind text; do
            case $kind in
            fail)
                printf -- '  - failure: %s\n' "$text"
                printf -- '- gate %s, %s: %s\n' "$CUR_GATE" "$title" "$text" >>"$RUN_DIR/.report/failures"
                ;;
            warn)
                printf -- '  - warning: %s\n' "$text"
                printf -- '- gate %s, %s: %s\n' "$CUR_GATE" "$title" "$text" >>"$RUN_DIR/.report/warnings"
                ;;
            *) printf -- '  - %s\n' "$text" ;;
            esac
        done <"$items"
    } >>"$body"
    return 0
}

gate_end() {
    local n=$CUR_GATE
    if ((CUR_GATE_FAILED > 0)); then GATE_STATUS[$n]=FAIL; else GATE_STATUS[$n]=PASS; fi
    GATE_COUNTS[$n]="$CUR_GATE_PASSED passed, $CUR_GATE_FAILED failed"
    printf '\n' >>"$RUN_DIR/.report/gate$n.md"
    _progress "== Gate $n: ${GATE_STATUS[$n]}"
}

gate_not_implemented() {
    local n=$1 title=$2 text=$3
    REPORT_GATES+=("$n")
    GATE_TITLE[$n]=$title
    GATE_STATUS[$n]="NOT IMPLEMENTED"
    GATE_COUNTS[$n]="none run"
    printf '## Gate %s: %s\n\n**NOT IMPLEMENTED**: %s This gate neither passes nor fails yet.\n\n' \
        "$n" "$title" "$text" >"$RUN_DIR/.report/gate$n.md"
    _progress "== Gate $n: $title: NOT IMPLEMENTED"
}

# After an interrupt: the running gate becomes INTERRUPTED, and each gate in
# <all-gates> that never started becomes NOT RUN, so the report lists every gate.
report_mark_interrupted() {
    local n
    if [[ -n $CUR_GATE && -z ${GATE_STATUS[$CUR_GATE]:-} ]]; then
        GATE_STATUS[$CUR_GATE]=INTERRUPTED
        GATE_COUNTS[$CUR_GATE]="$CUR_GATE_PASSED passed, $CUR_GATE_FAILED failed before the interrupt"
        printf -- '- **INTERRUPTED** while a check of this gate was running.\n\n' \
            >>"$RUN_DIR/.report/gate$CUR_GATE.md"
    fi
    for n in "$@"; do
        [[ -n ${GATE_STATUS[$n]:-} ]] && continue
        REPORT_GATES+=("$n")
        GATE_TITLE[$n]="(not reached)"
        GATE_STATUS[$n]="NOT RUN"
        GATE_COUNTS[$n]="none run"
        printf '## Gate %s\n\n**NOT RUN**: the validate command was interrupted first.\n\n' \
            "$n" >"$RUN_DIR/.report/gate$n.md"
    done
}

# Overall result: FAIL if any gate failed, else PASS.
report_failed_gates() {
    local n out=()
    for n in "${REPORT_GATES[@]}"; do
        [[ ${GATE_STATUS[$n]:-} == FAIL ]] && out+=("$n")
    done
    echo "${out[*]}"
}

# report_write <file> <header-markdown> <footer-markdown> [interrupted]
report_write() {
    local file=$1 header=$2 footer=$3 interrupted=${4:-}
    local failed n result
    failed=$(report_failed_gates)
    if [[ -n $interrupted ]]; then
        result="FAIL (interrupted; gates after the running one did not run)"
    elif [[ $failed == *' '* ]]; then
        result="FAIL (gates ${failed// /, } failed)"
    elif [[ -n $failed ]]; then
        result="FAIL (gate $failed failed)"
    else
        result=PASS
        local ni=()
        for n in "${REPORT_GATES[@]}"; do
            [[ ${GATE_STATUS[$n]} == "NOT IMPLEMENTED" ]] && ni+=("$n")
        done
        if ((${#ni[@]})); then
            local list="${ni[*]}"
            if ((${#ni[@]} > 1)); then
                result+=" (gates ${list// /, } not yet implemented)"
            else
                result+=" (gate $list not yet implemented)"
            fi
        fi
    fi
    local nfail nwarn
    nfail=$(wc -l <"$RUN_DIR/.report/failures")
    nwarn=$(wc -l <"$RUN_DIR/.report/warnings")
    {
        printf '# Validation report\n\nResult: %s\n\n' "$result"
        printf '%s\n\n' "$header"
        printf '## Summary\n\n| Gate | Name | Result | Checks |\n|------|------|--------|--------|\n'
        for n in "${REPORT_GATES[@]}"; do
            printf '| %s | %s | %s | %s |\n' "$n" "${GATE_TITLE[$n]}" "${GATE_STATUS[$n]}" "${GATE_COUNTS[$n]}"
        done
        printf '\nFailures: %d. Warnings: %d.\n\n' "$nfail" "$nwarn"
        for n in "${REPORT_GATES[@]}"; do
            cat "$RUN_DIR/.report/gate$n.md"
        done
        printf '## All failures (%d)\n\n' "$nfail"
        if ((nfail)); then cat "$RUN_DIR/.report/failures"; else echo "None."; fi
        printf '\n## All warnings (%d)\n\n' "$nwarn"
        if ((nwarn)); then cat "$RUN_DIR/.report/warnings"; else echo "None."; fi
        printf '\n%s\n' "$footer"
    } >"$file"
}
