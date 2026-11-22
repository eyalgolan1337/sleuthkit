#!/bin/bash
# Check that the Validation VM matches its specification.
#
# Usage: test/vm/verify-vm.sh
#
# Host side: no snapshots, one thin single-file .vmdk, VM files and credentials outside the
# repository. Guest side (the VM is started if needed and left in the state it was found
# in): partition layout, file-system features, services; see guest/verify-layout.sh.
# Exits 1 if any check fails.

set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=vm-common.sh
. "$here/vm-common.sh"

[ -f "$TSK_VM_VMX" ] || vm_die "no Validation VM at $TSK_VM_VMX"

fails=0
pass() { printf 'PASS  %s\n' "$*"; }
fail() {
    printf 'FAIL  %s\n' "$*"
    fails=$((fails + 1))
}

echo "== host checks ($TSK_VM_DIR)"

snapshots=$(vm_vmrun listSnapshots "$TSK_VM_VMX" | sed -n 's/^Total snapshots: //p')
if [ "$snapshots" = 0 ]; then pass "no snapshots"; else fail "no snapshots (vmrun reports '$snapshots')"; fi

if grep -q '^snapshot.disabled = "TRUE"' "$TSK_VM_VMX"; then
    pass "snapshots disabled in the .vmx"
else
    fail "snapshots disabled in the .vmx"
fi

shopt -s nullglob
vmdks=("$TSK_VM_DIR"/*.vmdk)
snapfiles=("$TSK_VM_DIR"/*.vmsn)
shopt -u nullglob
if [ "${#vmdks[@]}" = 1 ] && [ "${vmdks[0]}" = "$TSK_VM_VMDK" ]; then
    pass "exactly one .vmdk file"
else
    fail "exactly one .vmdk file (found: ${vmdks[*]})"
fi
if [ "${#snapfiles[@]}" = 0 ]; then pass "no snapshot (.vmsn) files"; else fail "no snapshot (.vmsn) files"; fi
if [ "$(vm_vmdk_create_type)" = monolithicSparse ]; then
    pass "the .vmdk is a single-file growable (monolithicSparse) disk"
else
    fail "the .vmdk is a single-file growable (monolithicSparse) disk"
fi
if grep -q "^scsi0:0.fileName = \"$TSK_VM_NAME.vmdk\"" "$TSK_VM_VMX"; then
    pass "the VM's only disk is $TSK_VM_NAME.vmdk"
else
    fail "the VM's only disk is $TSK_VM_NAME.vmdk"
fi
if grep -Eq '^(ide|sata)[0-9]+:[0-9]+\.present = "TRUE"' "$TSK_VM_VMX"; then
    fail "no installer ISO attached"
else
    pass "no installer ISO attached"
fi
for key in 'numvcpus = "1"' 'memsize = "1536"' 'firmware = "bios"'; do
    if grep -qx "$key" "$TSK_VM_VMX"; then pass ".vmx has $key"; else fail ".vmx has $key"; fi
done

repo=$(git -C "$here" rev-parse --show-toplevel)
case $TSK_VM_DIR/ in
"$repo"/*) fail "VM directory is outside the repository" ;;
*) pass "VM directory is outside the repository" ;;
esac

echo
was_running=1
vm_is_running || was_running=0
"$here/vm.sh" up
echo "== guest checks"
if ! "$here/vm.sh" ssh 'bash -s' <"$here/guest/verify-layout.sh"; then
    fails=$((fails + 1))
fi
if [ "$was_running" = 0 ]; then
    "$here/vm.sh" stop
fi

echo
if [ "$fails" -gt 0 ]; then
    echo "verify-vm: FAILED"
    exit 1
fi
echo "verify-vm: all checks passed"
