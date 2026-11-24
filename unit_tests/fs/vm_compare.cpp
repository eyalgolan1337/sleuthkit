/*
 * Compiles test/vm/manifest/compare.cpp into test_fs (see Makefile.am). Included
 * rather than listed in test_fs_SOURCES, which would need automake's
 * subdir-objects and put object files in the source tree.
 */
// NOLINTNEXTLINE(bugprone-suspicious-include): including the .cpp is the point
#include "../../test/vm/manifest/compare.cpp"
