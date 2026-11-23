/*
 * The CLI tool checks of the Strict checks (test/vm/README.md, "Gate 3: CLI
 * tool checks"): TSK's command-line tools fls, icat, fsstat and istat as
 * consumers of the API, their output parsed as upstream prints it and turned
 * into Manifests or file-system summaries for the comparison step.
 *
 * C++14; no dependency on libtsk: the tools run in the guest or in a one-off
 * container, and only their output reaches this code.
 */
#ifndef TSK_TEST_VM_TOOLCHECK_H
#define TSK_TEST_VM_TOOLCHECK_H

#include "compare.h"
#include "manifest.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace tskval {

// What one run of a tool left: its exit status, standard output and standard
// error.
struct ToolOutput {
    int status = 0;
    std::string out;
    std::string err;
};

// The error code of a Manifest for a tool that failed as a whole: "fs-open"
// when libtsk didn't recognise the file system (standard error starts with
// one of its autodetection errors: "Cannot determine file system type",
// "Possible encryption detected", "Encryption detected" or "Multiple file
// system types detected"; the harness's code for the same failure),
// "tool-failed" otherwise. The detail is the first line of standard error.
void set_tool_error(Manifest &m, const ToolOutput &t);

// --- fls -r -p ---

// A name as fls prints it: each control character (0x00-0x1F, 0x7F) becomes
// '^' (tsk_print_sanitized). Other bytes are printed as they are.
std::string fls_sanitize(const std::string &raw);

// Parses the output of `fls -r -p` into a Manifest with each entry's path
// and type only. A type is a file type when fls's name type and metadata
// type agree (r/r, d/d, l/l, c/c, b/b, p/p, s/s), Other otherwise. libtsk's
// virtual entries (V/V, e.g. $OrphanFiles) and everything below them are left
// out, as the harness leaves them out. Fails on a line it can't parse.
bool parse_fls(const std::string &text, Manifest &out, std::string &error);

// The run Manifest of one fls run: parse_fls of its output, or an !error
// Manifest if fls failed (set_tool_error) or its output didn't parse
// ("tool-output").
Manifest fls_run(const ToolOutput &t);

// What fls should list for a Ground truth Manifest: every entry but the root
// directory, its path as fls prints it (fls_sanitize) and its type; the other
// fields stay at their defaults, so the comparison sees only paths and types.
Manifest fls_expected(const Manifest &ground_truth);

// --- icat ---

// What icat should read: each regular file of Ground truth with its path,
// type and SHA-256 only. run-tools.sh runs icat once per regular file inode
// (hard links share one).
Manifest icat_expected(const Manifest &ground_truth);

// The run Manifest of the icat check. `probe` is icat of the root directory,
// which shows whether icat can open the file system at all: if it failed,
// the Manifest is an !error one (set_tool_error). `results` holds one line
// per icat run, "<inode> TAB <exit status> TAB <sha256 or -> TAB <first line
// of standard error>" (test/vm/toolcheck/run-tools.sh writes it). Each
// regular file of Ground truth gets the SHA-256 of its inode's run, or none
// if that run failed or is missing. Malformed results: "tool-output".
Manifest icat_run(const Manifest &ground_truth, const ToolOutput &probe,
    const std::string &results);

// --- istat ---

// The istat check's Ground truth: the guest's stat records of the spot-check
// paths, one line per path (tsk-validate.sh capture-tools writes them):
//   <path> TAB <inode> TAB <mode, hex %f> TAB <uid> TAB <gid> TAB <size> TAB
//   <links> TAB <mtime %y> TAB <ctime %z> TAB <birth %w> TAB <target or ->
// with times as stat prints them in UTC ("2021-02-03 04:05:06.123456789
// +0000"; a birth time of "-" means none), the path encoded as in a Manifest
// and the symlink target as readlink printed it. A path stat couldn't find is
// "<path> TAB !missing TAB <error>". The Manifest has every compared field
// but sha256; a missing path makes it an !error Manifest ("spot-check"), a
// malformed line too ("stat-output").
Manifest istat_expected(const std::string &stat_records);

// Parses one istat output into the fields it shares with stat: inode, type
// and mode (from the ls-style mode string), uid, gid, size, link count,
// symlink target, and mtime, ctime and crtime ("File Modified", "Inode
// Modified", "File Created"), which must be printed in UTC (TZ=UTC). istat
// prints a time at or before the epoch as 0000-00-00 00:00:00; that is read
// as 0, and a crtime of 0 or no "File Created" line as no crtime.
bool parse_istat(const std::string &text, Entry &out, std::string &error);

// The run Manifest of the istat check: for each spot-check entry of the
// expected Manifest, istat's output for its inode (by inode number). An
// inode without output, or whose istat failed, is left out (so it is
// "missing"); if any istat failed because the file system wasn't recognised,
// the Manifest is an !error one (set_tool_error), and output that doesn't
// parse makes it "tool-output".
Manifest istat_run(
    const Manifest &expected, const std::map<uint64_t, ToolOutput> &outputs);

// --- fsstat ---

// The fsstat check's Ground truth: the guest's own summary of the file
// system, from what tsk-validate.sh capture-tools writes: "== blkid" and the
// output of blkid -p -o export (TYPE, UUID, LABEL), then "== dumpe2fs" and
// dumpe2fs -h for ext2/3/4, or "== xfs_info" and xfs_info for XFS. Fields
// (SUMMARY_FIELDS): fs-type (blkid's TYPE), block-size, inode-size,
// group-count (ext: block groups, XFS: allocation groups), uuid, label (empty
// if none) and version (ext: the revision level, XFS: 4 or 5). Anything it
// can't read makes an !error summary ("summary").
Summary guest_summary(const std::string &raw);

// The run summary of fsstat's output, in the same fields. It reads the ext2/3/4
// output of release 4.15.0; another file system type, or a missing line, is
// "tool-output" until this check learns its output. A failed fsstat is an
// !error summary (set_tool_error's codes). ext fsstat prints the UUID as two
// 64-bit hex numbers without leading zeros ("Volume ID"); it is read as the
// expected UUID if it is exactly how that one prints, as a UUID if it has
// all 32 digits, and as "Volume ID <digits>" otherwise.
Summary fsstat_run(const ToolOutput &t, const Summary &expected);

} // namespace tskval

#endif
