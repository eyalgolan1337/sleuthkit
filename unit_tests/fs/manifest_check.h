/*
 * The Manifest-comparison helper of the fs unit tests: walks the file system
 * in an image through the public tsk_fs_* API and compares the result with
 * the image's expected Manifest, entry by entry.
 *
 * The walk is the validation setup's harness (test/vm/harness/harness.h:
 * tsk_img_open_utf8, tsk_fs_open_img with TSK_FS_TYPE_DETECT, a walk of every
 * allocated name from the root, SHA-256 of every regular file's content). The
 * comparison is its comparison step under the Strict rules
 * (test/vm/manifest/compare.h): entries are matched by path, an entry on one
 * side only is "missing" or "extra", and each compared field that differs is
 * one failure named after the field. atime is never compared. See
 * test/vm/README.md ("Manifests and the comparison step").
 *
 * check_by_inode does the same comparison without the directory walk: it
 * opens each expected entry by its inode number. check_meta_walk does it
 * with the inode walk (tsk_fs_meta_walk) instead.
 *
 * C++14. Test-only.
 */
#ifndef TSK_UNIT_TESTS_FS_MANIFEST_CHECK_H
#define TSK_UNIT_TESTS_FS_MANIFEST_CHECK_H

#include "compare.h"
#include "manifest.h"

#include <cstdint>
#include <string>
#include <vector>

namespace fstest {

struct WalkCheck {
    bool passed = false; // nothing failed
    // Readable report: one line per finding (FAIL, KNOWN or NOTE, the encoded
    // path, the difference and both values; "Ground truth" is the expected
    // Manifest), then a result line, then whatever the walk reported.
    std::string report;
    tskval::CompareResult result; // the findings themselves
};

// Walks the file system in `image` (the image is the file system, as the
// Fixture images are) and compares it with `expected`.
//
// `known_failures` tolerates differences that a test expects, one per line,
// in the Known failure list's format without its check and partition
// columns: "<subject> <difference> [comment]", where subject is an encoded
// path or "*" (for "* error:<code>", a walk that failed as a whole), and
// difference is "missing", "extra" or a field name. Blank lines and lines
// starting with '#' are skipped. A tolerated difference is reported as KNOWN;
// a line that matches no difference fails ("no longer occurs").
//
// Fails, saying why, if the image or its file system can't be read (the walk
// is then an !error Manifest), or if `known_failures` is malformed.
WalkCheck check_walk(const std::string &image, const tskval::Manifest &expected,
    const std::string &known_failures = "");

// The same, with the expected Manifest read from a file, such as a Fixture
// image's.
WalkCheck check_walk(const std::string &image,
    const std::string &expected_manifest,
    const std::string &known_failures = "");

// Instead of walking, opens each entry of `expected` by its inode number
// (tsk_fs_file_open_meta on the file system that autodetection finds in
// `image`), records its metadata as the walk does (the harness's
// entry_from_meta, keeping the expected path) and the SHA-256 of each regular
// file's content (tsk_fs_file_read), and compares the result with `expected`
// in the same way. An entry that doesn't open, or whose content can't be read
// to the end, is reported and left out (so it is "missing") or left without
// its SHA-256. Hard links are opened once per path.
WalkCheck check_by_inode(const std::string &image,
    const tskval::Manifest &expected, const std::string &known_failures = "");

// The result of check_meta_walk: the comparison, and every inode number the
// walk visited, in the order it visited them.
struct MetaWalkCheck : WalkCheck {
    std::vector<uint64_t> visited;
};

// Walks the allocated inodes of the file system that autodetection finds in
// `image` (tsk_fs_meta_walk from first_inum to last_inum with
// TSK_FS_META_FLAG_ALLOC), records each visited inode as the walk does
// (entry_from_meta, under each path that `expected` gives the inode, and the
// SHA-256 of each regular file's content) and compares the result with
// `expected` in the same way. An inode is recorded at its first visit only;
// `visited` shows repeats, and the inodes that `expected` doesn't have.
// Fails, saying why, if the walk fails.
MetaWalkCheck check_meta_walk(const std::string &image,
    const tskval::Manifest &expected, const std::string &known_failures = "");

} // namespace fstest

#endif
