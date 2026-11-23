# shellcheck shell=bash
# Gate 5: clang-format and clang-tidy on changed lines only.

gate5_format() {
    local tracked=() untracked=() f
    mapfile -d '' -t tracked < <(changes_filter "$C_CXX_ERE" CHANGED_TRACKED)
    mapfile -d '' -t untracked < <(changes_filter "$C_CXX_ERE" CHANGED_UNTRACKED)
    if ((${#tracked[@]} + ${#untracked[@]} == 0)); then
        item_note "no changed C/C++ files"
        return 0
    fi
    local rc=0 out file line
    if ((${#tracked[@]})); then
        # Lines that differ between MERGE_BASE and the working tree.
        out=$(git clang-format --diff "$MERGE_BASE" -- "${tracked[@]}" 2>&1) || rc=$?
        printf '%s\n' "$out"
        if grep -q '^@@ ' <<<"$out"; then
            while IFS= read -r line; do
                case $line in
                '+++ b/'*) file=${line#+++ b/} ;;
                '@@ '*)
                    line=${line#*+}
                    item_fail "clang-format: $file:${line%%[, ]*}: changed lines are not formatted (fix: git clang-format $BASE_REF)"
                    ;;
                esac
            done <<<"$out"
        elif ((rc != 0)); then
            item_fail "git clang-format failed (exit $rc): $(head -n1 <<<"$out")"
        fi
    fi
    # Untracked files are new: every line is a changed line.
    for f in "${untracked[@]}"; do
        out=$(clang-format --dry-run --Werror "$f" 2>&1) && continue
        printf '%s\n' "$out"
        line=$(grep -m1 -oE '^[^:]+:[0-9]+' <<<"$out" || true)
        item_fail "clang-format: ${line:-$f}: new file is not formatted ($(grep -c 'clang-format-violations' <<<"$out") violation(s); fix: clang-format -i $f)"
    done
    item_note "checked $((${#tracked[@]} + ${#untracked[@]})) changed C/C++ file(s)"
}

gate5_tidy() {
    local tracked=() untracked=() f diff="$RUN_DIR/.report/tidy.diff"
    mapfile -d '' -t tracked < <(changes_filter "$C_CXX_ERE" CHANGED_TRACKED)
    mapfile -d '' -t untracked < <(changes_filter "$C_CXX_ERE" CHANGED_UNTRACKED)
    : >"$diff"
    if ((${#tracked[@]})); then
        git diff -U0 --no-color --no-ext-diff --src-prefix=a/ --dst-prefix=b/ \
            "$MERGE_BASE" -- "${tracked[@]}" >>"$diff"
    fi
    for f in "${untracked[@]}"; do
        git diff -U0 --no-color --no-ext-diff --src-prefix=a/ --dst-prefix=b/ \
            --no-index /dev/null "$f" >>"$diff"
    done
    if [[ ! -s $diff ]]; then
        item_note "no changed C/C++ lines"
        return 0
    fi
    # After a failed build the database is partial and keeps relative paths,
    # which clang-tidy-diff's line filter never matches: a pass would be empty.
    [[ -f $BUILD_OK && -f compile_commands.json ]] || {
        item_fail "not run: the gate 1 build failed, so compile_commands.json is incomplete"
        return 1
    }
    local rc=0 out
    out=$(clang-tidy-diff -p1 -path "$REPO" -j "$JOBS" -quiet <"$diff" 2>&1) || rc=$?
    printf '%s\n' "$out"
    local n=0 path rest
    while IFS= read -r line; do
        [[ $line =~ ^([^:]+):([0-9]+):([0-9]+):\ (warning|error):\ (.*)$ ]] || continue
        path=${BASH_REMATCH[1]#"$REPO"/}
        rest="${BASH_REMATCH[4]}: ${BASH_REMATCH[5]}"
        item_fail "clang-tidy: $path:${BASH_REMATCH[2]}: $rest"
        n=$((n + 1))
    done <<<"$out"
    if ((rc != 0 && n == 0)); then
        item_fail "clang-tidy-diff failed (exit $rc); see the log"
    fi
    item_note "clang-tidy reported $n diagnostic(s) on changed lines"
    return 0
}

gate_5() {
    gate_begin 5 "clang-format and clang-tidy on changed lines"
    run_check format "clang-format on changed lines" gate5_format
    run_check tidy "clang-tidy on changed lines" gate5_tidy
    gate_end
}
