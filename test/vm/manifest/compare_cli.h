/*
 * Command line of the comparison step (compare-manifests). See
 * test/vm/README.md ("The comparison step") for the options.
 */
#ifndef TSK_TEST_VM_COMPARE_CLI_H
#define TSK_TEST_VM_COMPARE_CLI_H

#include <cstddef>
#include <iosfwd>
#include <string>
#include <vector>

namespace tskval {

// Exit status of compare-manifests.
enum CompareExit {
    COMPARE_PASS = 0, // no failures (warnings allowed)
    COMPARE_FAIL = 1, // at least one failure
    COMPARE_ERROR = 2 // bad usage or unreadable input
};

struct CompareResult;

// Appends the validate command's report items for a comparison to `out`:
// failures and warnings (at most max_items of each, then a count), notes, and
// a summary note, each "fail|warn|note TAB [label: ]text".
void write_items(std::ostream &out, const CompareResult &r,
    const std::string &label, size_t max_items);

// args excludes the program name. The report goes to `out`, usage and input
// errors to `err`.
int compare_cli(
    const std::vector<std::string> &args, std::ostream &out, std::ostream &err);

} // namespace tskval

#endif
