/*
 * XFS symlinks, special files, timestamps and hard links (#43), on the
 * Fixture images through the public API: the target of inline and remote
 * symlinks (meta->link), the types of device files, FIFOs and sockets,
 * timestamps to the nanosecond at their limits, and link counts. Cases the
 * Fixture images don't hold (a target in several extents, damaged symlinks,
 * a socket, hard links) use patched copies of the start of an image.
 */
#include "fs_test_util.h"
#include "xfs_fixture_data.h"

#include "manifest.h"
#include "toolcheck.h"

#include <tsk/libtsk.h>

#include <cppunit/extensions/HelperMacros.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

using fstest::ALL_FIXTURES;
using fstest::entry_of;
using fstest::error_text;
using fstest::FilePtr;
using fstest::Fixture;
using fstest::FsPtr;
using fstest::image_path;
using fstest::ImgPtr;
using fstest::open_image;
using fstest::read_manifest;

// A file system and the image it is in.
struct OpenedFs {
    ImgPtr img;
    FsPtr fs;
    explicit OpenedFs(
        const std::string &path, TSK_IMG_TYPE_ENUM type = TSK_IMG_TYPE_DETECT)
        : img(open_image(path, type))
    {
        fs.reset(tsk_fs_open_img(img.get(), 0, TSK_FS_TYPE_XFS));
        CPPUNIT_ASSERT_MESSAGE(path + ": " + error_text(), fs != nullptr);
    }
};

// Opens `path` in `fs` by name; asserts that it opened.
FilePtr
open_path(TSK_FS_INFO *fs, const std::string &path)
{
    tsk_error_reset();
    FilePtr file(tsk_fs_file_open(fs, nullptr, path.c_str()));
    CPPUNIT_ASSERT_MESSAGE(path + ": " + error_text(), file != nullptr);
    return file;
}

// Opens inode `inum` of `fs`; asserts that it opened.
FilePtr
open_inode(TSK_FS_INFO *fs, TSK_INUM_T inum)
{
    tsk_error_reset();
    FilePtr file(tsk_fs_file_open_meta(fs, nullptr, inum));
    CPPUNIT_ASSERT_MESSAGE(
        std::to_string(inum) + ": " + error_text(), file != nullptr);
    return file;
}

// The file's symlink target, or "(none)" if it has none.
std::string
target_of(const TSK_FS_FILE *file)
{
    return file->meta->link != nullptr ? file->meta->link : "(none)";
}

// A target of `n` bytes that no two offsets share four bytes of.
std::string
long_target(size_t n)
{
    std::string t;
    for (int i = 0; t.size() < n; ++i) {
        t += "dir" + std::to_string(i) + "/";
    }
    return t.substr(0, n);
}

// Big-endian stores in an image copy.
void
put32(std::vector<char> &b, size_t off, uint32_t v)
{
    for (size_t i = 0; i < 4; ++i) {
        b[off + i] = static_cast<char>(v >> (24 - 8 * i));
    }
}
void
put64(std::vector<char> &b, size_t off, uint64_t v)
{
    put32(b, off, static_cast<uint32_t>(v >> 32));
    put32(b, off + 4, static_cast<uint32_t>(v));
}

// The first MiB of the Fixture image.
std::vector<char>
image_start(const Fixture &f)
{
    const std::string path = image_path(f);
    std::ifstream in(path, std::ios::binary);
    std::vector<char> b(1 << 20);
    in.read(b.data(), static_cast<std::streamsize>(b.size()));
    CPPUNIT_ASSERT_MESSAGE(path + " is short", in.good());
    return b;
}

// Writes `bytes` as xfs-link-<name>.img in the test data directory and
// returns its path. The file system is cut short, which libtsk accepts.
std::string
write_image(const std::string &name, const std::vector<char> &bytes)
{
    const std::string path = fstest::data_path("xfs-link-" + name + ".img");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    CPPUNIT_ASSERT_MESSAGE("cannot write " + path, out.good());
    return path;
}

// Inode fields (xfs_dinode): offsets in the inode.
const size_t DI_MODE = 2, DI_FORMAT = 5, DI_NLINK = 16, DI_BIG_NEXTENTS = 24;
const size_t DI_SIZE = 56, DI_NEXTENTS = 76, DI_FLAGS2 = 120;
const char FMT_LOCAL = 1, FMT_BTREE = 3; // di_format
const size_t CORE_V2 = 100, CORE_V3 = 176;

// A symlink of a patched copy: where its inode and data fork are, the block
// size, and whether remote target blocks start with a header (v5).
struct Link {
    TSK_INUM_T inum;
    size_t inode; // byte offset
    size_t core;  // size of the inode core: the data fork follows
    size_t block;
    bool header;
};

// /symlink-long of the v5-1k Fixture (xfs_db): inode 78 (block 39, index 0,
// 512-byte inodes), one extent, file block 0 at block 27: the header (56
// bytes) and the 719 bytes of the target.
const Link V5_1K_LONG = {78, 39 * 1024, CORE_V3, 1024, true};
// /symlink-short of the v5-1k Fixture: inode 77 (block 38, index 1), the
// 9 bytes of the target in the inode.
const Link V5_1K_SHORT = {77, 38 * 1024 + 512, CORE_V3, 1024, true};
// /symlink-long of the v4-512 Fixture: inode 46 (block 23, index 0,
// 256-byte inodes), one extent, file blocks 0-1 at block 67: the target.
const Link V4_512_LONG = {46, 23 * 512, CORE_V2, 512, false};

const size_t SYMLINK_HDR = 56;

// Sets the symlink's size and its number of extent records (di_big_nextents
// on an inode with XFS_DIFLAG2_NREXT64, else di_nextents).
void
set_size(
    std::vector<char> &image, const Link &l, uint64_t size, uint32_t nextents)
{
    put64(image, l.inode + DI_SIZE, size);
    if (l.core == CORE_V3 && (image[l.inode + DI_FLAGS2 + 7] & 0x10) != 0) {
        put64(image, l.inode + DI_BIG_NEXTENTS, nextents);
    }
    else {
        put32(image, l.inode + DI_NEXTENTS, nextents);
    }
}

// Writes extent record `index` of the symlink's data fork (xfs_bmbt_rec:
// the unwritten flag, the file offset, the start block and the length, 1,
// 54, 52 and 21 bits from the top). AG 0, so the fsblock is the block.
void
set_extent(std::vector<char> &image, const Link &l, size_t index,
    uint64_t startoff, uint64_t fsblock, uint64_t count)
{
    const size_t rec = l.inode + l.core + 16 * index;
    put64(image, rec, (startoff << 9) | (fsblock >> 43));
    put64(image, rec + 8, (fsblock << 21) | count);
}

// Writes a piece of a remote target at block `fsblock`: on v5 the header
// (xfs_dsymlink_hdr: magic "XSLM", the piece's offset in the target, its
// length, then CRC, UUID, owner, block number and LSN, which libtsk doesn't
// check), then the bytes.
void
set_piece(std::vector<char> &image, const Link &l, uint64_t fsblock,
    uint32_t offset, const std::string &bytes)
{
    size_t at = fsblock * l.block;
    if (l.header) {
        std::memset(&image[at], 0, SYMLINK_HDR);
        std::memcpy(&image[at], "XSLM", 4);
        put32(image, at + 4, offset);
        put32(image, at + 8, static_cast<uint32_t>(bytes.size()));
        put64(image, at + 32, l.inum);
        at += SYMLINK_HDR;
    }
    std::memcpy(&image[at], bytes.data(), bytes.size());
}

class XfsLinkTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(XfsLinkTest);
    CPPUNIT_TEST(testSymlinkTargets);
    CPPUNIT_TEST(testIstatShowsTarget);
    CPPUNIT_TEST(testLongestTargetInTwoExtents);
    CPPUNIT_TEST(testLongestTargetInOneExtentOfTwoBlocks);
    CPPUNIT_TEST(testLongestTargetWithoutHeaders);
    CPPUNIT_TEST(testLongestInlineTarget);
    CPPUNIT_TEST(testDamagedSymlinkOpensWithoutTarget);
    CPPUNIT_TEST(testUnreadableTargetFailsLookup);
    CPPUNIT_TEST(testControlCharactersInTarget);
    CPPUNIT_TEST(testReusedFileDropsTarget);
    CPPUNIT_TEST(testSpecialFileTypes);
    CPPUNIT_TEST(testSocket);
    CPPUNIT_TEST(testTimestampLimits);
    CPPUNIT_TEST(testHardLinks);
    CPPUNIT_TEST_SUITE_END();

  public:
    // The inline symlink (target in the inode) and the remote one (target
    // in blocks: one 4 KiB or 64 KiB block, two 512-byte blocks, and on v5
    // behind a header) of every Fixture have the Manifest's target, opened
    // by path and by inode number. Only symlinks have one.
    void testSymlinkTargets()
    {
        for (const Fixture &f : ALL_FIXTURES) {
            const std::string v = f.variant;
            const tskval::Manifest manifest = read_manifest(f);
            const OpenedFs o(image_path(f));
            for (const char *path : {"/symlink-short", "/symlink-long"}) {
                const tskval::Entry want = entry_of(manifest, path);
                CPPUNIT_ASSERT_MESSAGE(v + path, want.has_target);
                const FilePtr named = open_path(o.fs.get(), path);
                CPPUNIT_ASSERT_EQUAL_MESSAGE(
                    v + path, want.target, target_of(named.get()));
                CPPUNIT_ASSERT_EQUAL_MESSAGE(
                    v + path, TSK_FS_META_TYPE_LNK, named->meta->type);
                CPPUNIT_ASSERT_EQUAL_MESSAGE(v + path,
                    static_cast<TSK_OFF_T>(want.target.size()),
                    named->meta->size);
                const FilePtr numbered = open_inode(o.fs.get(), want.inode);
                CPPUNIT_ASSERT_EQUAL_MESSAGE(
                    v + path, want.target, target_of(numbered.get()));
            }
            for (const char *path : {"/", "/small.txt", "/fifo", "/chardev"}) {
                const FilePtr file = open_path(o.fs.get(), path);
                CPPUNIT_ASSERT_MESSAGE(v + path, file->meta->link == nullptr);
            }
        }
    }

    // istat prints the target, as ext's does ("symbolic link to:"), where
    // the validate command's istat check reads it.
    void testIstatShowsTarget()
    {
        for (const Fixture &f : ALL_FIXTURES) {
            const std::string v = f.variant;
            const tskval::Manifest manifest = read_manifest(f);
            const OpenedFs o(image_path(f));
            for (const char *path : {"/symlink-short", "/symlink-long"}) {
                const tskval::Entry want = entry_of(manifest, path);
                const std::string text =
                    fstest::istat_text(o.fs.get(), want.inode);
                tskval::Entry got;
                std::string error;
                const bool parsed = tskval::parse_istat(text, got, error);
                std::string where = v + path;
                where.append(": ").append(error).append("\n").append(text);
                CPPUNIT_ASSERT_MESSAGE(where, parsed);
                CPPUNIT_ASSERT_MESSAGE(where, got.has_target);
                CPPUNIT_ASSERT_EQUAL_MESSAGE(v + path, want.target, got.target);
            }
            const std::string plain = fstest::istat_text(
                o.fs.get(), entry_of(manifest, "/small.txt").inode);
            CPPUNIT_ASSERT_MESSAGE(v + ": /small.txt has a target",
                plain.find("symbolic link") == std::string::npos);
        }
    }

    // The longest target (1023 bytes) on v5 with 1 KiB blocks doesn't fit
    // one block behind its header (968 bytes do). In two extents, each
    // starts with its own header: the pieces are joined without them.
    void testLongestTargetInTwoExtents()
    {
        const Link &l = V5_1K_LONG;
        const std::string target = long_target(1023);
        const size_t first = l.block - SYMLINK_HDR;
        std::vector<char> image = image_start(fstest::V5_1K);
        set_size(image, l, target.size(), 2);
        set_extent(image, l, 0, 0, 27, 1);
        set_extent(image, l, 1, 1, 900, 1);
        set_piece(image, l, 27, 0, target.substr(0, first));
        set_piece(
            image, l, 900, static_cast<uint32_t>(first), target.substr(first));
        const OpenedFs o(write_image("two-extents", image), TSK_IMG_TYPE_RAW);
        const FilePtr file = open_inode(o.fs.get(), l.inum);
        CPPUNIT_ASSERT_EQUAL(target, target_of(file.get()));
        CPPUNIT_ASSERT_EQUAL(TSK_OFF_T{1023}, file->meta->size);
    }

    // In one extent of two blocks there is one header, at its start: the
    // second block is target bytes from its first byte.
    void testLongestTargetInOneExtentOfTwoBlocks()
    {
        const Link &l = V5_1K_LONG;
        const std::string target = long_target(1023);
        std::vector<char> image = image_start(fstest::V5_1K);
        set_size(image, l, target.size(), 1);
        set_extent(image, l, 0, 0, 900, 2);
        set_piece(image, l, 900, 0, target);
        const OpenedFs o(write_image("two-blocks", image), TSK_IMG_TYPE_RAW);
        const FilePtr file = open_inode(o.fs.get(), l.inum);
        CPPUNIT_ASSERT_EQUAL(target, target_of(file.get()));
    }

    // v4 target blocks have no header. The longest target in two extents
    // of one 512-byte block each, the second one not next to the first.
    void testLongestTargetWithoutHeaders()
    {
        const Link &l = V4_512_LONG;
        const std::string target = long_target(1023);
        std::vector<char> image = image_start(fstest::V4_512);
        set_size(image, l, target.size(), 2);
        set_extent(image, l, 0, 0, 67, 1);
        set_extent(image, l, 1, 1, 1500, 1);
        set_piece(image, l, 67, 0, target.substr(0, 512));
        set_piece(image, l, 1500, 512, target.substr(512));
        const OpenedFs o(write_image("v4-extents", image), TSK_IMG_TYPE_RAW);
        const FilePtr file = open_inode(o.fs.get(), l.inum);
        CPPUNIT_ASSERT_EQUAL(target, target_of(file.get()));
    }

    // An inline target can fill the data fork: 336 bytes of a 512-byte v3
    // inode without an attr fork.
    void testLongestInlineTarget()
    {
        const Link &l = V5_1K_SHORT;
        const std::string target = long_target(512 - CORE_V3);
        std::vector<char> image = image_start(fstest::V5_1K);
        CPPUNIT_ASSERT_EQUAL_MESSAGE("not the inline symlink xfs_db shows",
            FMT_LOCAL, image[l.inode + DI_FORMAT]);
        put64(image, l.inode + DI_SIZE, target.size());
        std::memcpy(&image[l.inode + l.core], target.data(), target.size());
        const OpenedFs o(write_image("inline-full", image), TSK_IMG_TYPE_RAW);
        const FilePtr file = open_inode(o.fs.get(), l.inum);
        CPPUNIT_ASSERT_EQUAL(target, target_of(file.get()));
    }

    // A symlink whose target can't be put together still opens, with its
    // other metadata and no target: a size no target has, a target larger
    // than its fork or its blocks, a header that is not the one of its
    // piece, a hole where the target starts, an extent outside the file
    // system, a fork format symlinks don't have.
    void testDamagedSymlinkOpensWithoutTarget()
    {
        struct Case {
            const char *name;
            const Link *link;
            void (*damage)(std::vector<char> &);
        };
        const Case cases[] = {
            {"remote-size-0", &V5_1K_LONG,
                [](std::vector<char> &b) {
                    put64(b, V5_1K_LONG.inode + DI_SIZE, 0);
                }},
            {"remote-size-1025", &V5_1K_LONG,
                [](std::vector<char> &b) {
                    put64(b, V5_1K_LONG.inode + DI_SIZE, 1025);
                }},
            {"remote-size-huge", &V5_1K_LONG,
                [](std::vector<char> &b) {
                    put64(b, V5_1K_LONG.inode + DI_SIZE, uint64_t{1} << 62);
                }},
            {"remote-short-blocks", &V5_1K_LONG,
                [](std::vector<char> &b) {
                    // One block holds 968 bytes behind its header.
                    put64(b, V5_1K_LONG.inode + DI_SIZE, 969);
                    put32(b, 27 * 1024 + 8, 969);
                }},
            {"header-magic", &V5_1K_LONG,
                [](std::vector<char> &b) { b[27 * 1024] = 'Y'; }},
            {"header-offset", &V5_1K_LONG,
                [](std::vector<char> &b) { put32(b, 27 * 1024 + 4, 1); }},
            {"header-bytes", &V5_1K_LONG,
                [](std::vector<char> &b) { put32(b, 27 * 1024 + 8, 718); }},
            {"hole-at-start", &V5_1K_LONG,
                [](std::vector<char> &b) {
                    set_extent(b, V5_1K_LONG, 0, 1, 27, 1);
                }},
            {"extent-outside", &V5_1K_LONG,
                [](std::vector<char> &b) {
                    // AG 9 of 4.
                    set_extent(
                        b, V5_1K_LONG, 0, 0, (uint64_t{9} << 17) | 27, 1);
                }},
            {"no-extents", &V5_1K_LONG,
                [](std::vector<char> &b) { set_size(b, V5_1K_LONG, 719, 0); }},
            {"too-many-extents", &V5_1K_LONG,
                [](std::vector<char> &b) {
                    // The fork has room for 21 records.
                    set_size(b, V5_1K_LONG, 719, 22);
                }},
            {"btree-fork", &V5_1K_LONG,
                [](std::vector<char> &b) {
                    b[V5_1K_LONG.inode + DI_FORMAT] = FMT_BTREE;
                }},
            {"inline-size-0", &V5_1K_SHORT,
                [](std::vector<char> &b) {
                    put64(b, V5_1K_SHORT.inode + DI_SIZE, 0);
                }},
            {"inline-over-fork", &V5_1K_SHORT,
                [](std::vector<char> &b) {
                    put64(b, V5_1K_SHORT.inode + DI_SIZE, 337);
                }},
            {"v4-hole-in-middle", &V4_512_LONG,
                [](std::vector<char> &b) {
                    set_size(b, V4_512_LONG, 719, 2);
                    set_extent(b, V4_512_LONG, 0, 0, 67, 1);
                    set_extent(b, V4_512_LONG, 1, 2, 68, 1);
                }},
        };
        for (const Case &c : cases) {
            const Fixture &f =
                c.link == &V4_512_LONG ? fstest::V4_512 : fstest::V5_1K;
            std::vector<char> image = image_start(f);
            c.damage(image);
            const OpenedFs o(write_image(c.name, image), TSK_IMG_TYPE_RAW);
            tsk_error_reset();
            const FilePtr file(
                tsk_fs_file_open_meta(o.fs.get(), nullptr, c.link->inum));
            CPPUNIT_ASSERT_MESSAGE(
                c.name + (": " + error_text()), file != nullptr);
            CPPUNIT_ASSERT_MESSAGE(
                c.name + (": target " + target_of(file.get())),
                file->meta->link == nullptr);
            CPPUNIT_ASSERT_EQUAL_MESSAGE(
                c.name, TSK_FS_META_TYPE_LNK, file->meta->type);
            CPPUNIT_ASSERT_EQUAL_MESSAGE(c.name, 1, file->meta->nlink);
            CPPUNIT_ASSERT_EQUAL_MESSAGE(
                c.name, static_cast<uint32_t>(0), tsk_error_get_errno());
        }
    }

    // A remote target whose blocks can't be read fails the lookup with a
    // read error, as ext's does: here its extent is past the end of the
    // image (block 2000 of a copy cut after 1 MiB).
    void testUnreadableTargetFailsLookup()
    {
        const Link &l = V5_1K_LONG;
        std::vector<char> image = image_start(fstest::V5_1K);
        set_extent(image, l, 0, 0, 2000, 1);
        const OpenedFs o(write_image("unreadable", image), TSK_IMG_TYPE_RAW);
        tsk_error_reset();
        const FilePtr file(tsk_fs_file_open_meta(o.fs.get(), nullptr, l.inum));
        CPPUNIT_ASSERT_MESSAGE("opened", file == nullptr);
        CPPUNIT_ASSERT_EQUAL_MESSAGE(error_text(),
            static_cast<uint32_t>(TSK_ERR_FS_READ), tsk_error_get_errno());
        tsk_error_reset();
        // The inline symlink next to it still opens.
        const FilePtr inline_link = open_inode(o.fs.get(), V5_1K_SHORT.inum);
        CPPUNIT_ASSERT_EQUAL(
            std::string("small.txt"), target_of(inline_link.get()));
    }

    // Control characters in a target are replaced with '^', as in ext's
    // and UFS's targets.
    void testControlCharactersInTarget()
    {
        const Link &l = V5_1K_SHORT;
        std::vector<char> image = image_start(fstest::V5_1K);
        std::memcpy(&image[l.inode + l.core], "sm\x01ll\t\x7fxt", 9);
        const OpenedFs o(write_image("control", image), TSK_IMG_TYPE_RAW);
        const FilePtr file = open_inode(o.fs.get(), l.inum);
        CPPUNIT_ASSERT_EQUAL(std::string("sm^ll^^xt"), target_of(file.get()));
    }

    // A TSK_FS_FILE opened again for another inode doesn't keep the target
    // of the symlink it was.
    void testReusedFileDropsTarget()
    {
        const Fixture &f = fstest::V5;
        const tskval::Manifest manifest = read_manifest(f);
        const OpenedFs o(image_path(f));
        TSK_FS_FILE *file = tsk_fs_file_open_meta(
            o.fs.get(), nullptr, entry_of(manifest, "/symlink-long").inode);
        CPPUNIT_ASSERT_MESSAGE(error_text(), file != nullptr);
        const FilePtr closer(file);
        CPPUNIT_ASSERT_EQUAL(size_t{719}, std::strlen(file->meta->link));
        CPPUNIT_ASSERT_MESSAGE(error_text(),
            tsk_fs_file_open_meta(o.fs.get(), file,
                entry_of(manifest, "/small.txt").inode) == file);
        CPPUNIT_ASSERT_MESSAGE(
            "a regular file with a target", file->meta->link == nullptr);
        CPPUNIT_ASSERT_MESSAGE(error_text(),
            tsk_fs_file_open_meta(o.fs.get(), file,
                entry_of(manifest, "/symlink-short").inode) == file);
        CPPUNIT_ASSERT_EQUAL(std::string("small.txt"), target_of(file));
    }

    // Device files and FIFOs have their type, in the inode and (with ftype)
    // in the directory entry, the Manifest's mode and owner, and no
    // content.
    void testSpecialFileTypes()
    {
        struct Special {
            const char *path;
            TSK_FS_META_TYPE_ENUM meta;
            TSK_FS_NAME_TYPE_ENUM name;
        };
        const Special specials[] = {
            {"/fifo", TSK_FS_META_TYPE_FIFO, TSK_FS_NAME_TYPE_FIFO},
            {"/chardev", TSK_FS_META_TYPE_CHR, TSK_FS_NAME_TYPE_CHR},
            {"/blockdev", TSK_FS_META_TYPE_BLK, TSK_FS_NAME_TYPE_BLK},
            {"/symlink-short", TSK_FS_META_TYPE_LNK, TSK_FS_NAME_TYPE_LNK},
        };
        for (const Fixture &f : ALL_FIXTURES) {
            const std::string v = f.variant;
            const tskval::Manifest manifest = read_manifest(f);
            const OpenedFs o(image_path(f));
            for (const Special &s : specials) {
                const tskval::Entry want = entry_of(manifest, s.path);
                const FilePtr file = open_path(o.fs.get(), s.path);
                CPPUNIT_ASSERT_EQUAL_MESSAGE(
                    v + s.path, s.meta, file->meta->type);
                CPPUNIT_ASSERT_EQUAL_MESSAGE(v + s.path,
                    f.ftype ? s.name : TSK_FS_NAME_TYPE_UNDEF,
                    file->name->type);
                CPPUNIT_ASSERT_EQUAL_MESSAGE(v + s.path,
                    static_cast<TSK_INUM_T>(want.inode), file->meta->addr);
                CPPUNIT_ASSERT_EQUAL_MESSAGE(v + s.path, want.mode,
                    static_cast<uint32_t>(file->meta->mode));
                CPPUNIT_ASSERT_EQUAL_MESSAGE(v + s.path, want.uid,
                    static_cast<uint32_t>(file->meta->uid));
                CPPUNIT_ASSERT_EQUAL_MESSAGE(v + s.path, want.gid,
                    static_cast<uint32_t>(file->meta->gid));
                if (s.meta == TSK_FS_META_TYPE_LNK) {
                    continue;
                }
                CPPUNIT_ASSERT_EQUAL_MESSAGE(
                    v + s.path, TSK_OFF_T{0}, file->meta->size);
                const TSK_FS_ATTR *attr = tsk_fs_file_attr_get(file.get());
                CPPUNIT_ASSERT_MESSAGE(
                    v + s.path + ": " + error_text(), attr != nullptr);
                CPPUNIT_ASSERT_EQUAL_MESSAGE(
                    v + s.path, TSK_OFF_T{0}, attr->size);
            }
        }
    }

    // A socket (the Fixtures have none: mkfs.xfs can't make one). /fifo of
    // a copy of v4 (inode 138: block 8, index 10, 256-byte inodes) with a
    // socket's mode.
    void testSocket()
    {
        const size_t inode = 8 * 4096 + 10 * 256;
        std::vector<char> image = image_start(fstest::V4);
        CPPUNIT_ASSERT_EQUAL_MESSAGE(
            "not the FIFO xfs_db shows", '\x11', image[inode + DI_MODE]);
        image[inode + DI_MODE] = '\xc1'; // 0140600
        const OpenedFs o(write_image("socket", image), TSK_IMG_TYPE_RAW);
        const FilePtr file = open_inode(o.fs.get(), 138);
        CPPUNIT_ASSERT_EQUAL(TSK_FS_META_TYPE_SOCK, file->meta->type);
        CPPUNIT_ASSERT_EQUAL(
            uint32_t{0600}, static_cast<uint32_t>(file->meta->mode));
    }

    // Timestamps at their limits, to the nanosecond: the epoch, before
    // 1970, both ends of the classic range and, on v5 with bigtime, after
    // 2038 up to the bigtime maximum. Every time of each file is the same
    // instant (the Fixture generator sets them so); crtime exists on v5
    // only, where a crtime of exactly 0 is the epoch.
    void testTimestampLimits()
    {
        struct Instant {
            const char *path;
            int64_t sec;
            uint32_t nsec;
            bool bigtime; // v5 Fixtures only
        };
        const Instant instants[] = {
            {"/times/epoch", 0, 0, false},
            {"/times/pre-1970", -1000000000, 5, false},
            {"/times/int32-min", INT64_C(-2147483648), 0, false},
            {"/times/int32-max", INT64_C(2147483647), 999999999, false},
            {"/times/post-2038", INT64_C(4102444800), 1, true},
            // The kernel's last bigtime timestamp (2486).
            {"/times/bigtime-max", INT64_C(16299260424), 999999999, true},
        };
        for (const Fixture &f : ALL_FIXTURES) {
            const std::string v = f.variant;
            const OpenedFs o(image_path(f));
            for (const Instant &i : instants) {
                if (i.bigtime && f.version != 5) {
                    continue;
                }
                const FilePtr file = open_path(o.fs.get(), i.path);
                const TSK_FS_META *m = file->meta;
                const std::string at = v + i.path;
                const auto sec = [](time_t t) {
                    return static_cast<int64_t>(t);
                };
                CPPUNIT_ASSERT_EQUAL_MESSAGE(
                    at + " mtime", i.sec, sec(m->mtime));
                CPPUNIT_ASSERT_EQUAL_MESSAGE(
                    at + " mtime ns", i.nsec, m->mtime_nano);
                CPPUNIT_ASSERT_EQUAL_MESSAGE(
                    at + " ctime", i.sec, sec(m->ctime));
                CPPUNIT_ASSERT_EQUAL_MESSAGE(
                    at + " ctime ns", i.nsec, m->ctime_nano);
                CPPUNIT_ASSERT_EQUAL_MESSAGE(
                    at + " atime", i.sec, sec(m->atime));
                CPPUNIT_ASSERT_EQUAL_MESSAGE(
                    at + " atime ns", i.nsec, m->atime_nano);
                CPPUNIT_ASSERT_EQUAL_MESSAGE(at + " crtime",
                    f.version == 5 ? i.sec : int64_t{0}, sec(m->crtime));
                CPPUNIT_ASSERT_EQUAL_MESSAGE(at + " crtime ns",
                    f.version == 5 ? i.nsec : uint32_t{0}, m->crtime_nano);
            }
        }
    }

    // Hard links: two names of one inode open the same inode number, with
    // its link count. A copy of v4 whose root directory (a block directory
    // in block 20) names /setuid's inode (135) for /setgid too, with a link
    // count of 2.
    void testHardLinks()
    {
        std::vector<char> image = image_start(fstest::V4);
        const std::string block(&image[20 * 4096], 4096);
        // An entry: the inode number (8 bytes), the name length, the name.
        const size_t name = block.find("\x06setgid");
        CPPUNIT_ASSERT_MESSAGE("no setgid in the root directory xfs_db shows",
            name != std::string::npos && name >= 8);
        put64(image, 20 * 4096 + name - 8, 135);
        put32(image, 8 * 4096 + 7 * 256 + DI_NLINK, 2);
        const OpenedFs o(write_image("hard-links", image), TSK_IMG_TYPE_RAW);
        const FilePtr a = open_path(o.fs.get(), "/setuid");
        const FilePtr b = open_path(o.fs.get(), "/setgid");
        CPPUNIT_ASSERT_EQUAL(TSK_INUM_T{135}, a->meta->addr);
        CPPUNIT_ASSERT_EQUAL(TSK_INUM_T{135}, b->meta->addr);
        CPPUNIT_ASSERT_EQUAL(TSK_INUM_T{135}, b->name->meta_addr);
        CPPUNIT_ASSERT_EQUAL(2, a->meta->nlink);
        CPPUNIT_ASSERT_EQUAL(2, b->meta->nlink);
        CPPUNIT_ASSERT_EQUAL(04755, static_cast<int>(b->meta->mode));
        // A file with one name.
        const FilePtr single = open_path(o.fs.get(), "/small.txt");
        CPPUNIT_ASSERT_EQUAL(1, single->meta->nlink);
    }
};

CPPUNIT_TEST_SUITE_REGISTRATION(XfsLinkTest);

} // namespace
