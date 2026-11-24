/*
 * Where the fs unit tests find their data. make check makes it before the
 * tests run (Makefile.am, check_DATA), in the build directory, which the
 * Makefile passes as FS_TEST_DATA_DIR; so test_fs runs from any directory.
 */
#ifndef TSK_UNIT_TESTS_FS_TEST_DATA_H
#define TSK_UNIT_TESTS_FS_TEST_DATA_H

#include <string>

#ifndef FS_TEST_DATA_DIR
#define FS_TEST_DATA_DIR "."
#endif

namespace fstest {

// A file in the test data directory, e.g. data_path("xfs/v4.img").
inline std::string
data_path(const std::string &name)
{
    return std::string(FS_TEST_DATA_DIR) + "/" + name;
}

} // namespace fstest

#endif
