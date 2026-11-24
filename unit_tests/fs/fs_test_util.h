/*
 * Helpers shared by the fs unit tests: RAII handles for libtsk images and
 * file systems, the text of the current TSK error, and what a file system's
 * fsstat callback prints. Public API only. C++14. Test-only.
 */
#ifndef TSK_UNIT_TESTS_FS_TEST_UTIL_H
#define TSK_UNIT_TESTS_FS_TEST_UTIL_H

#include <tsk/libtsk.h>

#include <cppunit/extensions/HelperMacros.h>

#include <cstdio>
#include <memory>
#include <sstream>
#include <string>

namespace fstest {

struct ImgCloser {
    void operator()(TSK_IMG_INFO *img) const
    {
        tsk_img_close(img);
    }
};
struct FsCloser {
    void operator()(TSK_FS_INFO *fs) const
    {
        tsk_fs_close(fs);
    }
};
using ImgPtr = std::unique_ptr<TSK_IMG_INFO, ImgCloser>;
using FsPtr = std::unique_ptr<TSK_FS_INFO, FsCloser>;

// The current TSK error's text, or "".
inline std::string
error_text()
{
    const char *msg = tsk_error_get();
    return msg != nullptr ? msg : "";
}

// Opens `path` as a single-file image of `type`; asserts that it opened.
inline ImgPtr
open_image(
    const std::string &path, TSK_IMG_TYPE_ENUM type = TSK_IMG_TYPE_DETECT)
{
    const char *paths[] = {path.c_str()};
    tsk_error_reset();
    ImgPtr img(tsk_img_open_utf8(1, paths, type, 0));
    CPPUNIT_ASSERT_MESSAGE(
        "tsk_img_open_utf8 failed on " + path + ": " + error_text(),
        img != nullptr);
    return img;
}

// What the file system's fsstat callback prints (fs->fsstat, as the fsstat
// tool calls it); asserts that it succeeded.
inline std::string
fsstat_text(TSK_FS_INFO *fs)
{
    FILE *f = tmpfile();
    if (f == nullptr) {
        CPPUNIT_FAIL("tmpfile failed");
        return "";
    }
    const uint8_t rc = fs->fsstat(fs, f);
    std::string text;
    CPPUNIT_ASSERT_MESSAGE("fseek failed", fseek(f, 0, SEEK_SET) == 0);
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        text.append(buf, n);
    }
    fclose(f);
    CPPUNIT_ASSERT_EQUAL_MESSAGE(
        "fsstat failed: " + error_text(), 0, static_cast<int>(rc));
    return text;
}

// The value of fsstat's "<key>: <value>" line, or "(no line)".
inline std::string
fsstat_value(const std::string &text, const std::string &key)
{
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, key.size() + 2, key + ": ") == 0) {
            return line.substr(key.size() + 2);
        }
    }
    return "(no line)";
}

} // namespace fstest

#endif
