#!/usr/bin/env bash
# One-time setup after the devcontainer is created.
set -euo pipefail

cd "$(dirname "$0")/.."

echo "== Toolchain"
gcc --version | head -1
clangd --version | head -1
clang-format --version
clang-tidy --version | grep -i version

# A build tree configured on the host (different compiler) must not leak in.
if [[ -f Makefile ]]; then
    make -s distclean >/dev/null 2>&1 || true
fi

echo "== Configure and build under bear"
.devcontainer/build-compile-db.sh --fresh

echo "== Claude Code clangd plugin"
# A fresh Claude config only knows the built-in directory; add the marketplace
# that clangd-lsp comes from first.
claude plugin marketplace add anthropics/claude-plugins-official >/dev/null 2>&1 || true
claude plugin install clangd-lsp@claude-plugins-official --scope project \
    || echo "warning: plugin install failed; run it by hand after 'claude' login"
