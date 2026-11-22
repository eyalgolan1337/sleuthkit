# shellcheck shell=bash
# Files changed in the working tree since the merge-base with the base branch.
#
#   changes_init <base-ref>   # sets MERGE_BASE, CHANGED_TRACKED, CHANGED_UNTRACKED
#   changes_filter <ere> <array-name>...   # print entries of the arrays matching <ere>, NUL-separated
#
# CHANGED_TRACKED: tracked files that differ between MERGE_BASE and the working
# tree (committed, staged or unstaged; deleted files excluded).
# CHANGED_UNTRACKED: untracked files that .gitignore does not exclude.

C_CXX_ERE='\.(c|h|cpp|cc|cxx|c\+\+|C|hpp|hh|hxx|inc)$'

changes_init() {
    local base=$1 ref
    ref=$base
    if ! git rev-parse -q --verify "$ref^{commit}" >/dev/null; then
        ref="origin/$base"
        git rev-parse -q --verify "$ref^{commit}" >/dev/null || {
            echo "error: base '$base' is neither a local nor an origin/ ref" >&2
            return 1
        }
    fi
    BASE_REF=$ref
    MERGE_BASE=$(git merge-base "$ref" HEAD) || return 1
    mapfile -d '' -t CHANGED_TRACKED < <(git diff -z --name-only --diff-filter=d "$MERGE_BASE" --)
    mapfile -d '' -t CHANGED_UNTRACKED < <(git ls-files -z --others --exclude-standard)
}

changes_filter() {
    local ere=$1 name f
    shift
    for name in "$@"; do
        local -n arr=$name
        for f in "${arr[@]}"; do
            [[ $f =~ $ere ]] && printf '%s\0' "$f"
        done
        unset -n arr
    done
    return 0
}
