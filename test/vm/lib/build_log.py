#!/usr/bin/env python3
"""Extract compiler diagnostics from a recursive make log.

Usage: build_log.py --log FILE --repo DIR --items FILE CHANGED_FILE...

libtool and make report diagnostics relative to the directory being built,
so the current directory is tracked from make's "Entering directory" lines.
Writes report items ("fail|warn|note<TAB>text"):
  - every error, as a failure;
  - every warning located in a changed file, as a warning;
  - a note with the number of warnings elsewhere (pre-existing upstream code).
"""

import argparse
import os
import re
import sys

ENTER = re.compile(r"^make(?:\[\d+\])?: Entering directory '(.+)'")
DIAG = re.compile(r"^(.+?):(\d+):(?:(\d+):)? (fatal error|error|warning): (.*)$")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True)
    ap.add_argument("--repo", required=True)
    ap.add_argument("--items", required=True)
    ap.add_argument("changed", nargs="*")
    opt = ap.parse_args()
    repo = os.path.abspath(opt.repo)
    changed = {os.path.normpath(os.path.join(repo, f)) for f in opt.changed}

    def rel(p):
        r = os.path.relpath(p, repo)
        return p if r.startswith("..") else r

    cwd = repo
    seen = set()
    errors, other = [], 0
    changed_warnings = []
    with open(opt.log, errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            m = ENTER.match(line)
            if m:
                cwd = m.group(1)
                continue
            m = DIAG.match(line)
            if not m:
                continue
            path = os.path.normpath(os.path.join(cwd, m.group(1)))
            key = (path, m.group(2), m.group(5))
            if key in seen:  # headers repeat once per including file
                continue
            seen.add(key)
            text = "%s:%s: %s: %s" % (rel(path), m.group(2), m.group(4), m.group(5))
            if m.group(4) != "warning":
                errors.append(text)
            elif path in changed:
                changed_warnings.append(text)
            else:
                other += 1

    with open(opt.items, "a") as items:
        for e in errors:
            items.write("fail\t%s\n" % e)
        for w in changed_warnings:
            items.write("warn\t%s\n" % w)
        items.write("note\t%d compiler warning(s) in changed files, %d in unchanged files "
                    "(pre-existing; listed in the log)\n" % (len(changed_warnings), other))
    return 0


if __name__ == "__main__":
    sys.exit(main())
