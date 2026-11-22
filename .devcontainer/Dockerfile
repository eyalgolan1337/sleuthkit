# Development image for libtsk: GCC 16.1 (official image, Debian trixie),
# autotools, cppunit, the optional image-format libraries, LLVM tooling
# (clangd, clang-format, clang-tidy) and bear for compile_commands.json.
#
# GCC 16.1 is installed under /usr/local and comes first on PATH, so plain
# `gcc`/`g++` are 16.1. Debian's own gcc-14 (from buildpack-deps) stays in
# /usr/bin and is not used by the build.
FROM gcc:16.1.0-trixie@sha256:d8dec2859f964120119abd1380563247bf02c5d64f5bcdea496dbace1f48c6f0

ARG LLVM_VERSION=22
ARG USERNAME=dev
ARG USER_UID=1000
ARG USER_GID=1000

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

# Build dependencies, optional libtsk dependencies, debugging tools.
# libvslvm-dev (LVM pools) is deliberately absent and the build configures
# --without-libbfio: Debian's libewf 20140816 exports its own bundled
# libbfio_* symbols, and linking the system libbfio (a dependency of
# libvhdi-dev/libvmdk-dev) next to it makes every E01 close crash (double free).
RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        autoconf automake libtool pkg-config make \
        libcppunit-dev libsqlite3-dev zlib1g-dev \
        libewf-dev libvhdi-dev libvmdk-dev libafflib-dev \
        bear gdb valgrind \
        ca-certificates curl git gnupg less sudo file xxd \
    && rm -rf /var/lib/apt/lists/*

# LLVM tooling from apt.llvm.org (Debian trixie only ships LLVM 19).
RUN curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key \
        | gpg --dearmor -o /usr/share/keyrings/llvm.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] http://apt.llvm.org/trixie/ llvm-toolchain-trixie-${LLVM_VERSION} main" \
        > /etc/apt/sources.list.d/llvm.list \
    && apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        clangd-${LLVM_VERSION} clang-format-${LLVM_VERSION} clang-tidy-${LLVM_VERSION} \
    && rm -rf /var/lib/apt/lists/*

# Unversioned tool names. clangd and clang-tidy are wrapped so they resolve
# system headers from GCC 16.1 (/usr/local) instead of Debian's gcc-14.
RUN LLVM_BIN=/usr/lib/llvm-${LLVM_VERSION}/bin \
    && printf '#!/bin/sh\nexec %s/clangd --query-driver=/usr/local/bin/gcc*,/usr/local/bin/g++*,/usr/local/bin/c++,/usr/local/bin/cc "$@"\n' "$LLVM_BIN" \
        > /usr/local/bin/clangd \
    && printf '#!/bin/sh\nexec %s/clang-tidy --extra-arg-before=--gcc-toolchain=/usr/local "$@"\n' "$LLVM_BIN" \
        > /usr/local/bin/clang-tidy \
    && chmod 755 /usr/local/bin/clangd /usr/local/bin/clang-tidy \
    && ln -s "$LLVM_BIN/clang-format" /usr/local/bin/clang-format \
    && ln -s "$LLVM_BIN/git-clang-format" /usr/local/bin/git-clang-format \
    && ln -s "$LLVM_BIN/run-clang-tidy" /usr/local/bin/run-clang-tidy \
    && ln -s "/usr/bin/clang-tidy-diff-${LLVM_VERSION}.py" /usr/local/bin/clang-tidy-diff

# configure.ac builds unit_tests/ only if it finds cppunit-config, which
# Debian's cppunit (pkg-config only) does not ship. Provide it over pkg-config.
RUN printf '#!/bin/sh\ncase "$1" in\n  --cflags) exec pkg-config --cflags cppunit ;;\n  --libs) exec pkg-config --libs cppunit ;;\n  --version) exec pkg-config --modversion cppunit ;;\n  *) echo "usage: cppunit-config --cflags|--libs|--version" >&2; exit 1 ;;\nesac\n' \
        > /usr/local/bin/cppunit-config \
    && chmod 755 /usr/local/bin/cppunit-config

# Non-root user matching the usual host UID, with passwordless sudo.
RUN groupadd --gid "${USER_GID}" "${USERNAME}" \
    && useradd --uid "${USER_UID}" --gid "${USER_GID}" -m -s /bin/bash "${USERNAME}" \
    && echo "${USERNAME} ALL=(ALL) NOPASSWD:ALL" > "/etc/sudoers.d/${USERNAME}" \
    && chmod 0440 "/etc/sudoers.d/${USERNAME}" \
    && install -d -o "${USERNAME}" -g "${USERNAME}" "/home/${USERNAME}/.claude"

USER ${USERNAME}
