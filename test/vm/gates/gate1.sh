# shellcheck shell=bash
# Gate 1: devcontainer build and C++17/C++20 syntax check of changed C++ files.
# Later tickets add the static guest build and the comparison self-test here.

gate1_build() {
    local fresh=$VALIDATE_FRESH compiler
    # A tree configured by another compiler (the host's) must not be mixed
    # with a container build: start it over, as post-create.sh does.
    if [[ -f config.status ]]; then
        compiler=$(grep -m1 'gcc version' config.log 2>/dev/null)
        if [[ $compiler != *'gcc version 16.1.'* ]]; then
            item_warn "the build tree was configured with another compiler (${compiler:-unknown}); ran make distclean and configured it again"
            make -s distclean || true
            fresh=1
        fi
    fi
    local rc=0
    if [[ -n $fresh ]]; then
        .devcontainer/build-compile-db.sh --fresh || rc=$?
    else
        .devcontainer/build-compile-db.sh || rc=$?
    fi
    local changed=()
    mapfile -d '' -t changed < <(changes_filter "$C_CXX_ERE" CHANGED_TRACKED CHANGED_UNTRACKED)
    python3 "$VALIDATE_LIB/build_log.py" --log "$CHECK_LOG" --repo "$REPO" \
        --items "$VALIDATE_ITEMS" -- "${changed[@]}"
    # compile_commands.json is complete and usable only after a successful build.
    ((rc == 0)) && touch "$BUILD_OK"
    return "$rc"
}

gate1_cxx_std() {
    local std=$1 changed=() incomplete=()
    mapfile -d '' -t changed < <(changes_filter "$C_CXX_ERE" CHANGED_TRACKED CHANGED_UNTRACKED)
    # After a failed build a missing compile command means "unknown", not "not built".
    [[ -f $BUILD_OK ]] || incomplete=(--db-incomplete)
    python3 "$VALIDATE_LIB/cxx_std_check.py" --std "$std" --repo "$REPO" \
        --items "$VALIDATE_ITEMS" --jobs "$JOBS" "${incomplete[@]}" -- "${changed[@]}"
}

gate_1() {
    gate_begin 1 "Build and C++17/C++20 syntax"
    run_check build "Build (configure if needed, make, check programs, compile_commands.json)" gate1_build
    run_check cxx17 "C++17 syntax-only compile of changed C++ files" gate1_cxx_std c++17
    run_check cxx20 "C++20 syntax-only compile of changed C++ files" gate1_cxx_std c++20
    gate_end
}
