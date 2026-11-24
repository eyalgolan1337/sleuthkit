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
 * C++14. Test-only.
 */
#ifndef TSK_UNIT_TESTS_FS_MANIFEST_CHECK_H
#define TSK_UNIT_TESTS_FS_MANIFEST_CHECK_H

#include "compare.h"
#include "manifest.h"

#include <string>

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

} // namespace fstest

#endif
