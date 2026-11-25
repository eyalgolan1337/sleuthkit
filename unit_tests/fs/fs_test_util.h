/*
 * Helpers shared by the fs unit tests: RAII handles for libtsk images, file
 * systems and files, the text of the current TSK error, what a file system's
 * fsstat and istat callbacks print, and the blocks of a file. Public API only. C++14. Test-only.
 */
#ifndef TSK_UNIT_TESTS_FS_TEST_UTIL_H
#define TSK_UNIT_TESTS_FS_TEST_UTIL_H

#include <tsk/libtsk.h>

#include <cppunit/extensions/HelperMacros.h>

#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

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
struct FileCloser {
    void operator()(TSK_FS_FILE *file) const
    {
        tsk_fs_file_close(file);
    }
};
using ImgPtr = std::unique_ptr<TSK_IMG_INFO, ImgCloser>;
using FsPtr = std::unique_ptr<TSK_FS_INFO, FsCloser>;
using FilePtr = std::unique_ptr<TSK_FS_FILE, FileCloser>;

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

// What `print` writes to a temporary file, and whether it returned 0.
template <typename Print>
std::string
printed_text(Print print, bool &ok)
{
    FILE *f = tmpfile();
    if (f == nullptr) {
        CPPUNIT_FAIL("tmpfile failed");
        return "";
    }
    ok = print(f) == 0;
    std::string text;
    CPPUNIT_ASSERT_MESSAGE("fseek failed", fseek(f, 0, SEEK_SET) == 0);
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        text.append(buf, n);
    }
    fclose(f);
    return text;
}

// What the file system's fsstat callback prints (fs->fsstat, as the fsstat
// tool calls it); asserts that it succeeded.
inline std::string
fsstat_text(TSK_FS_INFO *fs)
{
    bool ok = false;
    const std::string text =
        printed_text([fs](FILE *f) { return fs->fsstat(fs, f); }, ok);
    CPPUNIT_ASSERT_MESSAGE("fsstat failed: " + error_text(), ok);
    return text;
}

// What the file system's istat callback prints for inode `inum`
// (fs->istat, as the istat tool calls it, with no forced block count and no
// clock skew); asserts that it succeeded.
inline std::string
istat_text(TSK_FS_INFO *fs, TSK_INUM_T inum,
    TSK_FS_ISTAT_FLAG_ENUM flags = TSK_FS_ISTAT_NONE)
{
    bool ok = false;
    const std::string text = printed_text(
        [&](FILE *f) { return fs->istat(fs, flags, f, inum, 0, 0); }, ok);
    CPPUNIT_ASSERT_MESSAGE(
        "istat " + std::to_string(inum) + " failed: " + error_text(), ok);
    return text;
}

// The addresses of the blocks that hold a file's content, in file order, as
// tsk_fs_file_walk reports them (no sparse blocks: TSK_FS_FILE_WALK_FLAG_AONLY
// and NOSPARSE); asserts that the walk succeeded.
inline std::vector<TSK_DADDR_T>
file_blocks(TSK_FS_FILE *file)
{
    struct Collect {
        static TSK_WALK_RET_ENUM cb(TSK_FS_FILE *, TSK_OFF_T, TSK_DADDR_T addr,
            char *, size_t, TSK_FS_BLOCK_FLAG_ENUM flags, void *ptr)
        {
            if (flags & TSK_FS_BLOCK_FLAG_RAW) {
                static_cast<std::vector<TSK_DADDR_T> *>(ptr)->push_back(addr);
            }
            return TSK_WALK_CONT;
        }
    };
    std::vector<TSK_DADDR_T> blocks;
    const uint8_t rc = tsk_fs_file_walk(file,
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): flags
        static_cast<TSK_FS_FILE_WALK_FLAG_ENUM>(
            TSK_FS_FILE_WALK_FLAG_AONLY | TSK_FS_FILE_WALK_FLAG_NOSPARSE),
        Collect::cb, &blocks);
    CPPUNIT_ASSERT_EQUAL_MESSAGE(
        "tsk_fs_file_walk failed: " + error_text(), 0, static_cast<int>(rc));
    return blocks;
}

// The value of fsstat's "<key>: <value>" or "<key> : <value>" line, or
// "(no line)".
inline std::string
fsstat_value(const std::string &text, const std::string &key)
{
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        for (const char *sep : {": ", " : "}) {
            const std::string prefix = key + sep;
            if (line.compare(0, prefix.size(), prefix) == 0) {
                return line.substr(prefix.size());
            }
        }
    }
    return "(no line)";
}

} // namespace fstest

#endif
