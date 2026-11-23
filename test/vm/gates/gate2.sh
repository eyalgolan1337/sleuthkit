# shellcheck shell=bash
# Gate 2: the full `make check`.

gate2_make_check() {
    local marker="$RUN_DIR/.report/make-check-start"
    touch "$marker"
    local rc=0
    # -k: a failure in one directory must not keep the tests of later ones from running.
    make -k -j"$JOBS" check || rc=$?

    # One .trs file per test, written by automake's test driver in this run.
    local trs name result ntests=0 nbad=0
    while IFS= read -r -d '' trs; do
        ntests=$((ntests + 1))
        name=${trs#./}
        name=${name%.trs}
        result=$(sed -n 's/^:global-test-result: //p' "$trs")
        case $result in
        PASS) item_note "PASS $name" ;;
        XFAIL) item_note "XFAIL $name (expected failure)" ;;
        SKIP) item_warn "SKIP $name (the test skipped itself; see $name.log)" ;;
        *)
            nbad=$((nbad + 1))
            item_fail "${result:-NO RESULT} $name (see $name.log)"
            ;;
        esac
    done < <(find . -path ./build -prune -o -name '*.trs' -newer "$marker" -print0 | sort -z)

    if ((ntests == 0)); then
        item_fail "no test ran (make check exited $rc before running tests); see the log"
    elif ((rc != 0 && nbad == 0)); then
        item_fail "make check exited $rc although every test that ran passed; see the log"
    fi
    return "$rc"
}

gate_2() {
    gate_begin 2 "make check"
    run_check make-check "Full make check" gate2_make_check
    gate_end
}
