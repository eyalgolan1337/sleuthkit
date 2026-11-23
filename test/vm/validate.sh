#!/usr/bin/env bash
# The validate command: one pass or fail for the current working tree.
# See test/vm/README.md. Run from anywhere, on the host or in the devcontainer.
#
#   test/vm/validate.sh [--base REF] [--fresh] [--keep-running]
#
# On the host, it first collects the Validation VM's results for gate 3 (the
# static guest build and the Host-side run's harness in the devcontainer, then
# test/vm/collect-vm.sh), then
# runs every gate in the devcontainer. Every gate runs even if an earlier one
# fails. The report goes to build/validate/<UTC time>/report.md
# (build/validate/latest/report.md) and is printed at the end. Exit status: 0 if no gate failed, 1 if a gate failed,
# 2 if the command itself could not run.
set -uo pipefail

VALIDATE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
VALIDATE_LIB=$VALIDATE_DIR/lib
REPO=$(cd "$VALIDATE_DIR/../.." && pwd)
DEVCONTAINER_CLI_VERSION=0.91.0
# shellcheck source=lib/report.sh
source "$VALIDATE_LIB/report.sh"

WINDOWS_NOTE="**Windows (MSVC) build: NOT VERIFIED.** This command builds and tests with GCC 16.1 on Linux only. MSVC 19.41 (Visual Studio 2022, \`win32/\`) was not run, so a pass here says nothing about the Windows build."

# The gate numbers, from test/vm/gates/gate<n>.sh, in order.
list_gates() {
    local g
    for g in "$VALIDATE_DIR"/gates/gate*.sh; do
        g=${g##*/gate}
        printf '%s\n' "${g%.sh}"
    done | sort -V
}

usage() {
    cat <<EOF
Usage: test/vm/validate.sh [options]

Builds and checks the current working tree in the devcontainer and writes
build/validate/latest/report.md. Exits 0 if no gate failed, 1 if a gate
failed, 2 if validation could not run.

Options:
  --base REF   compare with the merge-base of REF and HEAD (default: develop)
  --fresh      re-run bootstrap and configure before building
  --keep-running
               leave the Validation VM running afterwards (by default the
               command starts it if needed and stops it when done with it)
  -h, --help   show this help
EOF
}

BASE=develop
VALIDATE_FRESH=
KEEP_RUNNING=
ARGS=("$@")
while (($#)); do
    case $1 in
    --base)
        [[ $# -ge 2 ]] || { usage >&2; exit 2; }
        BASE=$2
        shift 2
        ;;
    --fresh) VALIDATE_FRESH=1; shift ;;
    --keep-running) KEEP_RUNNING=1; shift ;;
    -h | --help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

in_devcontainer() {
    [[ -f /.dockerenv ]] && gcc --version 2>/dev/null | head -n1 | grep -q ' 16\.1\.'
}

# The devcontainers CLI, 0.91.0 or newer (older ones hang on Docker 29+).
devcontainer_cli() {
    local v
    if command -v devcontainer >/dev/null; then
        v=$(devcontainer --version 2>/dev/null)
        if [[ $(printf '%s\n%s\n' "$DEVCONTAINER_CLI_VERSION" "$v" | sort -V | head -n1) == "$DEVCONTAINER_CLI_VERSION" ]]; then
            devcontainer "$@"
            return
        fi
    fi
    command -v npx >/dev/null || {
        echo "error: need the devcontainers CLI >= $DEVCONTAINER_CLI_VERSION or npx (Node.js) to reach the devcontainer" >&2
        return 2
    }
    npx --yes "@devcontainers/cli@$DEVCONTAINER_CLI_VERSION" "$@"
}

# Print the folder whose devcontainer must run this checkout. For the main
# checkout that is $REPO. For a linked git worktree it is the main checkout: the
# worktree's .git is only a file pointing into the main checkout's .git, and the
# devcontainers CLI keys containers by folder, so opening the worktree folder
# would start a second container that mounts the worktree alone and can't run
# git. The main checkout's container mounts it at the same path, so a worktree
# inside it is visible there. Fails for a worktree outside the main checkout.
devcontainer_folder() {
    local common main
    if ! common=$(git -C "$REPO" rev-parse --path-format=absolute --git-common-dir 2>/dev/null) ||
        [[ ${common##*/} != .git ]]; then
        printf '%s\n' "$REPO" # not a git checkout, or an unusual layout: use the folder itself
        return
    fi
    main=${common%/.git}
    if [[ $REPO != "$main" && $REPO != "$main"/* ]]; then
        echo "error: this git worktree ($REPO) is outside the main checkout ($main)," \
            "so the main checkout's devcontainer can't see it. Use a worktree inside $main" \
            "(for example under .claude/worktrees/), or run this command inside a devcontainer" \
            "that mounts this worktree." >&2
        return 2
    fi
    printf '%s\n' "$main"
}

# Creates build/validate/<UTC time>/ (or <time>-2, ... for two runs started in
# the same second) and sets RUN_NAME and RUN_DIR. TSK_VALIDATE_RUN_NAME names
# a run directory the host already created.
make_run_dir() {
    mkdir -p "$REPO/build/validate"
    if [[ -n ${TSK_VALIDATE_RUN_NAME:-} ]]; then
        RUN_NAME=$TSK_VALIDATE_RUN_NAME
        mkdir -p "$REPO/build/validate/$RUN_NAME"
    else
        local stem n=1
        RUN_NAME=$(date -u +%Y%m%dT%H%M%SZ)
        stem=$RUN_NAME
        while ! mkdir "$REPO/build/validate/$RUN_NAME" 2>/dev/null; do
            n=$((n + 1))
            RUN_NAME=$stem-$n
        done
    fi
    RUN_DIR=$REPO/build/validate/$RUN_NAME
    mkdir -p "$RUN_DIR/logs"
    ln -sfn "$RUN_NAME" "$REPO/build/validate/latest"
}

# Interrupted on the host before any gate ran: a report with every gate NOT RUN.
host_interrupted() {
    trap - INT TERM
    echo "Interrupted; writing a partial report." >&2
    report_init
    local gates=()
    mapfile -t gates < <(list_gates)
    report_mark_interrupted "${gates[@]}"
    report_write "$RUN_DIR/report.md" \
        "- Interrupted on the host while collecting the Validation VM's results, before any gate ran. Logs: \`build/validate/$RUN_NAME/logs/\`

$WINDOWS_NOTE" \
        "" interrupted
    cat "$RUN_DIR/report.md"
    exit 130
}

# On the host, before the gates: the Validation VM is reachable only from
# here (vmrun, its SSH key), so its results are collected now and gate 3
# compares them in the devcontainer. The static guest build (Guest-side runs)
# and the normal build's harness (Host-side runs, in one-off containers of the
# devcontainer's image) are built in the devcontainer first. Failures here
# don't stop the command: gate 3 reports them.
host_collect_vm() {
    make_run_dir
    trap host_interrupted INT TERM
    local fresh=() keep=() rc
    [[ -n $VALIDATE_FRESH ]] && fresh=(--fresh)
    [[ -n $KEEP_RUNNING ]] && keep=(--keep-running)
    echo "Building the static guest tools in the devcontainer (log: build/validate/$RUN_NAME/logs/vm-guest-build.log)" >&2
    devcontainer_cli exec --workspace-folder "$WORKSPACE" \
        "$REPO/test/vm/guest-build.sh" "${fresh[@]}" >"$RUN_DIR/logs/vm-guest-build.log" 2>&1
    echo "Building the Host-side run's harness in the devcontainer (log: build/validate/$RUN_NAME/logs/vm-host-build.log)" >&2
    devcontainer_cli exec --workspace-folder "$WORKSPACE" \
        "$REPO/test/vm/host-build.sh" "${fresh[@]}" >"$RUN_DIR/logs/vm-host-build.log" 2>&1
    # Gate 1 reads it: after a successful host-build.sh the tree is configured.
    echo $? >"$RUN_DIR/logs/vm-host-build.status"
    echo "Collecting the Validation VM's results for gate 3 (log: build/validate/$RUN_NAME/logs/vm-collect.log)" >&2
    # The one-off containers of the Host-side runs use the image of this
    # devcontainer (in a git worktree, the main checkout's).
    TSK_VALIDATE_WORKSPACE=$WORKSPACE "$VALIDATE_DIR/collect-vm.sh" "$RUN_DIR" "${keep[@]}" \
        >"$RUN_DIR/logs/vm-collect.log" 2>&1
    rc=$?
    ((rc == 130)) && host_interrupted
    trap - INT TERM
}

# On the host: never build here. Start the devcontainer if needed and re-run
# this command inside it. The repo is mounted at the same path in both.
if ! in_devcontainer; then
    if [[ -n ${TSK_VALIDATE_DELEGATED:-} ]]; then
        echo "error: delegated to the devcontainer, but this is not it (no /.dockerenv or gcc is not 16.1)" >&2
        exit 2
    fi
    WORKSPACE=$(devcontainer_folder) || exit 2
    echo "Delegating to the devcontainer (this machine is not the devcontainer; nothing is built here)." >&2
    if [[ $WORKSPACE != "$REPO" ]]; then
        echo "This is a git worktree: using the devcontainer of the main checkout, $WORKSPACE." >&2
    fi
    up_log=$(mktemp)
    if ! devcontainer_cli up --workspace-folder "$WORKSPACE" >"$up_log" 2>&1; then
        echo "error: could not start the devcontainer; last lines of 'devcontainer up':" >&2
        tail -n 20 "$up_log" >&2
        rm -f "$up_log"
        exit 2
    fi
    rm -f "$up_log"
    host_collect_vm
    devcontainer_cli exec --workspace-folder "$WORKSPACE" \
        env TSK_VALIDATE_DELEGATED=1 TSK_VALIDATE_RUN_NAME="$RUN_NAME" \
        TSK_VALIDATE_VM_COLLECTED=1 "$REPO/test/vm/validate.sh" "${ARGS[@]}"
    rc=$?
    case $rc in
    0 | 1 | 2 | 130) exit "$rc" ;;
    *)
        echo "error: 'devcontainer exec' failed (exit $rc)" >&2
        exit 2
        ;;
    esac
fi

# --- In the devcontainer from here on. ---
cd "$REPO" || exit 2
# shellcheck source=lib/changes.sh
source "$VALIDATE_LIB/changes.sh"
mapfile -t ALL_GATES < <(list_gates)
for n in "${ALL_GATES[@]}"; do
    # shellcheck source=/dev/null
    source "$VALIDATE_DIR/gates/gate$n.sh"
done

JOBS=$(nproc)
STARTED=$(date -u +%Y-%m-%dT%H:%M:%SZ)
make_run_dir
REPORT=$RUN_DIR/report.md
BUILD_OK=$RUN_DIR/.report/build-ok
export REPO RUN_DIR JOBS VALIDATE_LIB VALIDATE_FRESH BUILD_OK

changes_init "$BASE" || exit 2
export BASE_REF MERGE_BASE
report_init

dirty=no
[[ -n $(git status --porcelain --untracked-files=normal) ]] && dirty=yes
changed_list=$(printf -- '- `%s`\n' "${CHANGED_TRACKED[@]}")
untracked_list=$(printf -- '- `%s` (untracked)\n' "${CHANGED_UNTRACKED[@]}")
nchanged=$((${#CHANGED_TRACKED[@]} + ${#CHANGED_UNTRACKED[@]}))
if ((${#CHANGED_TRACKED[@]} == 0)); then changed_list=; fi
if ((${#CHANGED_UNTRACKED[@]} == 0)); then untracked_list=; fi
if [[ -n ${TSK_VALIDATE_DELEGATED:-} ]]; then
    ran_in="the devcontainer, delegated from the host"
else
    ran_in="the devcontainer"
fi

if [[ -n ${TSK_VALIDATE_VM_COLLECTED:-} ]]; then
    vm_line="its results were collected on the host before the gates (\`logs/vm-collect.log\`) and are checked in gate 3, which also says whether the collection succeeded"
else
    vm_line="not used: this run started inside the devcontainer, which can't reach the VM, so gate 3 fails. Run \`test/vm/validate.sh\` on the host."
fi

HEADER="- Started: $STARTED
- Tree: \`$REPO\`, branch \`$(git branch --show-current)\`, HEAD \`$(git rev-parse --short HEAD)\`, uncommitted or untracked changes: $dirty
- Base: \`$BASE_REF\`, merge-base \`$(git rev-parse --short "$MERGE_BASE")\`; $nchanged changed file(s), listed at the end
- Ran in: $ran_in ($(gcc --version | head -n1 | sed 's/^gcc ([^)]*) /GCC /'), $(clang-format --version | grep -oE 'clang-format version [0-9.]+'), $(clang-tidy --version | grep -oE 'LLVM version [0-9.]+' | sed 's/LLVM/clang-tidy/'))
- Logs: \`build/validate/$RUN_NAME/logs/\`
- Validation VM: $vm_line

$WINDOWS_NOTE"

FOOTER="## Changed files since \`$BASE_REF\` ($nchanged)

${changed_list}${changed_list:+$'\n'}${untracked_list}"
[[ $nchanged -eq 0 ]] && FOOTER+="None."

finish() {
    local interrupted=${1:-}
    report_write "$REPORT" "$HEADER" "$FOOTER" "$interrupted"
    echo >&2
    cat "$REPORT"
    echo
    echo "Report: ${REPORT#"$REPO"/} (also build/validate/latest/report.md)"
}

on_interrupt() {
    trap - INT TERM
    echo "Interrupted; writing a partial report." >&2
    report_mark_interrupted "${ALL_GATES[@]}"
    finish interrupted
    exit 130
}
trap on_interrupt INT TERM

echo "Validating $REPO against $BASE_REF; report: ${REPORT#"$REPO"/}" >&2
for n in "${ALL_GATES[@]}"; do
    "gate_$n"
done

finish
[[ -z $(report_failed_gates) ]]
exit $?
