# Validation VM

The Validation VM is a small AlmaLinux 9.9 virtual machine under VMware Workstation. Its disk holds XFS file systems with different format variants and an ext4 Control file system. TSK reads that disk from the host (the `.vmdk`) and from inside the guest (`/dev/sd*`), and the results are compared with what the guest's kernel sees. This directory builds the VM unattended and controls it from the host.

## Prerequisites

On the host (Linux):

- VMware Workstation 17 (tested with 17.6.2) with its kernel modules loaded: `/dev/vmmon` must exist and `systemctl status vmware` must be active. With Secure Boot on, the `vmmon` and `vmnet` modules must be signed with a key enrolled in MOK (`mokutil --test-key <cert>` must say it is enrolled). Otherwise every VM fails to start.
- The NAT network `vmnet8` with DHCP (the VMware default).
- `vmrun`, `vmware-vdiskmanager` (both come with Workstation), `openssl`, `ssh`, `ssh-keygen`, `sha256sum`, and one of `xorriso`, `genisoimage` or `mkisofs`.
- Network access during the install (see [Installation sources](#installation-sources)).
- The AlmaLinux 9.9 minimal ISO, `AlmaLinux-9.9-x86_64-minimal.iso`, in `~/vmware/tsk-xfs-alma9/iso/`. Download it from <https://repo.almalinux.org/almalinux/9.9/isos/x86_64/>. `build-vm.sh` checks it against the SHA-256 published in that directory's `CHECKSUM` file, which is pinned in `vm-common.sh`.
- About 25 GB free in `~/vmware/` (ISO 3 GB, disk up to 20 GB) and 1.5 GiB of RAM for the running VM.

## Where the VM files and credentials live

Everything the VM needs at run time lives outside the repository, in `~/vmware/tsk-xfs-alma9/` (override it with `TSK_VM_DIR`):

| Path | Contents |
| --- | --- |
| `iso/AlmaLinux-9.9-x86_64-minimal.iso` | Installation ISO (override with `TSK_VM_ISO`) |
| `tsk-xfs-alma9.vmx` | VM configuration |
| `tsk-xfs-alma9.vmdk` | The VM's only disk: 20 GB, thin, single file (`monolithicSparse`) |
| `credentials/id_ed25519`, `id_ed25519.pub` | SSH key the host uses to log in as root |
| `credentials/root-password` | Generated root password, for the VMware console only (SSH accepts only the key) |
| `credentials/known_hosts` | The guest's SSH host key |
| `build/` | Rendered kickstart, `oemdrv.iso` and the installer's serial log |

None of these are ever committed. The rendered kickstart contains the root password hash and the public key, so it stays in `build/` too.

## First-time VM creation

```sh
test/vm/build-vm.sh                 # build, verify, power off
test/vm/build-vm.sh --keep-running  # ... and leave it running
test/vm/build-vm.sh --force         # delete an existing VM (not the ISO) and rebuild
```

The build needs no input. On the development machine it took about 11 minutes, 8 of them for the install itself. It:

1. Checks the ISO's SHA-256. A mismatch aborts the build before anything is created.
2. Generates the root password and an ed25519 SSH key in `credentials/`.
3. Renders `kickstart/ks.cfg.in` and writes it as `ks.cfg` onto a small ISO labelled `OEMDRV`. Anaconda loads a kickstart from a volume with that label without any boot option, so the stock ISO boots unchanged.
4. Creates the 20 GB single-file thin `.vmdk` and the `.vmx`: BIOS firmware, 1 vCPU, 1536 MiB, PVSCSI disk, vmxnet3 on NAT, snapshots disabled, no floppy. PVSCSI and vmxnet3 are PCIe devices, so the `.vmx` also declares the PCIe root ports (`pciBridge4` to `pciBridge7`) that Workstation adds to the VMs it creates; without them the VM does not power on ("Too many PCI devices are already configured").
5. Boots the installer headless (`vmrun start ... nogui`). The ISO's default boot entry checks the media first, which adds a minute or two. The kickstart prints its progress to a virtual serial port that writes to `build/install-serial.log`, and powers the VM off when it finishes. The build gives up after 90 minutes (`TSK_VM_INSTALL_TIMEOUT`, in seconds).
6. Detaches both ISOs and the serial port, boots the installed system, waits for SSH and runs `verify-vm.sh`.

If the install fails, look at `build/install-serial.log`. For the installer's own logs, open the VM in the Workstation GUI before it powers off; Anaconda's shell is on the second virtual console (Ctrl+Alt+F2).

### Installation sources

The minimal ISO does not include open-vm-tools, which the host needs to find the guest's IP address. The kickstart therefore also uses the AlmaLinux 9.9 BaseOS and AppStream repositories at `repo.almalinux.org`. dnf may then install 9.9 updates that are newer than the packages on the ISO. When AlmaLinux 9.10 is released, 9.9 moves to `vault.almalinux.org`, and the two `repo` lines in `kickstart/ks.cfg.in` must point there.

## Disk layout

GPT, no LVM, on a 20 GiB disk (`/dev/sda` in the guest):

| Partition | Size | Mount | File system |
| --- | --- | --- | --- |
| `sda1` | 1 MiB | | BIOS boot |
| `sda2` | 1 GiB | `/boot` | XFS, distribution defaults |
| `sda3` | 10 GiB | `/` | XFS, distribution defaults |
| `sda4` | 4 GiB | `/data` | XFS v5, `mkfs.xfs` defaults: crc, reflink, bigtime, inobtcount, sparse inodes; 4 KiB blocks, 512-byte inodes |
| `sda5` | 2 GiB | `/data-compat` | XFS v5 with reflink, bigtime and inobtcount off; 2 KiB blocks, 1 KiB inodes |
| `sda6` | 1 GiB | `/control` | ext4, `mke2fs` defaults (the Control file system) |
| | ~2 GiB | | unallocated |

`/data`, `/data-compat` and `/control` are mounted `noatime`. They are formatted in the kickstart's `%post` with AlmaLinux 9.9's own `mkfs` (xfsprogs 6.4.0, e2fsprogs 1.46.5), so "defaults" means that version's defaults.

## Controlling the VM

```sh
test/vm/vm.sh up            # start headless if not running, then wait for SSH
test/vm/vm.sh ssh uname -r  # run a command as root over SSH (key only; no password)
test/vm/vm.sh ssh           # interactive root shell
test/vm/vm.sh ip            # guest IP address, from open-vm-tools via vmrun
test/vm/vm.sh status        # "running" or "stopped" (exit 1 if stopped)
test/vm/vm.sh stop          # clean shutdown; hard power-off after 120 s
```

`vm.sh start` and `vm.sh wait [SECONDS]` are the two halves of `up`. `vm.sh ssh` passes its standard input to the guest, so a local script can be run there with `test/vm/vm.sh ssh 'bash -s' < script.sh`.

The VM never has snapshots. A snapshot would freeze the base `.vmdk` and send the guest's writes to a delta disk, so the host would read stale data.

## Checking the VM

```sh
test/vm/verify-vm.sh
```

This prints one PASS/FAIL line per check and exits 1 if any fails. On the host, it checks for no snapshots, exactly one thin single-file `.vmdk`, no ISO attached, 1 vCPU, 1536 MiB, BIOS, and the VM directory outside the repository. In the guest (`guest/verify-layout.sh`, run over SSH), it checks the partition layout and sizes with `lsblk`, the XFS features with `xfs_info`, the ext4 features with `tune2fs -l`, `noatime`, and that `vmtoolsd` and `sshd` are running. `build-vm.sh` runs it at the end of every build. It starts the VM if needed and leaves it as it found it.

## XFS v4 on the AlmaLinux 9 kernel

`check-xfs-v4.sh` makes an XFS v4 (`crc=0`) image in the guest and tries to mount it.

Result on AlmaLinux 9.9 (tested 2026-11-23, kernel 5.14.0-741.24.1.el9_9, xfsprogs 6.4.0-10.el9_9): **XFS v4 mounts.** The kernel is built with `CONFIG_XFS_SUPPORT_V4=y`. `mkfs.xfs -m crc=0` warns "V4 filesystems are deprecated and will not be supported by future versions", and the mount logs "Deprecated V4 format (crc=0) will not be supported beginning with RHEL10", but the mount succeeds.

The Validation VM's disk has no v4 file system. XFS v4 is covered by fixture images built in the devcontainer.

## Files

| File | Runs on | Purpose |
| --- | --- | --- |
| `vm-common.sh` | host | Shared settings (paths, pinned ISO checksum) and helpers; sourced by the others |
| `build-vm.sh` | host | Unattended build of the VM |
| `vm.sh` | host | Start, wait, ssh, ip, status, stop |
| `verify-vm.sh` | host | Checks the VM against its specification |
| `check-xfs-v4.sh` | host | Tests whether the guest kernel mounts XFS v4 |
| `kickstart/ks.cfg.in` | installer | Kickstart template |
| `guest/verify-layout.sh` | guest | Layout and file-system checks, sent by `verify-vm.sh` |
