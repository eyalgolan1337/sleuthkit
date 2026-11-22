# The Sleuth Kit (fork)

A fork of The Sleuth Kit, a C/C++ library (`libtsk`) for forensic analysis of disk images, volume systems and file systems.

All work should be done against our fork, never upstream unless explicitly told to do so.
And all work should be done branched out of the latest release - 4.15 tag.

## Scope

Work on the C/C++ library in `tsk/` and its public API (`tsk/libtsk.h`). `tools/`, `samples/` and `test/` matter as consumers of that API. Leave the Java and Autopsy parts alone: `bindings/`, `case-uco/`, `debian/`, `rejistry++/`, `db_diff/`.

## Toolchain

- Linux: GCC 16.1. Windows: MSVC 19.41 (Visual Studio 2022 17.11). Code must build on both, as C++17.
- The owner consumes the library through their own vcpkg port, which lives outside this repo. The in-repo builds are autotools (`configure.ac`, one non-recursive `Makefile.am`) and Visual Studio (`win32/`); there is no CMake.
- A new source or installed header must be added to both `Makefile.am` and `win32/libtsk/libtsk.vcxproj`.

## Build and test

```sh
./bootstrap && ./configure --disable-java
make -j"$(nproc)"
make -j"$(nproc)" check
./test/catch_runner "test case name"   # one Catch2 test
```

After everything you implement, write unit tests to test the specific features you implemented, and run ALL the tests against the ELF files supplied in test/test_elfable/samples directory.

Every non-trivial function gets unit tests (CTest — match the existing framework in the repo; do not introduce a second one)

Format with clang-format using the repo's .clang-format; lint with clang-tidy (honor .clang-tidy). Do not reformat unrelated lines.

Use clangd-lsp plugin when necessary.

Use git best practices, trunk-based development, small branches and informative and standard git commit messages.

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
