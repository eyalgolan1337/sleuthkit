# The Sleuth Kit (fork)

A fork of The Sleuth Kit, a C/C++ library (`libtsk`) for forensic analysis of disk images, volume systems and file systems.

All work should be done against our fork, never upstream unless explicitly told to do so.
Branch all work from `develop`, which is release 4.15.0 (tag `sleuthkit-4.15.0`) plus the fork's tooling and docs (devcontainer, format and lint configs, `CLAUDE.md`, `docs/`). Do not branch from the bare tag: it has none of those.

## Upstream compatibility

The fork adds features on top of upstream; it should stay easy to diff against and merge with upstream, and stay source-compatible for existing API users.

- Add, don't modify. Prefer new files and new entries in existing lists (`_SOURCES`, `nobase_include_HEADERS`, vcxproj items, enum values, table rows) over editing upstream code.
- Do not change or remove existing public API: signatures, struct layouts, existing enum values, macros, installed header names, tool options or output formats. Additions that a new feature needs are expected, for example new `TSK_FS_TYPE_ENUM` values and the matching type-table and opener entries for a new file system.
- Avoid changing `configure.ac` and the build machinery (`m4/`, `bootstrap`, compiler flags, the C++ standard). Work around upstream build quirks outside the tree (the devcontainer does this for cppunit and libbfio) instead of fixing them here. If a change there is unavoidable, keep it to the smallest additive line and call it out in the PR.
- Do not fix, reformat or refactor upstream code that the task does not need, including the entries under "Known defects and traps" in `docs/llm/`. Report them instead.
- If a task seems to require breaking any of this, stop and ask first.

## Scope

Work on the C/C++ library in `tsk/` and its public API (`tsk/libtsk.h`). `tools/`, `samples/`, `tests/` and `unit_tests/` matter as consumers of that API. Leave the Java and Autopsy parts alone: `bindings/`, `case-uco/`, `debian/`, `rejistry++/`, `db_diff/`.

## Toolchain

- Linux: GCC 16.1, provided by the devcontainer (below); the host compiler is not the reference. Windows: MSVC 19.41 (Visual Studio 2022 17.11).
- C++ standard: the target is C++17, but the in-repo builds compile as C++14 (`configure.ac` forces `-std=c++14`; `win32/libtsk/libtsk.vcxproj` sets no `LanguageStandard`, so MSVC uses its C++14 default). New code must compile as both C++14 and C++17.
- The owner consumes the library through their own vcpkg port, which lives outside this repo. The in-repo builds are autotools (`configure.ac` plus one `Makefile.am` per directory, recursive) and Visual Studio (`win32/`); there is no CMake.
- A new library source goes in its layer's `tsk/<layer>/Makefile.am` `_SOURCES` list and in `win32/libtsk/libtsk.vcxproj` (and `.vcxproj.filters`). A new installed header also goes in `nobase_include_HEADERS` in the top-level `Makefile.am`.

## Development environment

Build, test, format and lint inside the devcontainer in `.devcontainer/` (GCC 16.1 on Debian trixie, LLVM 22 clangd/clang-format/clang-tidy, bear, gdb, valgrind, cppunit, libewf/libvhdi/libvmdk/afflib, Claude Code, gh). It mounts the repo at the same absolute path as on the host, so `compile_commands.json` paths are valid on both sides.

```sh
devcontainer up --workspace-folder .                # or VS Code: "Reopen in Container"
devcontainer exec --workspace-folder . bash         # shell inside
devcontainer exec --workspace-folder . claude       # Claude Code inside (log in once; config persists in a volume)
```

(`devcontainer` is `npx @devcontainers/cli@0.91.0`. Use 0.91.0 or newer: older versions hang after "Container started" on Docker 29+, whose events no longer carry the `status` field they wait for.) If you are not running inside the container (`gcc --version` is not 16.1), run build and test commands through `devcontainer exec --workspace-folder . <cmd>`.

- `post-create.sh` bootstraps, configures, builds everything under bear and installs the clangd-lsp plugin.
- `compile_commands.json` (gitignored) feeds clangd and clang-tidy. Regenerate it after adding or removing sources: `.devcontainer/build-compile-db.sh` (`--fresh` to re-run bootstrap and configure).
- Configure with `CFLAGS` and `CXXFLAGS` set to `-g -O2 -fno-strict-aliasing` (the script does): without it, GCC 16.1 breaks every file system autodetection (`docs/llm/build-and-test.md`, trap 19).
- Configure with `--without-libbfio` in the container (the script does). Debian's libewf exports bundled `libbfio_*` symbols, and linking the system libbfio next to it crashes every E01 close. This also turns off LVM pool support.
- Do not mix host and container builds in one tree; run `make distclean` before switching.

## Build and test

```sh
./bootstrap && ./configure --disable-java --without-libbfio \
    CFLAGS='-g -O2 -fno-strict-aliasing' CXXFLAGS='-g -O2 -fno-strict-aliasing'
make -j"$(nproc)"
make -j"$(nproc)" check                     # all tests; needs network the first time (tests/test_libraries.sh downloads images to tests/data)
make -C unit_tests check                    # cppunit unit tests only
./unit_tests/base/test_base                 # run the cppunit binary directly
```

Tests:

- Unit tests use **cppunit** (`unit_tests/`). Match it; do not introduce a second framework (no Catch2, GoogleTest or CTest). Fixtures register themselves with `CPPUNIT_TEST_SUITE_REGISTRATION`, and `unit_tests/base/test_base.cpp` runs the whole registry (there is no per-test filter). A test for another layer needs a new `unit_tests/<layer>/Makefile.am`, an entry in `SUBDIRS` in `unit_tests/Makefile.am` and in `AC_CONFIG_FILES` in `configure.ac`, then `./bootstrap`.
- `tests/` holds API test programs and scripts. `test_libraries.sh` runs `mmls` on vmdk/vhd/E01 images. `runtests.sh` is skipped (exit 77) unless the private image set in `~/from_brian` exists.
- `unit_tests/` is built only when configure finds the `cppunit-config` script (`grep CPPUNIT_TRUE config.status` shows `""`, not `"#"`). Debian's cppunit doesn't ship it; the devcontainer adds a shim over pkg-config. `--disable-cppunit` has no effect (upstream quirk; `configure.ac` is deliberately left as upstream).

Every non-trivial function you add or change gets cppunit unit tests for the specific behaviour. After any change, run the full `make check` in the devcontainer and report the results.

Formatting and lint (configs: `.clang-format`, `.clang-tidy` at the repo root). Most existing code predates them, so touch only your own lines:

```sh
git clang-format develop                    # format only lines changed since develop
git diff -U0 develop -- tsk tools | clang-tidy-diff -p1 -path .   # lint only changed lines
clang-tidy -p . tsk/fs/foo.cpp              # lint one file
```

Use the clangd-lsp plugin (go-to-definition, references, diagnostics) when navigating the code. It needs `compile_commands.json`.

Use git best practices, trunk-based development, small branches and informative, conventional commit messages.

## Reference docs

Read the matching file in `docs/llm/` before working in an area. Each ends with a "Known defects and traps" section.

- `docs/llm/base-and-img.md`: error handling, `TSK_TCHAR` and Windows/POSIX differences, core types, opening and reading disk images, `tsk/util/`.
- `docs/llm/vs-and-pool.md`: partition tables, APFS and LVM pools, offsets and units between layers.
- `docs/llm/fs.md`: file systems, files, directories, attributes, walks, encryption, adding a file system.
- `docs/llm/auto-hashdb-cpp.md`: `TskAuto`, hash databases, the C++ wrapper classes, `samples/`.
- `docs/llm/build-and-test.md`: repo map, build options, `HAVE_*` feature macros, installed headers, tests and test data, coding conventions, compiler portability notes.

## Agent skills

### Issue tracker

Issues live in the fork's GitHub Issues (eyalgolan1337/sleuthkit), managed with the `gh` CLI. See `docs/agents/issue-tracker.md`.

### Triage labels

Default vocabulary: `needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, `wontfix`. See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: `docs/llm/` is the per-area domain reference; a root `GLOSSARY.md` and `docs/adr/` are created only when needed. See `docs/agents/domain.md`.
