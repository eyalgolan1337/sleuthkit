# Build system, tests and repo layout (C/C++ library)

This file describes how The Sleuth Kit (TSK) C/C++ library is built and tested in this repo, where source and header lists live, and which build traps exist.
Read it before you add, remove or rename a source or header file, change a `HAVE_*` feature gate, run or add tests, or reason about what a consumer of the installed library sees.
The Unix build is recursive autotools (a `Makefile.am` in the top directory and in every built subdirectory); Windows uses a Visual Studio solution. There is no CMake build in the repo.
The owner of this fork consumes the library through a vcpkg port that is not in this repo, so everything below describes only the in-repo builds and what a port must reproduce.
Java, JNI and Autopsy code are out of scope. The API layers are covered in `base-and-img.md`, `vs-and-pool.md`, `fs.md` and `auto-hashdb-cpp.md`.

Facts here were checked against the source at tag `sleuthkit-4.15.0`. Nothing was compiled while writing this file, so statements about compiler behaviour are marked as things to check.

## Repo map

| Path | Content | Relevance to the C/C++ library |
|---|---|---|
| `tsk/` | The library. One subdirectory per layer: `base/`, `img/`, `vs/`, `pool/`, `fs/`, `hashdb/`, `auto/`, `util/` (with `util/Bitlocker/`). Also `libtsk.h` (public umbrella header), `tsk_tools_i.h` (internal umbrella header for tools and tests), `tsk.pc.in`, `tsk_config.h.in` (committed autoheader template), `docs/` (Doxygen pages and `Doxyfile`), `sorter/` (data files). | Core |
| `tsk/auto/sqlite3.c`, `sqlite3.h` | Bundled SQLite 3.8.11.1 (`SQLITE_VERSION` in `sqlite3.h`). There is no `vendors/` directory. | SQLite fallback |
| `tools/` | Command-line tools, one subdirectory per layer (`imgtools`, `vstools`, `fstools`, `pooltools`, `hashtools`, `srchtools`, `autotools`), plus `fiwalk/`, and the Perl scripts `sorter/` and `timeline/` (`mactime`). | Reference users of the API |
| `samples/` | Four small API examples (`callback-style.cpp`, `posix-style.cpp` and their C++ variants), built as `noinst_PROGRAMS`. | Reference |
| `tests/` | API test programs (`read_apis.cpp`, `fs_fname_apis.cpp`, `fs_attrlist_apis.cpp`, `fs_thread_test.cpp` with `tsk_thread.cpp`) and the scripts `runtests.sh`, `test_libraries.sh`, `wine_wrapper.sh`. | Tests |
| `unit_tests/` | CppUnit tests: `unit_tests/base/` (`test_base.cpp`, `errors_test.cpp`, `errors_test.h`) and, in the fork, `unit_tests/fs/` (`test_fs`, see `fs.md`, "Unit tests"). | Tests |
| `test/` | `test/tools/autotools/test_loaddb.sh`, which no `Makefile.am` references (orphan). `test/vm/`: the validation setup of #10. The validate command (`test/vm/validate.sh`, gates 1–5), the Validation VM's scripts (AlmaLinux 9.9 under VMware Workstation), the C++14 Manifest, comparison, harness, Ground truth capture, Test corpus and CLI tool check programs (built by `test/vm/manifest/manifest.mk`), and the XFS fixture images in `test/vm/fixtures/xfs/`; see `test/vm/README.md`. Not part of the autotools build. | Orphan; fork validation |
| `m4/` | Autoconf macros: `tsk_opt_dep_check.m4` (optional-dependency logic), `cppunit.m4` (`AM_PATH_CPPUNIT`), `ax_pkg_check_modules.m4`, `ax_pthread.m4`, `ax_cxx_compile_stdcxx.m4`, Java macros, and the unused `ax_check_openssl.m4`. | Unix build |
| `win32/` | Visual Studio solution `tsk-win.sln`, `libtsk/libtsk.vcxproj` (+ `.filters`, `packages.config`), one project directory per tool, `NugetPackages.props`, `updateAndBuildAll.py`, `BUILDING.txt`. | Windows build |
| `configure.ac`, `Makefile.am` (top and per directory), `bootstrap` | The Unix build. | Unix build |
| `NEWS.txt` | Release notes per version. | Read when checking history |
| `API-CHANGES.txt` | A to-do list of planned incompatible API changes. It is not a changelog. | Low |
| `docs/README.txt` | Points to the wiki; the upstream repo has no other docs here. `docs/llm/` and `docs/agents/` are fork additions. | Low |

Ignore these for library work: `bindings/java/`, `case-uco/java/`, `debian/` (builds only the Java package), `rejistry++/` and `tools/logicalimager/` (Windows-only, not in `tools/Makefile.am`), `xcode/` (not used by any build script), `db_diff/`, `packages/`, `release/` (except for version bumps), `man/`, `ruleset.xml` (a Java lint rule set), `ChangeLog.txt` (an obsolete stub pointing at svn), `appveyor.yml`, `travis_install_libs.sh`.

## Development environment

The reference Linux toolchain is the devcontainer in `.devcontainer/`: GCC 16.1 (official `gcc:16.1.0-trixie` image), LLVM 22 `clangd`/`clang-format`/`clang-tidy`, bear, gdb, valgrind, cppunit, xfsprogs and the Debian libewf/libvhdi/libvmdk/afflib packages. `CLAUDE.md` ("Development environment") has the commands. Facts that matter when reading the rest of this file:

- `.devcontainer/build-compile-db.sh` configures with `--disable-java --without-libbfio` (see "Known defects and traps", item 2), builds everything under `bear`, then builds the `check_PROGRAMS` (`make check TESTS=`) so `tests/` and `unit_tests/` sources are in `compile_commands.json`.
- In the container `clangd` and `clang-tidy` are wrappers that point clang at GCC 16.1's headers (`--query-driver` and `--gcc-toolchain=/usr/local`).
- `.clang-format` approximates the `.indent.pro` style; `.clang-tidy` enables bug-finding checks only (`bugprone-*`, `clang-analyzer-*`, `performance-*`, `portability-*`, a few `misc-*`). Both are meant for changed lines (`git clang-format`, `clang-tidy-diff`), not for whole files.
- The image provides a `cppunit-config` shim over `pkg-config cppunit`, because Debian's cppunit lacks the script and `configure.ac` builds `unit_tests/` only when it finds it (see "Tests").
- With this setup, `make check` at tag 4.15.0 passes: `test_base` (3 cppunit tests), `test_libraries.sh` (vmdk, vhd, E01), and `runtests.sh` skipped.

## Unix build commands

`bootstrap` runs `aclocal`, `libtoolize --force` (or `glibtoolize`), `automake --foreign --add-missing --copy` and `autoconf`. It does not run `autoheader`. `configure.ac` declares `AC_PREREQ(2.59)`.

```sh
./bootstrap
./configure --disable-java --without-libbfio \
    CFLAGS='-g -O2 -fno-strict-aliasing' CXXFLAGS='-g -O2 -fno-strict-aliasing'
    # Java is probed by default, always disable it; libbfio: see "Known defects and traps", item 2;
    # -fno-strict-aliasing: item 19 (GCC 16)
make -j"$(nproc)"                   # library, tools and samples
make -j"$(nproc)" check             # builds and runs tests/ (and unit_tests/ if enabled)
```

To build only the library, build its directory. The install targets below are the standard automake names for the primary variables used (`lib_LTLIBRARIES` and `nodist_pkgconfig_DATA` in `tsk/Makefile.am`, `nobase_include_HEADERS` in the top `Makefile.am`); they were not executed while writing this file.

```sh
make -C tsk -j"$(nproc)"                         # tsk/libtsk.la
make -C tsk install                              # library and tsk.pc
make install-nobase_includeHEADERS               # headers (top directory)
```

Useful variations:

```sh
make V=1                                       # show full compile and link commands
./configure --disable-java --without-libbfio --disable-shared    # static library only
./configure --disable-java --without-libbfio --disable-multithreading
```

Rules that apply to every Unix build:

- **Java is probed unless `--disable-java` is passed.** `AC_ARG_ENABLE([java])` has no default action, and the Java checks run when `enable_java != no`. The conditional `X_JNI` turns on only if JNI headers, `ant` and `java` are all found; it adds `bindings/java` and `case-uco/java` to `SUBDIRS`. Never pass `--enable-java`. `--enable-offline` only affects the Java `Makefile.am` files.
- There is no option to skip the tools or samples; `SUBDIRS = tsk tools tests samples man $(UNIT_TESTS) ...` in the top `Makefile.am`, so plain `make` always builds them. `tests/` contains only `check_PROGRAMS`, so plain `make` builds nothing there.
- **C++ standard:** `AX_CXX_COMPILE_STDCXX([14], [noext], [mandatory])` in `configure.ac` appends `-std=c++14` (no GNU extensions) to `$CXX`. The autotools build is C++14, not C++17.
- **C standard:** nothing is set, so C files use the compiler's default dialect.
- **Warnings:** `configure.ac` sets `AM_CFLAGS` and `AM_CXXFLAGS` to `-Wall -Wextra -Wno-unused-parameter`. `tsk/auto/Makefile.am` adds `-Wmultichar -Wsign-promo -Wno-overloaded-virtual` to both. `tsk/pool`, `tsk/util` and `tsk/util/Bitlocker` replace `AM_CXXFLAGS` with `-Wall -Wextra -Werror` (see "Known defects and traps"). The bundled `sqlite3.c` is compiled with `AM_CFLAGS = -w` (`tsk/auto/Makefile.am`).
- **Include paths:** sources include headers as `"tsk/..."`. Each directory's `AM_CPPFLAGS` adds the top directory (`-I../.. -I$(srcdir)/../..` in `tsk/<layer>/`, `-I.. -I$(srcdir)/..` in `tests/` and `samples/`). Automake's `DEFAULT_INCLUDES` adds `-I$(top_builddir)/tsk` (the directory of the config header), which is how `<libtsk.h>` and `<tsk_config.h>` resolve in `unit_tests/base/`.
- **Generated files:** configure writes `tsk/tsk_config.h` (all `HAVE_*` macros; never installed) and `tsk/tsk_incs.h` (installed; see "Installed headers"). The build passes `-DHAVE_CONFIG_H` (`DEFS`), and `tsk/base/tsk_base_i.h` and `tsk/base/tsk_unicode.h` include `tsk/tsk_config.h` only under `#if HAVE_CONFIG_H`. A build that omits `-DHAVE_CONFIG_H` silently compiles with every optional feature off.
- **Regeneration is manual.** `configure.ac` uses `AM_MAINTAINER_MODE` with no argument, so the rules that rerun autotools after a change to `configure.ac` or a `Makefile.am` are disabled by default. Rerun `./bootstrap` and `./configure` yourself. If you add a check that defines a new macro, also run `autoheader` (bootstrap does not), because `tsk/tsk_config.h.in` is committed and a macro without an `#undef` line there never reaches `tsk_config.h`.
- **Tools, tests and samples link libtsk statically.** `tools/*/Makefile.am` and `tests/Makefile.am` use `LDFLAGS += -static`, `samples/` uses `AM_LDFLAGS = -static`, `tools/srchtools` uses `sigfind_LDFLAGS = -static`, and `unit_tests/base` uses `LDFLAGS = -static`. With libtool, `-static` on a program means "link uninstalled libtool libraries statically", so these binaries do not depend on `libtsk.so` even in a shared build. `tools/fiwalk/src` is the exception.

## Where sources and headers are listed

`tsk/Makefile.am` sets `SUBDIRS = base img vs fs hashdb auto pool util util/Bitlocker`. Each subdirectory builds one convenience library (`noinst_LTLIBRARIES`), and `tsk/Makefile.am` merges them through `libtsk_la_LIBADD` into the single installed library `tsk/libtsk.la`, which has no sources of its own (`libtsk_la_SOURCES =`).

| Layer | File and variable |
|---|---|
| `tsk/auto` | `tsk/auto/Makefile.am`, `libtskauto_la_SOURCES` |
| `tsk/base` | `tsk/base/Makefile.am`, `libtskbase_la_SOURCES` |
| `tsk/fs` | `tsk/fs/Makefile.am`, `libtskfs_la_SOURCES` |
| `tsk/hashdb` | `tsk/hashdb/Makefile.am`, `libtskhashdb_la_SOURCES` |
| `tsk/img` | `tsk/img/Makefile.am`, `libtskimg_la_SOURCES` |
| `tsk/pool` | `tsk/pool/Makefile.am`, `libtskpool_la_SOURCES` |
| `tsk/util` | `tsk/util/Makefile.am`, `libtskutil_la_SOURCES` |
| `tsk/util/Bitlocker` | `tsk/util/Bitlocker/Makefile.am`, `libtskbitlocker_la_SOURCES` |
| `tsk/vs` | `tsk/vs/Makefile.am`, `libtskvs_la_SOURCES` |

Checklist when you add a file to the library:

1. Add the `.c` or `.cpp` file to the layer's `_SOURCES` variable in `tsk/<layer>/Makefile.am`. List internal headers there too, or `make dist` drops them.
2. If the header is public, add it to `nobase_include_HEADERS` in the top-level `Makefile.am` instead. That list preserves paths, so `tsk/fs/foo.h` installs as `<includedir>/tsk/fs/foo.h`.
3. Add a `<ClCompile Include="..\..\tsk\...">` (source) or `<ClInclude ...>` (header) item to `win32/libtsk/libtsk.vcxproj`, and the matching entry in `win32/libtsk/libtsk.vcxproj.filters`.
4. A new subdirectory needs its own `Makefile.am`, an entry in the parent's `SUBDIRS` and in `libtsk_la_LIBADD`, and an entry in `AC_CONFIG_FILES` in `configure.ac`.
5. Tell the user that their external vcpkg port may keep its own file list. This repo cannot show whether it does.

At the time of writing, every `.c` and `.cpp` file under `tsk/` is listed in its directory's `Makefile.am`, and every header except `tsk/pool/tsk_lvm.hpp` is listed somewhere. The vcxproj list is shorter; see "Differences between the autotools and Visual Studio builds".

Other build outputs: the pkg-config file `tsk/tsk.pc` (generated by a `sed` rule in `tsk/Makefile.am` from `tsk/tsk.pc.in`), the sorter data files (`nobase_dist_data_DATA` in the top `Makefile.am`), and the shared-library version `-version-info 23:1:0` in `libtsk_la_LDFLAGS` (`tsk/Makefile.am`). That libtool version is separate from the package version. On mingw, configure adds `-no-undefined` through `LIBTSK_LDFLAGS`.

## Optional dependencies and `HAVE_*` macros

Each optional library except SQLite is probed by the macro `TSK_OPT_DEP_CHECK` in `m4/tsk_opt_dep_check.m4`. The behaviour is the same for every library:

- With no option, or with `--with-NAME`, the library is used if found; a missing library is not an error. `--without-NAME` disables it. `--with-NAME=DIR` adds `DIR/include` and `DIR/lib`, skips pkg-config, and is a configure error only if `DIR/include` does not exist.
- pkg-config is tried first when the library has a pkg-config variable prefix. Then `AC_CHECK_HEADERS` and `AC_CHECK_LIB` run in every case, and that `AC_CHECK_LIB` call is what defines the C macro `HAVE_LIB<NAME>` in `tsk/tsk_config.h`.
- A library found through pkg-config is added to `Requires.private` in `tsk.pc`; one found without pkg-config is added to `Libs.private`.

| Configure option | Macro defined | What it gates |
|---|---|---|
| `--with-libewf` | `HAVE_LIBEWF` | EnCase (E01) images: `tsk/img/ewf.cpp`, `ewf.h`, `img_open.cpp`, `img_types.c`; also a branch in `tsk/auto/auto_db.cpp` |
| `--with-libvhdi` | `HAVE_LIBVHDI` | VHD images: `tsk/img/vhd.c`, `vhd.h` |
| `--with-libvmdk` | `HAVE_LIBVMDK` | VMDK images: `tsk/img/vmdk.c`, `vmdk.h` |
| `--with-afflib` | `HAVE_LIBAFFLIB` | AFF images: `tsk/img/aff.c`, `aff.h` (no pkg-config module) |
| `--with-zlib` | `HAVE_LIBZ` | Compressed data in HFS and APFS: `tsk/fs/decmpfs.c`, `hfs.c`, `apfs_compat.cpp` |
| `--with-libvslvm` | `HAVE_LIBVSLVM` | Linux LVM pools: `tsk/pool/lvm_pool.cpp`, `lvm_pool_compat.cpp`, `lvm_pool_compat.hpp`, `tsk_lvm.hpp`, `pool_open.cpp`. Checked only if libbfio was found; otherwise it is silently off. |
| `--with-libbfio` | `HAVE_LIBBFIO` | `tsk/pool/img_bfio_handle.c`, the I/O bridge that the LVM code needs |
| `--disable-multithreading` | (removes `TSK_MULTITHREAD_LIB`) | Locks and per-thread error state. See "Installed headers". |

There are no options for QCOW, AFF4 or OpenSSL in 4.15; the source has no `qcow` or `aff4` files.

Every gated source file is always compiled; its body is wrapped in `#if`/`#ifdef` and becomes empty when the macro is absent. `tsk/img/img_types.c` and `tsk/img/img_open.cpp` use the same macros to decide which image types are advertised and opened.

SQLite is required and is handled in `configure.ac`, not by `TSK_OPT_DEP_CHECK`. Configure tries pkg-config module `sqlite3`, then `AC_CHECK_HEADERS([sqlite3.h])` and `AC_CHECK_LIB([sqlite3], [sqlite3_open])`. If that library check succeeds, `HAVE_LIBSQLITE3` is defined both as a C macro (by `AC_CHECK_LIB`) and as an automake conditional, and `-lsqlite3` goes into `Libs.private`. Otherwise `tsk/auto/sqlite3.c` is compiled into the `auto` layer (`if !HAVE_LIBSQLITE3` in `tsk/auto/Makefile.am`).

Macros that the code tests but no in-repo Unix build defines:

- `HAVE_LIBMBEDTLS` gates all of `tsk/util/Bitlocker/`, `tsk/fs/encryptionHelper.cpp` and `encryptionHelper.h`, and a branch in `tsk/fs/fs_io.c`. Neither `configure.ac` nor any `m4/` file mentions mbedtls, so BitLocker support is compiled out of every autotools build. Only the Visual Studio project defines the macro.
- `HAVE_LIBOPENSSL` gates APFS decryption: `tsk/util/crypto.cpp`, `crypto.hpp`, `tsk/fs/apfs.cpp`, `apfs_fs.cpp`, `apfs_fs.hpp`, `apfs_compat.cpp`. The OpenSSL check in `configure.ac` is commented out (`ax_openssl=no`), so encrypted APFS cannot be read from an autotools build. Only `Release|x64` in the vcxproj defines it.
- `GUID_LIBUUID`, `GUID_CFUUID`, `GUID_ANDROID` in `tsk/auto/guid.cpp`. Only `GUID_WINDOWS` is defined, and only by the vcxproj. Without one of them `GuidGenerator::newGuid()` has no definition; its caller in `tsk/auto/db_sqlite.cpp` is itself under `#ifdef GUID_WINDOWS`.
- `HAVE_LIBMAGIC` and `HAVE_EXIV2` in `tools/fiwalk/src/` (tool only).

`fiwalk` is built only if `regexec` is found (`AC_SEARCH_LIBS(regexec, ...)`, conditional `HAVE_REGEX` used in `tools/fiwalk/Makefile.am`).

## Windows build

The Windows build is `win32/tsk-win.sln` (Visual Studio 2019 format, `VisualStudioVersion = 16.0`). `win32/libtsk/libtsk.vcxproj` produces a **static library only** (`ConfigurationType` `StaticLibrary` in every configuration).

The in-repo driver is `win32/updateAndBuildAll.py`. It requires the environment variable `TSK_HOME` (the repo root), runs `nuget restore tsk-win.sln`, and then calls MSBuild on the whole solution with `/p:configuration=...`, `/p:platform=...`, `/t:clean` and `/t:build`. An equivalent manual command for the library alone, not executed while writing this file, is:

```bat
cd win32
nuget restore tsk-win.sln
msbuild tsk-win.sln /t:libtsk /p:Configuration=Release_NoLibs /p:Platform=x64 /m
```

`libtsk.vcxproj` imports `$(SolutionDir)\NugetPackages.props`, so build it through the solution or pass `SolutionDir` yourself.

Settings in `libtsk.vcxproj` that a port must match or consciously change:

- **Configurations:** `Debug`, `Release`, `Debug_NoLibs`, `Release_NoLibs`, `Release_XPNoLibs`, each for `Win32` and `x64`. The `*_NoLibs` configurations use no third-party library and need no NuGet packages.
- **Toolset:** `v142` (Visual Studio 2019) everywhere except `Release_XPNoLibs|Win32`, which uses `v141_xp`. Building with a newer Visual Studio needs those toolsets installed or a retarget (for example `/p:PlatformToolset=v143`).
- **Language:** there is no `LanguageStandard` element, so MSVC uses its default (`/std:c++14`). Warning level is 3 with warnings 4200, 4814 and 5030 disabled, and warnings are not errors. `CharacterSet` is `Unicode`.
- **Defines in every configuration:** `NOMINMAX`, `_CRT_SECURE_NO_DEPRECATE`, `_CRT_SECURE_NO_WARNINGS`, `GUID_WINDOWS`, `WIN32`, `_LIB`, and `WINVER` (`0x0601`, except `0x0501` in `Release|Win32`).
- **Feature defines:** there is no `tsk_config.h` on MSVC, so every `HAVE_*` macro comes from `PreprocessorDefinitions`. `Debug` and `Release` define `HAVE_LIBEWF`, `HAVE_LIBVMDK`, `HAVE_LIBVHDI`, `HAVE_LIBZ` and `HAVE_LIBMBEDTLS`. Only `Release|x64` also defines `HAVE_LIBOPENSSL`. The `*_NoLibs` configurations define none. `HAVE_LIBSQLITE3` is never defined, so the bundled SQLite header is used.
- **C runtime:** `/MDd` for `Debug`, `/MD` for `Release`, `/MT` for `Release_NoLibs` and `Release_XPNoLibs`. `Debug_NoLibs` is inconsistent: `/MDd` on `Win32` and `/MTd` on `x64`.
- **Dependencies** come only from NuGet packages listed in `win32/libtsk/packages.config` (libewf, libvhdi, libvmdk, zlib, OpenSSL, mbedtls), with paths hard-coded in `win32/NugetPackages.props`. No environment variable points at a dependency. `MbedTlsInclude` and `OpenSslInclude` are set only for `x64`.
- **SQLite:** `tsk/auto/sqlite3.c` is always compiled into the library.

On MSVC, `tsk/base/tsk_base.h` does not include `tsk/tsk_incs.h` (`#if !defined(_MSC_VER)`), and `tsk/base/tsk_os.h` defines `TSK_WIN32`, `TSK_MULTITHREAD_LIB`, `UNICODE` and `_UNICODE` itself. `TSK_TCHAR` is therefore a wide character on Windows; see `base-and-img.md`.

A mingw cross-build goes through autotools instead (`./configure --host=x86_64-w64-mingw32 --disable-java`). `configure.ac` adds `-Wno-format` for mingw hosts, and `tsk/base/tsk_os.h` defines `TSK_WIN32` and `UNICODE` under `__MINGW32__`. `tests/wine_wrapper.sh` is meant to be passed as `LOG_COMPILER` so that `.exe` tests run under Wine.

## Differences between the autotools and Visual Studio builds

| Feature | Autotools | Visual Studio (`libtsk.vcxproj`) |
|---|---|---|
| C++ standard | `-std=c++14` | MSVC default (no `LanguageStandard`) |
| BitLocker (`HAVE_LIBMBEDTLS`) | Never built | Built in `Debug` and `Release` |
| APFS decryption (`HAVE_LIBOPENSSL`) | Never built | `Release|x64` only |
| AFF images | Optional | Source is listed, but no configuration defines `HAVE_LIBAFFLIB` |
| LVM pools | Optional | Not available: `tsk/pool/lvm_pool.cpp`, `lvm_pool_compat.cpp`, `img_bfio_handle.c` are not in the project |
| NTFS USN journal | Built | Not available: `tsk/fs/usn_journal.c` and `usnjls_lib.c` are not in the project |
| `tsk/base/crc.c` | Built | Not in the project; its only caller (`ext4_group_desc_csum` in `tsk/fs/ext2fs.c`) is under `#ifdef EXT4_CHECKSUMS`, which is commented out |
| SQLite | System library if found, else bundled | Always bundled |
| Device-ID GUID generation in `tsk/auto/db_sqlite.cpp` | Not compiled (`GUID_WINDOWS` undefined) | Compiled |
| Library type | Shared and static (libtool) | Static only |
| Tools | All, including `usnjls`, `fiwalk`, `sigfind`, `srch_strings`, `mactime`, `sorter` | Those six have no project |
| Tests | `make check` | None in the solution |

## Installed headers and consumer hazards

The installed set is exactly `nobase_include_HEADERS` in the top-level `Makefile.am`, which includes the generated `tsk/tsk_incs.h`. `tsk/tsk.pc` gives consumers `-I${includedir}` and `-ltsk`, and it sets no preprocessor defines. Headers ending in `_i.h` are internal and are not installed, and neither are `tsk/tsk_config.h`, `tsk/tsk_tools_i.h`, `tsk/auto/tsk_case_db.h`, `tsk/auto/tsk_db.h`, `tsk/auto/tsk_db_sqlite.h`, `tsk/pool/tsk_lvm.hpp` and the image-format headers in `tsk/img/` (`ewf.h`, `vhd.h`, `vmdk.h`, `aff.h`, `raw.h`).

`tsk/tsk_incs.h` is written by configure (`AC_CONFIG_COMMANDS` in `configure.ac`). It includes `<unistd.h>`, `<inttypes.h>` (after defining `__STDC_FORMAT_MACROS`) and `<sys/param.h>` when they exist, and defines `TSK_MULTITHREAD_LIB` when multithreading is on. `tsk_base.h` includes it on every compiler except MSVC, so a port that does not run configure must generate an equivalent file.

The umbrella header `tsk/libtsk.h` is safe: it and everything it includes (`tsk_base.h`, `tsk_img.h`, `tsk_vs.h`, `tsk_fs.h`, `tsk_hashdb.h`, `tsk_auto.h`, `tsk_pool.h`, `tsk_os.h`, `tsk_incs.h`) reference only installed headers. The hazards are in the other installed headers.

Installed headers that include a header that is not installed, so they cannot be compiled against an installed tree:

| Installed header | Includes (not installed) |
|---|---|
| `tsk/auto/tsk_is_image_supported.h` | `tsk/tsk_tools_i.h` |
| `tsk/fs/tsk_fatfs.h`, `tsk_fatxxfs.h`, `tsk_exfatfs.h` | `tsk/fs/tsk_fs_i.h` |
| `tsk/fs/encryptionHelper.h` | `tsk/img/tsk_img_i.h`, `tsk/fs/tsk_fs_i.h` |
| `tsk/pool/lvm_pool_compat.hpp` | `tsk/base/tsk_base_i.h`, and `tsk/pool/tsk_lvm.hpp` under `HAVE_LIBVSLVM` |
| `tsk/util/detect_encryption.h` | `tsk/base/tsk_base_i.h` |
| `tsk/util/Bitlocker/BitlockerParser.h`, `DataTypes.h`, `MetadataEntry.h`, `MetadataUtils.h` | `tsk/base/tsk_base_i.h` (and `tsk_img_i.h` in `BitlockerParser.h`) |

Installed headers whose declarations change with a `HAVE_*` macro. The library is compiled with the macro set from `tsk_config.h`; a consumer normally has none of them defined, so the two sides can disagree about a type's layout or existence:

- `tsk/fs/apfs_fs.hpp`: `APFSJObjTree::crypto` gains a `std::unique_ptr<aes_xts_decryptor>` member under `HAVE_LIBOPENSSL` (only relevant to MSVC `Release|x64`).
- `tsk/util/crypto.hpp`: the whole `aes_xts_decryptor` class exists only under `HAVE_LIBOPENSSL`, and it gains a `std::mutex` member under `TSK_MULTITHREAD_LIB`.
- `tsk/pool/lvm_pool_compat.hpp`: contents exist only under `HAVE_LIBVSLVM`.
- `tsk/fs/encryptionHelper.h` and every header in `tsk/util/Bitlocker/`: contents depend on `HAVE_LIBMBEDTLS`.

`TSK_MULTITHREAD_LIB` is safe on Unix because it travels in the installed `tsk_incs.h`. Three Bitlocker headers (`BitlockerUtils.h`, `DataTypes.h`, `MetadataUtils.h`) contain `using namespace std;` at file scope.

## Tests

There are two test suites, both run by `make check` through automake's parallel test harness (the default, since `AM_INIT_AUTOMAKE([foreign tar-ustar])` does not request `serial-tests`). There is no Catch2, no `test/catch_runner` and no CLI test runner in 4.15.

`make check` at the top recurses through `SUBDIRS`. At 4.15.0 only two directories define `TESTS` (the fork adds a third, `unit_tests/fs/`, below):

- **`tests/`** (`tests/Makefile.am`): builds `check_PROGRAMS = read_apis fs_fname_apis fs_attrlist_apis fs_thread_test` and runs `TESTS = runtests.sh test_libraries.sh`. The tests run with `tests/` in the build tree as the current directory.
  - `runtests.sh` runs `./fs_thread_test` on five images in `${HOME}/from_brian` (`ext2fs.dd`, `misc-ufs1.dd`, `test_hfs.dmg`, `ntfs-img-kw-1.dd`, `fat32.dd`) and diffs per-thread logs. If that directory is missing it exits 77 (skipped). Its own header says it is not used anywhere and that the images are not public.
  - `test_libraries.sh` downloads `imageformat_mmls_1.vhd`, `.vmdk` and `.E01` from Google Drive into `tests/data/` (gitignored) with `wget`, then asks `../tools/vstools/mmls -i list` which image types are compiled in and runs `mmls` on each supported image. Unsupported formats print "Tools not compiled with ..." and do not fail. The download runs whether or not the formats are supported, and a file still missing after download fails the script (exit 1). It needs network access.
  - `read_apis`, `fs_fname_apis` and `fs_attrlist_apis` are built but never run. Each takes one argument, a directory containing the images it opens by fixed name (`fat12.dd`, `fat-img-kw.dd`, `fe_test_1.img`, `ntfs-img-kw-1.dd`, `ntfs-comp-1.img`). None of these images are in the repo.
- **`unit_tests/base/`**: CppUnit program `test_base` (`TESTS = test_base`), built only when the automake conditional `CPPUNIT` is true (`if CPPUNIT` in the top `Makefile.am` adds `unit_tests` to `SUBDIRS`). It links `../../tsk/libtsk.la $(CPPUNIT_LIBS)`.
  - `test_base.cpp` holds `main`: it runs everything in `CppUnit::TestFactoryRegistry` through a `TextUi::TestRunner` with a `CompilerOutputter`, ignores `argv`, and returns 1 on any failure.
  - `errors_test.h`/`errors_test.cpp` define the fixture `ErrorsTest` (registered with `CPPUNIT_TEST_SUITE_REGISTRATION`) with `testInitialState`, `testLengthChecks` and, under `HAVE_PTHREAD` from `tsk_config.h`, `testMultithreaded`. They test `tsk_error_*` in `tsk/base/tsk_error.c`.
  - To add a test file: add it to `test_base_SOURCES` in `unit_tests/base/Makefile.am`. To add a layer directory: create `unit_tests/<layer>/Makefile.am` in the same shape, add it to `SUBDIRS` in `unit_tests/Makefile.am`, add `unit_tests/<layer>/Makefile` to `AC_CONFIG_FILES`, then rerun `./bootstrap` and `./configure`.
- **`unit_tests/fs/`** (fork): CppUnit program `test_fs` for the fs layer, in the same shape as `unit_tests/base/` but with `AM_LDFLAGS = -static` (so it keeps configure's `LDFLAGS`) and `$(top_srcdir)` include paths. Before it runs, `make check` unpacks the XFS Fixture images from `test/vm/fixtures/xfs/` and builds a small ext4 image with e2fsprogs (`check_DATA`); details in `fs.md`, "Unit tests". It needs `xz`, `mkfs.ext4` and `debugfs`, all in the devcontainer. It compiles the harness and comparison step from `test/vm/` (through one-line `vm_*.cpp` wrappers, so no `subdir-objects` is needed) and takes its data from there, so it can't be built from a `make dist` tarball, which has no `test/vm/`. Without e2fsprogs or `xz`, `make check` fails at the data step rather than skipping.

How cppunit is detected (`configure.ac`, `m4/cppunit.m4`):

- `AM_PATH_CPPUNIT(1.12.1)` runs unconditionally near the top of `configure.ac`. It looks only for the `cppunit-config` script (options `--with-cppunit-prefix`, `--with-cppunit-exec-prefix`, or a preset `CPPUNIT_CONFIG` variable) and calls it with `--cflags`, `--libs` and `--version`. The conditional `CPPUNIT`, which decides whether `unit_tests/` is built, is set from this result alone.
- Later, unless `--disable-cppunit` is given, configure tries pkg-config module `cppunit >= 1.12.1`, falls back to `AM_PATH_CPPUNIT` again, and does a link check for `CppUnit::TextUi::TestRunner`. These set only the conditional `HAVE_CPPUNIT`, which no `Makefile.am` uses. See "Known defects and traps", item 18.
- Debian's cppunit 1.15.1 ships a pkg-config file but no `cppunit-config`, so on such a system `unit_tests/` is not built. The devcontainer installs a `/usr/local/bin/cppunit-config` shim that forwards to `pkg-config cppunit`; `configure.ac` is left as upstream.

Commands (run from the top of the build directory; tests use paths relative to their own directory):

```sh
make -j"$(nproc)" check                          # everything
make check VERBOSE=1                             # print the log of each failing test
make -C tests check TESTS=test_libraries.sh      # one test script
make -C tests check TESTS=                       # build tests/ programs only
make -C unit_tests/base check                    # cppunit tests (only if CPPUNIT is on)
./unit_tests/base/test_base                      # run them directly
make -C unit_tests/fs check                      # fs layer cppunit tests (makes their data first)
./unit_tests/fs/test_fs                          # run them directly (after the data exists)
(cd tests && ./read_apis /path/to/image/dir)     # run an API test program by hand
(cd tests && ./fs_thread_test -f ntfs img.dd 4 2) # args: [-f fstype] [-o imgoffset] [-v] image nthreads niters
grep CPPUNIT_TRUE config.status                  # "#" means unit_tests/ is not built
```

`test_base` has no command-line filter, so a single CppUnit test cannot be selected without changing `main` (for example passing a test name to `TestRunner::run`). Each test writes `<test>.log` and `<test>.trs` next to itself, and `test-suite.log` in that directory summarises failures. Exit code 77 means "skipped". Because of `-static`, the programs in `tests/` and `unit_tests/base/` are linked against the static libtsk; rebuild them after changing the library.

`test/tools/autotools/test_loaddb.sh` is not wired into any `Makefile.am`. It runs `tools/autotools/tsk_loaddb` (relative to the top build directory) on three images under `$SLEUTHKIT_TEST_DATA_DIR` and has no exit-77 guard for a missing variable. Nothing else in the tree reads `SLEUTHKIT_TEST_DATA_DIR`.

### CI

All GitHub Actions workflows are disabled in this fork (renamed to `.github/workflows/*.yml-disabled`), so nothing runs on push or pull request. `appveyor.yml` is still in the tree; whether an AppVeyor project is attached to the fork is not visible from the repo. For reference only:

- `build-unix.yml-disabled` builds on macOS and Ubuntu (gcc and clang) and mingw under Wine, runs `make check`, then reconfigures with `--enable-address-sanitizer` and checks a `make dist` tarball. It passes options that 4.15's `configure` does not have (`--with-libqcow`, `--enable-address-sanitizer`), and it collects logs from `test/` although the 4.15 test logs are under `tests/` and `unit_tests/`.
- `code-coverage.yml-disabled` builds with `--coverage` and runs `make check`.
- `compile-windows.yml-disabled` runs `python win32\updateAndBuildAll.py -m` and the Java build on `windows-2025`; it runs no C/C++ tests.
- `appveyor.yml` builds the Visual Studio solution and Java on a Visual Studio 2019 image, and runs `make check` on Ubuntu 22.04 and macOS.

## Version numbers

The version is stored in several places that are edited by hand or by `release/release-unix.pl`. At this tag they agree:

| Location | Value |
|---|---|
| `configure.ac`, `AC_INIT` | `4.15.0` |
| `tsk/base/tsk_base.h`, `TSK_VERSION_STR` | `"4.15.0"` |
| `tsk/base/tsk_base.h`, `TSK_VERSION_NUM` | `0x041500ff` |
| `packages/sleuthkit.spec` | `4.15.0` |
| `bindings/java/build.xml` | `4.15.0` |
| `tsk/docs/Doxyfile`, `PROJECT_NUMBER` | `4.15.0` |
| `tsk/Makefile.am`, `-version-info` | `23:1:0` (libtool interface version, bumped interactively by the release script) |
| `NEWS.txt`, newest entry | 4.15.0 |

`tsk_version_get_str()` (`tsk/base/tsk_version.c`) returns `TSK_VERSION_STR`. `tsk.pc` takes its version from `AC_INIT`.

## Coding conventions

- **C and C++ split:** most of the library is C (`tsk/base/`, `tsk/vs/`, most of `tsk/img/` such as `raw.c`, `img_io.c`, `vhd.c`, `vmdk.c`, most of `tsk/fs/` including the `*_lib.c` files, most of `tsk/hashdb/`). C++ is used for APFS, pools, `tsk/auto/`, `tsk/util/crypto.cpp` and Bitlocker, and for files such as `img_open.cpp`, `ewf.cpp`, `fatfs_dent.cpp`, `ntfs_dent.cpp`, `yaffs.cpp`, `logical_fs.cpp` and `sqlite_hdb.cpp`. The public API is C, declared inside `extern "C"` blocks, with thin C++ wrapper classes in the same headers.
- **Header names:** `tsk_<layer>.h` is the public header of a layer, `*_i.h` is internal, `.hpp` is C++-only.
- **Function names:** `tsk_<layer>_<object>_<verb>`, for example `tsk_fs_dir_walk`. File-system internals use a per-format prefix (`ext2fs_`, `ntfs_`, `hfs_`).
- **Type names:** upper snake case typedefs (`TSK_FS_INFO`), enums named `TSK_*_ENUM`. C++ classes are camel case (`TskAuto`, `APFSFileSystem`) with `m_` or `_` member prefixes.
- **Dispatch:** per-format behaviour is reached through function pointers in the info structs (`fs->istat`, `fs->jopen`), not through C++ virtual functions.
- **Indentation:** `.indent.pro` files for GNU indent exist in every `tsk/` layer directory, in each C++ tool directory under `tools/`, and in `samples/` and `tests/`. They contain `-kr -psl -nce -ip2 -nlp -nut` (K&R style, 4 spaces, no tabs, return type on its own line above the function name), except `tsk/hashdb/.indent.pro`, which omits `-nlp`. Each of those `Makefile.am` files has an `indent` target.
- **Error handling:** error state is per thread. A failing function calls `tsk_error_reset()`, `tsk_error_set_errno(TSK_ERR_...)` and `tsk_error_set_errstr("fmt", ...)`, then returns `1` (for `uint8_t` results) or `NULL`; `0` means success. Walk callbacks return `TSK_WALK_CONT`, `TSK_WALK_STOP` or `TSK_WALK_ERROR`. Newer C++ code throws internally and converts at the C boundary with a function-try-block (see `tsk_apfs_set_snapshot` in `tsk/fs/apfs_compat.cpp`). Details are in `base-and-img.md`.
- **Attributes:** there are no shared `FALLTHROUGH` or `UNUSED` macros. `tsk/base/tsk_unicode.c` defines its own local `FALLTHROUGH`.
- **Doxygen:** Javadoc-style `/** ... */` blocks and `///<` trailing comments, with backslash commands (`\file`, `\ingroup`, `\param`, sometimes `@param`). Group pages are `tsk/docs/*.dox`. `make api-docs` runs Doxygen, but it also runs the Java Doxygen; use `doxygen tsk/docs/Doxyfile` for the C/C++ part alone.
- **File header:** files start with a "The Sleuth Kit" comment naming the Common Public License 1.0. Third-party files (`tsk/auto/sqlite3.c`, `tsk/auto/guid.cpp`, `tsk/base/crc.c`, `md5c.c`, `sha1c.c`, `tsk_unicode.c`, `XGetopt.c`, among others) keep their own licenses; `licenses/README.md` is the inventory.

## Tools and the library functions they exercise

Most tools include `tsk/tsk_tools_i.h` (`usnjls.cpp` includes `tsk/fs/tsk_fs_i.h` directly; `srch_strings.c` uses no TSK header; `fiwalk` uses its own `fiwalk.h`). The usual sequence is `tsk_img_open` (`tsk/img/img_open.cpp`), then optionally `tsk_pool_open_img_sing` (`tsk/pool/pool_open.cpp`), then `tsk_fs_open_img_decrypt` (`tsk/fs/fs_open.c`), then the function below.

| Tool (directory under `tools/`) | Purpose | Library entry point |
|---|---|---|
| `img_stat` (`imgtools`) | Image details | `imgstat` function pointer of the image |
| `img_cat` (`imgtools`) | Raw image bytes | `tsk_img_read`, `tsk/img/img_io.c` |
| `mmls` (`vstools`) | List partitions | `tsk_vs_open` (`tsk/vs/mm_open.c`), `tsk_vs_part_walk` (`mm_part.c`) |
| `mmstat` (`vstools`) | Volume system type | `tsk_vs_open`, `tsk_vs_type_toname` (`mm_types.c`) |
| `mmcat` (`vstools`) | Partition contents | `tsk_vs_part_get`, `tsk_vs_part_read_block` (`mm_io.c`) |
| `pstat` (`pooltools`) | Pool details | `pool->poolstat` |
| `fsstat` (`fstools`) | File system details | `fs->fsstat` |
| `fls` | List file names | `tsk_fs_fls`, `tsk/fs/fls_lib.c` |
| `ffind` | Name for an inode | `tsk_fs_ffind`, `ffind_lib.c` |
| `ils` | List inodes | `tsk_fs_ils`, `ils_lib.c` |
| `istat` | Inode details | `fs->istat` |
| `icat` | File content by inode | `tsk_fs_icat`, `icat_lib.c` |
| `ifind` | Inode for a block, name or parent | `tsk_fs_ifind_data`, `tsk_fs_ifind_path`, `tsk_fs_ifind_par`, `ifind_lib.c` |
| `fcat` | File content by path | `tsk_fs_ifind_path`, then `tsk_fs_icat` |
| `blkls` | Block listing and unallocated space | `tsk_fs_blkls`, `dls_lib.c` |
| `blkcat` | Block content | `tsk_fs_blkcat`, `dcat_lib.c` |
| `blkstat` | Block allocation status | `tsk_fs_blkstat`, `dstat_lib.c` |
| `blkcalc` | Map a `blkls` address to the original | `tsk_fs_blkcalc`, `dcalc_lib.c` |
| `jls`, `jcat` | Journal entries and blocks | `fs->jopen`, then `fs->jentry_walk` or `fs->jblk_walk` |
| `usnjls` | NTFS USN journal | `tsk_fs_usnjls`, `usnjls_lib.c`; declared only in the internal `tsk_fs_i.h` |
| `hfind` (`hashtools`) | Hash database lookup and indexing | `tsk_hdb_open`, `tsk_hdb_lookup_str`, `tsk_hdb_make_index`, `tsk/hashdb/tsk_hashdb.c` |
| `sigfind` (`srchtools`) | Search an image for a signature | `tsk_img_open_utf8_sing`, `tsk_img_read` |
| `tsk_loaddb` (`autotools`) | Image to SQLite case database | `TskCaseDb`, `TskAutoDb`, `tsk/auto/case_db.cpp`, `auto_db.cpp` |
| `tsk_recover`, `tsk_comparedir`, `tsk_gettimes` | Walk every file in an image | Subclasses of `TskAuto`, `tsk/auto/auto.cpp` |
| `tsk_imageinfo` | Report whether an image is supported or encrypted | `TskIsImageSupported`, `tsk/auto/is_image_supported.cpp` |
| `fiwalk` (`fiwalk/src`) | DFXML file listing | `tsk_img_open_utf8`, `tsk_fs_open_img`, `tsk_vs_part_walk`, `tsk_fs_dir_walk` |

Note the file-name mismatch for the block tools: the `blk*` functions live in `d*_lib.c` files. `srch_strings` does not link the library. `tools/hashtools/md5.c`, `sha1.c` and `tools/fstools/fscheck.cpp` are distributed (`EXTRA_DIST`) but never compiled. `fiwalk` is built only when `HAVE_REGEX` is true, and `tools/fiwalk/plugins` builds `jpeg_extract`.

## Compiler portability notes

These are observations of the source, not build results.

- **C dialect is the compiler default.** The Unix build passes no `-std` for C. Newer GCC releases default to C23; check with `gcc -dM -E -x c /dev/null | grep __STDC_VERSION__`. Commit `37bb48f0d` ("fix compiler errors on C23"), which is in this tag, adjusted `tsk/base/crc.c`, `md5c.c`, `sha1c.c` and `tsk_base.h`.
- **Empty parameter lists in C-visible headers**, for example `tsk_error_get_info()`, `tsk_error_get_errno()`, `tsk_error_get()` and `tsk_error_reset()` in `tsk/base/tsk_base.h`, `tsk_img_type_supported()` in `tsk_img.h`, `tsk_vs_type_supported()` in `tsk_vs.h`, `tsk_fs_type_supported()` in `tsk_fs.h`, and the function-pointer fields `uses_external_indexes` and `accepts_updates` in `tsk/hashdb/tsk_hashdb.h`. In C23 `()` means `(void)`; before C23 it means "unspecified arguments". A consumer compiling C with `-Wstrict-prototypes` will get warnings from these headers.
- **C++ dialect.** Configure forces `-std=c++14`; the vcxproj leaves the MSVC default. `NEWS.txt` (4.13.0 entry) says "Updated to C++17", which does not match `configure.ac` in this tag. A port with its own build scripts must choose the standard explicitly instead of relying on the compiler default; check the default with `g++ -dM -E -x c++ /dev/null | grep __cplusplus`.
- **`-Werror` is part of the build** for C++ files in `tsk/pool`, `tsk/util` and `tsk/util/Bitlocker`. A new warning from a newer compiler in those directories fails the build.
- **`std::wstring_convert` and `<codecvt>`** (deprecated in C++17) are used in `tsk/util/Bitlocker/BitlockerParser.cpp`, which is compiled only with `HAVE_LIBMBEDTLS`.
- **`tsk/util/Bitlocker/BitlockerUtils.h`** uses `uint8_t`, `uint32_t`, `uint64_t` and `size_t` but includes only `<string>`. It compiles only if another header provided those types first. This matters only when `HAVE_LIBMBEDTLS` is defined.
- **`tsk/base/tsk_os.h` on MSVC** defines `WIN32_LEAN_AND_MEAN`, the typedefs `mode_t` and `ssize_t` (with `HAVE_SSIZE_T`), and the macros `strcasecmp`, `putenv`, `roundup` and `fseeko`, and includes `<windows.h>`. On mingw it defines `daddr_t` and `roundup` as macros and includes `<winsock2.h>` and `<windows.h>`. These leak into every consumer translation unit that includes a TSK header.
- **Clang-only flag in `Makefile.am` files.** Most tool, test and sample directories add `-Wno-unused-command-line-argument` to `AM_CXXFLAGS`. GCC ignores an unknown `-Wno-*` option unless another diagnostic is emitted, in which case it reports it; check this if GCC output mentions that option.
- **Windows-only code outside the library** has problems with newer C++ standards that do not affect `libtsk`: `std::auto_ptr` (removed in C++17) in `tools/logicalimager/RegParser.cpp`, and `throw()` exception specifications (removed in C++20) in `rejistry++/src/RejistryException.h`.

## Known defects and traps

Each item was confirmed by reading the named file.

1. **`-Werror` in three library directories.** `tsk/pool/Makefile.am`, `tsk/util/Makefile.am` and `tsk/util/Bitlocker/Makefile.am` set `AM_CXXFLAGS = -Wall -Wextra -Werror`, replacing the configure-wide flags (so `-Wno-unused-parameter` is also lost there). Any new compiler warning in those C++ files breaks the build.
2. **Distro libewf plus system libbfio crashes on E01 images.** Debian/Ubuntu `libewf.so.2` (20140816) exports its own bundled `libbfio_*` symbols. When configure also finds the system libbfio (pulled in by `libvhdi-dev`/`libvmdk-dev`), the tools link `-lbfio` ahead of `-lewf`, libewf's internal calls bind to the newer incompatible libbfio, and `libewf_handle_close` aborts with a double free (`mmls` on any `.E01`; `tests/test_libraries.sh` fails with "ewf test failed"). Configure with `--without-libbfio` (also disables LVM pools), or use a libewf built against the system libbfio.
3. **BitLocker and APFS decryption are unreachable from autotools.** `HAVE_LIBMBEDTLS` and `HAVE_LIBOPENSSL` are defined only in `win32/libtsk/libtsk.vcxproj`; the OpenSSL check in `configure.ac` is commented out.
4. **SQLite headers can mismatch the linked library.** `tsk/hashdb/sqlite_hdb.cpp` always includes the bundled `tsk/auto/sqlite3.h` (3.8.11.1), while `tsk/auto/tsk_db_sqlite.h` uses `<sqlite3.h>` when `HAVE_LIBSQLITE3` is defined. With a system SQLite, `sqlite_hdb.cpp` is compiled against the old bundled header and linked against the system library.
5. **`tsk/pool/tsk_lvm.hpp` is in no `Makefile.am`.** `make dist` omits it and `make install` does not install it, yet the installed `tsk/pool/lvm_pool_compat.hpp` includes it under `HAVE_LIBVSLVM`, so a tarball built with libvslvm support cannot compile.
6. **Installed headers include headers that are not installed**, and some change layout with `HAVE_*` macros. See "Installed headers and consumer hazards".
7. **`win32/libtsk/libtsk.vcxproj` is missing six sources** that autotools builds: `tsk/base/crc.c`, `tsk/fs/usn_journal.c`, `tsk/fs/usnjls_lib.c`, `tsk/pool/img_bfio_handle.c`, `tsk/pool/lvm_pool.cpp`, `tsk/pool/lvm_pool_compat.cpp`. It has no `LanguageStandard`, so MSVC builds with its own default rather than an explicit standard.
8. **`Debug_NoLibs` uses different C runtimes per platform** in `libtsk.vcxproj`: `MultiThreadedDebugDLL` on `Win32`, `MultiThreadedDebug` on `x64`.
9. **NuGet include paths are x64-only for mbedtls and OpenSSL.** `win32/NugetPackages.props` sets `MbedTlsInclude` and `OpenSslInclude` only for `x64`, but `Debug|Win32` and `Release|Win32` define `HAVE_LIBMBEDTLS`.
10. **Most of `make check` tests nothing by default.** `tests/runtests.sh` exits 77 without `${HOME}/from_brian`; `test_libraries.sh` passes without running `mmls` when libvhdi, libvmdk and libewf are absent; `read_apis`, `fs_fname_apis` and `fs_attrlist_apis` are built but are not in `TESTS`; `unit_tests/` is skipped unless `cppunit-config` is found.
11. **`tests/test_libraries.sh` needs the network.** It downloads three images from Google Drive on every run where `tests/data/` lacks them, and fails if a file is missing afterwards.
12. **`test/tools/autotools/test_loaddb.sh` is orphaned.** No `Makefile.am` references it, and it has no skip guard for a missing `SLEUTHKIT_TEST_DATA_DIR`.
13. **`unit_tests/base/Makefile.am` overrides user variables.** It assigns `LDFLAGS = -static`, discarding the `LDFLAGS` that configure computed, and its `AM_CPPFLAGS = -I../.. $(CPPUNIT_CFLAGS)` has no `$(srcdir)` form, so it works only in an in-tree build.
14. **Autotools regeneration is manual.** `AM_MAINTAINER_MODE` disables the rebuild rules, and `bootstrap` does not run `autoheader` although `tsk/tsk_config.h.in` is committed. See "Unix build commands".
15. **pthread link flags are dropped.** The `AX_PTHREAD` action in `configure.ac` assigns `CLIBS="$PTHREAD_LIBS $LIBS"`; nothing reads `CLIBS`, so `PTHREAD_LIBS` reaches neither the library's `LIBS` nor `tsk.pc`. Only `PTHREAD_CFLAGS` is added to `CPPFLAGS` and `LDFLAGS`. Check static linking of a consumer against the generated `tsk.pc`.
16. **The disabled CI workflow does not match this tag.** `build-unix.yml-disabled` passes `--with-libqcow` and `--enable-address-sanitizer`, which 4.15's `configure` does not define, and reads logs from `test/`. Fix it before re-enabling.
17. **`win32/updateAndBuildAll.py` searches fixed MSBuild paths** (Visual Studio 2019 Community, 2022 Enterprise and a legacy MSBuild 14.0 path) when `MSBuild.exe` is not on `PATH`.
18. **`--disable-cppunit` does not disable the unit tests, and pkg-config detection does not enable them.** The conditional `CPPUNIT` that controls `SUBDIRS` comes only from the first, unconditional `AM_PATH_CPPUNIT` call in `configure.ac`, which needs `cppunit-config`. `--disable-cppunit`, the pkg-config probe and the link check only set `HAVE_CPPUNIT`, which no `Makefile.am` uses. The link check also sets `CFLAGS="$CPPUNIT_CLFAGS"` (misspelled, always empty), passes the libraries in `LDFLAGS`, and its result is ignored. A build without `cppunit-config` on `PATH` (for example the owner's production build) simply skips `unit_tests/`.
19. **GCC 16.1 at `-O2` breaks file system autodetection.** In `tsk_fs_open_img_decrypt` (`tsk/fs/fs_open.c`), GCC 16.1 with the default `-g -O2` stores the `name` and `type` of each entry of the local `FS_OPENERS` table but not its `open` function pointer, then calls through it. Every `tsk_fs_open_img`/`tsk_fs_open_vol` call with `TSK_FS_TYPE_DETECT` (and so `fls`, `fsstat` or `icat` without `-f`) jumps through stack garbage and crashes, even on a valid ext4 image; an explicit type such as `-f ext4` works. `-fno-strict-aliasing` or `-fno-tree-dse` brings the stores back, and so does making the table `static const`. A reduced standalone copy of the function did not reproduce it, so whether this is a GCC bug or aliasing undefined behaviour elsewhere in the translation unit is open. `make check` doesn't catch it, because no test autodetects a file system. The devcontainer build (`.devcontainer/build-compile-db.sh`) configures with `CFLAGS` and `CXXFLAGS` set to `-g -O2 -fno-strict-aliasing`; any other GCC 16 build needs the same.
