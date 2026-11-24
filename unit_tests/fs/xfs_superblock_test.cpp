/*
 * XFS superblocks that no XFS file system can have, and one with a label
 * that fills all 12 bytes, made by patching a copy of the start of the v5
 * Fixture image. Through the public API: an impossible superblock is a TSK
 * error, never a file system, and never a crash.
 */
#include "fs_test_data.h"

#include <tsk/libtsk.h>

#include <cppunit/extensions/HelperMacros.h>

#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<char>;

// The first 64 KiB of the v5 Fixture image: the primary superblock and the
// rest of AG 0's header. The file system is cut short, which libtsk accepts
// (last_block_act), so this copy unchanged still opens as XFS.
Bytes
v5_start()
{
    std::ifstream in(fstest::data_path("xfs/v5.img"), std::ios::binary);
    CPPUNIT_ASSERT_MESSAGE("no unpacked xfs/v5.img", in.good());
    Bytes b(65536);
    in.read(b.data(), static_cast<std::streamsize>(b.size()));
    CPPUNIT_ASSERT_MESSAGE("xfs/v5.img is short", in.good());
    return b;
}

// Big-endian stores at a superblock byte offset (xfs_format.h, xfs_dsb).
void
put8(Bytes &b, size_t off, uint8_t v)
{
    b[off] = static_cast<char>(v);
}
void
put16(Bytes &b, size_t off, uint16_t v)
{
    put8(b, off, static_cast<uint8_t>(v >> 8));
    put8(b, off + 1, static_cast<uint8_t>(v));
}
void
put32(Bytes &b, size_t off, uint32_t v)
{
    put16(b, off, static_cast<uint16_t>(v >> 16));
    put16(b, off + 2, static_cast<uint16_t>(v));
}
void
put64(Bytes &b, size_t off, uint64_t v)
{
    put32(b, off, static_cast<uint32_t>(v >> 32));
    put32(b, off + 4, static_cast<uint32_t>(v));
}

// Superblock offsets used here.
const size_t SB_MAGIC = 0, SB_BLOCKSIZE = 4, SB_DBLOCKS = 8, SB_ROOTINO = 56,
             SB_AGBLOCKS = 84, SB_AGCOUNT = 88, SB_VERSIONNUM = 100,
             SB_SECTSIZE = 102, SB_INODESIZE = 104, SB_INOPBLOCK = 106,
             SB_FNAME = 108, SB_BLOCKLOG = 120, SB_SECTLOG = 121,
             SB_INODELOG = 122, SB_INOPBLOG = 123, SB_AGBLKLOG = 124,
             SB_IMAX_PCT = 127, SB_DIRBLKLOG = 192;

// The v5 image: version number 0xb4a5, 4 KiB blocks, 512-byte sectors and
// inodes, 4 AGs of 20480 blocks (agblklog 15), 81920 blocks, root 128.
const uint16_t V5_VERSIONNUM = 0xb4a5;

std::string
write_image(const std::string &name, const Bytes &b)
{
    const std::string path = fstest::data_path("xfs-sb-" + name + ".img");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(b.data(), static_cast<std::streamsize>(b.size()));
    CPPUNIT_ASSERT_MESSAGE("cannot write " + path, out.good());
    return path;
}

std::string
error_text()
{
    const char *msg = tsk_error_get();
    return msg != nullptr ? msg : "";
}

// Opens `path` as an image and its file system with `type`. Returns whether
// the file system opened (as XFS, if `ftype` is given); otherwise `errno_out`
// and `text` are the TSK error.
bool
open_fs(const std::string &path, TSK_FS_TYPE_ENUM type, uint32_t &errno_out,
    std::string &text, std::string *fsstat = nullptr)
{
    const char *paths[] = {path.c_str()};
    tsk_error_reset();
    TSK_IMG_INFO *img = tsk_img_open_utf8(1, paths, TSK_IMG_TYPE_RAW, 0);
    CPPUNIT_ASSERT_MESSAGE(
        "image open failed: " + error_text(), img != nullptr);
    TSK_FS_INFO *fs = tsk_fs_open_img(img, 0, type);
    errno_out = tsk_error_get_errno();
    text = error_text();
    bool xfs = false;
    if (fs != nullptr) {
        xfs = fs->ftype == TSK_FS_TYPE_XFS;
        if (fsstat != nullptr) {
            FILE *f = tmpfile();
            CPPUNIT_ASSERT(f != nullptr);
            CPPUNIT_ASSERT_EQUAL(0, static_cast<int>(fs->fsstat(fs, f)));
            CPPUNIT_ASSERT(fseek(f, 0, SEEK_SET) == 0);
            char buf[4096];
            size_t n = 0;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
                fsstat->append(buf, n);
            }
            fclose(f);
        }
        tsk_fs_close(fs);
    }
    tsk_img_close(img);
    CPPUNIT_ASSERT_MESSAGE(
        "a file system other than XFS opened", fs == nullptr || xfs);
    return fs != nullptr;
}

struct Damage {
    const char *name;
    std::function<void(Bytes &)> apply;
};

class XfsSuperblockTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(XfsSuperblockTest);
    CPPUNIT_TEST(testUndamagedCopyOpens);
    CPPUNIT_TEST(testImpossibleSuperblocks);
    CPPUNIT_TEST(testShorterThanASuperblock);
    CPPUNIT_TEST(testLabelOfTwelveBytes);
    CPPUNIT_TEST_SUITE_END();

  public:
    // The control for the damaged copies: unchanged, the same 64 KiB open as
    // XFS, by type and by autodetection.
    void testUndamagedCopyOpens()
    {
        const std::string path = write_image("control", v5_start());
        uint32_t err = 0;
        std::string text;
        CPPUNIT_ASSERT_MESSAGE(
            "xfs: " + text, open_fs(path, TSK_FS_TYPE_XFS, err, text));
        CPPUNIT_ASSERT_MESSAGE(
            "detect: " + text, open_fs(path, TSK_FS_TYPE_DETECT, err, text));
    }

    // One field per copy that makes the geometry impossible. Opening it as
    // XFS fails with a TSK error; autodetection finds no file system.
    void testImpossibleSuperblocks()
    {
        const Damage damages[] = {
            {"magic", [](Bytes &b) { put32(b, SB_MAGIC, 0x58465343); }},
            {"version-3",
                [](Bytes &b) {
                    put16(b, SB_VERSIONNUM, (V5_VERSIONNUM & ~0xf) | 3);
                }},
            {"version-6",
                [](Bytes &b) {
                    put16(b, SB_VERSIONNUM, (V5_VERSIONNUM & ~0xf) | 6);
                }},
            {"block-size-not-log",
                [](Bytes &b) { put32(b, SB_BLOCKSIZE, 8192); }},
            {"block-log-128k",
                [](Bytes &b) {
                    put32(b, SB_BLOCKSIZE, 131072);
                    put8(b, SB_BLOCKLOG, 17);
                }},
            {"v5-block-512",
                [](Bytes &b) {
                    // Consistent 512-byte blocks of 512-byte inodes, which
                    // v5 forbids (minimum 1 KiB).
                    put32(b, SB_BLOCKSIZE, 512);
                    put8(b, SB_BLOCKLOG, 9);
                    put16(b, SB_INOPBLOCK, 1);
                    put8(b, SB_INOPBLOG, 0);
                }},
            {"sector-size-0", [](Bytes &b) { put16(b, SB_SECTSIZE, 0); }},
            {"sector-log", [](Bytes &b) { put8(b, SB_SECTLOG, 10); }},
            {"inode-size-128",
                [](Bytes &b) {
                    put16(b, SB_INODESIZE, 128);
                    put8(b, SB_INODELOG, 7);
                    put16(b, SB_INOPBLOCK, 32);
                    put8(b, SB_INOPBLOG, 5);
                }},
            {"v5-inode-size-256",
                [](Bytes &b) {
                    put16(b, SB_INODESIZE, 256);
                    put8(b, SB_INODELOG, 8);
                    put16(b, SB_INOPBLOCK, 16);
                    put8(b, SB_INOPBLOG, 4);
                }},
            {"inodes-per-block", [](Bytes &b) { put16(b, SB_INOPBLOCK, 9); }},
            {"inodes-per-block-log", [](Bytes &b) { put8(b, SB_INOPBLOG, 4); }},
            {"ag-count-0", [](Bytes &b) { put32(b, SB_AGCOUNT, 0); }},
            {"ag-blocks-63",
                [](Bytes &b) {
                    put32(b, SB_AGBLOCKS, 63);
                    put8(b, SB_AGBLKLOG, 6);
                    put64(b, SB_DBLOCKS, 4 * 63);
                }},
            {"ag-block-log", [](Bytes &b) { put8(b, SB_AGBLKLOG, 16); }},
            {"blocks-beyond-ags",
                [](Bytes &b) { put64(b, SB_DBLOCKS, 4 * 20480 + 1); }},
            {"last-ag-too-short",
                [](Bytes &b) { put64(b, SB_DBLOCKS, 3 * 20480 + 63); }},
            {"directory-block-128k",
                [](Bytes &b) { put8(b, SB_DIRBLKLOG, 5); }},
            {"inode-space-101", [](Bytes &b) { put8(b, SB_IMAX_PCT, 101); }},
            {"root-inode-0", [](Bytes &b) { put64(b, SB_ROOTINO, 0); }},
            {"root-inode-ag-4",
                [](Bytes &b) {
                    // AG number above agblklog 15 + inopblog 3 bits.
                    put64(b, SB_ROOTINO, (uint64_t{4} << 18) | 128);
                }},
            {"root-inode-past-ag-end",
                [](Bytes &b) {
                    // AG block 20480 of AG 0: within agblklog's 15 bits, but
                    // past the AG's 20480 blocks.
                    put64(b, SB_ROOTINO, uint64_t{20480} << 3);
                }},
        };
        for (const Damage &d : damages) {
            Bytes b = v5_start();
            d.apply(b);
            const std::string path = write_image(d.name, b);
            uint32_t err = 0;
            std::string text;
            CPPUNIT_ASSERT_MESSAGE(std::string(d.name) + ": opened as xfs",
                !open_fs(path, TSK_FS_TYPE_XFS, err, text));
            CPPUNIT_ASSERT_MESSAGE(
                std::string(d.name) + ": no TSK error as xfs",
                err != 0 && !text.empty());
            CPPUNIT_ASSERT_MESSAGE(std::string(d.name) + ": autodetected",
                !open_fs(path, TSK_FS_TYPE_DETECT, err, text));
            CPPUNIT_ASSERT_MESSAGE(
                std::string(d.name) + ": no TSK error from autodetection",
                err != 0);
        }
    }

    // An image too short to hold a superblock is a read error.
    void testShorterThanASuperblock()
    {
        Bytes b = v5_start();
        b.resize(100);
        const std::string path = write_image("short", b);
        uint32_t err = 0;
        std::string text;
        CPPUNIT_ASSERT(!open_fs(path, TSK_FS_TYPE_XFS, err, text));
        CPPUNIT_ASSERT_MESSAGE("no TSK error", err != 0 && !text.empty());
    }

    // A label of 12 bytes has no terminating NUL in the superblock; fsstat
    // prints exactly the 12 bytes.
    void testLabelOfTwelveBytes()
    {
        Bytes b = v5_start();
        const std::string label = "abcdefghijkl";
        std::copy(label.begin(), label.end(), b.begin() + SB_FNAME);
        const std::string path = write_image("label-12", b);
        uint32_t err = 0;
        std::string text;
        std::string fsstat;
        CPPUNIT_ASSERT_MESSAGE(
            text, open_fs(path, TSK_FS_TYPE_XFS, err, text, &fsstat));
        CPPUNIT_ASSERT_MESSAGE(fsstat,
            fsstat.find("\nVolume Name: abcdefghijkl\n") != std::string::npos);
    }
};

CPPUNIT_TEST_SUITE_REGISTRATION(XfsSuperblockTest);

} // namespace
