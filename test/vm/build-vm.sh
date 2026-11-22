#!/bin/bash
# Build the Validation VM unattended: AlmaLinux 9.9 minimal under VMware Workstation.
#
# Usage: test/vm/build-vm.sh [--force] [--keep-running]
#   --force         stop and delete an existing Validation VM (its VM files, credentials
#                   and build files; the ISO is kept) and build a new one
#   --keep-running  leave the VM running when the build has finished
#
# Environment: TSK_VM_DIR (default ~/vmware/tsk-xfs-alma9) and TSK_VM_ISO (default
# $TSK_VM_DIR/iso/AlmaLinux-9.9-x86_64-minimal.iso). See test/vm/README.md.
#
# Steps: check the ISO's SHA-256 (nothing is created before this passes), generate the
# root password and SSH key, render the kickstart onto an OEMDRV ISO, create a 20 GB
# single-file growable .vmdk and the .vmx, install headless (the kickstart powers the VM
# off when done), detach the ISOs, boot, wait for SSH and run verify-vm.sh.

set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=vm-common.sh
. "$here/vm-common.sh"

force=0
keep_running=0
for arg in "$@"; do
    case $arg in
    --force) force=1 ;;
    --keep-running) keep_running=1 ;;
    -h | --help)
        sed -n '2,16p' "$0"
        exit 0
        ;;
    *) vm_die "unknown option: $arg" ;;
    esac
done

install_timeout=${TSK_VM_INSTALL_TIMEOUT:-5400} # seconds
build_dir=$TSK_VM_DIR/build
serial_log=$build_dir/install-serial.log

# --- 1. Prerequisites and the ISO checksum. Nothing is created before this passes. ---
for tool in vmrun vmware-vdiskmanager openssl ssh ssh-keygen sha256sum; do
    command -v "$tool" >/dev/null || vm_die "missing tool: $tool"
done
if command -v xorriso >/dev/null; then
    mkiso() { xorriso -as mkisofs -quiet "$@"; }
elif command -v genisoimage >/dev/null; then
    mkiso() { genisoimage -quiet "$@"; }
elif command -v mkisofs >/dev/null; then
    mkiso() { mkisofs -quiet "$@"; }
else
    vm_die "missing tool: xorriso, genisoimage or mkisofs"
fi

[ -f "$TSK_VM_ISO" ] || vm_die "ISO not found: $TSK_VM_ISO"
vm_log "checking SHA-256 of $TSK_VM_ISO"
actual_sha=$(sha256sum "$TSK_VM_ISO" | cut -d' ' -f1)
if [ "$actual_sha" != "$TSK_VM_ISO_SHA256" ]; then
    vm_die "ISO checksum mismatch: expected $TSK_VM_ISO_SHA256, got $actual_sha. Nothing was created."
fi
vm_log "ISO checksum OK"

# --- 2. Existing VM ---
if [ -e "$TSK_VM_VMX" ] || [ -e "$TSK_VM_VMDK" ]; then
    [ "$force" = 1 ] || vm_die "a Validation VM already exists in $TSK_VM_DIR (use --force to rebuild it)"
    if vm_is_running; then
        vm_log "stopping the existing VM"
        vm_vmrun stop "$TSK_VM_VMX" hard || true
    fi
    vm_log "deleting the existing VM files"
    rm -rf "$TSK_VM_DIR/$TSK_VM_NAME".* "$TSK_VM_DIR/$TSK_VM_NAME".vmdk.lck \
        "$TSK_VM_DIR"/vmware*.log "$TSK_VM_DIR"/*.nvram "$TSK_VM_DIR"/*.vmem \
        "$TSK_VM_DIR"/*.vmss "$TSK_VM_DIR"/*.vmsn "$TSK_VM_DIR"/*.vmsd \
        "$TSK_VM_CRED_DIR" "$build_dir"
fi

# --- 3. Credentials: random root password (console use only) and SSH key ---
rm -rf "$TSK_VM_CRED_DIR" "$build_dir"
umask 077
mkdir -p "$TSK_VM_CRED_DIR" "$build_dir"
openssl rand -base64 24 >"$TSK_VM_ROOT_PASSWORD"
rootpw_hash=$(openssl passwd -6 -stdin <"$TSK_VM_ROOT_PASSWORD")
ssh-keygen -q -t ed25519 -N '' -C "root@$TSK_VM_NAME" -f "$TSK_VM_SSH_KEY"
: >"$TSK_VM_KNOWN_HOSTS"
vm_log "credentials written to $TSK_VM_CRED_DIR"

# --- 4. Kickstart on an OEMDRV ISO ---
shopt -u patsub_replacement 2>/dev/null || true
ks=$(<"$here/kickstart/ks.cfg.in")
ks=${ks//@VM_HOSTNAME@/$TSK_VM_NAME}
ks=${ks//@ROOTPW_HASH@/$rootpw_hash}
ks=${ks//@SSH_PUBKEY@/$(<"$TSK_VM_SSH_KEY.pub")}
if grep -n '@[A-Z_]\+@' <<<"$ks" >&2; then
    vm_die "unreplaced placeholder in the kickstart"
fi
mkdir -p "$build_dir/oemdrv"
printf '%s\n' "$ks" >"$build_dir/oemdrv/ks.cfg"
mkiso -V OEMDRV -J -R -o "$build_dir/oemdrv.iso" "$build_dir/oemdrv"
vm_log "kickstart ISO: $build_dir/oemdrv.iso"

# --- 5. Disk and VM configuration ---
umask 022
# -t 0: single growable virtual disk (one thin monolithicSparse .vmdk file).
vmware-vdiskmanager -q -c -s 20GB -a lsilogic -t 0 "$TSK_VM_VMDK" >/dev/null
[ "$(vm_vmdk_create_type)" = monolithicSparse ] ||
    vm_die "$TSK_VM_VMDK is not a monolithicSparse disk"

cat >"$TSK_VM_VMX" <<EOF
.encoding = "UTF-8"
config.version = "8"
virtualHW.version = "21"
displayName = "$TSK_VM_NAME"
guestOS = "almalinux-64"
firmware = "bios"
numvcpus = "1"
cpuid.coresPerSocket = "1"
memsize = "1536"
mainMem.useNamedFile = "FALSE"
snapshot.disabled = "TRUE"
msg.autoAnswer = "TRUE"
tools.syncTime = "FALSE"
scsi0.present = "TRUE"
scsi0.virtualDev = "pvscsi"
scsi0:0.present = "TRUE"
scsi0:0.fileName = "$TSK_VM_NAME.vmdk"
ethernet0.present = "TRUE"
ethernet0.connectionType = "nat"
ethernet0.virtualDev = "vmxnet3"
ethernet0.addressType = "generated"
bios.bootOrder = "cdrom,hdd"
ide1:0.present = "TRUE"
ide1:0.deviceType = "cdrom-image"
ide1:0.fileName = "$TSK_VM_ISO"
ide1:0.startConnected = "TRUE"
ide1:1.present = "TRUE"
ide1:1.deviceType = "cdrom-image"
ide1:1.fileName = "$build_dir/oemdrv.iso"
ide1:1.startConnected = "TRUE"
serial0.present = "TRUE"
serial0.fileType = "file"
serial0.fileName = "$serial_log"
EOF

# --- 6. Unattended install ---
vm_log "starting the installation (headless; progress in $serial_log)"
vm_vmrun start "$TSK_VM_VMX" nogui
start=$(date +%s)
last_line=
while vm_is_running; do
    if (($(date +%s) - start > install_timeout)); then
        vm_vmrun stop "$TSK_VM_VMX" hard || true
        vm_die "installation did not finish within ${install_timeout}s; see $serial_log and the anaconda logs"
    fi
    line=$(grep -a '^tsk-ks:' "$serial_log" 2>/dev/null | tail -n1 || true)
    if [ -n "$line" ] && [ "$line" != "$last_line" ]; then
        vm_log "${line#tsk-ks: }"
        last_line=$line
    fi
    sleep 15
done
grep -aq '^tsk-ks: post: done' "$serial_log" ||
    vm_die "the VM powered off before the kickstart %post finished; see $serial_log"
vm_log "installation finished after $(($(date +%s) - start))s"

# --- 7. Detach the ISOs and the serial port, boot from the disk ---
sed -i -E '/^(ide1:[0-9]+\.|serial0\.|bios\.bootOrder )/d' "$TSK_VM_VMX"

# --- 8. First boot and verification ---
"$here/vm.sh" up
"$here/verify-vm.sh"
if [ "$keep_running" = 0 ]; then
    "$here/vm.sh" stop
fi
vm_log "Validation VM ready: $TSK_VM_VMX"
