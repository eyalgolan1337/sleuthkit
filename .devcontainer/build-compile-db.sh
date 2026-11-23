#!/usr/bin/env bash
# Configure and build libtsk, tools, samples, every test program and the
# validation setup's C++ tools (test/vm/manifest/) under bear, writing
# compile_commands.json at the repo root for clangd and clang-tidy.
# Run inside the devcontainer. Re-run after adding or removing source files.
#
#   .devcontainer/build-compile-db.sh            # incremental: reuse configure, rebuild everything
#   .devcontainer/build-compile-db.sh --fresh    # bootstrap + configure from scratch
set -euo pipefail

cd "$(dirname "$0")/.."

if [[ "${1:-}" == "--fresh" || ! -f config.status ]]; then
    ./bootstrap
    # --without-libbfio: see the libewf/libbfio clash noted in .devcontainer/Dockerfile.
    ./configure --disable-java --without-libbfio
fi

# Only translation units that are actually compiled get an entry, so clean
# first: objects left from an earlier build would be skipped by make.
make -s clean >/dev/null
make -s -f test/vm/manifest/manifest.mk clean
bear --output compile_commands.json -- make -j"$(nproc)"
# Build (but do not run) the check programs: unit_tests/ and tests/.
bear --append --output compile_commands.json -- make -j"$(nproc)" check TESTS=
# The Manifest library, the comparison step and its self-test (plain make;
# not part of the autotools build). Output in build/vm-tools/.
bear --append --output compile_commands.json -- make -j"$(nproc)" -f test/vm/manifest/manifest.mk

# libtool compiles each file by a name relative to its directory, so tools
# report diagnostics against e.g. "tsk_list.c" and clang-tidy-diff's line
# filter ("tsk/base/tsk_list.c") never matches. Make every source path absolute.
python3 - <<'PY'
import json, os
with open("compile_commands.json") as f:
    db = json.load(f)
for e in db:
    src = os.path.normpath(os.path.join(e["directory"], e["file"]))
    args = e.get("arguments")
    if args:
        e["arguments"] = [src if os.path.normpath(os.path.join(e["directory"], a)) == src else a
                          for a in args]
    e["file"] = src
with open("compile_commands.json", "w") as f:
    json.dump(db, f, indent=2)
PY

echo "compile_commands.json: $(grep -c '"file"' compile_commands.json) entries"
