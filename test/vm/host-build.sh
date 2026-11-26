#!/usr/bin/env bash
# The build that the Host-side run of the Strict checks uses
# (test/vm/README.md, "Gate 3: Strict checks"): the normal devcontainer build
# of the library, build/vm-tools/tsk-harness, which reads .vmdk files
# through libvmdk, and the CLI tool checks' fls, icat, fsstat, istat, ils,
# ifind, blkls and mmls.
# Run in the devcontainer, from anywhere:
#
#   test/vm/host-build.sh            # incremental
#   test/vm/host-build.sh --fresh    # bootstrap and configure first
#
# On the host, the validate command runs it before collecting the Validation
# VM's results, because the Host-side run happens then, before gate 1 builds
# the tree. An unconfigured tree, or one configured by another compiler, gets
# the full build of .devcontainer/build-compile-db.sh --fresh; otherwise only
# the library, those tools and the harness are rebuilt. Exit status 0 if they
# were built.
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
HARNESS=build/vm-tools/tsk-harness
cd "$REPO"

fresh=
case ${1:-} in
--fresh) fresh=1 ;;
'') ;;
*)
    sed -n '2,17p' "$0" >&2
    exit 2
    ;;
esac

# A failed build must not leave an old harness for the Host-side run.
rm -f "$HARNESS"

if [[ -f config.status ]] && ! grep -q 'gcc version 16\.1\.' config.log 2>/dev/null; then
    echo "== the tree was configured by another compiler: make distclean"
    make -s distclean || true
fi
if [[ -n $fresh || ! -f config.status ]] || ! grep -q -- '-fno-strict-aliasing' config.status; then
    echo "== full build (.devcontainer/build-compile-db.sh --fresh)"
    .devcontainer/build-compile-db.sh --fresh
else
    echo "== library"
    make -j"$(nproc)" -C tsk
fi
# The CLI tool checks' tools, built before the harness: if they fail, there
# is no harness, and the Host-side run reports that instead of running old
# tools.
echo "== fls, icat, fsstat, istat, ils, ifind, blkls and mmls"
make -j"$(nproc)" -C tools/fstools fls icat fsstat istat ils ifind blkls
make -j"$(nproc)" -C tools/vstools mmls
echo "== harness"
make -j"$(nproc)" -f test/vm/manifest/manifest.mk "$HARNESS"
ls -l "$HARNESS"
