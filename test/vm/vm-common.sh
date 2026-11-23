# Shared settings and helpers for the Validation VM scripts. Source it; do not run it.
#
# Everything the VM needs at run time (VM files, ISO, credentials, SSH key) lives in
# TSK_VM_DIR, outside the repository. Override TSK_VM_DIR to use another location.

TSK_VM_SCRIPTS=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd) # this directory (test/vm)
TSK_VM_NAME=tsk-xfs-alma9
TSK_VM_DIR=${TSK_VM_DIR:-$HOME/vmware/$TSK_VM_NAME}
TSK_VM_VMX=$TSK_VM_DIR/$TSK_VM_NAME.vmx
TSK_VM_VMDK=$TSK_VM_DIR/$TSK_VM_NAME.vmdk

# AlmaLinux 9.9 minimal ISO, pinned by the SHA-256 published in
# https://repo.almalinux.org/almalinux/9.9/isos/x86_64/CHECKSUM
TSK_VM_ISO_NAME=AlmaLinux-9.9-x86_64-minimal.iso
TSK_VM_ISO_SHA256=0a0929bd4bbad481fe30066278aa66534a1d7a219a39c0495f0d51f82511bfd5
TSK_VM_ISO=${TSK_VM_ISO:-$TSK_VM_DIR/iso/$TSK_VM_ISO_NAME}

TSK_VM_CRED_DIR=$TSK_VM_DIR/credentials
TSK_VM_SSH_KEY=$TSK_VM_CRED_DIR/id_ed25519
TSK_VM_KNOWN_HOSTS=$TSK_VM_CRED_DIR/known_hosts
TSK_VM_ROOT_PASSWORD=$TSK_VM_CRED_DIR/root-password

vm_log() { printf '[vm] %s\n' "$*" >&2; }
vm_die() {
    printf '[vm] error: %s\n' "$*" >&2
    exit 1
}

vm_vmrun() { vmrun -T ws "$@"; }

# Print the createType of the VM's .vmdk (monolithicSparse for one thin, single-file disk).
# A single-file .vmdk embeds its text descriptor near the start, so only the first 64 KiB
# are read, not the whole multi-GB disk.
vm_vmdk_create_type() {
    head -c 65536 "$TSK_VM_VMDK" 2>/dev/null | tr -d '\0' |
        sed -n 's/^createType="\([^"]*\)".*/\1/p' | head -n1
}

# True if the VM is powered on. `vmrun list` prints the full .vmx path of each running VM.
vm_is_running() {
    [ -f "$TSK_VM_VMX" ] || return 1
    vm_vmrun list 2>/dev/null | grep -Fxq "$TSK_VM_VMX"
}

# Print the guest's IP address as reported by open-vm-tools; fails if it is not known yet.
vm_ip() {
    local ip
    ip=$(vm_vmrun getGuestIPAddress "$TSK_VM_VMX" 2>/dev/null) || return 1
    [[ $ip =~ ^[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}$ ]] || return 1
    printf '%s\n' "$ip"
}

# Start the VM and wait for SSH, for a script that needs it only while it runs. If the VM
# was stopped, an EXIT trap stops it again however the script ends (failure or interrupt
# included), so the VM is left in the state it was found in. The trap replaces any EXIT
# trap the script set before. Returns non-zero if the VM does not start or SSH does not
# answer.
vm_up_for_script() {
    if ! vm_is_running; then
        trap '"$TSK_VM_SCRIPTS/vm.sh" stop >&2 || true' EXIT
    fi
    "$TSK_VM_SCRIPTS/vm.sh" up >&2
}

# Options for every ssh call. The host key is recorded under an alias, not the IP, so a new
# DHCP lease does not look like a different machine.
vm_ssh_opts() {
    printf '%s\n' -i "$TSK_VM_SSH_KEY" \
        -o BatchMode=yes -o IdentitiesOnly=yes \
        -o StrictHostKeyChecking=accept-new \
        -o UserKnownHostsFile="$TSK_VM_KNOWN_HOSTS" \
        -o HostKeyAlias="$TSK_VM_NAME" \
        -o ConnectTimeout=10 -o LogLevel=ERROR
}

# vm_ssh [ssh args...] -- runs ssh to root@guest; extra args are passed to ssh.
vm_ssh() {
    local ip
    ip=$(vm_ip) || vm_die "cannot get the guest's IP address (is the VM running with open-vm-tools?)"
    local -a opts
    mapfile -t opts < <(vm_ssh_opts)
    ssh "${opts[@]}" "root@$ip" "$@"
}
