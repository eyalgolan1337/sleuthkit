# The Sleuth Kit (fork)

Terms used by this fork when validating file system support against real disks. Library concepts (`TSK_FS_INFO`, pool, attribute, run, …) are defined in `docs/llm/`.

## Validation

**Validation VM**:
The single AlmaLinux 9 virtual machine, run under VMware Workstation, whose disk is read to check that TSK lists and reads files correctly.
_Avoid_: test machine, guest box

**Host-side run**:
TSK running on the host machine and reading the Validation VM's `.vmdk` while the VM is running. It runs in a one-off container of the devcontainer image that sees the `.vmdk` read-only.
_Avoid_: offline run, vmdk test

**Guest-side run**:
TSK running inside the Validation VM and reading one of its own block devices (`/dev/sd*`) as a raw image while that file system is mounted.
_Avoid_: live run, in-VM test

**Ground truth**:
The Validation VM's own view of a mounted file system (its paths, file types, sizes and content hashes), taken through the kernel at the same moment as a run.
_Avoid_: expected output, reference listing

**Test corpus**:
The generated set of files and directories on the dedicated `/data` file system, built to exercise each on-disk format variant.
_Avoid_: fixtures, sample data

**Control file system**:
An ext4 file system on the Validation VM that holds the Test corpus. Because TSK already reads ext4, it shows that the validation setup itself works.
_Avoid_: baseline partition, sanity disk

**Manifest**:
A one-line-per-entry record of a file system's contents, in one shared format, produced both for Ground truth and by every run.
_Avoid_: listing, dump, snapshot

**Known failure**:
A recorded, expected difference that a gate tolerates until the step that fixes it. Once a Known failure starts passing, it must be removed.
_Avoid_: expected failure, allowlist entry

**Strict check**:
A comparison of a run against Ground truth while the file system is frozen. Every entry and every byte must match.
_Avoid_: exact test

**Best-effort check**:
A comparison of a run against Ground truth on a file system that is in use and not frozen. A difference is tolerated, and reported as a warning, only under a Volatile path or for an entry changed in the 10 minutes before the run.
_Avoid_: fuzzy test, live check

**Volatile path**:
A path whose contents are expected to change while a Best-effort check runs, so a mismatch under it is not a failure.
_Avoid_: ignore list, exclusion

**Validate command**:
The single command, `test/vm/validate.sh`, that runs every gate on the current working tree and gives one pass or fail with a report.
_Avoid_: CI, test script, check script

**Gate**:
One numbered group of checks in the validate command (build, `make check`, Strict checks, Best-effort checks, format and lint). The validate command passes only if no gate fails.
_Avoid_: stage, step, phase

**Fixture image**:
A small committed XFS image, built in the devcontainer from a protofile without mounting, with its expected Manifest. Fixture images cover format variants the Validation VM's disk doesn't have, and need no VM, network or root. Not the Test corpus, which lives on the Validation VM.
_Avoid_: test image, sample image
