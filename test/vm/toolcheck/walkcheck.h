/*
 * The CLI tool checks of the inode walk and the block walk (test/vm/README.md,
 * "Gate 3: CLI tool checks"): ils, ifind -d and blkls, compared with Ground
 * truth that the guest records in the frozen window with xfs_db and xfs_bmap
 * (XFS) or dumpe2fs and debugfs (ext2/3/4).
 *
 * The guest only runs those tools and keeps their output, in one file of
 * sections (the walk record, see parse_walk_truth); this code reads it, here
 * and in the guest (check-tool plan-inodes, plan-blocks), so that every
 * reading of the tools' output is in one place.
 *
 * C++14; no dependency on libtsk.
 */
#ifndef TSK_TEST_VM_WALKCHECK_H
#define TSK_TEST_VM_WALKCHECK_H

#include "compare.h"
#include "manifest.h"
#include "toolcheck.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace tskval {

// --- The walk record ---
//
// Sections, each starting with a line "== <name> [<arguments>]":
//
// XFS:
//   == xfs_db sb          xfs_db: sb 0, print blocksize agcount agblocks
//                         dblocks agblklog inopblog rbmino rsumino uquotino
//                         gquotino pquotino
//   == xfs_db agi N       xfs_db: agi N, print count level freecount, addr
//                         root, btdump (the AG's inode B+tree)
//   == xfs_db bnobt N     xfs_db: agf N, addr bnoroot, btdump
//   == xfs_bmap INODE PATH
//                         xfs_bmap -v of a spot-check path with that inode
// ext2/3/4:
//   == dumpe2fs           dumpe2fs (with the groups)
//   == debugfs INODE PATH debugfs stat <INODE> of a spot-check path
// Both:
//   == inode-ctime        "<inode> <hex>" per inode of plan-inodes: the raw
//                         ctime field of the on-disk inode (XFS: di_ctime, 64
//                         bits, read by check-tool read-bytes at the offset
//                         xfs_db's stack gives; ext: i_ctime, 32 bits, from
//                         debugfs stat).
//
// Anything before the first section is ignored.

// One ifind -d question: the block, and the inode that owns it (0: none).
// subject names it in the comparison: "<path>@first", "@middle" or "@last"
// for a spot-check path's data blocks in file order, "/free-block" for a
// free block and "/header-block" for the first block of AG (or group) 1.
struct IfindExpectation {
    std::string subject;
    uint64_t block = 0;
    uint64_t inode = 0;
};

// Ground truth of the walk checks, from the walk record.
struct WalkTruth {
    std::string fs_type; // "xfs" or "ext"
    uint64_t block_count = 0;
    // The inodes the inode walk visits: XFS, every inode of every chunk of
    // the AGs' inode B+trees (the sum of agi_count); ext, inodes per group
    // times the number of groups.
    uint64_t inode_count = 0;
    // Allocated inodes no name refers to, with what they are: XFS, the
    // realtime bitmap and summary inodes and the quota inodes the superblock
    // names; ext, the reserved inodes below the first inode that are
    // allocated and in use (a ctime that isn't 0).
    std::vector<std::pair<uint64_t, std::string>> internal_inodes;
    // ext only: libtsk's virtual $OrphanFiles directory, inode count + 1,
    // which ext's inode walk visits as allocated. 0 for XFS.
    uint64_t virtual_inode = 0;
    // Free inodes that were once in use: a raw ctime that isn't 0.
    std::set<uint64_t> used_free;
    std::vector<IfindExpectation> ifind;
    // Facts worth a note in the report, such as each AG's inode B+tree
    // level.
    std::vector<std::string> notes;
};

// The inodes whose raw ctime the guest must read for parse_walk_truth (the
// inode-ctime section), in ascending order: XFS, every free inode of the
// inode B+trees' chunks (not the holes of sparse chunks); ext, the reserved
// inodes below the first inode and every free inode a group has used (below
// its unused-inode tail, or all of the group's free inodes if its inode table
// isn't marked zeroed). Needs only the xfs_db sb and agi sections or
// dumpe2fs.
bool walk_plan_inodes(
    const std::string &record, std::vector<uint64_t> &out, std::string &error);

// The blocks ifind -d must be asked about: those of every expectation of
// parse_walk_truth, ascending, each once. Doesn't need the inode-ctime
// section.
bool walk_plan_blocks(
    const std::string &record, std::vector<uint64_t> &out, std::string &error);

// Reads the whole walk record. Fails on a missing section, a line it can't
// read, a sum that doesn't add up (XFS: the records' inode counts against
// agi_count; ext: inodes per group against the inode count), or an inode of
// plan-inodes without a ctime.
bool parse_walk_truth(
    const std::string &record, WalkTruth &out, std::string &error);

// --- ils ---

// One line of ils's output: the inode and its allocation letter ('a', 'f').
struct IlsLine {
    uint64_t inode = 0;
    char alloc = 'a';
};

// Parses ils's output: its three header lines (class, ils, the field names)
// then one line per inode with 11 '|'-separated fields.
bool parse_ils(
    const std::string &text, std::vector<IlsLine> &out, std::string &error);

// The ils check's Ground truth, a Manifest of path-only entries:
//   /allocated/<inode>   ils -a: the Ground truth Manifest's inodes, the
//                        internal inodes and the virtual inode
//   /used-free/<inode>   ils (no option): the free inodes once in use
//   /inode-count         ils -e: its size is the number of inodes it lists
//                        (inode count, plus the virtual inode)
// `deleted` holds the inodes the Test corpus deleted, one per line; each must
// be among the free inodes once in use, otherwise Ground truth is an !error
// Manifest ("corpus-deleted"), as it is when the walk record can't be read
// ("walk-record").
Manifest ils_expected(const Manifest &ground_truth, const std::string &record,
    const std::string &deleted);

// The run Manifest of the ils check from ils -a, ils -e and the default ils.
// A failed ils makes it an !error Manifest (set_tool_error), unparseable
// output one with "tool-output".
Manifest ils_run(const ToolOutput &allocated, const ToolOutput &all,
    const ToolOutput &used_free);

// --- ifind -d ---

// The ifind check's Ground truth: one entry per expectation, its path the
// subject, its inode the owner (0: none) and its size the block. An unread
// walk record makes it an !error Manifest ("walk-record").
Manifest ifind_expected(const std::string &record);

// The run Manifest of the ifind check: for each entry of the expected
// Manifest, ifind -d's answer for its block (outputs by block). "Inode not
// found" and "Meta Data" are both no inode (0). A block without output, or
// whose ifind failed, leaves its entries out (they are missing); a failure to
// recognise the file system makes the whole Manifest an !error one
// (set_tool_error), output it can't read "tool-output".
Manifest ifind_run(
    const Manifest &expected, const std::map<uint64_t, ToolOutput> &outputs);

// --- blkls ---

// The blkls check's Ground truth: the summary field block-count. An unread
// walk record makes it an !error summary ("walk-record").
Summary blkls_expected(const std::string &record);

// The run summary of blkls -e -l. Its output is kept short (run-tools.sh):
// `t.out` holds its first three lines (the header) and its last line, and
// `lines` the number of lines it printed. block-count is lines - 3.
Summary blkls_run(const ToolOutput &t, const std::string &lines);

} // namespace tskval

#endif
