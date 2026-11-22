# Build system, tests and repo layout (C/C++ library)

This file describes how The Sleuth Kit (TSK) C/C++ library is built and tested in this repo, where source and header lists live, and which build traps exist.
Read it before you add, remove or rename a source or header file, change a `HAVE_*` feature gate, run or add tests, or reason about what a consumer of the installed library sees.
The build is one non-recursive autotools `Makefile.am` on Unix and a Visual Studio solution on Windows; there is no CMake build in the repo.
The owner of this fork consumes the library through a vcpkg port that is not in this repo, so everything below describes only the in-repo builds and what a port must reproduce.
Java, JNI and Autopsy code are out of scope. The API layers are covered in `base-and-img.md`, `vs-and-pool.md`, `fs.md` and `auto-hashdb-cpp.md`.

Facts here were checked against the source at commit `424ee3ce5`. Nothing was compiled while writing this file, so statements about compiler behaviour are marked as things to check.

## Repo map

| Path | Content | Relevance to the C/C++ library |
|---|---|---|
| `tsk/` | The library. One subdirectory per layer: `base/`, `img/`, `vs/`, `pool/`, `fs/`, `hashdb/`, `auto/`, `util/` (with `util/Bitlocker/`). Also `libtsk.h` (public umbrella header), `tsk_tools_i.h` (internal umbrella header for the tools), `tsk.pc.in`, `docs/` (Doxygen pages), `sorter/` (data files). | Core |
| `tools/` | Command-line tools, one subdirectory per layer, plus `util.cpp` (argument conversion helper). | Reference users of the API |
| `samples/` | Four small API examples (`callback-style.cpp`, `posix-style.cpp` and their C++ variants). | Reference |
| `test/` | All tests and small test images. There is no `tests/` or `unit_tests/` directory. | Tests |
| `vendors/` | `sqlite3.c`, `sqlite3.h` (SQLite 3.47.0) and `catch.hpp` (Catch2 2.13.6, single header). | SQLite fallback; test framework |
| `m4/` | Autoconf macros. `tsk_opt_dep_check.m4` holds the optional-dependency logic. | Unix build |
| `win32/` | Visual Studio solution `tsk-win.sln`, `libtsk/libtsk.vcxproj`, one project per tool, `NugetPackages.props`, `updateAndBuildAll.py`, `BUILDING.txt`. | Windows build |
| `ossfuzz/` | libFuzzer targets for `fls` and `mmls`. | Optional |
| `configure.ac`, `Makefile.am`, `bootstrap` | The whole Unix build. There is no `Makefile.am` in any subdirectory. | Unix build |
| `NEWS.txt` | Release notes per version. | Read when checking history |
| `API-CHANGES.txt` | A to-do list of planned incompatible API changes. It is not a changelog. | Low |
| `docs/Testing_Plan.md` | Test policy. | Tests |

Ignore these for library work: `bindings/java/`, `case-uco/java/`, `debian/` (builds only the Java package), `rejistry++/` and `tools/logicalimager/` (Windows-only tool, not in `Makefile.am`), `xcode/` (stale project), `db_diff/`, `packages/`, `release/` (except for version bumps), `man/`, `ruleset.xml` (a Java lint rule set), `ChangeLog.txt` (an obsolete stub).

## Unix build commands

`bootstrap` runs `autoreconf -fi` and nothing else. It needs autoconf 2.69 or newer, automake, libtool and (optionally) pkg-config.

```sh
./bootstrap
./configure --disable-java          # Java is already off by default
make -j"$(nproc)"                   # library, tools and samples
make -j"$(nproc)" check             # builds and runs all tests
```

To build only the library, name its libtool target. The install targets below are the standard automake names for the three primary variables this `Makefile.am` uses (`lib_LTLIBRARIES`, `nobase_include_HEADERS`, `nodist_pkgconfig_DATA`); they were not executed while writing this file.

```sh
make -j"$(nproc)" tsk/libtsk.la
make install-libLTLIBRARIES install-nobase_includeHEADERS install-nodist_pkgconfigDATA
```

Useful variations:

```sh
make V=1                                   # show full compile and link commands
make CFLAGS=-Werror CXXFLAGS=-Werror       # what CI does; the build files never add -Werror
./configure --enable-static --disable-shared   # static library; tools are then linked with -all-static
./configure --enable-address-sanitizer --enable-undefined-sanitizer
```

Rules that apply to every Unix build:

- **Java stays disabled** unless `--enable-java` is passed (`configure.ac`, `AC_ARG_ENABLE([java])`, default `no`). All Java rules sit inside `if X_JNI` in `Makefile.am`. Never pass `--enable-java`.
- There is no option to skip the tools or samples; plain `make` always builds them.
- **C++ standard:** `AX_CXX_COMPILE_STDCXX([17], [noext], [mandatory])` appends `-std=c++17` (strict, no GNU extensions) to `$CXX`.
- **C standard:** nothing is set, so C files use the compiler's default dialect.
- **Warnings:** `AM_CFLAGS = -Wall -Wextra`, `AM_CXXFLAGS = -Wall -Wextra -Woverloaded-virtual`. The `auto` layer adds `-Wmultichar` and, for C++, `-Wsign-promo`.
- **Include paths:** sources include headers as `"tsk/..."` relative to the repo root (automake's default `-I$(srcdir)`), and `AM_CPPFLAGS` adds `-I$(top_srcdir)/tsk`.
- **Generated files:** configure writes `tsk/tsk_config.h` (all `HAVE_*` macros; never installed) and `tsk/tsk_incs.h` (installed; see "Installed headers"). Autoconf passes `-DHAVE_CONFIG_H`, and `tsk/base/tsk_base_i.h` includes `tsk/tsk_config.h` only under `#if HAVE_CONFIG_H`. A build that omits `-DHAVE_CONFIG_H` silently compiles with every optional feature off.

## Where sources and headers are listed

`Makefile.am` builds one convenience library (a libtool library that is not installed) per layer and merges them into the single installed library `tsk/libtsk.la`, which has no sources of its own.

| Layer | Variable in `Makefile.am` |
|---|---|
| `tsk/auto` | `tsk_auto_libtskauto_la_SOURCES` |
| `tsk/base` | `tsk_base_libtskbase_la_SOURCES` |
| `tsk/fs` | `tsk_fs_libtskfs_la_SOURCES` |
| `tsk/hashdb` | `tsk_hashdb_libtskhashdb_la_SOURCES` |
| `tsk/img` | `tsk_img_libtskimg_la_SOURCES` |
| `tsk/pool` | `tsk_pool_libtskpool_la_SOURCES` |
| `tsk/util` | `tsk_util_libtskutil_la_SOURCES` |
| `tsk/util/Bitlocker` | `tsk_util_Bitlocker_libtskbitlocker_la_SOURCES` |
| `tsk/vs` | `tsk_vs_libtskvs_la_SOURCES` |

Checklist when you add a file to the library:

1. Add the `.c` or `.cpp` file, and its header, to the layer's `_SOURCES` variable in `Makefile.am`. Headers must be listed too, or `make dist` drops them.
2. If the header is public, also add it to `nobase_include_HEADERS` in `Makefile.am`. That list preserves paths, so `tsk/fs/foo.h` installs as `<includedir>/tsk/fs/foo.h`.
3. Add a `<ClCompile Include="..\..\tsk\...">` (source) or `<ClInclude ...>` (header) item to `win32/libtsk/libtsk.vcxproj`, and the matching entry in `win32/libtsk/libtsk.vcxproj.filters`.
4. Tell the user that their external vcpkg port may keep its own file list. This repo cannot show whether it does.

When you add a unit test file, add it to `test_catch_runner_SOURCES` in `Makefile.am`. A test file that is not listed there is never compiled (see `test/tsk/fs/test_fls.cpp` under "Known defects and traps").

At the time of writing, every `.c` and `.cpp` file under `tsk/` is listed in `Makefile.am`. The vcxproj list is shorter; see "Differences between the autotools and Visual Studio builds".

Other build outputs defined in `Makefile.am`: the pkg-config file `tsk/tsk.pc` (from `tsk/tsk.pc.in`), the sorter data files (`nobase_dist_data_DATA`), and the shared-library version `-version-info 22:0:0` in `tsk_libtsk_la_LDFLAGS`. That libtool version is separate from the package version.

## Optional dependencies and `HAVE_*` macros

Each optional library is probed by the macro `TSK_OPT_DEP_CHECK` in `m4/tsk_opt_dep_check.m4`. The behaviour is the same for every library:

- With no option, the library is used if found. `--without-NAME` disables it. `--with-NAME` makes a missing library a configure error. `--with-NAME=DIR` uses `DIR/include` and `DIR/lib` and skips pkg-config.
- pkg-config is tried first when the library has a module name. Then a header check and `AC_CHECK_LIB` run in every case, and that `AC_CHECK_LIB` call is what defines the C macro `HAVE_LIB<NAME>` in `tsk/tsk_config.h`.
- A library found through pkg-config is added to `Requires.private` in `tsk.pc`; one found without pkg-config is added to `Libs.private`.

| Configure option | Macro defined | What it gates |
|---|---|---|
| `--with-libewf` | `HAVE_LIBEWF` | EnCase (E01) images: `tsk/img/ewf.cpp`. Also `HAVE_LIBEWF_HANDLE_READ_BUFFER_AT_OFFSET` for newer libewf. |
| `--with-libvhdi` | `HAVE_LIBVHDI` | VHD images: `tsk/img/vhd.cpp` |
| `--with-libvmdk` | `HAVE_LIBVMDK` | VMDK images: `tsk/img/vmdk.cpp` |
| `--with-libqcow` | `HAVE_LIBQCOW` | QCOW images: `tsk/img/qcow.cpp` |
| `--with-afflib` | `HAVE_LIBAFFLIB` | AFF images: `tsk/img/aff.c` (no pkg-config module) |
| `--with-libaff4` | `HAVE_LIBAFF4` | AFF4 images: `tsk/img/aff4.cpp` (no pkg-config module) |
| `--with-zlib` | `HAVE_LIBZ` | Compressed data in HFS, APFS and btrfs: `tsk/fs/decmpfs.cpp`, `hfs.cpp`, `apfs_compat.cpp`, `btrfs.cpp`, `tsk_btrfs.h` |
| `--with-libcrypto` | `HAVE_LIBCRYPTO` | APFS decryption (OpenSSL): `tsk/util/crypto.cpp`, `crypto.hpp`, `tsk/fs/apfs*.cpp`, `apfs_fs.hpp` |
| `--with-libvslvm` | `HAVE_LIBVSLVM` | Linux LVM pools: `tsk/pool/lvm_pool*.cpp`, `tsk_lvm.hpp`, plus branches in `tsk/auto/` and `tsk/pool/pool_open.cpp`. Requesting it without libbfio is a configure error. |
| `--with-libbfio` | `HAVE_LIBBFIO` | `tsk/pool/img_bfio_handle.cpp`, the I/O bridge that the LVM code needs |
| `--disable-multithreading` | (removes `TSK_MULTITHREAD_LIB`) | Locks and per-thread error state. See "Installed headers". |

Every gated source file is always compiled; its body is wrapped in `#if`/`#ifdef` and becomes empty when the macro is absent. `tsk/img/img_types.c` and `tsk/img/img_open.cpp` use the same macros to decide which image types are advertised and opened.

SQLite is required and is handled separately. Configure looks for a system SQLite (pkg-config module `sqlite3`, then header plus `-lsqlite3`). If none is found, the automake conditional `HAVE_LIBSQLITE3` is false and `vendors/sqlite3.c` is compiled into the `auto` layer with `-DSQLITE_OMIT_LOAD_EXTENSION`.

Macros that the code tests but no in-repo Unix build defines:

- `HAVE_LIBMBEDTLS` gates all of `tsk/util/Bitlocker/`, `tsk/fs/encryptionHelper.cpp` and a branch in `tsk/fs/fs_io.c`. Neither `configure.ac` nor any `m4/` file mentions mbedtls, so BitLocker support is compiled out of every autotools build. Only the Visual Studio project defines the macro.
- `HAVE_LIBSQLITE3` as a C macro. See "Known defects and traps".
- `GUID_LIBUUID`, `GUID_CFUUID`, `GUID_ANDROID` in `tsk/auto/guid.cpp`. Only `GUID_WINDOWS` is defined, and only by the vcxproj. Without one of them `GuidGenerator::newGuid()` has no definition; its single caller in `tsk/auto/db_sqlite.cpp` is itself under `#ifdef GUID_WINDOWS`.
- `HAVE_LIBMAGIC` and `HAVE_EXIV2` in `tools/fiwalk/` (tool only).

## Windows build

The Windows build is `win32/tsk-win.sln`. The solution file is in Visual Studio 2019 format, and `win32/libtsk/libtsk.vcxproj` produces a **static library only**. The library sources contain no `__declspec(dllexport)` annotations, so a DLL built from them with MSVC would export nothing.

The in-repo driver is `win32/updateAndBuildAll.py`. It requires the environment variable `TSK_HOME` (the repo root), runs `nuget restore tsk-win.sln`, and then calls MSBuild on the whole solution with `/p:configuration=...` and `/p:platform=...`. An equivalent manual command for the library alone, not executed while writing this file, is:

```bat
cd win32
nuget restore tsk-win.sln
msbuild tsk-win.sln /t:libtsk /p:Configuration=Release_NoLibs /p:Platform=x64 /m
```

`libtsk.vcxproj` imports `$(SolutionDir)\NugetPackages.props`, so build it through the solution or pass `SolutionDir` yourself.

Settings in `libtsk.vcxproj` that a port must match or consciously change:

- **Configurations:** `Debug`, `Release`, `Debug_NoLibs`, `Release_NoLibs`, `Release_XPNoLibs`, each for `Win32` and `x64`. The `*_NoLibs` configurations use no third-party library and need no NuGet packages.
- **Toolset:** `v142` (Visual Studio 2019) everywhere except `Release_XPNoLibs|Win32`, which uses `v141_xp`. Building with Visual Studio 2022 needs those toolsets installed or a retarget to `v143` (for example `/p:PlatformToolset=v143`). Whether the code builds cleanly with MSVC 19.41 is something to check, not something this repo's CI shows: `.github/workflows/compile-windows.yml` uses the `windows-2025` runner and runs no tests.
- **Language:** `LanguageStandard` is `stdcpp17`. Warning level is 3 with warnings 4200, 4814 and 5030 disabled, and warnings are not errors.
- **Defines in every configuration:** `NOMINMAX`, `_CRT_SECURE_NO_DEPRECATE`, `_CRT_SECURE_NO_WARNINGS`, `GUID_WINDOWS`, `WIN32`, `_LIB`, and `WINVER` (`0x0601`, except `0x0501` in `Release|Win32`).
- **Feature defines:** there is no `tsk_config.h` on MSVC, so every `HAVE_*` macro comes from `PreprocessorDefinitions`. `Debug` and `Release` define `HAVE_LIBEWF`, `HAVE_LIBVMDK`, `HAVE_LIBVHDI`, `HAVE_LIBZ` and `HAVE_LIBMBEDTLS`. Only `Release|x64` also defines `HAVE_LIBCRYPTO`. The `*_NoLibs` configurations define none.
- **C runtime:** `/MDd` for `Debug`, `/MD` for `Release`, `/MT` for `Release_NoLibs` and `Release_XPNoLibs`. `Debug_NoLibs` is inconsistent: `/MDd` on `Win32` and `/MTd` on `x64`.
- **Dependencies** come only from NuGet packages listed in `win32/libtsk/packages.config`, with paths hard-coded in `win32/NugetPackages.props`. No environment variable points at a dependency.
- **SQLite:** `vendors/sqlite3.c` is always compiled into the library.

On MSVC, `tsk/base/tsk_base.h` does not include `tsk/tsk_incs.h`, and `tsk/base/tsk_os.h` defines `TSK_WIN32`, `TSK_MULTITHREAD_LIB` and `UNICODE` itself. `TSK_TCHAR` is therefore a wide character on Windows; see `base-and-img.md`.

A mingw cross-build goes through autotools instead (`./configure --host=x86_64-w64-mingw32 --disable-java`); CI runs its tests under Wine.

## Differences between the autotools and Visual Studio builds

| Feature | Autotools | Visual Studio (`libtsk.vcxproj`) |
|---|---|---|
| BitLocker (`HAVE_LIBMBEDTLS`) | Never built | Built in `Debug` and `Release` |
| QCOW images | Optional | Not available: `tsk/img/qcow.cpp` is not in the project |
| AFF4 images | Optional | Not available: `tsk/img/aff4.cpp` is not in the project |
| AFF images | Optional | Source is listed, but no configuration defines `HAVE_LIBAFFLIB` |
| LVM pools | Optional | Not available: `tsk/pool/lvm_pool.cpp`, `lvm_pool_compat.cpp`, `img_bfio_handle.cpp` are not in the project |
| NTFS USN journal | Built | Not available: `tsk/fs/usn_journal.cpp` and `usnjls_lib.cpp` are not in the project |
| `strnlen` fallback | `tsk/base/tsk_base_i.c` | Not in the project (the file is empty unless `HAVE_STRNLEN` is missing) |
| APFS decryption (`HAVE_LIBCRYPTO`) | Optional | `Release|x64` only |
| SQLite | System library if found, else vendored | Always vendored |
| Device-ID GUID generation in `tsk/auto/db_sqlite.cpp` | Not compiled (`GUID_WINDOWS` undefined) | Compiled |
| Library type | Shared and static (libtool) | Static only |
| Tools | All, including `usnjls`, `fiwalk`, `sigfind`, `srch_strings` | Those four have no project |
| Tests | `make check` | None |

## Installed headers and consumer hazards

The installed set is exactly `nobase_include_HEADERS` in `Makefile.am`, plus the generated `tsk/tsk_incs.h`. `tsk/tsk.pc` gives consumers `-I${includedir}` and `-ltsk`, and it sets no preprocessor defines. Headers ending in `_i.h` are internal and are not installed, and neither are `tsk/tsk_config.h`, `tsk/tsk_tools_i.h`, `tsk/auto/tsk_case_db.h`, `tsk/auto/tsk_db.h` and `tsk/auto/tsk_db_sqlite.h`.

`tsk/tsk_incs.h` is written by configure (`AC_CONFIG_COMMANDS` in `configure.ac`). It includes `<unistd.h>`, `<inttypes.h>` and `<sys/param.h>` when they exist and defines `TSK_MULTITHREAD_LIB` when pthreads are in use. `tsk_base.h` includes it on every compiler except MSVC, so a port that does not run configure must generate an equivalent file.

The umbrella header `tsk/libtsk.h` is safe: it and everything it includes (`tsk_base.h`, `tsk_img.h`, `tsk_vs.h`, `tsk_fs.h`, `tsk_hashdb.h`, `tsk_auto.h`, `tsk_pool.h`, `tsk_os.h`, `tsk_incs.h`) reference only installed headers. The hazards are in the other installed headers.

Installed headers that include a header that is not installed, so they cannot be compiled against an installed tree:

| Installed header | Includes (not installed) |
|---|---|
| `tsk/auto/tsk_is_image_supported.h` | `tsk/tsk_tools_i.h` |
| `tsk/fs/tsk_fatfs.h`, `tsk_fatxxfs.h`, `tsk_exfatfs.h` | `tsk/fs/tsk_fs_i.h` |
| `tsk/fs/encryptionHelper.h` | `tsk/img/tsk_img_i.h`, `tsk/fs/tsk_fs_i.h` |
| `tsk/img/logical_img.h`, `tsk/img/pool.hpp` | `tsk/img/tsk_img_i.h` |
| `tsk/pool/tsk_lvm.hpp`, `lvm_pool_compat.hpp` | `tsk/base/tsk_base_i.h` |
| `tsk/util/detect_encryption.h` | `tsk/base/tsk_base_i.h` |
| `tsk/util/Bitlocker/BitlockerParser.h`, `DataTypes.h`, `MetadataEntry.h`, `MetadataUtils.h` | `tsk/base/tsk_base_i.h` (and `tsk_img_i.h` in `BitlockerParser.h`) |
| `tsk/fs/apfs_fs.hpp`, `tsk/util/crypto.hpp` | `tsk/tsk_config.h`, only under `#if HAVE_CONFIG_H` |

Installed headers whose declarations change with a `HAVE_*` macro. The library is compiled with the macro set from `tsk_config.h`; a consumer normally has none of them defined, so the two sides can disagree about a type's layout or existence:

- `tsk/fs/tsk_btrfs.h`: under `HAVE_LIBZ` it includes `<zlib.h>`, adds the enumerator `BTRFS_ED_TYPE_COMP_ZLIB` (which shifts `BTRFS_ED_TYPE_UNKNOWN`) and adds zlib stream members to a data-walk struct.
- `tsk/fs/apfs_fs.hpp`: `APFSJObjTree::crypto` gains a `std::unique_ptr<aes_xts_decryptor>` member under `HAVE_LIBCRYPTO`.
- `tsk/util/crypto.hpp`: the whole `aes_xts_decryptor` class exists only under `HAVE_LIBCRYPTO`, and it gains a `std::mutex` member under `TSK_MULTITHREAD_LIB`.
- `tsk/pool/tsk_lvm.hpp`, `tsk/pool/lvm_pool_compat.hpp`: contents exist only under `HAVE_LIBVSLVM`.
- `tsk/fs/encryptionHelper.h` and every header in `tsk/util/Bitlocker/`: contents depend on `HAVE_LIBMBEDTLS`.

`TSK_MULTITHREAD_LIB` is safe on Unix because it travels in the installed `tsk_incs.h`. Three Bitlocker headers (`BitlockerUtils.h`, `DataTypes.h`, `MetadataUtils.h`) contain `using namespace std;` at file scope.

## Tests

All tests live under `test/` and run through `make check`. The unit tests use Catch2 version 2 from `vendors/catch.hpp`; `test/catch_runner.cpp` supplies `main`. Unit test files mirror the library tree: `test/tsk/<layer>/test_*.cpp`. Policy (`docs/Testing_Plan.md`): use `REQUIRE` for preconditions and `CHECK` for independent assertions, and deliver a bug fix as a failing test followed by the fix.

`make check` runs three kinds of test:

- **Shell tests** listed in `check_TESTS`: `test/tools/fstools/test_fls.sh`, `test_ils.sh`, `test/tools/vstools/test_mmls.sh`, `test/test_libraries.sh`, `test/img_dump/test_imgs.sh`, `test/legacy/run_fs_thread_test.sh`, `test/tools/autotools/test_loaddb.sh`, `test/check_static.sh`. Two more E01 scripts are added when libewf is in use (conditional `HAVE_E01`), and `test/test_static_tools.sh` is added for static builds.
- **`test/catch_runner`**: all Catch2 unit tests.
- **`test/fiwalk/fiwalk_test`** and **`test/tools/tsk_cli_runner`**. The CLI runner reads `test/tools/cli_tests.txt`, one test per line in the form `TestID|command|expected_stdout_file|exit_code[|expected_stderr_file]`, with expected output under `test/tools/output/`.

Commands (run from the top of the build directory, because tests use relative paths such as `tools/fstools/fls` and `test/data`):

```sh
make -j"$(nproc)" check                         # everything
make check VERBOSE=1                            # print the log of each failing test
make check TESTS=test/catch_runner              # one test program
make check TESTS=test/tools/fstools/test_fls.sh # one shell test
make test/catch_runner                          # build the unit-test binary only

./test/catch_runner --list-tests                # list Catch2 test cases
./test/catch_runner "exact test case name"      # run one test case
./test/catch_runner "[tag]"                     # run test cases with a tag
./test/catch_runner "name" -s                   # also print passing assertions
./libtool --mode=execute gdb ./test/catch_runner   # debug a shared build

srcdir=. bash test/tools/fstools/test_fls.sh    # run a shell test by hand
```

In a shared build `test/catch_runner` is a libtool wrapper script, which is why the debugger needs `libtool --mode=execute`. Each test writes `<test>.log` next to itself, and `test-suite.log` at the top summarises failures. Exit code 77 means "skipped". Shell tests need the `srcdir` environment variable, which `make check` sets.

### Test data

Small images are committed in `test/data/`. Larger images are an external corpus that tests locate through the environment variable `SLEUTHKIT_TEST_DATA_DIR`:

```sh
wget https://digitalcorpora.s3.amazonaws.com/corpora/drives/tsk-2024/sleuthkit_test_data.zip
unzip sleuthkit_test_data.zip
cd sleuthkit_test_data && make unpack
export SLEUTHKIT_TEST_DATA_DIR="$PWD"
```

Without the variable, `make check` still passes, but with reduced coverage:

- `test/tools/autotools/test_loaddb.sh` exits 77 (skipped).
- `test/fiwalk/fiwalk_test` exits 77 at its first image check, so the whole program is reported as skipped.
- `test/tools/tsk_cli_runner` skips each line of `cli_tests.txt` that mentions `$SLEUTHKIT_TEST_DATA_DIR`, which is every case except `fls_0` and `mmls_0`.
- Unit tests in `test/tsk/fs/test_fatfs.cpp` and `test/tsk/fs/test_xfs_dent.cpp` print a Catch2 warning and return early, so they are counted as **passed** without testing anything.

Tests for optional image formats depend on the matching library being compiled in, and some corpus images are E01 files that need libewf.

### Fuzzing and CI

Setting the environment variable `LIB_FUZZING_ENGINE` when running configure enables the `fls_*_fuzzer` and `mmls_*_fuzzer` programs (`ossfuzz/`, conditional `HAVE_LIB_FUZZING_ENGINE`). The fuzzing workflow file is disabled (`.github/workflows/build-ossfuzz.yml-disabled`).

`.github/workflows/build-unix.yml` builds on macOS, Ubuntu (gcc and clang, with `-Werror`), a static Ubuntu build and mingw, then rebuilds with sanitizers and runs `make distcheck`. `code-coverage.yml` produces coverage reports. `compile-windows.yml` builds the Visual Studio solution and runs no tests. `appveyor.yml` does the same on a Visual Studio 2019 image and also runs `make check` on Ubuntu and macOS.

## Version numbers

The version is stored in several places that are edited by hand or by `release/release-unix.pl`, and they currently disagree:

| Location | Value |
|---|---|
| `configure.ac`, `AC_INIT` | `4.15.0-develop` |
| `tsk/base/tsk_base.h`, `TSK_VERSION_STR` | `"4.15.0-develop"` |
| `tsk/base/tsk_base.h`, `TSK_VERSION_NUM` | `0x041400ff`, which encodes 4.14.0 |
| `packages/sleuthkit.spec` | `4.15.0-develop` |
| `bindings/java/build.xml` | `4.14.0` |
| `Makefile.am`, `-version-info` | `22:0:0` (libtool interface version, bumped interactively by the release script) |
| `NEWS.txt`, newest entry | 4.14.0 |

`tsk_version_get_str()` returns `TSK_VERSION_STR`. Do not use `TSK_VERSION_NUM` for feature checks on this branch. `tsk.pc` takes its version from `AC_INIT`.

## Coding conventions

- **C and C++ split:** older layers are C (`tsk/vs/*.c`, `tsk/base/*.c`, parts of `tsk/fs/` such as `fs_open.c` and `fs_io.c`); newer code is C++17 (APFS, btrfs, XFS, pool, img, auto, Bitlocker). Use of C++17 features is light. The public API is C, declared inside `extern "C"` blocks, with thin C++ wrapper classes in the same headers.
- **Header names:** `tsk_<layer>.h` is the public header of a layer, `*_i.h` is internal, `.hpp` is C++-only.
- **Function names:** `tsk_<layer>_<object>_<verb>`, for example `tsk_fs_dir_walk`. File-system internals use a per-format prefix (`ext2fs_`, `ntfs_`, `hfs_`).
- **Type names:** upper snake case typedefs (`TSK_FS_INFO`), enums named `TSK_*_ENUM`. C++ classes are camel case (`TskAuto`, `APFSFileSystem`) with `m_` or `_` member prefixes.
- **Dispatch:** per-format behaviour is reached through function pointers in the info structs (`fs->istat`, `fs->jopen`), not through C++ virtual functions.
- **Indentation:** each source directory has a `.indent.pro` for GNU indent with `-kr -psl -nce -ip2 -nlp -nut`: K&R style, 4 spaces, no tabs, and the return type on its own line above the function name. There is no `.clang-format`, `.clang-tidy` or `.editorconfig`.
- **Error handling:** error state is per thread. A failing function calls `tsk_error_reset()`, `tsk_error_set_errno(TSK_ERR_...)` and `tsk_error_set_errstr("fmt", ...)`, then returns `1` (for `uint8_t` results) or `NULL`; `0` means success. Walk callbacks return `TSK_WALK_CONT`, `TSK_WALK_STOP` or `TSK_WALK_ERROR`. Newer C++ code throws internally and converts at the C boundary with a function-try-block (see `tsk_apfs_set_snapshot` in `tsk/fs/apfs_compat.cpp`). Details are in `base-and-img.md`.
- **Attributes:** use the `FALLTHROUGH` and `UNUSED` macros from `tsk/base/tsk_os.h`; they are GCC attributes and expand to nothing on MSVC.
- **Doxygen:** Javadoc-style `/** ... */` blocks and `///<` trailing comments, with backslash commands (`\file`, `\ingroup`, `\param`, sometimes `@param`). Group pages are `tsk/docs/*.dox`. `make api-docs` runs Doxygen, but it also runs the Java Doxygen; use `doxygen tsk/docs/Doxyfile` for the C/C++ part alone.
- **File header:** files start with a "The Sleuth Kit" comment naming the Common Public License 1.0. Third-party files (`tsk/fs/lzvn.c`, `tsk/base/md5c.c`, `sha1c.c`, `crc.c`, `tsk_unicode.c`, `tsk/auto/guid.cpp`) keep their own licenses; `licenses/README.md` is the inventory.

## Tools and the library functions they exercise

Every tool includes `tsk/tsk_tools_i.h`. The usual sequence is `tsk_img_open` (`tsk/img/img_open.cpp`), then optionally `tsk_pool_open_img_sing` (`tsk/pool/pool_open.cpp`), then `tsk_fs_open_img_decrypt` (`tsk/fs/fs_open.c`), then the function below.

| Tool (directory under `tools/`) | Purpose | Library entry point |
|---|---|---|
| `img_stat` (`imgtools`) | Image details | `imgstat` function pointer of the image |
| `img_cat` (`imgtools`) | Raw image bytes | `tsk_img_read`, `tsk/img/img_io.cpp` |
| `mmls` (`vstools`) | List partitions | `tsk_vs_open` (`tsk/vs/mm_open.c`), `tsk_vs_part_walk` (`mm_part.c`) |
| `mmstat` (`vstools`) | Volume system type | `tsk_vs_open`, `tsk_vs_type_toname` |
| `mmcat` (`vstools`) | Partition contents | `tsk_vs_part_get`, `tsk_vs_part_read_block` (`mm_io.c`) |
| `pstat` (`pooltools`) | Pool details | `pool->poolstat` |
| `fsstat` (`fstools`) | File system details | `fs->fsstat` |
| `fls` | List file names | `tsk_fs_fls`, `tsk/fs/fls_lib.cpp` |
| `ffind` | Name for an inode | `tsk_fs_ffind`, `ffind_lib.cpp` |
| `ils` | List inodes | `tsk_fs_ils`, `ils_lib.cpp` |
| `istat` | Inode details | `fs->istat` |
| `icat` | File content by inode | `tsk_fs_icat`, `icat_lib.cpp` |
| `ifind` | Inode for a block, name or parent | `tsk_fs_ifind_data`, `tsk_fs_ifind_path`, `tsk_fs_ifind_par`, `ifind_lib.cpp` |
| `fcat` | File content by path | `tsk_fs_ifind_path`, then `tsk_fs_icat` |
| `blkls` | Block listing and unallocated space | `tsk_fs_blkls`, `dls_lib.cpp` |
| `blkcat` | Block content | `tsk_fs_blkcat`, `dcat_lib.cpp` |
| `blkstat` | Block allocation status | `tsk_fs_blkstat`, `dstat_lib.cpp` |
| `blkcalc` | Map a `blkls` address to the original | `tsk_fs_blkcalc`, `dcalc_lib.cpp` |
| `jls`, `jcat` | Journal entries and blocks | `fs->jopen`, then `fs->jentry_walk` or `fs->jblk_walk` |
| `usnjls` | NTFS USN journal | `tsk_fs_usnjls`, `usnjls_lib.cpp`; declared only in the internal `tsk_fs_i.h` |
| `hfind` (`hashtools`) | Hash database lookup and indexing | `tsk_hdb_open`, `tsk_hdb_lookup_str`, `tsk_hdb_make_index`, `tsk/hashdb/` |
| `sigfind` (`srchtools`) | Search an image for a signature | `tsk_img_open_utf8_sing`, `tsk_img_read` |
| `tsk_loaddb` (`autotools`) | Image to SQLite case database | `TskCaseDb`, `TskAutoDb`, `tsk/auto/case_db.cpp`, `auto_db.cpp` |
| `tsk_recover`, `tsk_comparedir`, `tsk_gettimes` | Walk every file in an image | Subclasses of `TskAuto`, `tsk/auto/auto.cpp` |
| `tsk_imageinfo` | Report whether an image is supported or encrypted | `TskIsImageSupported`, `tsk/auto/is_image_supported.cpp` |
| `fiwalk` | DFXML file listing | `tsk_vs_part_walk`, `tsk_fs_dir_walk`; sources in `tools/fiwalk/src/` |

Note the file-name mismatch for the block tools: the `blk*` functions live in `d*_lib.cpp` files. `srch_strings` does not link the library. `tools/hashtools/md5.c` and `sha1.c` are distributed but never compiled.

## Compiler portability notes for GCC 16 / MSVC 19.41

These are observations of the source, not build results. The CI in this repo uses the compilers of Ubuntu 22.04/24.04, macOS 15 and `windows-2025`, so neither target compiler is exercised here.

- **C dialect is the compiler default.** The Unix build passes no `-std` for C. GCC 15 changed its default C dialect to C23 (`gnu23`); confirm what GCC 16 uses with `gcc -dM -E -x c /dev/null | grep __STDC_VERSION__`. Commit `37bb48f0d` ("fix compiler errors on C23") already adjusted `tsk/base/crc.c`, `md5c.c`, `sha1c.c` and `tsk_base.h`.
- **Empty parameter lists in C-visible headers**, for example `tsk_error_get_info()` and `tsk_error_reset()` in `tsk/base/tsk_base.h`, `tsk_img_type_supported()` in `tsk_img.h`, `tsk_vs_type_supported()` in `tsk_vs.h`, `tsk_fs_type_supported()` in `tsk_fs.h`, and the function-pointer fields `uses_external_indexes` and `accepts_updates` in `tsk/hashdb/tsk_hashdb.h`. In C23 `()` means `(void)`; before C23 it means "unspecified arguments". All callers pass no arguments. A consumer compiling C with `-Wstrict-prototypes` will get warnings from these headers.
- **`errno` used as a parameter name** in `TSK_ERROR_LISTENER_CB` (`tsk/base/tsk_base.h`). `errno` is a macro in the C library, so this declaration depends on what the macro expands to wherever `<errno.h>` was included first. Check it on both compilers if a build error points at that line.
- **C++ dialect.** Configure forces `-std=c++17` and the vcxproj sets `stdcpp17`. `NEWS.txt` says a move to C++20 is pending, so the code has not been validated as C++20. A port with its own build scripts must set C++17 explicitly instead of relying on the compiler default; check GCC 16's default with `g++ -dM -E -x c++ /dev/null | grep __cplusplus`.
- **`-Werror` is not part of the build.** New `-Wall -Wextra` diagnostics from a newer GCC fail the build only if the caller adds `-Werror`, as CI does.
- **`tsk/auto/tsk_auto.h`** uses `#pragma GCC diagnostic` to silence `-Woverloaded-virtual`. Check whether MSVC reports it as an unknown pragma (warning C4068) at the consumer's warning level.
- **`std::wstring_convert` and `<codecvt>`** (deprecated in C++17) are used in `tsk/util/Bitlocker/BitlockerParser.cpp`, which is compiled only with `HAVE_LIBMBEDTLS`. Expect a deprecation warning from MSVC, and from GCC if a port enables mbedtls there.
- **`tsk/util/Bitlocker/BitlockerUtils.h`** uses `uint8_t`, `uint32_t`, `uint64_t` and `size_t` but includes only `<string>`. It compiles only if another header provided those types first. This matters only when `HAVE_LIBMBEDTLS` is defined; check it if a GCC build enables BitLocker.
- **`tsk/base/tsk_os.h` on MSVC** defines `mode_t` and `ssize_t` typedefs and the macros `strcasecmp`, `putenv`, `roundup` and `fseeko`. On mingw it defines `daddr_t` as a macro. These leak into every consumer translation unit that includes a TSK header.
- **Catch2 2.13.6** (`vendors/catch.hpp`) sizes its signal stack from `SIGSTKSZ` at run time. If the test binary fails to compile on a new glibc, look there first.
- **Windows-only code outside the library** has known C++17 problems that do not affect `libtsk`: `std::auto_ptr` in `tools/logicalimager/RegParser.cpp` and dynamic exception specifications in `rejistry++/src/RejistryException.h`.

## Known defects and traps

Each item was confirmed by reading the named file.

1. **`HAVE_LIBSQLITE3` is never a C macro.** `configure.ac` creates only an automake conditional of that name (`AM_CONDITIONAL([HAVE_LIBSQLITE3])`); nothing calls `AC_DEFINE` for it. `tsk/auto/tsk_db_sqlite.h` and `tsk/hashdb/sqlite_hdb.cpp` test `#ifdef HAVE_LIBSQLITE3` and therefore always take the `#include "vendors/sqlite3.h"` branch. With a system SQLite the code is compiled against the vendored 3.47.0 header and linked against the system library. A port that unbundles SQLite must define `HAVE_LIBSQLITE3` itself.
2. **BitLocker is unreachable from autotools.** `HAVE_LIBMBEDTLS` is defined only in `win32/libtsk/libtsk.vcxproj`. See "Optional dependencies".
3. **Configure summary swaps two lines.** In the final `AC_MSG_NOTICE` of `configure.ac`, "libbfio support" prints `$ax_libqcow` and "libqcow support" prints `$ax_libbfio`. Trust `tsk/tsk_config.h`, not the summary.
4. **`TSK_VERSION_NUM` is stale.** `tsk/base/tsk_base.h` has `0x041400ff` next to `TSK_VERSION_STR "4.15.0-develop"`. See "Version numbers".
5. **Installed headers include headers that are not installed**, and some change layout with `HAVE_*` macros. See "Installed headers and consumer hazards".
6. **`win32/libtsk/libtsk.vcxproj` is missing eight sources** that `Makefile.am` builds: `tsk/base/tsk_base_i.c`, `tsk/fs/usn_journal.cpp`, `tsk/fs/usnjls_lib.cpp`, `tsk/img/aff4.cpp`, `tsk/img/qcow.cpp`, `tsk/pool/img_bfio_handle.cpp`, `tsk/pool/lvm_pool.cpp`, `tsk/pool/lvm_pool_compat.cpp`. It also lists the header `tsk/img/legacy_cache.h` as a `ClCompile` item.
7. **`Debug_NoLibs` uses different C runtimes per platform** in `libtsk.vcxproj`: `MultiThreadedDebugDLL` on `Win32`, `MultiThreadedDebug` on `x64`.
8. **`m4/slg_noopt.m4` is dead.** It contains bare `AC_ARG_ENABLE` code for `--disable-opt` and `--disable-o3` with no `AC_DEFUN`, and nothing references it. Those options do not exist in the generated configure.
9. **`test/tsk/fs/test_fls.cpp` is not built.** It exists on disk but is absent from `test_catch_runner_SOURCES` in `Makefile.am`.
10. **`test/img_dump/test_imgs.sh` always reports "skipped".** Its second case names `test/data/image/fat_dst_test.dd`, which does not exist (the file is `test/data/fat_dst_test.dd`, with no `.json`). `img_differ.sh` exits 77 for a missing image and the script runs under `bash -e`, so the whole script is skipped even though its first case ran.
11. **Tests that pass without testing.** Catch2 cases that need `SLEUTHKIT_TEST_DATA_DIR` return early with a warning and count as passed. See "Test data".
12. **`adjust_tool_path` in `test/tools/test_utils.cpp`** replaces 10 characters for the 9-character token `$DATA_DIR`, so the character after the token (normally `/`) is dropped. No line in `cli_tests.txt` uses `$DATA_DIR` today.
13. **`CATCH2_CPPFLAGS`** is referenced by the test targets in `Makefile.am` but is never set by configure; it is always empty.
14. **Stale comment in `Makefile.am`.** The TODO above `check_PROGRAMS` says nothing runs `fs_thread_test`, but `test/legacy/run_fs_thread_test.sh` does. `fs_attrlist_apis`, `fs_fname_apis` and `read_apis` are built by `make check` and never run.
15. **The disabled fuzzing workflow** passes `--disable-libewf --disable-libvmdk --disable-libcrypto` to configure. Those options do not exist; the real ones are `--without-libewf` and so on.
16. **`tsk.pc` does not list everything.** `Libs.private` and `Requires.private` receive the optional libraries and system SQLite, but `tsk/tsk.pc.in` has no entry for the pthread flags or `-lstdc++` that `TSK_LIB_LIBS` links. Check static linking of a consumer against the generated `tsk.pc`.
17. **`win32/updateAndBuildAll.py` searches fixed MSBuild paths** (Visual Studio 2019 Community, 2022 Enterprise and a legacy MSBuild 14.0 path) when `MSBuild.exe` is not on `PATH`.
