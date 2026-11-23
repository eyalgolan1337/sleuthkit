/*
 * Command line of the CLI tool checks (check-tool). See test/vm/README.md
 * ("Gate 3: CLI tool checks") for the options and the files it reads.
 */
#ifndef TSK_TEST_VM_TOOLCHECK_CLI_H
#define TSK_TEST_VM_TOOLCHECK_CLI_H

#include <iosfwd>
#include <string>
#include <vector>

namespace tskval {

// args excludes the program name. The report goes to `out`, usage and input
// errors to `err`. Exit status as compare-manifests's (CompareExit): 0 pass,
// 1 failures, 2 bad usage or unreadable input.
int check_tool_cli(
    const std::vector<std::string> &args, std::ostream &out, std::ostream &err);

} // namespace tskval

#endif
