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

# A header has no compile command and may not compile on its own, so changed
# headers are linted through a source that includes them (tidy_headers.py);
# changed sources go to clang-tidy-diff.
GATE5_HEADER_ERE='\.(h|hh|hpp|hxx|inc)$'

# Appends the -U0 diff of each named file against MERGE_BASE (tracked) or
# /dev/null (untracked) to the file $1.
gate5_diff() {
    local out=$1 kind=$2 f
    shift 2
    if [[ $kind == tracked ]]; then
        (($#)) || return 0
        git diff -U0 --no-color --no-ext-diff --src-prefix=a/ --dst-prefix=b/ \
            "$MERGE_BASE" -- "$@" >>"$out"
        return 0
    fi
    for f in "$@"; do
        git diff -U0 --no-color --no-ext-diff --src-prefix=a/ --dst-prefix=b/ \
            --no-index /dev/null "$f" >>"$out"
    done
    return 0
}

gate5_tidy() {
    local tracked=() untracked=() f
    local diff="$RUN_DIR/.report/tidy.diff" hdiff="$RUN_DIR/.report/tidy-headers.diff"
    local src_t=() src_u=() hdr_t=() hdr_u=()
    mapfile -d '' -t tracked < <(changes_filter "$C_CXX_ERE" CHANGED_TRACKED)
    mapfile -d '' -t untracked < <(changes_filter "$C_CXX_ERE" CHANGED_UNTRACKED)
    for f in "${tracked[@]}"; do
        if [[ $f =~ $GATE5_HEADER_ERE ]]; then hdr_t+=("$f"); else src_t+=("$f"); fi
    done
    for f in "${untracked[@]}"; do
        if [[ $f =~ $GATE5_HEADER_ERE ]]; then hdr_u+=("$f"); else src_u+=("$f"); fi
    done
    : >"$diff"
    : >"$hdiff"
    gate5_diff "$diff" tracked "${src_t[@]}"
    gate5_diff "$diff" untracked "${src_u[@]}"
    gate5_diff "$hdiff" tracked "${hdr_t[@]}"
    gate5_diff "$hdiff" untracked "${hdr_u[@]}"
    if [[ ! -s $diff && ! -s $hdiff ]]; then
        item_note "no changed C/C++ lines"
        return 0
    fi
    # After a failed build the database is partial and keeps relative paths,
    # which clang-tidy-diff's line filter never matches: a pass would be empty.
    [[ -f $BUILD_OK && -f compile_commands.json ]] || {
        item_fail "not run: the gate 1 build failed, so compile_commands.json is incomplete"
        return 1
    }
    local rc=0 hrc=0 out="" hout=""
    if [[ -s $diff ]]; then
        out=$(clang-tidy-diff -p1 -path "$REPO" -j "$JOBS" -quiet <"$diff" 2>&1) || rc=$?
        printf '%s\n' "$out"
    fi
    if [[ -s $hdiff ]]; then
        hout=$(python3 "$VALIDATE_LIB/tidy_headers.py" --repo "$REPO" \
            --items "$VALIDATE_ITEMS" --diff "$hdiff" 2>&1) || hrc=$?
        printf '%s\n' "$hout"
    fi
    local n=0 path rest line
    while IFS= read -r line; do
        [[ $line =~ ^([^:]+):([0-9]+):([0-9]+):\ (warning|error):\ (.*)$ ]] || continue
        path=${BASH_REMATCH[1]#"$REPO"/}
        rest="${BASH_REMATCH[4]}: ${BASH_REMATCH[5]}"
        item_fail "clang-tidy: $path:${BASH_REMATCH[2]}: $rest"
        n=$((n + 1))
    done <<<"$out"$'\n'"$hout"
    if ((rc != 0 && n == 0)); then
        item_fail "clang-tidy-diff failed (exit $rc); see the log"
    fi
    if ((hrc != 0 && n == 0)); then
        item_fail "clang-tidy on changed headers failed (exit $hrc); see the log"
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
