# shellcheck shell=bash
# Gate 1: devcontainer build, C++17/C++20 syntax check of changed C++ files,
# and the self-tests of the comparison step, Ground truth capture and the
# harness, and the static guest build for the Validation VM.

gate1_build() {
    local fresh=$VALIDATE_FRESH compiler
    # On the host, test/vm/host-build.sh already built this tree for the
    # Host-side runs, configuring it afresh with --fresh or when it was
    # configured by another compiler. Report that here; don't do it twice.
    local hb_log=$RUN_DIR/logs/vm-host-build.log
    if [[ -n ${TSK_VALIDATE_VM_COLLECTED:-} && $(cat "$RUN_DIR/logs/vm-host-build.status" 2>/dev/null) == 0 ]]; then
        fresh=
        if grep -q '^== the tree was configured by another compiler' "$hb_log"; then
            item_warn "the build tree was configured with another compiler; test/vm/host-build.sh ran make distclean and configured it again (logs/vm-host-build.log)"
        fi
    fi
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

# Runs a CppUnit self-test binary and reports each failed case.
run_cppunit_selftest() {
    local selftest=$1 out rc=0 name n
    out=$("$selftest" 2>&1) || rc=$?
    printf '%s\n' "$out"
    if ((rc == 0)); then
        n=$(sed -nE 's/^OK \(([0-9]+)\)$/\1/p' <<<"$out")
        item_note "$(basename "$selftest"): ${n:-all} cases passed"
        return 0
    fi
    while IFS= read -r name; do
        item_fail "self-test case failed: ${name#Test name: }"
    done < <(grep '^Test name: ' <<<"$out")
    grep -q '^Test name: ' <<<"$out" || item_fail "$selftest exited with status $rc; see the log"
    return 1
}

# The comparison step's self-test: hand-made Manifests, no Validation VM
# (test/vm/manifest/selftest.cpp), plus the SHA-256 and Ground truth capture
# cases linked into the same binary. The build check already built it; make
# only rebuilds what is out of date, so this check also stands on its own.
gate1_manifest_selftest() {
    if ! make -j"$JOBS" -f test/vm/manifest/manifest.mk all; then
        item_fail "could not build the validation setup's C++ tools; see the log"
        return 1
    fi
    run_cppunit_selftest build/vm-tools/manifest-selftest
}

# The harness: its CppUnit self-test, then the end-to-end check on an ext4
# image (test/vm/harness/selftest.sh, which reports its own items).
gate1_harness_selftest() {
    local rc=0
    if ! make -j"$JOBS" -f test/vm/manifest/manifest.mk all; then
        item_fail "could not build the harness; see the log"
        return 1
    fi
    run_cppunit_selftest build/vm-tools/harness-selftest || rc=1
    test/vm/harness/selftest.sh || rc=1
    return "$rc"
}

# The static guest build (test/vm/guest-build.sh). On the host, the validate
# command already ran it before collecting the VM's results (gate 3), so this
# is then an incremental rebuild of the same tree, and reports its result.
gate1_guest_build() {
    local fresh=()
    [[ -n $VALIDATE_FRESH && -z ${TSK_VALIDATE_VM_COLLECTED:-} ]] && fresh=(--fresh)
    if ! test/vm/guest-build.sh "${fresh[@]}"; then
        item_fail "the static guest build failed; see the log"
        return 1
    fi
    item_note "built fls, icat, fsstat, istat, ils, ifind, blkls, tsk-harness, capture-ground-truth, tsk-corpus and check-tool, statically linked, in build/guest-static/bin"
}

# The Known failure list as a whole. Each comparison of gates 3 and 4 reads
# only its own lines, so a line no comparison reads (a misspelled check, a
# partition no longer checked) could never be found stale: it fails here.
gate1_known_failures() {
    # shellcheck source=../lib/vm_checks.sh
    source "$VALIDATE_LIB/vm_checks.sh"
    if ! make -s -j"$JOBS" -f test/vm/manifest/manifest.mk build/vm-tools/compare-manifests; then
        item_fail "could not build compare-manifests; see the log"
        return 1
    fi
    build/vm-tools/compare-manifests --lint-known-failures "$KNOWN_FAILURES" \
        --comparisons "$(vm_comparisons)" --items "$VALIDATE_ITEMS"
}

# The committed XFS fixture images (test/vm/fixtures/xfs/): each image must
# still match its expected Manifest under the comparison step's Strict rules.
gate1_xfs_fixtures() {
    local out rc=0 line
    out=$(test/vm/fixtures/xfs/xfs-fixtures.py verify 2>&1) || rc=$?
    printf '%s\n' "$out"
    if ((rc == 0)); then
        item_note "$(tail -n1 <<<"$out")"
        return 0
    fi
    while IFS= read -r line; do
        item_fail "${line#FAIL: }"
    done < <(grep '^FAIL: ' <<<"$out")
    grep -q '^FAIL: ' <<<"$out" || item_fail "xfs-fixtures.py verify exited with status $rc; see the log"
    return 1
}

gate_1() {
    gate_begin 1 "Build, C++17/C++20 syntax and self-tests"
    run_check build "Build (configure if needed, make, check programs, compile_commands.json)" gate1_build
    run_check guest-build "Static guest build (build/guest-static)" gate1_guest_build
    run_check cxx17 "C++17 syntax-only compile of changed C++ files" gate1_cxx_std c++17
    run_check cxx20 "C++20 syntax-only compile of changed C++ files" gate1_cxx_std c++20
    run_check manifest-selftest "Comparison step and Ground truth capture self-test" gate1_manifest_selftest
    run_check harness-selftest "Harness self-test (ext4 image: raw, offset, .vmdk, Ground truth)" gate1_harness_selftest
    run_check known-failures "Known failure list: every line belongs to a comparison of gates 3 and 4" gate1_known_failures
    run_check xfs-fixtures "XFS fixture images match their expected Manifests" gate1_xfs_fixtures
    gate_end
}
