# shellcheck shell=bash
# Gate 1: devcontainer build, C++17/C++20 syntax check of changed C++ files,
# and the comparison step's self-test. A later ticket adds the static guest
# build here.

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

# The comparison step's self-test: hand-made Manifests, no Validation VM
# (test/vm/manifest/selftest.cpp). The build check already built it; make
# only rebuilds what is out of date, so this check also stands on its own.
gate1_manifest_selftest() {
    local selftest=build/vm-tools/manifest-selftest out rc=0 name
    if ! make -j"$JOBS" -f test/vm/manifest/manifest.mk all; then
        item_fail "could not build the comparison step and its self-test; see the log"
        return 1
    fi
    out=$("$selftest" 2>&1) || rc=$?
    printf '%s\n' "$out"
    if ((rc == 0)); then
        local n
        n=$(sed -nE 's/^OK \(([0-9]+)\)$/\1/p' <<<"$out")
        item_note "${n:-all} self-test cases passed"
        return 0
    fi
    while IFS= read -r name; do
        item_fail "self-test case failed: ${name#Test name: }"
    done < <(grep '^Test name: ' <<<"$out")
    grep -q '^Test name: ' <<<"$out" || item_fail "$selftest exited with status $rc; see the log"
    return 1
}

gate_1() {
    gate_begin 1 "Build, C++17/C++20 syntax and comparison self-test"
    run_check build "Build (configure if needed, make, check programs, compile_commands.json)" gate1_build
    run_check cxx17 "C++17 syntax-only compile of changed C++ files" gate1_cxx_std c++17
    run_check cxx20 "C++20 syntax-only compile of changed C++ files" gate1_cxx_std c++20
    run_check manifest-selftest "Comparison step self-test (hand-made Manifests)" gate1_manifest_selftest
    gate_end
}
