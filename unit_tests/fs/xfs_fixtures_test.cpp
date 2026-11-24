/*
 * The XFS Fixture images (test/vm/fixtures/xfs/, unpacked by make check into
 * xfs/), read through the public API. Until XFS support exists, each image
 * opens as an image and file system autodetection rejects it with a TSK
 * error; the XFS tickets of #11 replace these expectations as they go.
 */
#include "fs_test_data.h"
#include "manifest_check.h"

#include "manifest.h"

#include <tsk/libtsk.h>

#include <cppunit/extensions/HelperMacros.h>

#include <fstream>
#include <string>

namespace {

class XfsFixturesTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(XfsFixturesTest);
    CPPUNIT_TEST(testV4);
    CPPUNIT_TEST(testV4Blocks512);
    CPPUNIT_TEST(testV5Blocks1k);
    CPPUNIT_TEST(testV5Blocks64k);
    CPPUNIT_TEST(testV5);
    CPPUNIT_TEST_SUITE_END();

  public:
    void testV4()
    {
        check_fixture("v4", 1006);
    }

    void testV4Blocks512()
    {
        check_fixture("v4-512", 1006);
    }

    void testV5Blocks1k()
    {
        check_fixture("v5-1k", 1181);
    }

    void testV5Blocks64k()
    {
        check_fixture("v5-64k", 18821);
    }

    void testV5()
    {
        check_fixture("v5", 1181);
    }

  private:
    // The image opens as a raw image, autodetection fails with a TSK error,
    // and the expected Manifest is there for the walk tests, with the entry
    // count that test/vm/README.md ("XFS fixture images") documents. The walk
    // fails as a whole: no file system is recognised (fs-open).
    static void check_fixture(
        const std::string &variant, size_t expected_entries)
    {
        const std::string image = fstest::data_path("xfs/" + variant + ".img");
        const std::string manifest_path =
            fstest::data_path("xfs/" + variant + ".manifest");
        const char *paths[] = {image.c_str()};

        tsk_error_reset();
        TSK_IMG_INFO *img = tsk_img_open_utf8(1, paths, TSK_IMG_TYPE_DETECT, 0);
        CPPUNIT_ASSERT_MESSAGE(variant + ": tsk_img_open_utf8 failed on " +
                image + ": " + error_text(),
            img != nullptr);
        const TSK_IMG_TYPE_ENUM itype = img->itype;
        const TSK_OFF_T size = img->size;
        TSK_FS_INFO *fs = tsk_fs_open_img(img, 0, TSK_FS_TYPE_DETECT);
        const uint32_t err = tsk_error_get_errno();
        const std::string text = error_text();
        if (fs != nullptr) {
            tsk_fs_close(fs);
        }
        tsk_img_close(img);

        CPPUNIT_ASSERT_EQUAL_MESSAGE(variant + ": image type",
            static_cast<int>(TSK_IMG_TYPE_RAW), static_cast<int>(itype));
        CPPUNIT_ASSERT_MESSAGE(variant + ": empty image", size > 0);
        CPPUNIT_ASSERT_MESSAGE(
            variant + ": autodetection recognised a file system",
            fs == nullptr);
        CPPUNIT_ASSERT_MESSAGE(
            variant + ": autodetection failed without a TSK error",
            err != 0 && !text.empty());

        std::ifstream in(manifest_path, std::ios::binary);
        CPPUNIT_ASSERT_MESSAGE(variant + ": no unpacked Manifest", in.good());
        tskval::Manifest manifest;
        std::string error;
        const bool read = tskval::read_manifest(in, manifest, error);
        CPPUNIT_ASSERT_MESSAGE(variant + ": Manifest: " + error, read);
        CPPUNIT_ASSERT_MESSAGE(
            variant + ": Manifest has !error", !manifest.has_error);
        CPPUNIT_ASSERT_EQUAL_MESSAGE(variant + ": Manifest entries",
            expected_entries, manifest.entries.size());

        const fstest::WalkCheck walk = fstest::check_walk(image, manifest);
        CPPUNIT_ASSERT_MESSAGE(variant + ": " + walk.report, !walk.passed);
        const fstest::WalkCheck tolerated =
            fstest::check_walk(image, manifest, "* error:fs-open\n");
        CPPUNIT_ASSERT_MESSAGE(
            variant + ": " + tolerated.report, tolerated.passed);
    }

    static std::string error_text()
    {
        const char *msg = tsk_error_get();
        return msg != nullptr ? msg : "";
    }
};

CPPUNIT_TEST_SUITE_REGISTRATION(XfsFixturesTest);

} // namespace
