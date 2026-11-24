/*
 * The Manifest-comparison helper (manifest_check.h), on something libtsk can
 * already read: the ext4 image that make-ext-image.sh builds at make check
 * time. EXT_SMALL is its expected Manifest, written from that script's recipe:
 * mkfs.ext4 with E2FSPROGS_FAKE_TIME=1700000000 stamps the root and
 * /lost+found, mkfs.ext4 allocates inodes 2, 11, 12 and 13 and sizes the
 * directories in 1 KiB blocks (12 for /lost+found), and debugfs sets the
 * owners, modes and times of /sub and /sub/hello.txt. The SHA-256 is that of
 * "hello\n".
 */
#include "fs_test_data.h"
#include "manifest_check.h"

#include <cppunit/extensions/HelperMacros.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

const char EXT_SMALL[] =
    "#tsk-manifest v1\n"
    "/\tdir\t2\t0755\t0\t0\t1024\t4\t-\t-\t1700000000000000000\t"
    "1700000000000000000\t1700000000000000000\t1700000000000000000\n"
    "/lost+found\tdir\t11\t0700\t0\t0\t12288\t2\t-\t-\t1700000000000000000\t"
    "1700000000000000000\t1700000000000000000\t1700000000000000000\n"
    "/sub\tdir\t12\t0750\t1000\t100\t1024\t2\t-\t-\t1500000001000000000\t"
    "1500000002000000000\t1500000003000000000\t1500000000000000000\n"
    "/sub/hello.txt\tfile\t13\t0640\t1001\t100\t6\t1\t-\t"
    "5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03\t"
    "1600000001000000000\t1600000002000000000\t1600000003000000000\t"
    "1600000000000000000\n";

std::string
ext_image()
{
    return fstest::data_path("ext-small.img");
}

tskval::Manifest
ext_expected()
{
    std::istringstream in(EXT_SMALL);
    tskval::Manifest m;
    std::string error;
    if (!tskval::read_manifest(in, m, error)) {
        CPPUNIT_FAIL("EXT_SMALL: " + error);
    }
    return m;
}

class ManifestCheckTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(ManifestCheckTest);
    CPPUNIT_TEST(testMatchingWalkPasses);
    CPPUNIT_TEST(testWrongFieldFailsNamingEntryAndField);
    CPPUNIT_TEST(testEntryMissingFromWalkFails);
    CPPUNIT_TEST(testEntryOnlyInWalkFails);
    CPPUNIT_TEST(testKnownFailureToleratesItsDifference);
    CPPUNIT_TEST(testKnownFailureThatNoLongerOccursFails);
    CPPUNIT_TEST(testMalformedKnownFailureFails);
    CPPUNIT_TEST(testExpectedManifestFromFile);
    CPPUNIT_TEST(testUnreadableExpectedManifestFails);
    CPPUNIT_TEST(testUnopenableImageFails);
    CPPUNIT_TEST_SUITE_END();

  public:
    void testMatchingWalkPasses()
    {
        const fstest::WalkCheck r =
            fstest::check_walk(ext_image(), ext_expected());
        CPPUNIT_ASSERT_MESSAGE(r.report, r.passed);
        CPPUNIT_ASSERT_EQUAL(size_t(0), r.result.failures);
        CPPUNIT_ASSERT_EQUAL(size_t(4), r.result.ground_truth_entries);
        CPPUNIT_ASSERT_EQUAL(size_t(4), r.result.run_entries);
    }

    void testWrongFieldFailsNamingEntryAndField()
    {
        tskval::Manifest expected = ext_expected();
        find(expected, "/sub/hello.txt").size = 7;
        const fstest::WalkCheck r = fstest::check_walk(ext_image(), expected);
        CPPUNIT_ASSERT_MESSAGE(r.report, !r.passed);
        assert_single_failure(r, "/sub/hello.txt", "size");
        assert_contains(
            r.report, "/sub/hello.txt: size differs: Ground truth 7, run 6");
    }

    void testEntryMissingFromWalkFails()
    {
        tskval::Manifest expected = ext_expected();
        tskval::Entry extra = find(expected, "/sub/hello.txt");
        extra.path = "/sub/not here";
        expected.entries.push_back(extra);
        const fstest::WalkCheck r = fstest::check_walk(ext_image(), expected);
        CPPUNIT_ASSERT_MESSAGE(r.report, !r.passed);
        assert_single_failure(r, "/sub/not here", "missing");
        assert_contains(
            r.report, "/sub/not%20here: in Ground truth, missing from the run");
    }

    void testEntryOnlyInWalkFails()
    {
        tskval::Manifest expected = ext_expected();
        expected.entries.erase(expected.entries.begin() + 1); // /lost+found
        const fstest::WalkCheck r = fstest::check_walk(ext_image(), expected);
        CPPUNIT_ASSERT_MESSAGE(r.report, !r.passed);
        assert_single_failure(r, "/lost+found", "extra");
        assert_contains(
            r.report, "/lost+found: in the run, not in Ground truth");
    }

    void testKnownFailureToleratesItsDifference()
    {
        tskval::Manifest expected = ext_expected();
        find(expected, "/sub/hello.txt").size = 7;
        find(expected, "/sub").mode = 0700;
        const fstest::WalkCheck r = fstest::check_walk(ext_image(), expected,
            "# tolerated until the ticket that fixes it\n"
            "\n"
            "/sub/hello.txt size a comment\n"
            "/sub\tmode\n");
        CPPUNIT_ASSERT_MESSAGE(r.report, r.passed);
        CPPUNIT_ASSERT_EQUAL_MESSAGE(r.report, size_t(0), r.result.failures);
        CPPUNIT_ASSERT_EQUAL_MESSAGE(r.report, size_t(2), r.result.known);
        assert_contains(r.report, "/sub/hello.txt: size differs");
    }

    void testKnownFailureThatNoLongerOccursFails()
    {
        const fstest::WalkCheck r = fstest::check_walk(
            ext_image(), ext_expected(), "/sub/hello.txt size\n");
        CPPUNIT_ASSERT_MESSAGE(r.report, !r.passed);
        CPPUNIT_ASSERT_EQUAL_MESSAGE(r.report, size_t(1), r.result.failures);
        assert_contains(r.report, "no longer occurs");
    }

    void testMalformedKnownFailureFails()
    {
        const fstest::WalkCheck r =
            fstest::check_walk(ext_image(), ext_expected(), "/sub/hello.txt\n");
        CPPUNIT_ASSERT_MESSAGE(r.report, !r.passed);
        assert_contains(r.report, "Known failure");
        assert_contains(r.report, "line 1");
    }

    void testExpectedManifestFromFile()
    {
        const std::string path =
            fstest::data_path("manifest_check_test.manifest");
        {
            std::ofstream out(path, std::ios::binary);
            out << EXT_SMALL;
        }
        const fstest::WalkCheck matching =
            fstest::check_walk(ext_image(), path);
        {
            std::ofstream out(path, std::ios::binary);
            std::string wrong = EXT_SMALL;
            wrong.replace(wrong.find("\t0640\t"), 6, "\t0644\t");
            out << wrong;
        }
        const fstest::WalkCheck wrong = fstest::check_walk(ext_image(), path);
        std::remove(path.c_str());

        CPPUNIT_ASSERT_MESSAGE(matching.report, matching.passed);
        CPPUNIT_ASSERT_MESSAGE(wrong.report, !wrong.passed);
        assert_single_failure(wrong, "/sub/hello.txt", "mode");
    }

    void testUnreadableExpectedManifestFails()
    {
        const std::string path = fstest::data_path("no-such.manifest");
        const fstest::WalkCheck r = fstest::check_walk(ext_image(), path);
        CPPUNIT_ASSERT_MESSAGE(r.report, !r.passed);
        assert_contains(r.report, path);
    }

    void testUnopenableImageFails()
    {
        const fstest::WalkCheck r = fstest::check_walk(
            fstest::data_path("no-such.img"), ext_expected());
        CPPUNIT_ASSERT_MESSAGE(r.report, !r.passed);
        assert_contains(r.report, "img-open");
    }

  private:
    static tskval::Entry &find(tskval::Manifest &m, const std::string &path)
    {
        for (tskval::Entry &e : m.entries) {
            if (e.path == path) {
                return e;
            }
        }
        CPPUNIT_FAIL("no entry " + path); // throws
        return m.entries.front();         // not reached
    }

    static void assert_single_failure(const fstest::WalkCheck &r,
        const std::string &path, const std::string &difference)
    {
        CPPUNIT_ASSERT_EQUAL_MESSAGE(r.report, size_t(1), r.result.failures);
        for (const tskval::Finding &f : r.result.findings) {
            if (f.severity == tskval::Severity::Failure) {
                CPPUNIT_ASSERT_EQUAL_MESSAGE(r.report, path, f.path);
                CPPUNIT_ASSERT_EQUAL_MESSAGE(
                    r.report, difference, f.difference);
            }
        }
    }

    static void assert_contains(
        const std::string &text, const std::string &part)
    {
        CPPUNIT_ASSERT_MESSAGE("expected \"" + part + "\" in:\n" + text,
            text.find(part) != std::string::npos);
    }
};

CPPUNIT_TEST_SUITE_REGISTRATION(ManifestCheckTest);

} // namespace
