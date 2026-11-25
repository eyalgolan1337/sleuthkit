/*
 * Compiles test/vm/toolcheck/toolcheck.cpp (the CLI tool checks' parsers,
 * such as parse_istat) into test_fs. See vm_harness.cpp for why it is
 * included rather than listed in test_fs_SOURCES.
 */
// NOLINTNEXTLINE(bugprone-suspicious-include): including the .cpp is the point
#include "../../test/vm/toolcheck/toolcheck.cpp"
