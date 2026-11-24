#!/usr/bin/env python3
"""clang-tidy on the changed lines of changed headers (gate 5).

Usage: tidy_headers.py --repo DIR --items FILE --diff FILE [--db-incomplete]

A header has no compile command of its own, and many of this repo's headers
are not self-contained (tsk/fs/tsk_fs.h needs tsk/base/tsk_base.h first), so
clang-tidy can't compile one alone. Instead, each changed header is linted
through one compiled source that includes it, according to automake's
dependency files (.deps/*.P*o, as gate 1's cxx_std_check.py finds them): a
C++ source if there is one, so the header's __cplusplus parts are seen too,
preferably one in the header's own directory. -line-filter limits the
diagnostics to the header's changed lines, and -header-filter lets them
through.

--diff is a unified diff with -U0 of the changed headers, against the
merge-base (git diff, or git diff --no-index /dev/null for new files).

clang-tidy's output goes to stdout, where gate 5 reads its diagnostics like
clang-tidy-diff's. Notes go to the items file as "fail|warn|note<TAB>text"
lines (the validate command's report protocol): which source each header was
linted through, and a warning for a changed header that no compiled source
includes (a failure with --db-incomplete: the gate 1 build failed).
Exit status: 0 if clang-tidy ran for every header that has an including
source, 1 otherwise.
"""

import argparse
import json
import os
import re
import subprocess
import sys

sys.dont_write_bytecode = True  # no __pycache__ in the source tree
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cxx_std_check import CXX_EXT, HEADER_EXT, header_users  # noqa: E402

SOURCE_EXT = CXX_EXT + (".c",)
DIAG = re.compile(r"^(.+?):\d+:\d+: (?:warning|error|note): ")
HUNK = re.compile(r"^@@ -\S+ \+(\d+)(?:,(\d+))? @@")


def changed_lines(diff_text, repo):
    """Map each header in the diff (absolute path) to its changed line ranges."""
    ranges = {}
    current = None
    for line in diff_text.splitlines():
        if line.startswith("+++ "):
            name = line[4:].split("\t")[0]
            if name.startswith("b/"):
                name = name[2:]
            current = None
            if name != "/dev/null" and name.endswith(HEADER_EXT):
                current = os.path.normpath(os.path.join(repo, name))
            continue
        m = HUNK.match(line)
        if m and current is not None:
            start = int(m.group(1))
            count = 1 if m.group(2) is None else int(m.group(2))
            if count > 0:  # count 0: lines only removed here
                ranges.setdefault(current, []).append([start, start + count - 1])
    return ranges


def compiled_sources(repo):
    """Every C and C++ source with a compile command, and its directory."""
    with open(os.path.join(repo, "compile_commands.json")) as f:
        db = json.load(f)
    return {os.path.normpath(os.path.join(e["directory"], e["file"])): e["directory"]
            for e in db if e["file"].endswith(SOURCE_EXT)}


def pick_source(header, users):
    """The including source to lint through: C++ first, then same directory."""
    hdir = os.path.dirname(header)
    return sorted(users, key=lambda s: (not s.endswith(CXX_EXT),
                                        os.path.dirname(s) != hdir, s))[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--items", required=True)
    ap.add_argument("--diff", required=True)
    ap.add_argument("--db-incomplete", action="store_true")
    opt = ap.parse_args()
    repo = os.path.abspath(opt.repo)
    items = open(opt.items, "a")

    def item(kind, text):
        items.write("%s\t%s\n" % (kind, text.replace("\n", " ")))

    def rel(p):
        return os.path.relpath(p, repo)

    with open(opt.diff, errors="replace") as f:
        ranges = changed_lines(f.read(), repo)
    if not ranges:
        return 0
    try:
        sources = compiled_sources(repo)
    except (OSError, ValueError) as e:
        item("fail", "cannot read compile_commands.json: %s" % e)
        return 1
    users = header_users(repo, set(sources))
    unchecked = "fail" if opt.db_incomplete else "warn"
    why = " (the gate 1 build failed)" if opt.db_incomplete else ""
    rc = 0
    for header in sorted(ranges):
        tus = users.get(header, set())
        if not tus:
            item(unchecked, "clang-tidy: header %s is not included by any compiled "
                 "file%s, so its changed lines were not linted" % (rel(header), why))
            continue
        tu = pick_source(header, tus)
        # clang-tidy names the header as the source's compile command reaches
        # it (../../tsk/fs/tsk_fs.h from tsk/fs/), so both filters match its
        # repo-relative path as a suffix (-line-filter names always do).
        name = rel(header)
        line_filter = json.dumps([{"name": name, "lines": ranges[header]}])
        cmd = ["clang-tidy", "-p", repo, "--quiet",
               "-line-filter=" + line_filter,
               "-header-filter=(^|/)" + re.escape(name) + "$", tu]
        print("==== %s through %s, lines %s" % (rel(header), rel(tu), ranges[header]))
        sys.stdout.flush()
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           universal_newlines=True)
        # compile_commands.json may list a source twice (libtool's PIC and
        # non-PIC compiles), so clang-tidy can repeat a diagnostic.
        # Paths are relative to the source's compile directory; print them
        # absolute, as clang-tidy-diff's are.
        seen = set()
        for out_line in p.stdout.splitlines():
            m = DIAG.match(out_line)
            if m:
                path = os.path.normpath(os.path.join(sources[tu], m.group(1)))
                out_line = path + out_line[len(m.group(1)):]
                if out_line in seen:
                    continue
                seen.add(out_line)
            print(out_line)
        sys.stdout.flush()
        if p.returncode != 0 and ": error: " not in p.stdout and \
                ": warning: " not in p.stdout:
            item("fail", "clang-tidy failed on %s (exit %d), see the log"
                 % (rel(tu), p.returncode))
            rc = 1
        item("note", "header %s: changed lines linted through %s (one of %d "
             "file(s) that include it)" % (rel(header), rel(tu), len(tus)))
    return rc


if __name__ == "__main__":
    sys.exit(main())
