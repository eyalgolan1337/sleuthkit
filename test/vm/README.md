# Validation setup

Checks that the fork's file system support matches what a real, running Linux system sees. The setup is specified in issue #10. Terms in bold are defined in [`GLOSSARY.md`](../../GLOSSARY.md).

Its entry point is the **validate command**, `test/vm/validate.sh`. It gives one pass or fail for the current working tree. A development step is done only when the validate command passes.

## Prerequisites

- Docker (29 or newer works) and the devcontainer image in `.devcontainer/`. Everything is built and checked there, with GCC 16.1 as the only reference toolchain (see `CLAUDE.md`, "Development environment").
- On the host, the devcontainers CLI 0.91.0 or newer (`devcontainer`). If it isn't installed, the command uses `npx @devcontainers/cli@0.91.0`, so Node.js with `npx` is enough. Older CLI versions hang on Docker 29+.
- Network access on the first run: `make check` downloads its test images to `tests/data/` (`tests/test_libraries.sh`).

Later parts of the setup (the **Validation VM**) add their own prerequisites below.

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
| 1 | Clean build of the library, tools, samples and test programs under `bear` (`.devcontainer/build-compile-db.sh`), which also regenerates `compile_commands.json`. Then a syntax-only compile of each changed C++ file as C++17 and as C++20. | Implemented |
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
    gate_begin 1 "Build and C++17/C++20 syntax"
    run_check build "Build (...)" gate1_build   # run_check <slug> <title> <command> [args...]
    ...
    gate_end
}
```

`run_check` runs the command in a subshell from the repository root, with its output going to the check's log. The command reports findings with `item_fail`, `item_warn` and `item_note`. A program that isn't a shell function appends `fail|warn|note<TAB>text` lines to the file named by `$VALIDATE_ITEMS`. A check fails if its command exits non-zero or reports a failure. A gate that isn't implemented calls `gate_not_implemented` instead. Its checks are added when it is implemented.
