#!/usr/bin/env bash
# Host side of the validate command's VM gates (test/vm/README.md, "Gate 3:
# Strict checks"). Runs on the host, never in the devcontainer: it needs vmrun
# and the VM's SSH key. It builds nothing; the static guest build
# (test/vm/guest-build.sh) must already be in build/guest-static/bin.
#
#   test/vm/collect-vm.sh RUN_DIR [--keep-running]
#
# It starts the Validation VM if it isn't running, copies the static guest
# build to it, and for each Strict check partition runs the Strict check
# sequence: generate the Test corpus, sync and freeze, capture Ground truth,
# flush the block-device cache and drop page caches, check the corpus's XFS
# formats, the Guest-side run on /dev/sdN, the Host-side run on the .vmdk,
# unfreeze. Results go to RUN_DIR/vm/ for gate 3, which compares them in the
# devcontainer. Then it stops the VM, unless --keep-running is given.
#
# The Host-side run uses the harness of the normal devcontainer build
# (build/vm-tools/tsk-harness; test/vm/host-build.sh builds it) in a one-off
# container of the devcontainer image (TSK_VALIDATE_IMAGE, or the image of the
# running devcontainer of the folder TSK_VALIDATE_WORKSPACE, default this
# checkout; validate.sh passes it), with the repository and the VM
# directory mounted read-only and only the partition's result directory
# writable. test/vm/host-run.sh runs in it.
#
# The file system is unfrozen on every exit path: after each sequence, from an
# EXIT trap on failures, timeouts and interrupts, and, if the host can't reach
# the guest any more, by a timer in the guest armed at each freeze.
#
# Exit status: 0 when it ran to the end (the checks themselves may have found
# problems; gate 3 reports them), 1 if it couldn't collect, 130 if interrupted.
#
# Test-only hooks (environment):
#   TSK_VALIDATE_BREAK=PART:PATH   after Ground truth, flip one byte of the file
#                                  PATH on PART's device, and put it back after
#                                  the Host-side run (e.g. sda6:/corpus/links/target.txt)
#   TSK_VALIDATE_FROZEN_PAUSE=N    wait N seconds in each frozen window
# Limits (seconds): TSK_VALIDATE_STEP_TIMEOUT (600) per guest step and per
# Host-side run,
# TSK_VALIDATE_FREEZE_LIMIT (1800) after which the guest unfreezes by itself.
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
# shellcheck source=vm-common.sh
. "$HERE/vm-common.sh"
# shellcheck source=lib/vm_checks.sh
. "$HERE/lib/vm_checks.sh"

RUN_DIR=${1:?usage: collect-vm.sh RUN_DIR [--keep-running]}
KEEP_RUNNING=
[[ ${2:-} == --keep-running ]] && KEEP_RUNNING=1
OUT=$RUN_DIR/vm
GUEST_DIR=/root/tsk-validate
GUEST=$GUEST_DIR/tsk-validate.sh
STEP_TIMEOUT=${TSK_VALIDATE_STEP_TIMEOUT:-600}
FREEZE_LIMIT=${TSK_VALIDATE_FREEZE_LIMIT:-1800}
BREAK=${TSK_VALIDATE_BREAK:-}
PAUSE=${TSK_VALIDATE_FROZEN_PAUSE:-}

rm -rf "$OUT"
mkdir -p "$OUT"
# Absolute: the one-off container mounts the result directories by path.
OUT=$(cd "$OUT" && pwd)
# status: one line, "ok" or "failed<TAB>reason"; vm.txt: what happened to the VM.
status() { printf '%s\n' "$*" >"$OUT/status"; }
vmnote() { printf '%s\n' "$*" >>"$OUT/vm.txt"; }
log() { printf '[collect] %s\n' "$*" >&2; }
status $'failed\tthe collection did not finish'

FROZEN_MNT=     # the mount point frozen right now, if any
BROKEN=         # the break hook flipped a byte that isn't back yet
HOST_CONTAINER= # the one-off container of a Host-side run, while it runs

GUEST_IP=
SSH_OPTS=()
mapfile -t SSH_OPTS < <(vm_ssh_opts)

# gssh <timeout> <command...>: run a command in the guest as root. ssh is
# timeout's direct child, so a timeout kills it, and it stays in the
# foreground process group, so Ctrl-C reaches it at once.
gssh() {
    local t=$1
    shift
    [[ -n $GUEST_IP ]] || GUEST_IP=$(vm_ip) || return 255
    timeout --foreground "$t" ssh "${SSH_OPTS[@]}" "root@$GUEST_IP" "$(printf '%q ' "$@")"
}

# run_guest_step <timeout> <tsk-validate.sh args...>
run_guest_step() { gssh "$1" "$GUEST" "${@:2}"; }

cleanup() {
    local rc=$?
    trap - EXIT
    # A second Ctrl-C must not cut the unfreeze short. ssh inherits the
    # ignored signals; each call is still bounded by its timeout.
    trap '' INT TERM
    if [[ -n $HOST_CONTAINER ]]; then
        log "removing the one-off container $HOST_CONTAINER"
        docker rm -f "$HOST_CONTAINER" >/dev/null 2>&1
    fi
    if [[ -n $BROKEN ]]; then
        log "putting back the byte the break hook changed"
        run_guest_step 120 restore || log "error: could not put back the byte; the next run recreates the corpus"
    fi
    if [[ -n $FROZEN_MNT ]]; then
        log "unfreezing $FROZEN_MNT"
        if run_guest_step 120 unfreeze "$FROZEN_MNT"; then
            vmnote "unfroze $FROZEN_MNT on exit (status $rc)"
        else
            log "error: could not unfreeze $FROZEN_MNT; the guest's timer unfreezes it within ${FREEZE_LIMIT}s"
            vmnote "could not unfreeze $FROZEN_MNT on exit; the guest's timer unfreezes it within ${FREEZE_LIMIT}s"
        fi
    fi
    if [[ -z $KEEP_RUNNING ]] && vm_is_running; then
        log "stopping the Validation VM"
        if "$HERE/vm.sh" stop; then vmnote "stopped the VM"; else vmnote "could not stop the VM"; fi
    elif [[ -n $KEEP_RUNNING ]]; then
        vmnote "left the VM running (--keep-running)"
    fi
    exit "$rc"
}
trap cleanup EXIT
trap 'log "interrupted"; exit 130' INT TERM

fail_collect() {
    status "failed"$'\t'"$*"
    log "error: $*"
    exit 1
}

# --- The static guest build and the VM. ---
for t in "${GUEST_TOOLS[@]}"; do
    [[ -x $REPO/build/guest-static/bin/$t ]] ||
        fail_collect "the static guest build has no $t (test/vm/guest-build.sh failed; see gate 1)"
done
[[ -f $TSK_VM_VMX ]] || fail_collect "no Validation VM at $TSK_VM_VMX (build it with test/vm/build-vm.sh)"
command -v vmrun >/dev/null || fail_collect "vmrun is not installed (VMware Workstation)"

was_running=
vm_is_running && was_running=1
"$HERE/vm.sh" up || fail_collect "the Validation VM did not start or SSH did not answer"
if [[ -n $was_running ]]; then vmnote "the VM was already running"; else vmnote "started the VM"; fi

log "copying the static guest build to $GUEST_DIR"
tar -C "$REPO/build/guest-static" -c bin -C "$HERE/guest" tsk-validate.sh |
    gssh "$STEP_TIMEOUT" sh -c "rm -rf $GUEST_DIR && mkdir -p $GUEST_DIR && tar -x -C $GUEST_DIR" ||
    fail_collect "could not copy the static guest build to the guest"

run_guest_step "$STEP_TIMEOUT" tools >"$OUT/tools.txt" 2>&1 ||
    fail_collect "the static guest tools don't run in the guest: $(grep -m1 FAIL "$OUT/tools.txt")"

# --- The Host-side run's one-off container. ---
HOST_HARNESS=$REPO/build/vm-tools/tsk-harness
HOST_IMAGE=${TSK_VALIDATE_IMAGE:-}
HOST_ERR= # why Host-side runs can't happen, if they can't
if ! command -v docker >/dev/null; then
    HOST_ERR="docker is not installed on the host"
elif [[ ! -x $HOST_HARNESS ]]; then
    HOST_ERR="the devcontainer build has no build/vm-tools/tsk-harness (test/vm/host-build.sh failed; see logs/vm-host-build.log)"
elif [[ ! -f $TSK_VM_VMDK ]]; then
    HOST_ERR="no .vmdk at $TSK_VM_VMDK"
else
    if [[ -z $HOST_IMAGE ]]; then
        workspace=${TSK_VALIDATE_WORKSPACE:-$REPO}
        HOST_IMAGE=$(docker ps --filter "label=devcontainer.local_folder=$workspace" --format '{{.Image}}' | head -n1)
    fi
    if [[ -z $HOST_IMAGE ]]; then
        HOST_ERR="no devcontainer image: no running devcontainer for $workspace, and TSK_VALIDATE_IMAGE is not set"
    elif ! docker image inspect "$HOST_IMAGE" >/dev/null 2>&1; then
        HOST_ERR="the devcontainer image $HOST_IMAGE does not exist"
    fi
fi
if [[ -n $HOST_ERR ]]; then
    log "error: no Host-side runs: $HOST_ERR"
    printf 'no Host-side runs: %s\n' "$HOST_ERR" >"$OUT/host.txt"
else
    printf 'one-off containers of the devcontainer image %s, reading %s\n' "$HOST_IMAGE" "$TSK_VM_VMDK" >"$OUT/host.txt"
fi

# host_run <partition>: the Host-side run of the partition's Strict check, in
# a one-off container. The guest's sdaN is the Nth partition of the .vmdk.
# Results in strict-host/<partition>/, like a guest step's.
host_run() {
    local part=$1 dir=$OUT/strict-host/$1 rc start
    mkdir -p "$dir"
    : >"$dir/steps.tsv"
    start=$SECONDS
    if [[ -n $HOST_ERR ]]; then
        echo "$HOST_ERR" >"$dir/host-run.log"
        rc=125
    else
        log "Strict check, Host side: $TSK_VM_VMDK, partition ${part#sda}"
        HOST_CONTAINER=tsk-validate-host-$$-$part
        timeout --foreground "$STEP_TIMEOUT" docker run --rm --init --name "$HOST_CONTAINER" \
            --network none --user "$(id -u):$(id -g)" --workdir "$REPO" \
            --mount "type=bind,source=$REPO,target=$REPO,readonly" \
            --mount "type=bind,source=$TSK_VM_DIR,target=$TSK_VM_DIR,readonly" \
            --mount "type=bind,source=$dir,target=$dir" \
            "$HOST_IMAGE" "$REPO/test/vm/host-run.sh" "$dir/host.manifest" "${part#sda}" \
            "$TSK_VM_VMDK" "$TSK_VM_DIR" "$REPO" >"$dir/host-run.log" 2>&1
        rc=$?
        if ((rc == 124)); then
            echo "timed out after ${STEP_TIMEOUT}s" >>"$dir/host-run.log"
            docker rm -f "$HOST_CONTAINER" >/dev/null 2>&1
        fi
        HOST_CONTAINER=
    fi
    printf '%s\t%s\t%s\n' host-run "$rc" $((SECONDS - start)) >>"$dir/steps.tsv"
}

# --- Strict checks. ---
# step <part-dir> <tsk-validate.sh command> [args...]: runs one step, its
# output in <command>.log, its exit status appended to steps.tsv. Returns the
# exit status.
step() {
    local dir=$1 name=$2 rc start
    shift
    start=$SECONDS
    run_guest_step "$STEP_TIMEOUT" "$@" >"$dir/$name.log" 2>&1
    rc=$?
    ((rc == 124)) && echo "timed out after ${STEP_TIMEOUT}s" >>"$dir/$name.log"
    printf '%s\t%s\t%s\n' "$name" "$rc" $((SECONDS - start)) >>"$dir/steps.tsv"
    return "$rc"
}

fetch() { # fetch <guest-file> <host-file>
    gssh "$STEP_TIMEOUT" cat "$1" >"$2"
}

# strict_check <partition> <mount point>: the Strict check sequence. The
# guest's steps go to strict-guest/<partition>/, the Host-side run's to
# strict-host/<partition>/.
strict_check() {
    local part=$1 mnt=$2 dev=/dev/$1 dir=$OUT/strict-guest/$1 gout=$GUEST_DIR/out/$1
    local break_path= rc
    mkdir -p "$dir"
    : >"$dir/steps.tsv"
    log "Strict check: $mnt ($part)"
    [[ $BREAK == "$part":* ]] && break_path=${BREAK#*:}

    step "$dir" check-mount "$dev" "$mnt" || return 0
    step "$dir" corpus "$mnt" "$(corpus_large_size "$part")" || return 0
    gssh "$STEP_TIMEOUT" sh -c "rm -rf $gout && mkdir -p $gout" || return 0
    if [[ -n $break_path ]]; then
        step "$dir" break-locate "$mnt" "$dev" "$break_path" || return 0
    fi

    FROZEN_MNT=$mnt
    if ! step "$dir" freeze "$mnt" "$FREEZE_LIMIT"; then
        # A half-done freeze: unfreeze, or stop here so cleanup tries again.
        step "$dir" unfreeze "$mnt" || fail_collect "could not unfreeze $mnt"
        FROZEN_MNT=
        return 0
    fi
    if step "$dir" capture "$mnt" "$gout/ground-truth.manifest" || (($? == 1)); then
        if [[ -n $break_path ]]; then
            BROKEN=1
            step "$dir" break
        fi
        if step "$dir" flush "$dev"; then
            step "$dir" corpus-formats "$mnt" "$dev"
            step "$dir" run "$dev" "$gout/guest.manifest"
        fi
        host_run "$part"
        if [[ -n $PAUSE ]]; then
            log "test hook: waiting ${PAUSE}s with $mnt frozen"
            sleep "$PAUSE"
        fi
        if [[ -n $BROKEN ]]; then
            step "$dir" restore && BROKEN=
        fi
    fi
    step "$dir" unfreeze "$mnt"
    rc=$?
    ((rc == 0)) && FROZEN_MNT=
    # A step that failed to unfreeze stops the collection: cleanup tries again.
    ((rc == 0)) || fail_collect "could not unfreeze $mnt"

    fetch "$gout/ground-truth.manifest" "$dir/ground-truth.manifest" || rm -f "$dir/ground-truth.manifest"
    fetch "$gout/guest.manifest" "$dir/guest.manifest" || rm -f "$dir/guest.manifest"
    return 0
}

for entry in "${STRICT_PARTITIONS[@]}"; do
    read -r part mnt <<<"$entry"
    strict_check "$part" "$mnt"
done

status ok
log "done"
