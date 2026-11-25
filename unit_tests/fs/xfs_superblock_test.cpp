/*
 * XFS superblocks made by patching a copy of the start of the v5 Fixture
 * image, read through the public API: superblocks no XFS file system can
 * have give a TSK error (never a file system, never a crash), and unusual
 * but possible ones open and show in fsstat.
 */
#include "fs_test_data.h"
#include "fs_test_util.h"

#include <tsk/libtsk.h>

#include <cppunit/extensions/HelperMacros.h>

#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace {

using fstest::error_text;
using fstest::FsPtr;
using fstest::fsstat_text;
using fstest::fsstat_value;
using fstest::ImgPtr;
using fstest::open_image;

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
const size_t SB_MAGIC = 0, SB_BLOCKSIZE = 4, SB_DBLOCKS = 8, SB_RBLOCKS = 16,
             SB_LOGSTART = 48, SB_ROOTINO = 56, SB_AGBLOCKS = 84,
             SB_AGCOUNT = 88, SB_VERSIONNUM = 100, SB_SECTSIZE = 102,
             SB_INODESIZE = 104, SB_INOPBLOCK = 106, SB_FNAME = 108,
             SB_BLOCKLOG = 120, SB_SECTLOG = 121, SB_INODELOG = 122,
             SB_INOPBLOG = 123, SB_AGBLKLOG = 124, SB_IMAX_PCT = 127,
             SB_DIRBLKLOG = 192, SB_FEATURES2 = 200, SB_BAD_FEATURES2 = 204,
             SB_FEATURES_INCOMPAT = 216, SB_META_UUID = 248;

// The v5 image (xfs_info and xfs_db): version number 0xb4a5, 4 KiB blocks,
// 512-byte sectors and inodes, 4 AGs of 20480 blocks (agblklog 15), 81920
// blocks, root 128, features2 0x18a, incompat 0x2b (ftype, sparse, bigtime,
// nrext64), a 16384-block internal log.
const uint16_t V5_VERSIONNUM = 0xb4a5;
const uint32_t V5_FEATURES2 = 0x18a;
const uint32_t V5_INCOMPAT = 0x2b;

std::string
write_image(const std::string &name, const Bytes &b)
{
    const std::string path = fstest::data_path("xfs-sb-" + name + ".img");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(b.data(), static_cast<std::streamsize>(b.size()));
    CPPUNIT_ASSERT_MESSAGE("cannot write " + path, out.good());
    return path;
}

// The result of opening an image's file system.
struct Opened {
    FsPtr fs;         // null if it didn't open
    uint32_t err = 0; // the TSK error then
    std::string text; // its text
};

// Opens the raw image at `path` and its file system with `type`. Asserts
// that whatever opened is XFS.
Opened
open_fs(const std::string &path, TSK_FS_TYPE_ENUM type, ImgPtr &img)
{
    img = open_image(path, TSK_IMG_TYPE_RAW);
    Opened o;
    o.fs.reset(tsk_fs_open_img(img.get(), 0, type));
    o.err = tsk_error_get_errno();
    o.text = error_text();
    CPPUNIT_ASSERT_MESSAGE("a file system other than XFS opened",
        o.fs == nullptr || o.fs->ftype == TSK_FS_TYPE_XFS);
    return o;
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
    CPPUNIT_TEST(testInodesOf1KiB);
    CPPUNIT_TEST(testUnusualSuperblockInFsstat);
    CPPUNIT_TEST(testBadFeatures2Merged);
    CPPUNIT_TEST_SUITE_END();

  public:
    // The control for the damaged copies: unchanged, the same 64 KiB open as
    // XFS, by type and by autodetection, with the file system's blocks
    // clipped to the 16 the image holds.
    void testUndamagedCopyOpens()
    {
        const std::string path = write_image("control", v5_start());
        ImgPtr img;
        Opened o = open_fs(path, TSK_FS_TYPE_XFS, img);
        CPPUNIT_ASSERT_MESSAGE("xfs: " + o.text, o.fs != nullptr);
        CPPUNIT_ASSERT_EQUAL(
            uint64_t{81919}, static_cast<uint64_t>(o.fs->last_block));
        CPPUNIT_ASSERT_EQUAL(
            uint64_t{15}, static_cast<uint64_t>(o.fs->last_block_act));
        const std::string text = fsstat_text(o.fs.get());
        CPPUNIT_ASSERT_EQUAL_MESSAGE(text, std::string("0 - 15"),
            fsstat_value(text, "Total Range in Image"));
        o = open_fs(path, TSK_FS_TYPE_DETECT, img);
        CPPUNIT_ASSERT_MESSAGE("detect: " + o.text, o.fs != nullptr);
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
            {"inode-larger-than-block",
                [](Bytes &b) {
                    // 1 KiB blocks of 2 KiB inodes.
                    put32(b, SB_BLOCKSIZE, 1024);
                    put8(b, SB_BLOCKLOG, 10);
                    put16(b, SB_INODESIZE, 2048);
                    put8(b, SB_INODELOG, 11);
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
            {"ag-inode-bits-over-32",
                [](Bytes &b) {
                    // 2^30 + 1 blocks per AG: agblklog 31, plus inopblog 3.
                    const uint32_t agblocks = (uint32_t{1} << 30) + 1;
                    put32(b, SB_AGBLOCKS, agblocks);
                    put8(b, SB_AGBLKLOG, 31);
                    put64(b, SB_DBLOCKS, uint64_t{4} * agblocks);
                }},
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
            const std::string name = d.name;
            ImgPtr img;
            Opened o = open_fs(path, TSK_FS_TYPE_XFS, img);
            CPPUNIT_ASSERT_MESSAGE(name + ": opened as xfs", o.fs == nullptr);
            // A wrong magic is not XFS; any other damage is a corrupt XFS
            // superblock. Of the reasons, only a bad block size has a text
            // (the earlier XFS module's).
            CPPUNIT_ASSERT_EQUAL_MESSAGE(name + ": TSK error as xfs: " + o.text,
                name == "magic" ? uint32_t{TSK_ERR_FS_MAGIC}
                                : uint32_t{TSK_ERR_FS_CORRUPT},
                o.err);
            const bool block_size = name == "block-size-not-log" ||
                name == "block-log-128k" || name == "v5-block-512";
            CPPUNIT_ASSERT_EQUAL_MESSAGE(name + ": " + o.text, block_size,
                o.text.find("(block size)") != std::string::npos);
            o = open_fs(path, TSK_FS_TYPE_DETECT, img);
            CPPUNIT_ASSERT_MESSAGE(name + ": autodetected", o.fs == nullptr);
            CPPUNIT_ASSERT_MESSAGE(
                name + ": no TSK error from autodetection", o.err != 0);
        }
    }

    // An image too short to hold a superblock is a read error.
    void testShorterThanASuperblock()
    {
        Bytes b = v5_start();
        b.resize(100);
        const std::string path = write_image("short", b);
        ImgPtr img;
        const Opened o = open_fs(path, TSK_FS_TYPE_XFS, img);
        CPPUNIT_ASSERT(o.fs == nullptr);
        CPPUNIT_ASSERT_MESSAGE("no TSK error", o.err != 0 && !o.text.empty());
    }

    // A label of 12 bytes has no terminating NUL in the superblock; fsstat
    // prints exactly the 12 bytes.
    void testLabelOfTwelveBytes()
    {
        Bytes b = v5_start();
        const std::string label = "abcdefghijkl";
        std::copy(label.begin(), label.end(), b.begin() + SB_FNAME);
        ImgPtr img;
        const Opened o =
            open_fs(write_image("label-12", b), TSK_FS_TYPE_XFS, img);
        CPPUNIT_ASSERT_MESSAGE(o.text, o.fs != nullptr);
        const std::string text = fsstat_text(o.fs.get());
        CPPUNIT_ASSERT_EQUAL_MESSAGE(
            text, label, fsstat_value(text, "Volume Name"));
    }

    // A non-default inode size (1 KiB, as on the Validation VM's
    // /data-compat): fsstat shows it, and the inode numbers have 2 bits for
    // the inode in a block, so the largest possible inode is AG 3, block
    // 20479, inode 3: (3 << 17) | (20479 << 2) | 3 = 475135.
    void testInodesOf1KiB()
    {
        Bytes b = v5_start();
        put16(b, SB_INODESIZE, 1024);
        put8(b, SB_INODELOG, 10);
        put16(b, SB_INOPBLOCK, 4);
        put8(b, SB_INOPBLOG, 2);
        ImgPtr img;
        const Opened o =
            open_fs(write_image("inode-1k", b), TSK_FS_TYPE_DETECT, img);
        CPPUNIT_ASSERT_MESSAGE(o.text, o.fs != nullptr);
        const std::string text = fsstat_text(o.fs.get());
        CPPUNIT_ASSERT_EQUAL_MESSAGE(
            text, std::string("1024"), fsstat_value(text, "Inode Size"));
        CPPUNIT_ASSERT_EQUAL_MESSAGE(
            text, std::string("4"), fsstat_value(text, "Inode per Block"));
        CPPUNIT_ASSERT_EQUAL(
            uint64_t{475136}, static_cast<uint64_t>(o.fs->last_inum));
    }

    // Features this module doesn't know, a separate metadata UUID, an
    // external log and a realtime device are possible: the file system
    // opens, and fsstat names the features it knows.
    void testUnusualSuperblockInFsstat()
    {
        Bytes b = v5_start();
        put32(b, SB_FEATURES_INCOMPAT, V5_INCOMPAT | 0x4 | 0x80000000);
        for (size_t i = 0; i < 16; ++i) {
            put8(b, SB_META_UUID + i, 0x11);
        }
        put64(b, SB_LOGSTART, 0);
        put64(b, SB_RBLOCKS, 1000);
        ImgPtr img;
        const Opened o =
            open_fs(write_image("unusual", b), TSK_FS_TYPE_XFS, img);
        CPPUNIT_ASSERT_MESSAGE(o.text, o.fs != nullptr);
        const std::string text = fsstat_text(o.fs.get());
        const auto expect = [&text](const char *key, const char *want) {
            CPPUNIT_ASSERT_EQUAL_MESSAGE(
                text, std::string(want), fsstat_value(text, key));
        };
        expect(
            "InCompat Features", "ftype, sparse, meta-uuid, bigtime, nrext64");
        expect("Volume ID", "5f4b0c1e-0000-4000-8000-000000000005");
    }

    // Old kernels wrote some features2 bits to sb_bad_features2; the
    // kernel merges the two, and so does fsstat's Flags line. Without
    // MOREBITS, features2 is not read at all.
    void testBadFeatures2Merged()
    {
        Bytes b = v5_start();
        put32(b, SB_FEATURES2, V5_FEATURES2 & ~0x8u); // attr2 only in bad
        put32(b, SB_BAD_FEATURES2, 0x8);
        ImgPtr img;
        Opened o =
            open_fs(write_image("bad-features2", b), TSK_FS_TYPE_XFS, img);
        CPPUNIT_ASSERT_MESSAGE(o.text, o.fs != nullptr);
        std::string text = fsstat_text(o.fs.get());
        CPPUNIT_ASSERT_EQUAL_MESSAGE(text,
            std::string("nlink, align, logv2, extflg, dirv2, morebits, "
                        "lazy-count, attr2, projid32bit, crc"),
            fsstat_value(text, "Flags"));

        put16(b, SB_VERSIONNUM, V5_VERSIONNUM & ~0x8000u); // no MOREBITS
        o = open_fs(write_image("no-morebits", b), TSK_FS_TYPE_XFS, img);
        CPPUNIT_ASSERT_MESSAGE(o.text, o.fs != nullptr);
        text = fsstat_text(o.fs.get());
        CPPUNIT_ASSERT_EQUAL_MESSAGE(text,
            std::string("nlink, align, logv2, extflg, dirv2"),
            fsstat_value(text, "Flags"));
    }
};

CPPUNIT_TEST_SUITE_REGISTRATION(XfsSuperblockTest);

} // namespace
