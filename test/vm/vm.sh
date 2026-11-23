#!/bin/bash
# Control the Validation VM from the host.
#
# Usage: test/vm/vm.sh COMMAND [ARGS...]
#   start            power the VM on headless, unless it is already running
#   wait [SECONDS]   wait until the guest answers SSH (default 300 s)
#   up               start, then wait
#   ssh [CMD...]     run CMD in the guest as root over SSH (no CMD: interactive shell);
#                    stdin is passed through, the exit status is CMD's
#   ip               print the guest's IP address (from open-vm-tools, via vmrun)
#   status           print "running" or "stopped"; exit 1 if stopped
#   stop             shut the guest down cleanly (hard power-off after 120 s), if running
#
# Environment: TSK_VM_DIR (default ~/vmware/tsk-xfs-alma9).

set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=vm-common.sh
. "$here/vm-common.sh"

usage() {
    sed -n '2,14p' "$0" >&2
    exit 2
}

need_vm() {
    [ -f "$TSK_VM_VMX" ] || vm_die "no Validation VM at $TSK_VM_VMX (build it with test/vm/build-vm.sh)"
}

cmd_start() {
    need_vm
    if vm_is_running; then
        vm_log "already running"
    else
        vm_log "starting $TSK_VM_NAME (headless)"
        vm_vmrun start "$TSK_VM_VMX" nogui ||
            vm_die "the VM did not power on; see $TSK_VM_DIR/vmware.log"
    fi
}

cmd_wait() {
    local timeout=${1:-300} start
    need_vm
    start=$(date +%s)
    local -a opts
    mapfile -t opts < <(vm_ssh_opts)
    while :; do
        vm_is_running || vm_die "the VM is not running"
        local ip
        if ip=$(vm_ip) && ssh "${opts[@]}" -o ConnectTimeout=5 "root@$ip" true </dev/null 2>/dev/null; then
            vm_log "SSH is up at $ip"
            return 0
        fi
        (($(date +%s) - start < timeout)) || vm_die "SSH did not answer within ${timeout}s"
        sleep 5
    done
}

cmd_stop() {
    need_vm
    if ! vm_is_running; then
        vm_log "already stopped"
        return 0
    fi
    vm_log "shutting down $TSK_VM_NAME"
    # A soft stop asks open-vm-tools to shut the guest down. If it does not, fall back to
    # a hard power-off so the VM never stays half up.
    if ! timeout 120 vmrun -T ws stop "$TSK_VM_VMX" soft; then
        vm_log "soft stop failed or timed out; powering off"
        vm_vmrun stop "$TSK_VM_VMX" hard
    fi
    local i
    for i in $(seq 60); do
        vm_is_running || return 0
        sleep 2
    done
    vm_die "the VM is still running"
}

[ $# -ge 1 ] || usage
command=$1
shift
case $command in
start) cmd_start ;;
wait) cmd_wait "$@" ;;
up)
    cmd_start
    cmd_wait "$@"
    ;;
ssh)
    need_vm
    vm_ssh "$@"
    ;;
ip)
    need_vm
    vm_ip || vm_die "the guest's IP address is not known (is the VM running?)"
    ;;
status)
    need_vm
    if vm_is_running; then echo running; else
        echo stopped
        exit 1
    fi
    ;;
stop) cmd_stop ;;
*) usage ;;
esac
