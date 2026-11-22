#!/bin/bash
# Test whether the Validation VM's kernel can mount an XFS v4 (crc=0) file system.
#
# Usage: test/vm/check-xfs-v4.sh
#
# Makes a v4 image in a temporary file in the guest, tries to mount it read-only through a
# loop device, prints what happened and cleans up. The VM is started if needed and left in
# the state it was found in. Exit status: 0 if v4 mounts, 1 if it does not, 2 if the test
# could not be run. The result is recorded in test/vm/README.md.

set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=vm-common.sh
. "$here/vm-common.sh"

was_running=1
vm_is_running || was_running=0
"$here/vm.sh" up >&2
rc=0
"$here/vm.sh" ssh 'bash -s' <<'EOF' || rc=$?
set -u
echo "kernel: $(uname -r)"
echo "xfsprogs: $(rpm -q xfsprogs)"
echo "CONFIG_XFS_SUPPORT_V4: $(grep -h '^CONFIG_XFS_SUPPORT_V4=' "/boot/config-$(uname -r)" || echo 'not set')"
dir=$(mktemp -d)
trap 'umount "$dir/mnt" 2>/dev/null; rm -rf "$dir"' EXIT
mkdir "$dir/mnt"
truncate -s 512M "$dir/v4.img"
echo "--- mkfs.xfs -m crc=0"
if ! mkfs.xfs -q -f -m crc=0 "$dir/v4.img"; then
    echo "RESULT: mkfs.xfs in the guest cannot create a v4 file system"
    exit 2
fi
echo "--- mount -o loop,ro"
if mount -o loop,ro "$dir/v4.img" "$dir/mnt"; then
    echo "RESULT: XFS v4 mounts"
    exit 0
fi
dmesg | grep -i xfs | tail -n 5 || true
echo "RESULT: XFS v4 does not mount"
exit 1
EOF
if [ "$was_running" = 0 ]; then
    "$here/vm.sh" stop >&2
fi
exit "$rc"
