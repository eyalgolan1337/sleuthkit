/*
 * Ground truth capture: walks a mounted directory tree through the kernel
 * (no libtsk) and records it as Manifest entries, with nanosecond
 * timestamps and crtime where statx reports a birth time.
 * See test/vm/README.md ("Ground truth capture").
 *
 * C++14, Linux. Needs only the C++ standard library and glibc, so it links
 * statically into the guest build.
 */
#ifndef TSK_TEST_VM_CAPTURE_H
#define TSK_TEST_VM_CAPTURE_H

#include "manifest.h"

#include <iosfwd>
#include <string>
#include <vector>

namespace tskval {

// Records the tree under the directory `root` as one Entry per path, the
// root itself as "/". Never follows symlinks below the root. Stays on the
// root's file system: a directory on another device (a mount point) is
// recorded as stat sees it, but not entered.
//
// Returns false, with a message in `problems`, if the root cannot be read.
// Otherwise returns true and appends a message to `problems` for each entry
// it could not record fully (for example, an unreadable file is recorded
// without its SHA-256). Entries are in walk order: each directory's names
// sorted bytewise, a directory before its contents.
bool capture_tree(const std::string &root, std::vector<Entry> &entries,
    std::vector<std::string> &problems);

// Exit status of capture-ground-truth.
enum CaptureExit {
    CAPTURE_OK = 0,         // complete Manifest written
    CAPTURE_INCOMPLETE = 1, // Manifest written, some entries incomplete
    CAPTURE_ERROR = 2       // bad usage, root unreadable or output failed
};

// capture-ground-truth [-o FILE] DIR. args excludes the program name. The
// Manifest goes to FILE, or to `out` without -o; problems and usage errors
// go to `err`.
int capture_cli(
    const std::vector<std::string> &args, std::ostream &out, std::ostream &err);

} // namespace tskval

#endif
