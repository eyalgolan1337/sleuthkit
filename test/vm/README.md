# Validation setup

Checks that the fork's file system support matches what a real, running Linux system sees. The setup is specified in issue #10. Terms in bold are defined in [`GLOSSARY.md`](../../GLOSSARY.md).

Its entry point is the **validate command**, `test/vm/validate.sh`. It gives one pass or fail for the current working tree. A development step is done only when the validate command passes.

## Prerequisites

- Docker (29 or newer works) and the devcontainer image in `.devcontainer/`, usable by the host user without `sudo`: the Host-side runs of gates 3 and 4 start one-off containers of that image. Everything is built and checked there, with GCC 16.1 as the only reference toolchain (see `CLAUDE.md`, "Development environment").
- On the host, the devcontainers CLI 0.91.0 or newer (`devcontainer`). If it isn't installed, the command uses `npx @devcontainers/cli@0.91.0`, so Node.js with `npx` is enough. Older CLI versions hang on Docker 29+.
- Network access on the first run: `make check` downloads its test images to `tests/data/` (`tests/test_libraries.sh`).

The **Validation VM** has its own prerequisites; see [Validation VM](#validation-vm).

## Running the validate command

```sh
test/vm/validate.sh              # from the host or inside the devcontainer
test/vm/validate.sh --fresh      # re-run bootstrap and configure first
test/vm/validate.sh --base REF   # compare with REF instead of develop
test/vm/validate.sh --keep-running  # leave the Validation VM running afterwards
```

On the host, the command starts the devcontainer if necessary (`devcontainer up`), collects the Validation VM's results for gates 3 and 4 (see [Gate 3: Strict checks](#gate-3-strict-checks) and [Gate 4: Best-effort checks](#gate-4-best-effort-checks)), and runs itself inside the devcontainer (`devcontainer exec`) for the gates. It never builds in the host tree. The repository is mounted at the same path in both, so the report is visible on the host. In a git worktree, the command uses the main checkout's devcontainer (a container opened on the worktree folder alone couldn't see the main checkout's `.git`), so the worktree must be inside the main checkout, for example under `.claude/worktrees/`. That container is built from the main checkout's `.devcontainer/`. If the tree was configured outside the devcontainer, gate 1 runs `make distclean` and configures again, and the report says so.

"Changed" means changed since the merge-base of `develop` and `HEAD`: committed on the branch, staged, unstaged, or untracked and not ignored.

Exit status: `0` if no gate failed, `1` if any gate failed, `2` if the command couldn't run (for example, the devcontainer didn't start). Every gate runs even if an earlier one fails, and `make check` runs with `-k` so a failure in one directory doesn't stop the tests in the others. On Ctrl-C the command writes a partial report (the running gate `INTERRUPTED`, later ones `NOT RUN`) and exits 130.

A full run builds everything from clean and runs `make check`. It takes a few minutes, plus a minute or two for the Validation VM to boot if it isn't running.

Run it on the host. Inside the devcontainer the command still runs every gate, but it can't reach the Validation VM (no `vmrun`, no SSH key), so gates 3 and 4 fail and the report says why.

## Gates

| Gate | What it checks | Status |
|------|----------------|--------|
| 1 | Clean build of the library, tools, samples, test programs and the validation setup's C++ tools under `bear` (`.devcontainer/build-compile-db.sh`), which also regenerates `compile_commands.json`. The [static guest build](#the-static-guest-build). Then a syntax-only compile of each changed C++ file as C++17 and as C++20, the [comparison step's self-test](#gate-1-comparison-step-self-test) and the [harness self-test](#gate-1-harness-self-test). | Implemented |
| 2 | The full `make check`. | Implemented |
| 3 | **Strict checks** of `/data`, `/data-compat` and the **Control file system** on the Validation VM, each with its [CLI tool checks](#gate-3-cli-tool-checks) (`fls`, `icat`, `fsstat`, `istat`): [Gate 3: Strict checks](#gate-3-strict-checks). | Implemented |
| 4 | **Best-effort checks** of `/` and `/boot` on the Validation VM, in use, with no freeze and no cache flush: [Gate 4: Best-effort checks](#gate-4-best-effort-checks). | Implemented |
| 5 | `clang-format` and `clang-tidy` on changed lines only. | Implemented |

A gate that doesn't exist yet appears in the report as `NOT IMPLEMENTED`. It neither passes nor fails.

### Gate 1: C++17 and C++20 syntax

The in-repo build compiles as C++14, while the owner's vcpkg port may compile the same sources with a newer standard. For each changed C++ source, gate 1 takes the file's own command from `compile_commands.json`, replaces `-std=` with `-std=c++17` (then `-std=c++20`), and compiles syntax-only with:

- `-Werror=register`: GCC only warns about the removed `register` storage class.
- `-D_GLIBCXX_USE_DEPRECATED=0`: libstdc++ otherwise still provides facilities that C++17 removed (`std::auto_ptr`, `std::bind1st`, `std::random_shuffle`, …), which MSVC rejects.

A changed header is checked through every compiled C++ source that includes it, according to automake's dependency files (`.deps/`). A changed C++ file with no compile command (not built in this configuration), or a header no compiled C++ file includes, is reported as a warning because it wasn't checked. If the gate 1 build failed, it is a failure instead, because the compilation database is then incomplete. On release 4.15.0, all 132 C++ translation units pass this check.

### Gate 1: comparison step self-test

`build/vm-tools/manifest-selftest` checks the Manifest reader and writer and the comparison step on hand-made Manifests, the SHA-256 code, Ground truth capture on a temporary directory tree, and the Test corpus generator on corpora it writes to temporary directories. It needs no Validation VM. Each failed case is a failure of gate 1, named in the report. See [The self-test](#the-self-test).

### Gate 1: harness self-test

`build/vm-tools/harness-selftest` (CppUnit) checks the harness's command line, its mapping from libtsk metadata to Manifest entries, its `!error` Manifests, and its walk over a small ext4 image made with `mkfs.ext4 -d`: entries and their order, content hashes, hard links, symlinks, and the partition found through a GPT volume system, by offset or missing. Then `test/vm/harness/selftest.sh` checks the harness and Ground truth capture end to end on an ext4 image, and that `host-run.sh`, the [Host-side run](#gate-3-strict-checks)'s script, refuses a writable image or directory. See [Harness self-test](#harness-self-test).

### Gate 3: Strict checks

A **Strict check** compares TSK's view of a frozen file system with **Ground truth**, field by field (every Manifest field except atime). Gate 3 runs one per partition: `/data` (`sda4`, XFS), `/data-compat` (`sda5`, XFS) and the Control file system `/control` (`sda6`, ext4). Each Strict check has two runs, compared with the same Ground truth and reported as separate checks:

- the **Guest-side run**: the harness, statically linked, reads the partition's `/dev/sdN` device inside the Validation VM;
- the **Host-side run**: the harness of the normal devcontainer build reads partition N of the VM's `.vmdk` on the host, while the VM is running and the file system is frozen.

Until XFS support exists, `/data` and `/data-compat` are [Known failures](#known-failure-list) ("file system not recognised"), so in practice the gate shows that the Control file system passes. `/` and `/boot` are in use and can't be frozen; they get [Best-effort checks in gate 4](#gate-4-best-effort-checks).

**Where it runs.** Only the host can reach the Validation VM, so the work is split:

1. On the host, before the gates, the validate command runs the [static guest build](#the-static-guest-build) and `test/vm/host-build.sh` (the library and `build/vm-tools/tsk-harness` of the normal build; a full build if the tree isn't configured in the devcontainer yet) in the devcontainer, then `test/vm/collect-vm.sh`. That script starts the VM if it isn't running, copies the static guest build and `guest/tsk-validate.sh` to `/root/tsk-validate/` in the guest, checks that each static tool starts there (no glibc version error), and runs the Strict check sequence below for each partition. Each step is one SSH call. The results go to `build/validate/<run>/vm/`, the log to `logs/vm-collect.log`. Then it stops the VM, unless `--keep-running` was given. It stops it even if it was already running, so the VM doesn't hold memory during `make check`.
2. In the devcontainer, gate 3 checks each step's exit status and compares each run's Manifest with Ground truth: `compare-manifests --mode strict --known-failures test/vm/known-failures.txt --check strict-guest --partition sdaN` for the Guest-side run, `--check strict-host` for the Host-side run.

**The Strict check sequence**, per partition:

1. Check that `/dev/sdaN` is mounted on the expected mount point. Then remove `<mount>/corpus` and generate the [Test corpus](#the-test-corpus) there afresh (step `corpus`).
2. `sync`, arm the unfreeze timer, then `fsfreeze --freeze`.
3. Capture Ground truth with `capture-ground-truth` (static), into `/root/tsk-validate/out/` on `/`.
4. `blockdev --flushbufs /dev/sdaN` and `echo 3 > /proc/sys/vm/drop_caches`, so TSK reads the disk and not stale cache pages (#9).
5. On XFS, check with `xfs_db` that the corpus's directories and its extent B+tree file have the on-disk formats they were made for (step `corpus-formats`; see [Checking the XFS formats](#checking-the-xfs-formats)).
6. The Guest-side run: `tsk-harness /dev/sdaN` (static), then its [CLI tool checks](#gate-3-cli-tool-checks): `fls`, `icat`, `fsstat` and `istat` (static) on `/dev/sdaN`.
7. The Host-side run, on the host: `tsk-harness --partition N <vm-dir>/tsk-xfs-alma9.vmdk` in a one-off container (below), then its CLI tool checks on partition N of the `.vmdk` in a second one.
8. `fsfreeze --unfreeze`.

Step 3 also captures the CLI tool checks' Ground truth: the guest's file-system summary and `stat` of the `istat` spot-check paths.

**The Host-side run's one-off container.** `collect-vm.sh` runs `docker run --rm --init --network none` of the devcontainer's own image (the image of the running devcontainer of this checkout, found by its `devcontainer.local_folder` label), as the host user, with three bind mounts at their host paths:

- the repository, read-only (the harness and its staged `libtsk` are in `build/vm-tools/`);
- the VM directory (`TSK_VM_DIR`, default `~/vmware/tsk-xfs-alma9`), read-only;
- the partition's result directory, `build/validate/<run>/vm/strict-host/sdaN/`, the only writable path.

In it, `test/vm/host-run.sh` first checks that the `.vmdk` and both directories are there (a missing mount is an error), then tries to write: it opens the `.vmdk` for appending (which writes nothing) and tries to create a file in the VM directory and in the repository. Each attempt must fail, and the report shows the error, "Read-only file system" for these mounts; if one succeeds, it stops with exit status 3 and the check fails. Then it runs the harness, which opens the image read-only. The guest's `sdaN` is partition N of the `.vmdk`, as `mmls` numbers the allocated partitions. The VM keeps running and writing to the `.vmdk` meanwhile; the frozen partition is the one that can't change. The container is removed when the run ends, also on a timeout (`TSK_VALIDATE_STEP_TIMEOUT`) or an interrupt.

The devcontainer configuration (`.devcontainer/devcontainer.json`) has no VM mount: only these one-off containers see the VM directory, so the devcontainer works the same on a machine without the VM.

**Unfreezing is guaranteed.** The file system is unfrozen after each sequence and on every other exit path: a failed step (including a freeze that half worked), a step that takes longer than `TSK_VALIDATE_STEP_TIMEOUT` seconds (default 600; `timeout` kills its `ssh`), and Ctrl-C or `kill` of the validate command, through `collect-vm.sh`'s `EXIT` trap. If the host can't reach the guest any more (the host side was killed with `SIGKILL`, or the network went away), a transient systemd timer in the guest, armed at each freeze (`tsk-validate-unfreeze-<mount>.timer`), unfreezes it after `TSK_VALIDATE_FREEZE_LIMIT` seconds (default 1800). Checked on 2026-11-23: with the frozen window held open by `TSK_VALIDATE_FROZEN_PAUSE`, `SIGINT` to the validate command's process group (what Ctrl-C sends) in the window of `/control`, and in another run of `/data`, made the command exit 130 with a partial report. A write to the file system succeeded right afterwards, and no unfreeze timer was left. When you try this yourself, don't probe the frozen file system with `timeout N touch ...` first: a write to a frozen file system can't be killed, so `timeout` returns only after the unfreeze.

**The Test corpus** is generated afresh on each of the three partitions in each run, before the freeze. See [The Test corpus](#the-test-corpus).

**Test-only hooks** (environment variables of the validate command, passed to `collect-vm.sh`):

- `TSK_VALIDATE_BREAK=PART:PATH`, for example `sda6:/corpus/links/target.txt`: after Ground truth is captured, flip one byte of that file's first block directly on `/dev/PART`, and put it back after the Host-side run (also on failures and interrupts). Both runs read the changed byte, so both the Guest side and the Host side of gate 3 must then fail and name the entry and the field: `/corpus/links/target.txt` and `sha256` (three times: `/corpus/links/hard-1` and `/corpus/links/sub/hard-2` are the same file).
- `TSK_VALIDATE_FROZEN_PAUSE=N`: wait N seconds in each frozen window, to interrupt the command there.
- `TSK_VALIDATE_WRONG_SUMMARY=PART:FIELD=VALUE`, for example `sda6:block-size=2048`: the `fsstat` checks of `PART`, Guest side and Host side, expect `VALUE` for that [summary field](#gate-3-cli-tool-checks) instead of the guest's own value, so both must fail and name the field. Each of the two checks also reports a warning that the hook was active.
- `TSK_VALIDATE_IMAGE=IMAGE`: run the Host-side runs in one-off containers of IMAGE instead of the running devcontainer's image.

**Reading gate 3 in the report.** The gate has one check for the VM and two per partition, Guest side and Host side:

- `Validation VM and the static guest tools`: what happened to the VM (started, already running, stopped, left running), the image of the Host-side runs' one-off containers (or why there are none), and one note per static tool that started in the guest. It fails if the collection couldn't run, for example no VM, a failed static guest build, or a run inside the devcontainer.
- `Strict check, Guest side: <mount> (<partition>)`: a failed step of the sequence is a failure naming the step. The Test corpus generator's record: one note listing the features it made, and one note per feature it couldn't make on this file system, with the reason (for example `not made here: reflinks: FICLONE: Operation not supported; plain copies instead`). On XFS, one note per path checked by `corpus-formats` with its format, inode, size and extent count, or a failure if a path doesn't have the format it was made for. Then the comparison's findings: each difference is a `failure:` naming the encoded path, the field and both values, then a summary note with the counts, then one `Known failure:` note per tolerated Known failure with its comment from the list. A stale Known failure (listed, but the difference is gone) is a failure. The full per-entry comparison, each step's output and the step timings are in the check's log (`logs/gate3-strict-guest-sdaN.log`). The Manifests are in `vm/strict-guest/sdaN/`.
- `Strict check, Host side: <mount> (<partition>), .vmdk`: one note per refused write in the one-off container (`write refused, <path>: Read-only file system`), a failure for a writable path or a failed Host-side run (no Docker, no devcontainer image, a failed `host-build.sh`: see `logs/vm-host-build.log`), then the comparison's findings, as for the Guest side, against the same Ground truth. The steps the two runs share (freeze, capture, unfreeze) are reported by the Guest side only. Log: `logs/gate3-strict-host-sdaN.log`; the Manifest: `vm/strict-host/sdaN/host.manifest`.

### Gate 3: CLI tool checks

Each Strict check also checks TSK's command-line tools as consumers of the API, on the same frozen file system, against Ground truth taken in the same frozen window. There are four checks per side and partition, reported separately from the harness's: `CLI tool check, Guest side: <tool>, <mount> (<partition>)` and `CLI tool check, Host side: ...`. Their names in the [Known failure list](#known-failure-list) are `<tool>-guest` and `<tool>-host`.

| Tool | What runs | Compared with |
| --- | --- | --- |
| `fls` | `fls -r -p -u` | Ground truth: every path but `/`, and its type. fls prints each control character of a name (0x00 to 0x1F, 0x7F) as `^`, so Ground truth paths are compared in that form too. libtsk's virtual entries (`$OrphanFiles` and anything below it) are left out, as in the harness. A type is fls's letter when its name type and metadata type agree (`r/r`, `d/d`, `l/l`, ...), otherwise `other`. |
| `icat` | `icat` of the root directory's inode, which shows whether icat can open the file system at all, then `icat <inode> \| sha256sum` once per regular file inode of Ground truth (hard links share one) | Ground truth: the SHA-256 of every regular file |
| `fsstat` | `fsstat` | The guest's own file-system summary, in seven fields: `fs-type`, `block-size`, `inode-size`, `group-count` (ext: block groups, XFS: allocation groups), `uuid`, `label` (empty if none) and `version` (ext: the revision level, XFS: 4 or 5). The guest writes `blkid -p -o export` and `dumpe2fs -h` (ext2/3/4) or `xfs_info` (XFS). |
| `istat` | `istat <inode>` for each spot-check path's inode | The guest's `stat` of each path in the committed list [`test/vm/istat-spot-checks.txt`](istat-spot-checks.txt): inode, type, permission bits, uid, gid, size, link count, symlink target (`readlink`), mtime, ctime and crtime, compared as Manifest fields |

The output formats are upstream's; the checks parse them as they are. The tools run with `TZ=UTC`, because `istat` prints times in local time. Two quirks of 4.15.0's output matter here:

- `istat` prints a time at or before the epoch as `0000-00-00 00:00:00`, read as 0, so a file older than 1970 differs from `stat`. Leave such files out of the spot checks.
- ext `fsstat` prints the UUID (`Volume ID`) as two little-endian 64-bit numbers in hex without leading zeros, so its digits are the UUID's bytes in reverse order, sometimes fewer than 32. It counts as the guest's UUID if it is exactly how that UUID prints, as a UUID if it has all 32 digits, and otherwise as `Volume ID <digits>`. The `fsstat` check reads only ext2/3/4 output for now; the XFS work (#11) teaches it XFS's.

**The istat spot checks** are a fixed list of paths per partition, not inode numbers, because the corpus is recreated, with new inode numbers, in every run. In the frozen window the guest runs `stat` on each listed path, and `istat` runs on the inode number that `stat` reports. A listed path that doesn't exist fails the check. Paths are printable ASCII without spaces or `%`.

**Where it runs.** `test/vm/toolcheck/run-tools.sh` runs all four tools on one file system and keeps their output, exit status and standard error. In the guest it runs from `/root/tsk-validate/` with the static guest build (`tsk-validate.sh run-tools`, right after the Guest-side run) into `vm/strict-guest/sdaN/tools/`. On the host it runs in a second one-off container (`host-run.sh --tools`, right after the Host-side run) with `fls`, `icat`, `fsstat`, `istat` and `mmls` of the normal build, into `vm/strict-host/sdaN/tools/`. That container has the same read-only mounts and write probes as the Host-side run's. The steps' logs and exit statuses are kept apart from the harness's, in `vm/strict-guest/sdaN/tool-steps/` (`capture-tools`, `run-tools`) and `vm/strict-host/sdaN/tool-steps/` (`host-tools`), so a failed tool run fails the CLI tool checks, not the harness's Strict checks. It finds partition N with `mmls -a`, the Nth allocated partition, the same one the harness's `--partition N` opens, and passes its offset with `-o` and `-b`. Ground truth for the tools (`fs-summary.txt` and `stat.txt`) is captured by `tsk-validate.sh capture-tools` right after the Manifest, and is in `vm/strict-guest/sdaN/`.

**The comparison** is `build/vm-tools/check-tool`, C++14 like the comparison step and built by the same `manifest.mk`. It turns the tool's output and Ground truth into two Manifests that hold only the compared fields (`fls`: path and type; `icat`: path, type and SHA-256; `istat`: every field but SHA-256 and atime), and compares them with the comparison step's Strict rules and Known failures. `fsstat`'s summaries are compared field by field in the same way: a differing field is a difference named after the field (`fsstat-guest sda6 * uuid ...` in the Known failure list).

```sh
build/vm-tools/check-tool fls|icat|fsstat|istat --ground-truth-dir DIR --run-dir DIR \
    [--known-failures FILE --check NAME --partition NAME] [--items FILE] [--max-items N] \
    [--override FIELD=VALUE]
```

`--ground-truth-dir` holds `ground-truth.manifest`, `fs-summary.txt` and `stat.txt`. `--run-dir` holds what `run-tools.sh` wrote. `--override` (`fsstat` only) is the `TSK_VALIDATE_WRONG_SUMMARY` hook. Exit statuses are `compare-manifests`'s. A tool that fails as a whole makes its run an `!error` one: `fs-open` when libtsk didn't recognise the file system (the tool's error starts with one of libtsk's autodetection errors: `Cannot determine file system type`, `Possible encryption detected`, `Encryption detected` or `Multiple file system types detected`; the harness's code for the same failure, so the XFS partitions' Known failures read `<tool>-<side> sdaN * error:fs-open`), `tool-failed` for any other failure, and `tool-output` for output the check can't parse. A single `icat` that fails leaves its files without a SHA-256, and a single `istat` that fails leaves its entry `missing`.

**Reading the CLI tool checks in the report.** A failed capture of their Ground truth (`capture-tools`) or a failed tool run (`run-tools` in the guest, `host-tools` on the host) is a failure naming the step. A writable path in the host's one-off container is also a failure. Then the comparison's findings come as for the harness's checks, against the same Ground truth: for example `icat-guest sda6: /corpus/links/target.txt: sha256 differs: ...`. The check's log has the tool run's log, each tool's exit status and first line of standard error, and the full comparison; the raw output is in the `tools/` directories above. With `TSK_VALIDATE_BREAK`, the `icat` checks of both sides fail too, naming the file and `sha256`. The other tool checks are unaffected: they don't read file content.

**Self-test.** `test/vm/toolcheck/toolcheck_selftest.cpp` is linked into `manifest-selftest` (gate 1). It covers each parser on output captured from the 4.15.0 tools: fls paths, types, virtual entries and control characters; each autodetection error as "not recognised"; icat results by inode, failed files and probes; `stat` records and `istat` output, including mode strings with setuid, setgid and sticky bits, times before 1970 and after 2038, and istat's zero times; the guest's ext4 and XFS summaries and ext `fsstat` output, including the UUID without leading zeros. It also covers the summary comparison with Known failures, and the `check-tool` command: a pass for every tool, a changed byte failing `icat`, a wrong expected summary value failing `fsstat`, "not recognised" as a Known failure, and usage and input errors.

### Gate 4: Best-effort checks

A **Best-effort check** compares TSK's view of a file system that is in use with **Ground truth**, with no freeze and no cache flush, so it reflects what TSK sees on a running machine: metadata still only in the log, stale cache pages (#9), files changing during the read. Gate 4 runs one per file system that can't be frozen: `/boot` (`sda2`, XFS) and `/` (`sda3`, XFS). Each has a Guest-side run on `/dev/sdN` and a Host-side run on partition N of the `.vmdk`, like the Strict checks, reported as separate checks.

Until XFS support exists, both are [Known failures](#known-failure-list) ("file system not recognised", `error:fs-open`) on both sides, so in practice the gate shows that the sequence ran and that each run failed the expected way. libtsk words the error differently for the two: `Cannot determine file system type` for `/`, and `Possible encryption detected (High entropy ...)` for `/boot` (libtsk's entropy test, run when no file system type is recognised). Both are the same `fs-open` failure.

**The sequence**, per file system, run by `collect-vm.sh` on the host before the Strict checks, so the guest's caches are as normal use leaves them (the Strict checks drop them):

1. Check that `/dev/sdaN` is mounted on the expected mount point.
2. Read the guest's clock, as the start of Ground truth capture.
3. Capture Ground truth of the file system alone (`tsk-validate.sh capture-fs`): `capture-ground-truth` on a non-recursive, read-only, `noatime` bind mount of the mount point in `/run/tsk-validate/`. Other file systems are mounted on directories of `/` (`/boot`, `/proc`, `/run`, `/data`, ...). `stat` of such a directory describes the root mounted on it, not the directory that TSK reads on the device. The bind mount shows the file system without them, and the capture writes nothing to it. The Manifests go to `/run/tsk-validate/out/` (tmpfs), not to the file system being checked.
4. The Guest-side run: `tsk-harness /dev/sdaN`.
5. Read the guest's clock, as the Guest-side run's timestamp.
6. The Host-side run: `tsk-harness --partition N` on the `.vmdk`, in a one-off container, exactly as in [gate 3](#gate-3-strict-checks).
7. Read the guest's clock again, as the Host-side run's timestamp.

There is no `sync`, `fsfreeze` or `drop_caches` step. Gate 4 checks this from the steps the collection recorded: a `freeze`, `unfreeze` or `flush` step fails the check, and the report notes the steps that ran. Results go to `build/validate/<run>/vm/best-effort-guest/sdaN/` and `best-effort-host/sdaN/`.

**The comparison**: `compare-manifests --mode best-effort --run-time <timestamp> --volatile test/vm/volatile-paths/sdaN.txt --known-failures test/vm/known-failures.txt --check best-effort-guest|best-effort-host --partition sdaN`. A difference is tolerated only if its path is under a [Volatile path](#volatile-path-list), or if the entry's ctime (in Ground truth or in the run) is in the 10 minutes before the run finished. Every other difference is a failure. The timestamps come from the guest's clock, which also set the ctimes, so a host clock that is off doesn't matter. The window ends when the run ends. If the run ended more than 10 minutes after Ground truth capture started, the window no longer covers entries changed just before the capture, and the check says so in a warning. Today's runs take seconds; this matters once XFS is read and each run hashes all of `/`.

**Reading gate 4 in the report.** Per file system, two checks:

- `Best-effort check, Guest side: <mount> (<partition>)`: a failed step is a failure naming the step. A note confirms there was no freeze and no cache flush. An incomplete Ground truth (an entry vanished between the directory listing and its `stat`, which is normal on a file system in use) is a warning, not a failure; the comparison then sees the entry as `extra` in the run and applies the same tolerances. Then the comparison's findings, and one `Known failure:` note per tolerated Known failure.
- `Best-effort check, Host side: <mount> (<partition>), .vmdk`: the one-off container's refused writes, a failed Host-side run, then the comparison's findings against the Guest side's Ground truth.

**Best-effort warnings.** Each tolerated difference is a `warning:` that names the encoded path, the difference, both values and why it was tolerated: `under Volatile path /var/log`, or `ctime <time> is within the 10 minutes before the run's end (<time>)`. Warnings never fail the gate. Read them like this:

- Warnings under `/var/log`, `/var/lib` and the other Volatile paths are expected on every run: the system wrote there while the check ran.
- Recent-ctime warnings on files the validate command itself wrote (`/root/tsk-validate/` on `/`) are expected too.
- A warning elsewhere means a file changed in the 10 minutes before the run. Look at it if it repeats on runs where nothing should have changed it: TSK may be reading stale metadata (for example from the log or the cache, #9), or the ctime itself may be misread.
- A difference on an entry whose ctime is older than 10 minutes, outside the Volatile paths, is a failure: nothing on the running system explains it.
- `the run ended Ns after Ground truth capture started, more than the 10-minute recent-ctime window`: the run was too slow for the window to cover the whole check. Failures on entries changed just before the capture may then be live-disk effects, not misreadings.

The report shows at most 20 warnings per check, then a count of the rest. The check's log (`logs/gate4-best-effort-guest-sdaN.log`, `logs/gate4-best-effort-host-sdaN.log`) has the full per-entry comparison, with each step's output and timings.

### Gate 5: format and lint

- `clang-format`: `git clang-format --diff <merge-base>` on changed tracked files, and `clang-format --dry-run --Werror` on new untracked files. Any unformatted changed line fails the gate. To fix it, run `git clang-format develop` (or `clang-format -i <file>` for a new file).
- `clang-tidy`: `clang-tidy-diff` with the repo's `.clang-tidy` on the changed lines. Any diagnostic on a changed line fails the gate. If the gate 1 build failed, this check fails without running: the compilation database is incomplete and a clean result would mean nothing.

## Reading the report

The report is written to `build/validate/<UTC time>/report.md`, and `build/validate/latest` points to the newest run. It is also printed at the end. `build/` is gitignored. The report contains, in order:

1. `Result:` `PASS` or `FAIL`, with the failed gates (and the gates not yet implemented) in brackets.
2. The run: time, branch, `HEAD`, base and merge-base, the toolchain versions, where the logs are, and whether the Validation VM's results were collected (only on the host).
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
    gate_begin 1 "Build, C++17/C++20 syntax and self-tests"
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
| `sha256.h`, `sha256.cpp` | SHA-256 for file content, shared by the harness and Ground truth capture |
| `compare.h`, `compare.cpp` | The comparison step: `compare_manifests`, the Known failure and Volatile path list readers, the report |
| `compare_cli.h`, `compare_cli.cpp`, `compare_main.cpp` | The `compare-manifests` command |
| `selftest.cpp`, `sha256_selftest.cpp` | The self-test (CppUnit), run in gate 1 |
| `../toolcheck/` | The [CLI tool checks](#gate-3-cli-tool-checks): `toolcheck.h`, `toolcheck.cpp` (the tools' output parsers), `toolcheck_cli.*` and `toolcheck_main.cpp` (`check-tool`), `toolcheck_selftest.cpp` (linked into the self-test), `run-tools.sh` |
| `manifest.mk` | Plain GNU make file that builds all of it, the [harness and Ground truth capture](#the-harness-and-ground-truth-capture) and the [Test corpus generator](#the-test-corpus), into `build/vm-tools/` |

```sh
make -f test/vm/manifest/manifest.mk          # libtskmanifest.a, compare-manifests, capture-ground-truth,
                                              # tsk-harness, tsk-corpus, check-tool and the self-tests
                                              # (build the library first)
make -f test/vm/manifest/manifest.mk check    # ... and run the self-tests
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

- `check` and `partition` name the check and the partition the line belongs to (`[A-Za-z0-9._-]+`). The gate that runs the comparison passes the same names with `--check` and `--partition`. No wildcards, so a line always belongs to exactly one comparison. Check names: `strict-guest` and `strict-host` (gate 3, the Guest-side run and the Host-side run of the Strict check), `fls-guest`, `icat-guest`, `fsstat-guest`, `istat-guest` and their `-host` twins (gate 3, the [CLI tool checks](#gate-3-cli-tool-checks)), and `best-effort-guest` and `best-effort-host` (gate 4, the Guest-side run and the Host-side run of the Best-effort check). Partition names are the guest's device names: `sda2` (`/boot`), `sda3` (`/`), `sda4`, `sda5`, `sda6`.
- `subject` is an encoded path, or `*`. With a path, the line covers that entry only. With `*`, it covers that difference on every entry, or the whole run for `error:<code>`. A line naming the path takes precedence over a `*` line. A `*` line stays in use while any entry still has the difference, so a partial fix doesn't make it stale; prefer path lines once only a few entries are affected.
- `difference` is `missing`, `extra`, one of the 12 compared field names, one of the 7 summary fields of the `fsstat` checks (`fs-type`, `block-size`, `inode-size`, `group-count`, `uuid`, `label`, `version`, with the subject `*`), or `error:<code>`, which needs the subject `*`. `error:<code>` covers a run whose Manifest has `!error` with that code, i.e. a whole-file-system failure such as "file system not recognised".
- The rest of the line is a free-text comment that is shown in the report. Say why the failure is expected and which issue fixes it.
- Blank lines and lines starting with `#` are skipped. A duplicate line or an unknown difference is an error (exit status 2).

The committed list is `test/vm/known-failures.txt`. It records `/data` (`sda4`) and `/data-compat` (`sda5`) as `* error:fs-open` for `strict-guest` and `strict-host` and for each CLI tool check: no XFS support yet. On the Control file system (`sda6`), it records the libtsk ext4 defects the [Test corpus](#the-test-corpus) exposes, path by path, on both sides: times before 1970 and after 2106 (#30), slow symlinks with no target (#31), and three phantom names in the 50,000-entry directory (#32). The phantom names are bytes of the directory's htree index: name hashes (seeded per file system) and logical block numbers, both the same in every run on this file system. Two runs gave the same three names. `fls` lists the phantom names too, so they are also `extra` for `fls-guest` and `fls-host`. `fls` also prints a socket's metadata type as `h` (`s/h`): `tsk_fs_meta_type_str` has `s` and `h` in the wrong order for `TSK_FS_META_TYPE_ENUM`, an upstream output defect, so `/corpus/special/socket` is a `type` Known failure of both `fls` checks. `/boot` (`sda2`) and `/` (`sda3`) are `* error:fs-open` for `best-effort-guest` and `best-effort-host`, for the same reason: no XFS support yet.

### Volatile path list

A text file with one encoded path per line, relative to the file system root. Blank lines and lines starting with `#` are skipped. A Volatile path covers itself and everything below it.

The committed lists, one per Best-effort partition, are `test/vm/volatile-paths/<partition>.txt`. Their paths are relative to that file system's root, so `/boot/grub2/grubenv` is `/grub2/grubenv` in `sda2.txt`.

| List | File system | Volatile paths | Why they change |
| --- | --- | --- | --- |
| `sda3.txt` | `/` | `/var/log` | logs and the journal |
| | | `/var/lib` | state that services rewrite at any time: the rpm and dnf databases, systemd timers and random seed, NetworkManager and DHCP leases, logrotate, chrony |
| | | `/var/cache` | dnf metadata, man-db, ldconfig |
| | | `/var/spool` | mail and job queues |
| | | `/tmp`, `/var/tmp` | temporary files (on AlmaLinux 9, `/tmp` is a directory of `/`, not a tmpfs) |
| `sda2.txt` | `/boot` | `/grub2/grubenv` | GRUB's environment block, rewritten after each boot and login (`boot_success`) |

Keep the lists short: a path belongs here only if the running system changes it on its own. Everything else that changes during a run, such as the static guest build that the validate command copies to `/root/tsk-validate/`, is covered by the recent-ctime rule instead.

### The self-test

`selftest.cpp` holds CppUnit test suites over hand-made Manifests. CppUnit is the repository's unit test framework; no other is added. It covers:

- the format: hand-written example lines, round-trips of awkward paths and symlink targets (spaces, tabs, newlines, every byte value, non-UTF-8 bytes, 255-byte names, `-`), timestamps before 1970 and after 2262, `!error` lines, and rejection of malformed and non-canonical input;
- Strict mode: identical Manifests pass; a missing entry, an extra entry, a wrong hash, a wrong size, a one-nanosecond mtime difference and each other compared field fail; an atime difference is ignored;
- Best-effort mode: a difference under a Volatile path, or on an entry with a recent ctime (in Ground truth or in the run), is a warning, but the same pair fails in Strict mode; an old-ctime difference outside Volatile paths fails; both ends of the ctime window, and a ctime after the run (a misread one) tolerates nothing;
- Known failures: a listed difference is tolerated; a listed one that now passes fails; lines for other checks or partitions are ignored; `*` subjects; whole-run errors ("file system not recognised") tolerated, stale or with another code; list parsing;
- the `compare-manifests` command: exit statuses, report, items and their cap, and usage and input errors.

The same binary, `manifest-selftest`, also links `sha256_selftest.cpp`, `../capture/capture_selftest.cpp`, `../corpus/corpus_selftest.cpp` and `../toolcheck/toolcheck_selftest.cpp` (the [CLI tool checks' self-test](#gate-3-cli-tool-checks)):

- SHA-256: the FIPS 180-4 examples, lengths around the padding boundaries, all byte values, and the same data fed in pieces of different sizes;
- the [Test corpus generator](#the-test-corpus), see [Its self-test](#its-self-test);
- the [CLI tool checks](#gate-3-cli-tool-checks): the parsers of the tools' output and `check-tool`;
- Ground truth capture, on a tree built in a temporary directory: each entry type, hard links, symlinks that are recorded but not followed (including 300-byte and non-UTF-8 targets), odd names, nanosecond and pre-1970 timestamps, permission bits, crtime when `statx` reports a birth time, walk order, an unreadable file, a missing root, a round trip of the whole tree through the writer and reader, and the command's exit statuses.

Gate 1 runs it. A failing case fails gate 1 and is named in the report.

## The harness and Ground truth capture

Two programs write Manifests, both through `ManifestWriter`, and both hash file content with the same SHA-256 code (`test/vm/manifest/sha256.h`):

- **Ground truth capture** (`capture-ground-truth`) records a mounted directory tree as the kernel shows it. It runs in the Validation VM's guest.
- The **harness** (`tsk-harness`) records the file system in a disk image as libtsk reads it, through the public API only.

Neither is installed with the library. Both are C++14 and are built by `test/vm/manifest/manifest.mk` into `build/vm-tools/` (gate 1 builds them under `bear`).

| File | Contents |
| --- | --- |
| `capture/capture.h`, `capture.cpp`, `capture_main.cpp` | Ground truth capture: `capture_tree`, the `capture-ground-truth` command |
| `capture/capture_selftest.cpp` | Its CppUnit cases, linked into `manifest-selftest` |
| `harness/harness.h`, `harness.cpp`, `harness_main.cpp` | The harness: `parse_harness_args`, `entry_from_meta`, `run_harness`, the `tsk-harness` command |
| `harness/harness_selftest.cpp` | Its CppUnit cases (`harness-selftest`) |
| `harness/selftest.sh`, `harness/expected-differences.txt` | The [end-to-end self-test](#harness-self-test) on an ext4 image |

### Ground truth capture

```sh
build/vm-tools/capture-ground-truth [-o FILE] DIR
```

Writes one entry per path under `DIR`, `DIR` itself as `/`. Within each directory the names are sorted bytewise, and a directory comes before its contents. For each entry it uses `statx` without following symlinks: type, inode, permission bits, uid, gid, size, link count, and mtime, ctime and atime with nanoseconds. crtime is recorded when `statx` reports a birth time (`STATX_BTIME`), and is `-` otherwise. A regular file's content is read and hashed, opened with `O_NOATIME` where the kernel allows it (the file's owner, or root). A symlink's target is read with `readlinkat`.

- Symlinks are recorded, never followed.
- It stays on `DIR`'s file system. A directory on another device (a mount point) is recorded as `statx` sees it, which is the root of the file system mounted there, but its contents are not. To record what a mount hides, capture a bind mount of the file system instead.
- An entry it can't record fully is reported on standard error. A file that can't be read is recorded without its SHA-256, and an entry that vanished between listing and `statx` is left out.
- Exit status: `0` complete; `1` Manifest written but incomplete (see standard error); `2` bad usage, `DIR` unreadable or the output couldn't be written.

It uses only the C++ standard library and glibc, so it links fully statically for the guest, whose glibc is older than the devcontainer's: `make -f test/vm/manifest/manifest.mk capture-static` writes `build/vm-tools/static/capture-ground-truth`. The harness self-test links it this way and checks that it writes the same Manifest.

### The harness

```sh
build/vm-tools/tsk-harness [--partition N | --offset BYTES] [-o FILE] IMAGE...
```

Opens the image read-only with `tsk_img_open_utf8` (raw, `.vmdk` or any other format libtsk detects; several `IMAGE` arguments are the segments of one split image) and finds the file system:

- `--partition N`: the volume system is opened (`tsk_vs_open`) and the Nth allocated partition, counting from 1 in volume-system order, is opened with `tsk_fs_open_vol`. For a GPT disk whose entries have no gaps, such as the Validation VM's, partition `N` is `/dev/sdaN`. Standard error says at which byte the partition starts.
- `--offset BYTES`: the file system starts at that byte offset (`tsk_fs_open_img`).
- Neither: the image is the file system (offset 0), for example a `/dev/sdaN` device in a **Guest-side run**.

The file system type is always autodetected (`TSK_FS_TYPE_DETECT`). The harness then walks from the root directory with `tsk_fs_dir_open_meta`, `tsk_fs_dir_get_name` and `tsk_fs_dir_get`, writing one entry per allocated name in the same order as Ground truth capture. It skips `.`, `..` and libtsk's virtual entries (`$OrphanFiles` and other `TSK_FS_NAME_TYPE_VIRT*` names). It reads every regular file with `tsk_fs_file_read` and computes the SHA-256 itself; libtsk has no new hashing API. A directory that is one of its own ancestors isn't walked again.

Fields come from `TSK_FS_META`: `addr`, `type`, `mode & 07777`, `uid`, `gid`, `size`, `nlink`, `link` (symlink target), and each time with its `*_nano` part. libtsk reports a missing crtime as 0, so a crtime of exactly 0 is written as `-`.

If the image, volume system, partition, file system or root directory can't be opened, the Manifest is a single `!error` line (see [Failed runs](#failed-runs)) with one of these codes, and the detail is libtsk's error message:

| Code | Meaning |
| --- | --- |
| `img-open` | The image couldn't be opened |
| `vs-open` | `--partition` was given, but no volume system was found |
| `partition` | The volume system has fewer than N allocated partitions |
| `fs-open` | No file system was recognised: for example, an XFS partition before XFS support |
| `root` | The root directory's metadata couldn't be read |

An entry the harness can't record fully is reported on standard error: without metadata it is left out, and a file it can't read is recorded without its SHA-256. Exit status: `0` complete; `1` the Manifest has `!error`, or entries were left out or have no SHA-256; `2` bad usage, or the output couldn't be written.

**Public API only.** The harness includes nothing but `<tsk/libtsk.h>`, and it is compiled against a staged install of the library: `manifest.mk` runs `make install-nobase_includeHEADERS` and `make -C tsk install-libLTLIBRARIES install-nodist_pkgconfigDATA` with `DESTDIR=build/vm-tools/tsk-stage`. The staged include directory, passed with `-isystem`, is the harness's only libtsk include directory, so a header that isn't installed can't be found. It links the staged `libtsk.so`, with an rpath to it. The [harness self-test](#harness-self-test) checks the result: every libtsk header listed in the harness objects' dependency files (compiled with `-MD`, which also lists system headers) must come from the staged install, and none from `tsk/`.

File system autodetection needs the library built with `-fno-strict-aliasing` under GCC 16.1 (`docs/llm/build-and-test.md`, "Known defects and traps", item 19). `.devcontainer/build-compile-db.sh` configures it that way.

### Harness self-test

`test/vm/harness/selftest.sh` checks the harness and Ground truth capture end to end, in the devcontainer, without a Validation VM. Gate 1 runs it after `harness-selftest`, and it writes its findings to the report. Its files are in `build/harness-selftest/`. It needs `mkfs.ext4`, `sfdisk` and `qemu-img`, which come with the devcontainer image.

1. **Test tree.** It builds a directory tree with nested directories, an empty directory, a 300 000-byte file, an empty file, a file with three hard links, short (inline) and long (block) symlinks, relative and dangling symlinks, a FIFO and a socket. It also has names with a space, a tab, a newline, a Latin-1 byte (0xE9), UTF-8, `-`, `%` and 255 bytes, plus setuid, sticky and restrictive permission bits. `mkfs.ext4 -d` (e2fsprogs 1.47) stores whole seconds only and clamps times after 2038, so every entry gets a whole-second mtime it can store, including one before 1970 and 2147483647, the last second of 32-bit time. Nanoseconds and times after 2038 are left to the Validation VM, whose file systems the kernel writes.
2. **Ground truth** of that tree with `capture-ground-truth`, read back by `compare-manifests` (Strict, against itself). The statically linked build must be static and must write the same Manifest (Strict comparison; a byte-for-byte check would trip over the directory atimes that the first capture's `readdir` updated).
3. **Images.** `mkfs.ext4 -I 256 -E root_owner=UID:GID -d TREE` builds a 16 MiB ext4 file system (1 KiB blocks, extents). `sfdisk` makes a GPT disk image with one partition at 1 MiB, and the file system is copied into it. `qemu-img convert -O vmdk` turns the disk into a single-file `monolithicSparse` `.vmdk`, the type the Validation VM uses.
4. **One Manifest, four ways.** The harness reads the raw disk with `--partition 1` (volume system) and with `--offset 1048576`, the `.vmdk` with `--partition 1`, and the bare file system image. All four Manifests must be byte for byte identical.
5. **Harness against Ground truth.** `compare-manifests --mode strict` with `harness/expected-differences.txt` as its Known failure list (check `harness-selftest`, partition `ext4`). Every other difference fails, and so does a listed difference that no longer occurs.
6. **Installed headers only**, as described under [The harness](#the-harness).
7. **`host-run.sh`** (the Host-side run's script, see [Gate 3: Strict checks](#gate-3-strict-checks)). Here, read-only means without write permission, since the devcontainer runs as an ordinary user and has no read-only mounts. With a read-only copy of the `.vmdk` in a read-only directory, it must report both write attempts as refused and write the same Manifest as the harness on the `.vmdk` in step 4. With a writable image, or with a writable directory among the ones to probe, it must exit 3, name the writable path and write no Manifest, and its write probe must leave no file behind. Skipped when run as root.

How the harness was checked against what went into the image (step 5, run on 2026-11-23 with e2fsprogs 1.47.2): Ground truth has 26 entries and the harness 27. Every path, type, mode, uid, gid, file size, link count, symlink target and SHA-256 matches, apart from these expected differences (89 in all):

| Difference | Entries | Why |
| --- | --- | --- |
| inode | all | `mkfs.ext4 -d` allocates new inode numbers. |
| ctime, crtime | all | `mkfs.ext4 -d` sets both to the time it ran. The source's ctime can't be set, and its crtime belongs to its own file system. |
| mtime | `/` | `mkfs.ext4 -d` doesn't copy the root directory's mtime. |
| nlink | `/` | `/lost+found` adds a subdirectory to the root. |
| extra | `/lost+found` | Made by `mkfs.ext4`. |
| size | each directory | A directory's size depends on the file system: 1 KiB ext4 blocks against the source file system's. |
| mtime | `/empty` | **libtsk defect:** it reads ext4's 32-bit timestamps as unsigned, so −1500000000 (1922) comes back as 2794967296 (2058). By the ext4 on-disk format, the 32-bit value is signed when the extra epoch bits are 0. |
| target | `/long-symlink` | **libtsk defect:** `ext2fs_dinode_copy` reads a slow symlink's target (60 bytes or more) only for block-mapped inodes. ext4 maps it with extents, so `meta->link` is NULL and the harness writes `-`. |

Without `-fno-strict-aliasing` (above), every run of step 4 crashes in `tsk_fs_open_img`.

## The Test corpus

The **Test corpus** is the tree the Strict checks compare: 51,069 entries meant to exercise every on-disk format variant. `tsk-corpus`, a C++14 program in `test/vm/corpus/`, generates it from a fixed seed (`0x75b0c0de`). In each run of the validate command, `guest/tsk-validate.sh corpus` removes `<mount>/corpus` and generates it again, on `/data`, `/data-compat` and the Control file system `/control`, before the freeze (step 1 of [the Strict check sequence](#gate-3-strict-checks)).

| Path under `/corpus` | What it covers |
| --- | --- |
| `dirs/shortform`, `dirs/block`, `dirs/leaf` | Directories of 3, 40 and 300 small files. With 4 KiB directory blocks (both XFS partitions) they are a short-form directory (entries in the inode), a block directory (one directory block) and a leaf directory (several data blocks and one leaf block) |
| `dirs/node` | Node directory (more entries than the ~500 one leaf block holds): 600 hard links to the 50 files of `dirs/link-targets`. Its blocks must stay within 12 extents, all the inode holds as a list on the Validation VM, where the SELinux label's attribute fork takes the rest. With 2,000 new files, the files' inode chunks and data blocks interleaved with the directory's blocks and it had 24 extents; with 1,000 hard links, 13 on `/data`. Both made it a B+tree directory. Hard links allocate nothing but the directory's own blocks, and 600 entries need about 9 of them |
| `dirs/btree` | B+tree directory: 50,000 entries with names of 14 to 46 bytes, hard links to the files of `dirs/link-targets`. Hard links keep it to 50 inodes: an ext4 file system of 1 GiB has only 65,536 |
| `fragmented/extents.bin` | 2,000 data extents of 4 KiB, each followed by a 4 KiB hole: an extent B+tree on XFS (an inode's data fork holds 21 extents with 512-byte inodes, 53 with 1 KiB inodes). Holes keep every extent separate whatever the allocator does |
| `sparse/holes.bin`, `sparse/only-hole.bin` | 384 KiB with holes at the start, in the middle and at the end; 1 MiB of nothing but a hole |
| `files/empty`, `small.txt`, `multiblock.bin` | 0, 13 and 14,459 bytes (3.5 blocks of 4 KiB) |
| `files/large.bin` | The large file: 1 GiB on `/data`, 512 MiB on `/data-compat`, 256 MiB on `/control` (`corpus_large_size` in `lib/vm_checks.sh`) |
| `files/preallocated.bin` | 1 MiB allocated with `fallocate` and never written: unwritten extents, read as zeros |
| `reflink/clone.bin`, `reflink/partial.bin` | Reflinked copies of `reflink/source.bin` (1 MiB): whole, and with 4 KiB in the middle overwritten. Where the file system has no reflinks, plain copies with the same content |
| `links/` | A file with 3 hard links (`target.txt`, `hard-1`, `sub/hard-2`); symlinks with targets of 10 bytes (`short-symlink`), 300 bytes (`long-symlink-300`) and 1,023 bytes (`max-symlink-1023`, the longest XFS stores: `XFS_SYMLINK_MAXLEN`, 1,024, counts the terminating NUL), relative, absolute, dangling and to a directory |
| `names/` | UTF-8 names (2-, 3- and 4-byte sequences), names with spaces, a tab, a newline, `%` and a leading `-`, two files and a directory with 255-byte names (one of 85 three-byte UTF-8 characters) |
| `times/before-1970/`, `times/`, `times/after-2038/` | mtime and atime from 1901-12-13 (the 32-bit minimum) to 1969-12-31T23:59:59.999999999; the epoch and the last second of 32-bit time; 2038-01-19T03:14:08, 2100, 2262 (past 64-bit nanoseconds), 2446-05-10T22:38:55 (ext4's maximum) and 2486-07-02T20:20:24 (XFS bigtime's maximum, as the kernel clamps it) |
| `special/` | A FIFO, a socket, a character device and a block device |
| `owners/` | uid and gid 1000, 65534 and 100; setuid, setgid, sticky and mode `0000` |
| `empty-dir` | An empty directory |

Every file's content is derived from the seed and its path (xoshiro256\*\*, seeded with splitmix64), and every entry gets a seeded mtime and atime with nanoseconds, so two runs on an empty target give the same tree, content, modes, owners, mtimes and atimes. Only what the kernel chooses can differ: inode numbers, ctime, crtime and the physical layout.

### The record

The generator adapts to the file system and prints a record, one line per feature: `made`, `not-made` with the reason, or `failed` (the generator stopped; the step fails). Where it can, it makes the nearest thing instead, so the paths stay the same on every partition: plain copies instead of reflinks, and a timestamp the file system can't store is stored as the kernel clamps it. The record is the `corpus` step's log (`vm/strict-guest/sdaN/corpus.log` in the run directory), and gate 3 reports it (see [Gate 3: Strict checks](#gate-3-strict-checks)). On the Validation VM:

| Feature | `/data` (sda4) | `/data-compat` (sda5) | `/control` (sda6, ext4) |
| --- | --- | --- | --- |
| Reflinks | made | not made: `FICLONE: Operation not supported` (reflink off); plain copies | not made: the same |
| Timestamps after 2038 | made, up to 2486-07-02T20:20:24 | not made: each clamped to 2038-01-19T03:14:07 (no bigtime) | made up to 2446-05-10T22:38:55; 2486-07-02T20:20:24 clamped to that |
| Every other feature | made | made | made |
| Size of `files/large.bin` | 1 GiB | 512 MiB | 256 MiB |
| Space used after generation | 1.1 GiB of 4 GiB (28%) | 546 MiB of 2 GiB (28%) | 273 MiB of 974 MiB (31%); 479 of 65,536 inodes |

The generator takes 1 to 6 seconds per partition, and each Ground truth capture of a corpus 2 to 5 seconds.

### Checking the XFS formats

The format each XFS directory and the extent B+tree file has is checked in every run, inside the frozen window after the caches are flushed, so the device holds the current metadata (`tsk-validate.sh corpus-formats MNT DEV`, step 5 of the sequence). For each path it looks up the inode with `stat`, then reads it with `xfs_db -r`:

- `print core.format core.size core.nextents` on the inode: the data fork format is `local` (1) for short-form, `btree` (3) for `dirs/btree` and `fragmented/extents.bin`, `extents` (2) for the others.
- For an `extents` directory, `dblock 0` and `dblock <32 GiB in file system blocks>` (`XFS_DIR2_LEAF_OFFSET`), then `print`: a block directory has one block of the directory block size whose magic is `XDB3` (v5) or `XD2B` (v4) and nothing at the leaf offset. A leaf directory has a leaf1 block there (magic `0x3df1`, v4 `0xd2f1`), a node directory a da-btree node (`0x3ebe`, v4 `0xfebe`).

A path without its format fails gate 3. On ext4 the step prints one `SKIP` line. Result on the Validation VM (2026-11-23), `/data`:

| Path | Format | Inode data fork | Size | Extents |
| --- | --- | --- | --- | --- |
| `dirs/shortform` | short-form | `local` | 39 | 0 |
| `dirs/block` | block | `extents` | 4,096 | 1 |
| `dirs/leaf` | leaf | `extents`, leaf1 block at the leaf offset | 12,288 | 4 |
| `dirs/node` | node | `extents`, da-btree node at the leaf offset | 20,480 | 9 |
| `dirs/btree` | B+tree | `btree` | 2,322,432 | 220 |
| `fragmented/extents.bin` | extent B+tree | `btree` | 16,379,904 | 2,000 |

`/data-compat` has the same formats (leaf 3 extents, node 5, B+tree directory 222). The extent counts vary a little from run to run with the physical layout; the formats don't.

### Regenerating it

The validate command regenerates the corpus in every run. To regenerate it by hand in the guest, after a validate run has copied the static guest build to `/root/tsk-validate`:

```sh
test/vm/vm.sh ssh /root/tsk-validate/tsk-validate.sh corpus /data 1G
```

or, with the generator directly, into any empty or new directory:

```sh
tsk-corpus [--seed N] --large-size SIZE [--btree-entries N] DIR   # SIZE in bytes, or with K, M or G
```

Exit status: `0` complete, `1` the generator failed (see the record's `failed` line), `2` bad usage or `DIR` not empty. The generator needs only glibc and the C++ standard library; `manifest.mk` builds it as `build/vm-tools/tsk-corpus`, and its `corpus-static` target builds the static copy the guest uses.

### How determinism was checked

On the Validation VM (2026-11-23), the final generator ran twice on each partition. Each run went into an emptied `<mount>/corpus` and was followed by a Ground truth capture of it. In each pair of Manifests, the same 51,070 paths have the same type, mode, uid, gid, size, link count, symlink target, SHA-256, mtime and atime. Only inode numbers (on XFS; ext4 handed out the same ones), ctime and crtime differ, which the generator can't set. Both runs also printed the same record. The self-test checks the same property in gate 1 on every run.

### Its self-test

`corpus_selftest.cpp`, linked into `manifest-selftest` and run in gate 1, generates the corpus (with a 3 MiB large file and 1,500 B+tree directory entries) into temporary directories and checks it through the kernel and Ground truth capture, not through the generator's code:

- two runs give Manifests that are equal in every compared field except inode, ctime and crtime, and the same record; another seed gives other content;
- the PRNG's first output for seed 0 is pinned to the xoshiro256\*\* reference value, because changing the stream changes every corpus;
- the directory entry counts, the 50 inodes behind the node and B+tree directories and their link counts, the sizes, the holes (`SEEK_DATA`/`SEEK_HOLE`), the 2,000 extents, the reflinked or copied content, the links and symlink lengths, the 255-byte names, the timestamps, the modes, and that every entry has a seeded mtime;
- the record has each feature exactly once, made or not made with a reason (as an ordinary user: device files and owners not made, `Operation not permitted`), and a non-empty target, a missing parent and bad options are refused.

## The static guest build

The Validation VM runs AlmaLinux 9 (glibc 2.34), older than the devcontainer's glibc, so the tools that run in the guest are linked fully statically. `test/vm/guest-build.sh` (devcontainer; `--fresh` starts over) builds them:

1. A copy of the working tree in `build/guest-static/src`: every tracked file and every untracked file `.gitignore` doesn't exclude (`test/vm/lib/sync_tree.py`). Only files whose content changed are copied, and a copied file gets the current time as its modification time (not the source's, which may be older than the objects built from the previous version), so later builds are incremental and still rebuild what changed. Files removed from the tree are removed from the copy.
2. `./bootstrap` and `./configure --disable-shared --enable-static --disable-java --without-afflib --without-libewf --without-libvhdi --without-libvmdk --without-libbfio --without-libvslvm` with `-g -O2 -fno-strict-aliasing`, on the first build only.
3. `make -C tsk`, then `fls`, `icat`, `fsstat` and `istat` with `LDFLAGS=-all-static`, which makes libtool link glibc and libstdc++ statically too.
4. `tsk-harness`, `capture-ground-truth` and `tsk-corpus` with `manifest.mk`'s `harness-static`, `capture-static` and `corpus-static` targets (output in `build/guest-static/vm-tools/static/`). The harness is compiled against a staged install of this static library, like the normal harness, and linked with `-static` and the `-l` flags of `libtsk.la`'s `dependency_libs`.
5. Stripped copies in `build/guest-static/bin`, each checked to be statically linked. The directory is emptied first, so a failed build never leaves old binaries for the guest.

On the host, the validate command runs it before collecting the VM's results; gate 1's `Static guest build` check runs it again (an incremental no-op then) and reports the result. Gate 3's VM check shows that each binary starts in the guest.

## XFS fixture images

Small XFS images, committed in `test/vm/fixtures/xfs/`, that tests can use with no Validation VM, network or root. They cover format variants that the Validation VM's disk doesn't have, such as v4 and 512-byte blocks. Each **Fixture image** is committed xz-compressed, as `<variant>.img.xz`, next to its expected Manifest, `<variant>.manifest.xz` (xz-compressed too: the 64 KiB variant's Manifest alone is 5.6 MB of text).

| Variant | `mkfs.xfs` options | Image size | Block size | Directory block | Inode size | Format |
| --- | --- | --- | --- | --- | --- | --- |
| `v4` | `-m crc=0` | 320 MiB | 4096 | 4096 | 256 | v4 |
| `v4-512` | `-m crc=0 -b size=512` | 320 MiB | 512 | 4096 | 256 | v4 |
| `v5-1k` | `-b size=1024` | 320 MiB | 1024 | 4096 | 512 | v5, bigtime |
| `v5-64k` | `-b size=65536` | 512 MiB | 65536 | 65536 | 512 | v5, bigtime |
| `v5` | none (defaults) | 320 MiB | 4096 | 4096 | 512 | v5, bigtime |

Every variant also gets `-m uuid=<fixed>` and `-L tsk-<variant>`. "Defaults" are the defaults of the devcontainer's xfsprogs (Debian trixie, 6.13.0): for v5 that is crc, finobt, sparse inodes, rmapbt, reflink, bigtime, inobtcount and nrext64. The full `xfs_info` output is in a comment at the top of each Manifest. `mkfs.xfs` refuses file systems of 300 MB or less, and with 64 KiB blocks it needs 512 MiB for the minimum log size. The images are sparse when decompressed with `xz -d`, so they take little disk space.

Together the ten files are about 1.6 MB:

| Variant | `.img.xz` | `.manifest.xz` | Entries |
| --- | --- | --- | --- |
| `v4` | 84,200 | 14,956 | 1,006 |
| `v4-512` | 71,144 | 14,604 | 1,006 |
| `v5-1k` | 106,924 | 18,332 | 1,181 |
| `v5-64k` | 982,796 | 226,184 | 18,821 |
| `v5` | 118,184 | 17,912 | 1,181 |

### What the images contain

Every variant has the same tree, with entry counts scaled to its geometry:

| Path | What it covers |
| --- | --- |
| `/dirs/shortform` | Short-form directory: entries in the inode (data fork `local`) |
| `/dirs/block` | Block directory: one directory block |
| `/dirs/leaf` | Leaf directory: several data blocks and one leaf block |
| `/dirs/node` | Node directory: more entries than one leaf block holds, so a da-btree node |
| `/dirs/btree` | B+tree directory: more extents than fit in the inode (data fork `btree`), made by putting a file with one byte of data between directory blocks |
| `/empty`, `/small.txt`, `/multiblock.bin` | Regular files of 0 bytes, a few bytes and 3.5 blocks |
| `/sparse.bin` | 512 KiB file with holes at the start, in the middle and at the end |
| `/setuid`, `/setgid` | Modes 4755 and 2711 |
| `/xxx…x` | A 255-byte name |
| `/fifo`, `/chardev`, `/blockdev` | Special files |
| `/symlink-short`, `/symlink-long` | Symlinks stored in the inode and in a remote block (719-byte target) |
| `/times/*` | Timestamps at the epoch, before 1970, at both ends of the 32-bit range and, on bigtime variants, after 2038 and at the bigtime maximum (2486), each with nanoseconds |

uid and gid vary, and every inode's atime, mtime, ctime and (v5) crtime are different, fixed values with nanoseconds. A protofile can't express hard links, sockets, the sticky bit, extended attributes or names with white space, so the images have none of these.

### Building and checking the images

```sh
test/vm/fixtures/xfs/xfs-fixtures.py build [VARIANT...]   # build and check; write <variant>.img.xz and .manifest.xz
test/vm/fixtures/xfs/xfs-fixtures.py verify [VARIANT...]  # check the committed images against the committed Manifests
test/vm/fixtures/xfs/xfs-fixtures.py check [VARIANT...]   # rebuild in a temporary directory; compare with the committed Manifests
```

Run it in the devcontainer, as an ordinary user. It needs `xfsprogs` and `xz` (both in the devcontainer image; rebuild the container if `mkfs.xfs` is missing) and builds `build/vm-tools/compare-manifests` if necessary. Nothing is mounted and nothing needs root. `--keep DIR` also copies the uncompressed images and Manifests to `DIR`. A full `build` takes under a minute, most of it for `xz -9`.

How an image is built:

1. The script generates the tree description of the variant. It writes a protofile (see `mkfs.xfs(8)`, `-p`) with the content of each regular file in a staging directory. The sparse file's holes are holes in the staging file, which `mkfs.xfs` keeps.
2. `mkfs.xfs -p protofile` makes and fills the image in one step.
3. A protofile can't set timestamps. The script writes atime, mtime, ctime and crtime into each inode's bytes and recomputes the inode's CRC (v5). It finds the inodes with `xfs_db`'s `ls` and `convert`. It doesn't use `xfs_db`'s `write` for this: in 6.13, writing a bigtime timestamp's `sec` field stores the raw 64-bit value, and negative 32-bit values come out wrong.
4. The expected Manifest comes from the tree description, except for two fields that `mkfs.xfs` chooses: inode numbers and directory sizes, which are read from the image.
5. Only when every requested variant has been built and checked are the committed files replaced, so a failure leaves them as they were.

How an image is checked (`build`, `verify` and `check` all do this):

- `xfs_repair -n` finds no problems.
- The image is read back using `xfs_db` only, with no code shared with the writing side: the tree with `ls`, each inode's fields with `print` (including the timestamps, which `xfs_db` decodes itself, and on v5 that the inode CRC is correct), and file content and remote symlink targets from the blocks listed by `bmap`. The resulting Manifest must equal the expected one under the Strict rules of `compare-manifests`, so every path, type, inode number, mode, uid, gid, size, link count, symlink target, SHA-256, mtime, ctime and crtime is compared (atime is recorded but, as always, not compared). At build time, inode numbers and directory sizes are compared with themselves, because the expected Manifest took them from the image; `check` compares them with the committed Manifest.
- Each directory under `/dirs` has the format in its name, according to `xfs_db`. Short-form means the data fork is `local`. Block means `extents`, one directory block whose magic is a block directory's (`XD2B` on v4, `XDB3` on v5), and nothing at the leaf offset. Leaf and node mean `extents`, with a leaf1 block or a da-btree node block at the leaf offset (32 GiB). B+tree means the data fork is `btree`. The script prints each directory's format, size and extent count.
- `/sparse.bin` maps every block that holds data and none in the three holes.

`check` builds every image again in a temporary directory and then also compares the Manifest read back from the new image with the committed one. Regeneration isn't byte-identical (inode generation numbers are random, for example), but the tree, the inode numbers and the timestamps must be identical. The images were built with xfsprogs 6.13.0-2+deb13u1. The Dockerfile doesn't pin that version (Debian drops superseded versions from its archive, which would break the image build). Another xfsprogs version may place inodes differently or change the defaults. `check` then fails, and the images and Manifests must be rebuilt and committed together.

The checks were themselves tested by breaking a committed image (one byte of `/small.txt` changed: `sha256 differs`) and a committed Manifest (a mode changed: `mode differs`); `verify` failed both times. A directory that missed its format (an early B+tree directory on `v5-1k` had only 16 extents) and a sparse file mapped differently than expected both stopped the build.

## Validation VM

The **Validation VM** is a small AlmaLinux 9.9 virtual machine under VMware Workstation. Its disk holds XFS file systems with different format variants and an ext4 **Control file system**. **Host-side runs** read its `.vmdk` and **Guest-side runs** read its `/dev/sd*` devices, and both are compared with **Ground truth** from the guest's kernel. The scripts in this directory build the VM unattended (issue #13) and control it from the host.

Gates 3 and 4 of the validate command use it (see [Gate 3: Strict checks](#gate-3-strict-checks) and [Gate 4: Best-effort checks](#gate-4-best-effort-checks)).

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

The Validation VM's disk has no v4 file system. The [XFS fixture images](#xfs-fixture-images) cover v4 instead.

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
| `collect-vm.sh` | host | Collects the VM's results for gates 3 and 4: VM start and stop, the Best-effort checks, the Strict check sequence, the Host-side runs' one-off containers |
| `guest/tsk-validate.sh` | guest | One step of the Strict check sequence or the Best-effort check per call (Test corpus, freeze, capture, capture-fs, flush, XFS formats, run, unfreeze, test hooks), copied by `collect-vm.sh` |
| `guest-build.sh` | devcontainer | The [static guest build](#the-static-guest-build) |
| `host-build.sh` | devcontainer | The normal build's library and `tsk-harness`, for the Host-side runs |
| `host-run.sh` | one-off container | The Host-side run: read-only probe, then the harness on the `.vmdk`; started by `collect-vm.sh` |
| `lib/vm_checks.sh` | both | The Strict check and Best-effort check partitions and mount points, shared by `collect-vm.sh` and gates 3 and 4 |
| `volatile-paths/` | devcontainer | The committed [Volatile path lists](#volatile-path-list) |
| `known-failures.txt` | devcontainer | The committed [Known failure list](#known-failure-list) |
