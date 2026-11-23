#!/bin/bash
# Runs inside the Validation VM (sent over SSH by verify-vm.sh). Checks the disk layout,
# the file-system features and the guest services. Prints one PASS/FAIL line per check
# and exits 1 if any check fails.

set -uo pipefail

fails=0
pass() { printf 'PASS  %s\n' "$*"; }
fail() {
    printf 'FAIL  %s\n' "$*"
    fails=$((fails + 1))
}
check() { # description, then a command that must succeed
    local what=$1
    shift
    if "$@" >/dev/null 2>&1; then pass "$what"; else fail "$what"; fi
}

disk=/dev/sda
MiB=$((1024 * 1024))

echo "== lsblk"
lsblk -o NAME,SIZE,TYPE,FSTYPE,PARTLABEL,PARTTYPENAME,MOUNTPOINTS "$disk"
echo

check "partition table of $disk is GPT" test "$(lsblk -dno PTTYPE "$disk")" = gpt
check "no LVM volumes" test -z "$(lsblk -nro TYPE | grep -x lvm)"
check "exactly 6 partitions" test "$(lsblk -nro TYPE "$disk" | grep -cx part)" = 6

# Partition N: expected size in MiB, file system type, mount point.
part_size_mib() { echo $(($(lsblk -bdno SIZE "$1") / MiB)); }
expect_part() { # dev size_mib fstype mountpoint
    local dev=$1 size=$2 fstype=$3 mnt=$4
    check "$dev is $size MiB" test "$(part_size_mib "$dev")" = "$size"
    if [ "$fstype" = biosboot ]; then
        check "$dev is a BIOS boot partition" test \
            "$(lsblk -dno PARTTYPE "$dev")" = 21686148-6449-6e6f-744e-656564454649
        return
    fi
    check "$dev holds $fstype" test "$(blkid -p -s TYPE -o value "$dev")" = "$fstype"
    check "$dev is mounted on $mnt" test "$(findmnt -no SOURCE --mountpoint "$mnt")" = "$dev"
}
expect_part /dev/sda1 1 biosboot -
expect_part /dev/sda2 1024 xfs /boot
expect_part /dev/sda3 10240 xfs /
expect_part /dev/sda4 4096 xfs /data
expect_part /dev/sda5 2048 xfs /data-compat
expect_part /dev/sda6 1024 ext4 /control

free_mib=$(parted -ms "$disk" unit MiB print free | awk -F: '$5 == "free;" { sub("MiB", "", $4); s += $4 } END { printf "%d", s }')
check "about 2 GiB unallocated (${free_mib} MiB)" test "$free_mib" -ge 1900 -a "$free_mib" -le 2100

for mnt in /data /data-compat /control; do
    check "$mnt is mounted noatime" grep -qw noatime <<<"$(findmnt -no OPTIONS --mountpoint "$mnt")"
done

# xfs_info prints "key=value" pairs; the first bsize= is the data block size.
xfs_val() { xfs_info "$1" | grep -o "$2=[0-9]*" | head -n1 | cut -d= -f2; }
expect_xfs() { # mountpoint key value
    local got
    got=$(xfs_val "$1" "$2")
    check "$1: $2=$3 (got $got)" test "$got" = "$3"
}

echo
for mnt in /boot / /data /data-compat; do
    echo "== xfs_info $mnt"
    xfs_info "$mnt"
done
echo

expect_xfs /data crc 1
expect_xfs /data reflink 1
expect_xfs /data bigtime 1
expect_xfs /data sparse 1
expect_xfs /data bsize 4096
expect_xfs /data isize 512

expect_xfs /data-compat crc 1
expect_xfs /data-compat reflink 0
expect_xfs /data-compat bigtime 0
expect_xfs /data-compat inobtcount 0
expect_xfs /data-compat bsize 2048
expect_xfs /data-compat isize 1024

echo "== tune2fs -l /dev/sda6"
tune2fs -l /dev/sda6 | grep -E '^(Filesystem (volume name|magic number|features)|Block size|Inode size)'
echo
check "/control has ext4 features (extent, has_journal)" \
    bash -c 'tune2fs -l /dev/sda6 | grep "^Filesystem features" | grep -qw extent && tune2fs -l /dev/sda6 | grep "^Filesystem features" | grep -qw has_journal'

check "vmtoolsd is active" systemctl is-active --quiet vmtoolsd
check "sshd is active" systemctl is-active --quiet sshd
check "root password login over SSH is not permitted" \
    bash -c 'sshd -T | grep -qx "permitrootlogin without-password"'

echo
if [ "$fails" -gt 0 ]; then
    echo "guest checks: $fails failed"
    exit 1
fi
echo "guest checks: all passed"
