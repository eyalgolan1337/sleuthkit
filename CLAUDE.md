# The Sleuth Kit (fork)

A fork of The Sleuth Kit, a C/C++ library (`libtsk`) for forensic analysis of disk images, volume systems and file systems.

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

## Reference docs

Read the matching file in `docs/llm/` before working in an area. Each ends with a "Known defects and traps" section.

- `docs/llm/base-and-img.md`: error handling, `TSK_TCHAR` and Windows/POSIX differences, core types, opening and reading disk images, `tsk/util/`.
- `docs/llm/vs-and-pool.md`: partition tables, APFS and LVM pools, offsets and units between layers.
- `docs/llm/fs.md`: file systems, files, directories, attributes, walks, encryption, adding a file system.
- `docs/llm/auto-hashdb-cpp.md`: `TskAuto`, hash databases, the C++ wrapper classes, `samples/`.
- `docs/llm/build-and-test.md`: repo map, build options, `HAVE_*` feature macros, installed headers, tests and test data, coding conventions, compiler portability notes.
