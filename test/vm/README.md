# Validation setup

Checks that the fork's file system support matches what a real, running Linux system sees. The setup is specified in issue #10. Terms in bold are defined in [`GLOSSARY.md`](../../GLOSSARY.md).

Its entry point is the **validate command**, `test/vm/validate.sh`. It gives one pass or fail for the current working tree. A development step is done only when the validate command passes.

## Prerequisites

- Docker (29 or newer works) and the devcontainer image in `.devcontainer/`. Everything is built and checked there, with GCC 16.1 as the only reference toolchain (see `CLAUDE.md`, "Development environment").
- On the host, the devcontainers CLI 0.91.0 or newer (`devcontainer`). If it isn't installed, the command uses `npx @devcontainers/cli@0.91.0`, so Node.js with `npx` is enough. Older CLI versions hang on Docker 29+.
- Network access on the first run: `make check` downloads its test images to `tests/data/` (`tests/test_libraries.sh`).

The **Validation VM** has its own prerequisites; see [Validation VM](#validation-vm).

## Running the validate command

```sh
test/vm/validate.sh              # from the host or inside the devcontainer
test/vm/validate.sh --fresh      # re-run bootstrap and configure first
test/vm/validate.sh --base REF   # compare with REF instead of develop
```

On the host, the command starts the devcontainer if necessary (`devcontainer up`) and runs itself inside it (`devcontainer exec`). It never builds in the host tree. The repository is mounted at the same path in both, so the report is visible on the host. In a git worktree, the command uses the main checkout's devcontainer (a container opened on the worktree folder alone couldn't see the main checkout's `.git`), so the worktree must be inside the main checkout, for example under `.claude/worktrees/`. That container is built from the main checkout's `.devcontainer/`. If the tree was configured outside the devcontainer, gate 1 runs `make distclean` and configures again, and the report says so.

"Changed" means changed since the merge-base of `develop` and `HEAD`: committed on the branch, staged, unstaged, or untracked and not ignored.

Exit status: `0` if no gate failed, `1` if any gate failed, `2` if the command couldn't run (for example, the devcontainer didn't start). Every gate runs even if an earlier one fails, and `make check` runs with `-k` so a failure in one directory doesn't stop the tests in the others. On Ctrl-C the command writes a partial report (the running gate `INTERRUPTED`, later ones `NOT RUN`) and exits 130.

A full run builds everything from clean and runs `make check`. It takes a few minutes.

## Gates

| Gate | What it checks | Status |
|------|----------------|--------|
| 1 | Clean build of the library, tools, samples, test programs and the validation setup's C++ tools under `bear` (`.devcontainer/build-compile-db.sh`), which also regenerates `compile_commands.json`. Then a syntax-only compile of each changed C++ file as C++17 and as C++20, and the [comparison step's self-test](#gate-1-comparison-step-self-test). | Implemented |
| 2 | The full `make check`. | Implemented |
| 3 | **Strict checks** | Not yet implemented (#17, #18) |
| 4 | **Best-effort checks** | Not yet implemented (#21) |
| 5 | `clang-format` and `clang-tidy` on changed lines only. | Implemented |

A gate that doesn't exist yet appears in the report as `NOT IMPLEMENTED`. It neither passes nor fails.

### Gate 1: C++17 and C++20 syntax

The in-repo build compiles as C++14, while the owner's vcpkg port may compile the same sources with a newer standard. For each changed C++ source, gate 1 takes the file's own command from `compile_commands.json`, replaces `-std=` with `-std=c++17` (then `-std=c++20`), and compiles syntax-only with:

- `-Werror=register`: GCC only warns about the removed `register` storage class.
- `-D_GLIBCXX_USE_DEPRECATED=0`: libstdc++ otherwise still provides facilities that C++17 removed (`std::auto_ptr`, `std::bind1st`, `std::random_shuffle`, …), which MSVC rejects.

A changed header is checked through every compiled C++ source that includes it, according to automake's dependency files (`.deps/`). A changed C++ file with no compile command (not built in this configuration), or a header no compiled C++ file includes, is reported as a warning because it wasn't checked. If the gate 1 build failed, it is a failure instead, because the compilation database is then incomplete. On release 4.15.0, all 132 C++ translation units pass this check.

### Gate 1: comparison step self-test

`build/vm-tools/manifest-selftest` checks the Manifest reader and writer and the comparison step on hand-made Manifests. It needs no Validation VM. Each failed case is a failure of gate 1, named in the report. See [The self-test](#the-self-test).

### Gate 5: format and lint

- `clang-format`: `git clang-format --diff <merge-base>` on changed tracked files, and `clang-format --dry-run --Werror` on new untracked files. Any unformatted changed line fails the gate. To fix it, run `git clang-format develop` (or `clang-format -i <file>` for a new file).
- `clang-tidy`: `clang-tidy-diff` with the repo's `.clang-tidy` on the changed lines. Any diagnostic on a changed line fails the gate. If the gate 1 build failed, this check fails without running: the compilation database is incomplete and a clean result would mean nothing.

## Reading the report

The report is written to `build/validate/<UTC time>/report.md`, and `build/validate/latest` points to the newest run. It is also printed at the end. `build/` is gitignored. The report contains, in order:

1. `Result:` `PASS` or `FAIL`, with the failed gates (and the gates not yet implemented) in brackets.
2. The run: time, branch, `HEAD`, base and merge-base, the toolchain versions, and where the logs are.
3. **`Windows (MSVC) build: NOT VERIFIED.`** The validate command never builds with MSVC, so a pass says nothing about the Windows build.
4. A summary table: each gate with `PASS`, `FAIL` or `NOT IMPLEMENTED`.
5. One section per gate. Each check is listed with its result, duration and log file (`logs/gate<n>-<check>.log`), followed by its findings:
   - `failure:` makes the check, and so the gate, fail;
   - `warning:` doesn't fail anything, but read it. Examples: a compiler warning in a changed file, a skipped test, or a changed file that couldn't be checked;
   - notes such as the number of files checked and each test's result.
6. All failures and all warnings collected in one place, then the list of changed files.

Compiler warnings are listed only for changed files. Warnings in unchanged upstream code are counted, and the full log has them.

## Adding a check

Later parts of the setup add checks to the gate files in `test/vm/gates/` (`gate<n>.sh`, one `gate_<n>` function each) using the report API in `test/vm/lib/report.sh`:

```sh
gate_1() {
    gate_begin 1 "Build, C++17/C++20 syntax and comparison self-test"
    run_check build "Build (...)" gate1_build   # run_check <slug> <title> <command> [args...]
    ...
    gate_end
}
```

`run_check` runs the command in a subshell from the repository root, with its output going to the check's log. The command reports findings with `item_fail`, `item_warn` and `item_note`. A program that isn't a shell function appends `fail|warn|note<TAB>text` lines to the file named by `$VALIDATE_ITEMS`. A check fails if its command exits non-zero or reports a failure. A gate that isn't implemented calls `gate_not_implemented` instead. Its checks are added when it is implemented.

## Manifests and the comparison step

A **Manifest** records a file system's contents, one line per entry. **Ground truth** and every run write exactly the same format. The comparison step compares a run's Manifest with Ground truth under the rules of the **Strict check** or the **Best-effort check** and tolerates listed **Known failures**.

The code is C++14 and has no dependency on libtsk or any other language runtime. It is in `test/vm/manifest/`:

| File | Contents |
| --- | --- |
| `manifest.h`, `manifest.cpp` | The Manifest format: `Entry`, `Manifest`, path encoding, `ManifestWriter`, `read_manifest`. The harness and Ground truth capture use this API to write Manifests. |
| `compare.h`, `compare.cpp` | The comparison step: `compare_manifests`, the Known failure and Volatile path list readers, the report |
| `compare_cli.h`, `compare_cli.cpp`, `compare_main.cpp` | The `compare-manifests` command |
| `selftest.cpp` | The self-test (CppUnit), run in gate 1 |
| `manifest.mk` | Plain GNU make file that builds all of it into `build/vm-tools/` |

```sh
make -f test/vm/manifest/manifest.mk          # libtskmanifest.a, compare-manifests, manifest-selftest
make -f test/vm/manifest/manifest.mk check    # ... and run the self-test
```

Run these in the devcontainer, from the repository root. `.devcontainer/build-compile-db.sh` (and so gate 1) builds the same targets under `bear`, so the files have entries in `compile_commands.json`.

### Manifest format

A Manifest is a text file. Its first line is exactly `#tsk-manifest v1`. After that:

- a line that starts with `#` is a comment;
- a line that starts with `!` is a directive (only `!error` exists, see below);
- every other line is one entry: 14 fields separated by single tabs, in this order.

| # | Field | Value |
| --- | --- | --- |
| 1 | path | The entry's path relative to the file system root, starting with `/` (the root directory itself is `/`), [encoded](#path-encoding). There are no empty, `.` or `..` components and no trailing `/`. Each path appears at most once; a hard link appears once per path. |
| 2 | type | `file`, `dir`, `symlink`, `chr`, `blk`, `fifo`, `sock` or `other` |
| 3 | inode | inode number, decimal |
| 4 | mode | permission bits only (including setuid, setgid and sticky), exactly four octal digits, e.g. `0644`. The type is in field 2. |
| 5 | uid | decimal |
| 6 | gid | decimal |
| 7 | size | size in bytes, decimal |
| 8 | link count | decimal |
| 9 | symlink target | encoded like a path, or `-` if there is none (not a symlink, or not read) |
| 10 | SHA-256 | SHA-256 of the content, 64 lowercase hex digits, or `-` if there is none (not a regular file, or not read) |
| 11 | mtime | [timestamp](#timestamps) |
| 12 | ctime | timestamp |
| 13 | crtime | timestamp, or `-` if the file system or the reader doesn't provide one |
| 14 | atime | timestamp, or `-`. Recorded but never compared. |

Numbers are canonical: decimal digits with no sign, no leading zeros and no spaces. Each value has exactly one spelling, so two fields are equal exactly when their text is. The order of entries doesn't matter. The reader rejects anything else with the line number: a missing header, a wrong number of fields, a malformed value or a duplicate path.

Example (a regular file whose name contains a space, then a symlink created at 1969-12-31T23:59:58.5Z):

```text
#tsk-manifest v1
/dir/a%20file	file	131	0644	1000	100	11	1	-	a948904f2f0f479b8f8197694b30184b0d2ed1c1cd2a1ec0fb85d299a192a447	1700000000123456789	1700000001000000000	-	1700000002000000000
/dir/link	symlink	132	0777	0	0	9	1	../target	-	-1500000000	0	1	-
```

#### Path encoding

Paths and symlink targets are byte strings: names can contain spaces, newlines and bytes that aren't valid UTF-8. To keep one entry per line, each byte outside `!` to `~` (0x21 to 0x7E), and `%` itself, is written as `%XX` with two uppercase hex digits. Every other byte is written as itself. So a space is `%20`, a newline `%0A`, `%` is `%25` and the Latin-1 byte 0xE9 is `%E9`. UTF-8 names are encoded byte by byte (`é` is `%C3%A9`). A string that is exactly `-` is written `%2D`, because `-` alone marks an absent field. An empty symlink target is an empty field.

Decoding turns each `%XX` back into its byte. It accepts only what encoding writes, so each byte string has exactly one spelling: a malformed escape, a raw byte outside `!` to `~`, lowercase hex (`%c3`) and a needless escape (`%61` for `a`, `%2F` for `/`) are all rejected. This also holds for the paths in the Known failure and Volatile path lists. Any byte sequence round-trips unchanged; the self-test checks spaces, tabs, newlines, every byte value, non-UTF-8 bytes and 255-byte names.

#### Timestamps

A timestamp is the number of nanoseconds since 1970-01-01T00:00:00Z, written as a signed decimal integer: `1700000000123456789`, `0`, or `-1500000000` for 1.5 s before the epoch. It's canonical too: no leading zeros, no `+`, no `-0`. The reader keeps seconds and nanoseconds apart, so the range is the full 64-bit range of seconds, well beyond the 2262 limit of a 64-bit nanosecond count (XFS bigtime reaches 2486).

#### Failed runs

A producer that can't record the file system at all, for example because the file system isn't recognised, writes one directive line after the header:

```text
!error	fs-open	Cannot%20determine%20file%20system%20type
```

The second field is an error code (`[a-z0-9-]+`, chosen by the producer). The third is a free-text detail, encoded like a path. A Manifest with `!error` is incomplete, so the comparison step doesn't compare its entries. A Ground truth Manifest with `!error` always fails the comparison.

### The comparison step

```sh
build/vm-tools/compare-manifests --ground-truth GT --run RUN --mode strict|best-effort \
    [--run-time NS] [--volatile FILE] \
    [--known-failures FILE --check NAME --partition NAME] \
    [--items FILE] [--max-items N]
```

| Option | Meaning |
| --- | --- |
| `--ground-truth`, `--run` | The two Manifests |
| `--mode` | `strict` or `best-effort` |
| `--run-time` | The run's timestamp: when the run finished, in nanoseconds since the epoch (`date +%s%N` right after it). Required for `best-effort`. |
| `--volatile` | The [Volatile path list](#volatile-path-list) (`best-effort` only; ignored in `strict`) |
| `--known-failures` | The [Known failure list](#known-failure-list). Needs `--check` and `--partition`, which select its lines. |
| `--items` | Append findings to this file in the validate command's report protocol (`fail`, `warn` or `note`, a tab, the text): at most `--max-items` (default 20) failures and as many warnings, a count of the rest, and a summary note. Gates pass `$VALIDATE_ITEMS`. |

The full per-entry report goes to standard output, so it ends up in the check's log. Each finding is one line, starting with `FAIL`, `WARN`, `KNOWN` or `NOTE` and naming the encoded path, the difference and both values. The last line gives the result and the counts. Exit status: `0` if nothing failed (warnings allowed), `1` if anything failed, `2` for bad usage or unreadable input (a missing file, or a malformed Manifest or list, reported with its line number).

Rules:

- The entries of the two Manifests are matched by path. An entry only in Ground truth is `missing`. An entry only in the run is `extra`. For entries in both, each of the 12 compared fields (type, inode, mode, uid, gid, size, link count, symlink target, SHA-256, mtime, ctime, crtime) that differs is one difference, named after the field. A field present on one side and absent (`-`) on the other differs. atime is never compared.
- **Strict** (`--mode strict`): every difference is a failure.
- **Best-effort** (`--mode best-effort`): a difference is tolerated, and reported as a warning with the reason, only if
  - its path is under a Volatile path: the path itself or anything below it, matching whole components (`/var/log` covers `/var/log/messages` but not `/var/logger`), or
  - the entry's ctime, in Ground truth or in the run, is at most 10 minutes before `--run-time` and not after it. The run's ctime counts too, because a file may change between Ground truth capture and the run. For a `missing` or `extra` entry, the side that has it decides. A ctime after the end of the run can't be real, only misread (for example by a timestamp decoding bug), so it tolerates nothing.

  Every other difference is a failure.
- **Known failures** apply in both modes and are checked first. A difference that matches a line of the Known failure list for this `--check` and `--partition` is tolerated and reported as `KNOWN`; it is neither a warning nor a failure. A line that matches no difference is a failure ("Known failure no longer occurs"), so the list only ever shrinks: remove the line once the problem is fixed. If the run failed as a whole (`!error`), a `* error:<code>` line with the same code tolerates it. Lines about entries can't be checked then; they are reported as `NOTE` lines, not as failures.

### Known failure list

A text file with one Known failure per line, in fields separated by spaces or tabs:

```text
# check        partition  subject          difference      comment
strict-guest   sda4       *                error:fs-open   file system not recognised (no XFS support yet)
strict-host    sda6       /lost+found      crtime          TSK doesn't read ext4 crtime here (#NN)
strict-guest   sda6       *                crtime          the same for every entry
```

- `check` and `partition` name the check and the partition the line belongs to (`[A-Za-z0-9._-]+`). The gate that runs the comparison passes the same names with `--check` and `--partition`. No wildcards, so a line always belongs to exactly one comparison. The checks that commit the list (#17 onwards) define the check names, such as `strict-guest` and `strict-host`.
- `subject` is an encoded path, or `*`. With a path, the line covers that entry only. With `*`, it covers that difference on every entry, or the whole run for `error:<code>`. A line naming the path takes precedence over a `*` line. A `*` line stays in use while any entry still has the difference, so a partial fix doesn't make it stale; prefer path lines once only a few entries are affected.
- `difference` is `missing`, `extra`, one of the 12 compared field names, or `error:<code>`, which needs the subject `*`. `error:<code>` covers a run whose Manifest has `!error` with that code, i.e. a whole-file-system failure such as "file system not recognised".
- The rest of the line is a free-text comment that is shown in the report. Say why the failure is expected and which issue fixes it.
- Blank lines and lines starting with `#` are skipped. A duplicate line or an unknown difference is an error (exit status 2).

The committed list comes with the first Strict check (#17).

### Volatile path list

A text file with one encoded path per line, relative to the file system root. Blank lines and lines starting with `#` are skipped. A Volatile path covers itself and everything below it. The committed lists come with the Best-effort checks (#21).

### The self-test

`selftest.cpp` holds CppUnit test suites over hand-made Manifests. CppUnit is the repository's unit test framework; no other is added. It covers:

- the format: hand-written example lines, round-trips of awkward paths and symlink targets (spaces, tabs, newlines, every byte value, non-UTF-8 bytes, 255-byte names, `-`), timestamps before 1970 and after 2262, `!error` lines, and rejection of malformed and non-canonical input;
- Strict mode: identical Manifests pass; a missing entry, an extra entry, a wrong hash, a wrong size, a one-nanosecond mtime difference and each other compared field fail; an atime difference is ignored;
- Best-effort mode: a difference under a Volatile path, or on an entry with a recent ctime (in Ground truth or in the run), is a warning, but the same pair fails in Strict mode; an old-ctime difference outside Volatile paths fails; both ends of the ctime window, and a ctime after the run (a misread one) tolerates nothing;
- Known failures: a listed difference is tolerated; a listed one that now passes fails; lines for other checks or partitions are ignored; `*` subjects; whole-run errors ("file system not recognised") tolerated, stale or with another code; list parsing;
- the `compare-manifests` command: exit statuses, report, items and their cap, and usage and input errors.

Gate 1 runs it. A failing case fails gate 1 and is named in the report.

## Validation VM

The **Validation VM** is a small AlmaLinux 9.9 virtual machine under VMware Workstation. Its disk holds XFS file systems with different format variants and an ext4 **Control file system**. **Host-side runs** read its `.vmdk` and **Guest-side runs** read its `/dev/sd*` devices, and both are compared with **Ground truth** from the guest's kernel. The scripts in this directory build the VM unattended (issue #13) and control it from the host.

The validate command doesn't use the VM yet. Gates 3 and 4 will (#17, #18, #21).

### VM prerequisites

On the host (Linux), in addition to the [prerequisites](#prerequisites) above:

- VMware Workstation 17.5 or newer (tested with 17.6.2; the `.vmx` uses virtual hardware version 21, which 17.0 to 17.4 cannot open) with its kernel modules loaded: `/dev/vmmon` must exist and `systemctl status vmware` must be active. With Secure Boot on, the `vmmon` and `vmnet` modules must be signed with a key enrolled in MOK (`mokutil --test-key <cert>` must say it is enrolled). Otherwise every VM fails to start.
- The NAT network `vmnet8` with DHCP (the VMware default).
- `vmrun`, `vmware-vdiskmanager` (both come with Workstation), `openssl`, `ssh`, `ssh-keygen`, `sha256sum`, and one of `xorriso`, `genisoimage` or `mkisofs`.
- Network access during the install (see [Installation sources](#installation-sources)).
- The AlmaLinux 9.9 minimal ISO, `AlmaLinux-9.9-x86_64-minimal.iso`, in `~/vmware/tsk-xfs-alma9/iso/`. Download it from <https://repo.almalinux.org/almalinux/9.9/isos/x86_64/>. `build-vm.sh` checks it against the SHA-256 published in that directory's `CHECKSUM` file, which is pinned in `vm-common.sh`.
- About 25 GB free in `~/vmware/` (ISO 3 GB, disk up to 20 GB) and 1.5 GiB of RAM for the running VM.

### Where the VM files and credentials live

Everything the VM needs at run time lives outside the repository, in `~/vmware/tsk-xfs-alma9/` (override it with `TSK_VM_DIR`):

| Path | Contents |
| --- | --- |
| `iso/AlmaLinux-9.9-x86_64-minimal.iso` | Installation ISO (override with `TSK_VM_ISO`) |
| `tsk-xfs-alma9.vmx` | VM configuration |
| `tsk-xfs-alma9.vmdk` | The VM's only disk: 20 GB, thin, single file (`monolithicSparse`) |
| `credentials/id_ed25519`, `id_ed25519.pub` | SSH key the host uses to log in as root |
| `credentials/root-password` | Generated root password, for the VMware console only (SSH accepts only the key) |
| `credentials/known_hosts` | The guest's SSH host key |
| `build/` | Rendered kickstart, `oemdrv.iso` and the installer's serial log (not the repository's `build/`, which holds validate reports) |

None of these are ever committed. The rendered kickstart contains the root password hash and the public key, so it stays in `build/` too.

### First-time VM creation

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

If the install fails, look at `~/vmware/tsk-xfs-alma9/build/install-serial.log`. For the installer's own logs, open the VM in the Workstation GUI before it powers off; Anaconda's shell is on the second virtual console (Ctrl+Alt+F2).

#### Installation sources

The minimal ISO does not include open-vm-tools, which the host needs to find the guest's IP address. The kickstart therefore also uses the AlmaLinux 9.9 BaseOS and AppStream repositories at `repo.almalinux.org`. dnf may then install 9.9 updates that are newer than the packages on the ISO. When AlmaLinux 9.10 is released, 9.9 moves to `vault.almalinux.org`, and the two `repo` lines in `kickstart/ks.cfg.in` must point there.

### Disk layout

GPT, no LVM, on a 20 GiB disk (`/dev/sda` in the guest). `sda6` is the **Control file system**.

| Partition | Size | Mount | File system |
| --- | --- | --- | --- |
| `sda1` | 1 MiB | | BIOS boot |
| `sda2` | 1 GiB | `/boot` | XFS, distribution defaults |
| `sda3` | 10 GiB | `/` | XFS, distribution defaults |
| `sda4` | 4 GiB | `/data` | XFS v5, `mkfs.xfs` defaults: crc, reflink, bigtime, inobtcount, sparse inodes; 4 KiB blocks, 512-byte inodes |
| `sda5` | 2 GiB | `/data-compat` | XFS v5 with reflink, bigtime and inobtcount off; 2 KiB blocks, 1 KiB inodes |
| `sda6` | 1 GiB | `/control` | ext4, `mke2fs` defaults |
| | ~2 GiB | | unallocated |

`/data`, `/data-compat` and `/control` are mounted `noatime`. They are formatted in the kickstart's `%post` with AlmaLinux 9.9's own `mkfs` (xfsprogs 6.4.0, e2fsprogs 1.46.5), so "defaults" means that version's defaults.

### Controlling the VM

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

### Checking the VM

```sh
test/vm/verify-vm.sh
```

This prints one PASS/FAIL line per check and exits 1 if any fails. On the host, it checks for no snapshots, exactly one thin single-file `.vmdk`, no ISO attached, 1 vCPU, 1536 MiB, BIOS, and the VM directory outside the repository. In the guest (`guest/verify-layout.sh`, run over SSH), it checks the partition layout and sizes with `lsblk`, the XFS features with `xfs_info`, the ext4 features with `tune2fs -l`, `noatime`, and that `vmtoolsd` and `sshd` are running. `build-vm.sh` runs it at the end of every build. It starts the VM if needed and leaves it as it found it.

### XFS v4 on the AlmaLinux 9 kernel

`check-xfs-v4.sh` makes an XFS v4 (`crc=0`) image in the guest and tries to mount it.

Result on AlmaLinux 9.9 (tested 2026-11-23, kernel 5.14.0-741.24.1.el9_9, xfsprogs 6.4.0-10.el9_9): **XFS v4 mounts.** The kernel is built with `CONFIG_XFS_SUPPORT_V4=y`. `mkfs.xfs -m crc=0` warns "V4 filesystems are deprecated and will not be supported by future versions", and the mount logs "Deprecated V4 format (crc=0) will not be supported beginning with RHEL10", but the mount succeeds.

The Validation VM's disk has no v4 file system. The validation setup plans to cover v4 with fixture images built in the devcontainer (#16).

### VM scripts

| File | Runs on | Purpose |
| --- | --- | --- |
| `vm-common.sh` | host | Shared settings (paths, pinned ISO checksum) and helpers; sourced by the others |
| `build-vm.sh` | host | Unattended build of the VM |
| `vm.sh` | host | Start, wait, ssh, ip, status, stop |
| `verify-vm.sh` | host | Checks the VM against its specification |
| `check-xfs-v4.sh` | host | Tests whether the guest kernel mounts XFS v4 |
| `kickstart/ks.cfg.in` | installer | Kickstart template |
| `guest/verify-layout.sh` | guest | Layout and file-system checks, sent by `verify-vm.sh` |
