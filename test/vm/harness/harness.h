/*
 * The harness: writes a Manifest of a file system in a disk image, read
 * through the public libtsk API only (it is built against the installed
 * headers). See test/vm/README.md ("The harness").
 *
 * C++14. Test-only; not installed with the library.
 */
#ifndef TSK_TEST_VM_HARNESS_H
#define TSK_TEST_VM_HARNESS_H

#include "manifest.h"

#include <tsk/libtsk.h>

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace tskval {

struct HarnessOptions {
    std::vector<std::string> images; // one image, or the segments of one
    std::string output;              // Manifest file; empty: standard output
    uint64_t offset = 0;             // file system offset in bytes
    unsigned partition = 0; // 1-based, among allocated partitions; 0: none
    bool has_offset = false;
};

// Exit status of tsk-harness.
enum HarnessExit {
    HARNESS_OK = 0,         // complete Manifest written
    HARNESS_INCOMPLETE = 1, // Manifest written with !error, or entries left
                            // out or without content (see standard error)
    HARNESS_ERROR = 2       // bad usage, or the output could not be written
};

// Parses the command line (args excludes the program name). Returns false
// with a message in `error` on bad usage. Sets `help` for -h/--help.
bool parse_harness_args(const std::vector<std::string> &args,
    HarnessOptions &opts, bool &help, std::string &error);

// The Manifest entry for a file's metadata, without content: has_sha256 is
// false. crtime is recorded only when it is not zero, because libtsk
// reports a missing crtime as 0.
Entry entry_from_meta(const std::string &path, const TSK_FS_META &meta);

// Opens the image read-only, finds the file system (in the given partition,
// at the given offset, or at offset 0), walks every allocated entry from the
// root and hashes every regular file. Writes the Manifest to `out`, or an
// !error Manifest if the image, volume system, partition or file system
// cannot be opened. Problems go to `err`. Returns a HarnessExit.
int run_harness(
    const HarnessOptions &opts, std::ostream &out, std::ostream &err);

// The whole command: arguments, output file, run.
int harness_cli(
    const std::vector<std::string> &args, std::ostream &out, std::ostream &err);

} // namespace tskval

#endif
