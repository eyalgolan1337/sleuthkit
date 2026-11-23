#!/usr/bin/env python3
"""Syntax-only compile of changed C++ files under a newer C++ standard.

Usage: cxx_std_check.py --std c++17 --repo DIR --items FILE [--jobs N]
                        [--db-incomplete] FILE...

Each changed C++ source is compiled with its own command from
compile_commands.json, changed to -std=<std> -fsyntax-only plus flags that
turn C++14-only constructs into errors (see STRICT_FLAGS). A changed header
is checked through every compiled C++ source whose automake dependency file
(.deps/*.Po, *.Plo) lists it. Other files are ignored. A changed file that
can't be checked is a warning, or a failure with --db-incomplete (the build
failed, so compile_commands.json can't tell "not built" from "build broke").

Findings go to the items file as "fail|warn|note<TAB>text" lines (the
validate command's report protocol); full compiler output goes to stdout.
Exit status: 0 when every compile succeeded, 1 otherwise.
"""

import argparse
import concurrent.futures
import glob
import json
import os
import re
import shlex
import subprocess
import sys

# GCC accepts these C++14-only constructs under -std=c++17/c++20 with only a
# warning, while MSVC and the standard reject them:
#   -Werror=register             the removed `register` storage class
#   _GLIBCXX_USE_DEPRECATED=0    libstdc++ drops facilities removed from the
#                                standard (auto_ptr, bind1st, random_shuffle, ...)
STRICT_FLAGS = ["-fsyntax-only", "-Werror=register", "-D_GLIBCXX_USE_DEPRECATED=0"]

CXX_EXT = (".cpp", ".cc", ".cxx", ".c++", ".C")
HEADER_EXT = (".h", ".hh", ".hpp", ".hxx", ".inc")
DIAG = re.compile(r"^(.+?):(\d+):(?:(\d+):)? (fatal error|error|warning): (.*)$")


def syntax_args(args, std):
    out, skip = [], False
    for a in args:
        if skip:
            skip = False
            continue
        if a in ("-o", "-MF", "-MT", "-MQ"):
            skip = True
            continue
        if a in ("-c", "-MD", "-MMD", "-MP") or a.startswith("-std="):
            continue
        out.append(a)
    return out[:1] + ["-std=" + std] + STRICT_FLAGS + out[1:]


def load_db(repo):
    path = os.path.join(repo, "compile_commands.json")
    with open(path) as f:
        db = json.load(f)
    entries = {}
    for e in db:
        src = os.path.normpath(os.path.join(e["directory"], e["file"]))
        if src.endswith(CXX_EXT) and src not in entries:
            if "arguments" not in e:
                e["arguments"] = shlex.split(e["command"])
            entries[src] = e
    return entries


def header_users(repo, cxx_sources):
    """Map each header (absolute path) to the C++ sources that depend on it."""
    users = {}
    for dep in glob.glob(os.path.join(repo, "**", ".deps", "*.P*o"), recursive=True):
        base = os.path.dirname(os.path.dirname(dep))
        try:
            with open(dep, errors="replace") as f:
                words = f.read().replace("\\\n", " ").split()
        except OSError:
            continue
        paths = {os.path.normpath(os.path.join(base, w))
                 for w in words if not w.endswith(":")}
        srcs = paths & cxx_sources
        for p in paths:
            if p.endswith(HEADER_EXT):
                users.setdefault(p, set()).update(srcs)
    return users


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--std", required=True)
    ap.add_argument("--repo", required=True)
    ap.add_argument("--items", required=True)
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    ap.add_argument("--db-incomplete", action="store_true")
    ap.add_argument("files", nargs="*")
    opt = ap.parse_args()
    repo = os.path.abspath(opt.repo)
    items = open(opt.items, "a")
    std = opt.std

    def item(kind, text):
        items.write("%s\t%s\n" % (kind, text.replace("\n", " ")))

    unchecked = "fail" if opt.db_incomplete else "warn"
    why = " (the gate 1 build failed)" if opt.db_incomplete else ""

    def rel(p):
        r = os.path.relpath(p, repo)
        return p if r.startswith("..") else r

    changed = [os.path.normpath(os.path.join(repo, f)) for f in opt.files]
    sources = [f for f in changed if f.endswith(CXX_EXT)]
    headers = [f for f in changed if f.endswith(HEADER_EXT)]
    if not sources and not headers:
        item("note", "no changed C++ sources or headers")
        return 0

    try:
        db = load_db(repo)
    except (OSError, ValueError) as e:
        item("fail", "cannot read compile_commands.json: %s" % e)
        return 1

    targets = set()
    for s in sources:
        if s in db:
            targets.add(s)
        else:
            item(unchecked, "%s has no compile command in compile_commands.json%s, "
                 "so it was not checked" % (rel(s), why or " (not built in this configuration)"))
    if headers:
        users = header_users(repo, set(db))
        for h in headers:
            tus = users.get(h, set())
            if tus:
                targets |= tus
                item("note", "header %s checked through %d C++ file(s) that include it"
                     % (rel(h), len(tus)))
            else:
                item(unchecked, "header %s is not included by any compiled C++ file%s, "
                     "so it was not checked" % (rel(h), why))
    if not targets:
        return 0

    def compile_one(src):
        e = db[src]
        p = subprocess.run(syntax_args(e["arguments"], std), cwd=e["directory"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           universal_newlines=True)
        return src, e["directory"], p.returncode, p.stdout

    changed_set = set(changed)
    failed = 0
    seen_warnings = set()
    other_warnings = 0
    with concurrent.futures.ThreadPoolExecutor(max(1, opt.jobs)) as ex:
        results = sorted(ex.map(compile_one, sorted(targets)))
    for src, directory, rc, out in results:
        print("==== -std=%s %s (exit %d)" % (std, rel(src), rc))
        print(out, end="")
        errors = 0
        for line in out.splitlines():
            m = DIAG.match(line)
            if not m:
                continue
            path = rel(os.path.normpath(os.path.join(directory, m.group(1))))
            where = "%s:%s" % (path, m.group(2))
            kind, msg = m.group(4), m.group(5)
            if kind != "warning":
                errors += 1
                if errors <= 10:
                    ctx = "" if path == rel(src) else " (compiling %s)" % rel(src)
                    item("fail", "-std=%s: %s: %s: %s%s" % (std, where, kind, msg, ctx))
            elif os.path.join(repo, path) in changed_set:
                if (where, msg) not in seen_warnings:
                    seen_warnings.add((where, msg))
                    item("warn", "-std=%s: %s: warning: %s" % (std, where, msg))
            else:
                other_warnings += 1
        if rc != 0:
            failed += 1
            if errors == 0:
                item("fail", "-std=%s: %s does not compile (exit %d), see the log"
                     % (std, rel(src), rc))
            elif errors > 10:
                item("fail", "-std=%s: %s: %d more errors, see the log"
                     % (std, rel(src), errors - 10))
    item("note", "compiled %d C++ file(s) syntax-only with -std=%s %s: %d failed"
         % (len(targets), std, " ".join(STRICT_FLAGS), failed))
    if other_warnings:
        item("note", "%d warning(s) in unchanged files, see the log" % other_warnings)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
