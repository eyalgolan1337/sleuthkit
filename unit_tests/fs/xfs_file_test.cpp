/*
 * XFS file content (#42), on the Fixture images through the public API:
 * files whose extent map is a B+tree, reads that start or end inside a hole
 * or an unwritten extent, files larger than 4 GiB and reflinked (shared)
 * blocks. The last three use patched copies of the start of the v4 image,
 * as the Fixture images can't hold them (test/vm/README.md, "XFS fixture
 * images").
 */
#include "fs_test_util.h"
#include "xfs_fixture_data.h"

#include "sha256.h"

#include <tsk/libtsk.h>

#include <cppunit/extensions/HelperMacros.h>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using fstest::ALL_FIXTURES;
using fstest::error_text;
using fstest::Fixture;
using fstest::FsPtr;
using fstest::image_path;
using fstest::ImgPtr;
using fstest::open_image;

// /fragmented.bin (xfs-fixtures.py): FRAG_EXTENTS chunks of FRAG_CHUNK
// bytes, chunk i at 2 * i * FRAG_UNIT, holes between; FRAG_SIZE bytes.
const TSK_OFF_T FRAG_UNIT = 65536;
const int FRAG_EXTENTS = 300;
const size_t FRAG_CHUNK = 100;
const TSK_OFF_T FRAG_SIZE = (2 * FRAG_EXTENTS - 1) * FRAG_UNIT;
const TSK_OFF_T GIB_4 = TSK_OFF_T{1} << 32;

// The generator's content of `n` bytes from `seed` (xfs-fixtures.py,
// pattern()): the SHA-256 digests of "<seed>/0", "<seed>/1", ... one after
// the other.
std::string
pattern(const std::string &seed, size_t n)
{
    std::string out;
    for (int i = 0; out.size() < n; ++i) {
        tskval::Sha256 h;
        const std::string in = seed + "/" + std::to_string(i);
        h.update(in.data(), in.size());
        const std::string hex = h.hex_digest();
        for (size_t k = 0; k < hex.size(); k += 2) {
            out += static_cast<char>(std::stoi(hex.substr(k, 2), nullptr, 16));
        }
    }
    return out.substr(0, n);
}

std::string
zeros(size_t n)
{
    return std::string(n, '\0');
}

// `len` bytes of `file` at `off`; asserts that exactly `want` bytes were
// read (the rest of a read past the end is not returned).
std::string
read_at(TSK_FS_FILE *file, TSK_OFF_T off, size_t len, size_t want)
{
    std::string buf(len, 'x');
    tsk_error_reset();
    const ssize_t got =
        tsk_fs_file_read(file, off, &buf[0], len, TSK_FS_FILE_READ_FLAG_NONE);
    CPPUNIT_ASSERT_EQUAL_MESSAGE(
        "read at " + std::to_string(off) + ": " + error_text(),
        static_cast<ssize_t>(want), got);
    return buf.substr(0, want);
}

std::string
read_at(TSK_FS_FILE *file, TSK_OFF_T off, size_t len)
{
    return read_at(file, off, len, len);
}

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
fstest::FilePtr
open_path(TSK_FS_INFO *fs, const std::string &path)
{
    tsk_error_reset();
    fstest::FilePtr file(tsk_fs_file_open(fs, nullptr, path.c_str()));
    CPPUNIT_ASSERT_MESSAGE(path + ": " + error_text(), file != nullptr);
    return file;
}

// The blocks of a file walk (TSK_FS_FILE_WALK_FLAG_AONLY): the file offset
// of each, and whether it was read from disk or reported as sparse.
struct WalkedBlock {
    TSK_OFF_T off;
    bool raw;
    bool sparse;
};

TSK_WALK_RET_ENUM
collect_blocks(TSK_FS_FILE *, TSK_OFF_T off, TSK_DADDR_T, char *, size_t,
    TSK_FS_BLOCK_FLAG_ENUM flags, void *ptr)
{
    static_cast<std::vector<WalkedBlock> *>(ptr)->push_back(
        {off, (flags & TSK_FS_BLOCK_FLAG_RAW) != 0,
            (flags & TSK_FS_BLOCK_FLAG_SPARSE) != 0});
    return TSK_WALK_CONT;
}

// The patched copies: the first MiB of the v4 Fixture (xfs_db): 4 KiB
// blocks, 256-byte inodes, 16 per block; v2 inodes, whose data fork starts
// 100 bytes in. In it: /small.txt is inode 132 (1 extent, block 12),
// /multiblock.bin inode 133 (14336 bytes, 1 extent: file blocks 0-3 at
// block 13), /sparse.bin inode 134 (2 extents: file block 32 at block 17,
// file block 80 at block 65), /setuid inode 135 (1 extent, block 18),
// /fragmented.bin inode 186 (an extent B+tree).
const size_t V4_BLOCK = 4096;
const size_t CORE_V2 = 100;
const size_t DI_FORMAT = 5, DI_SIZE = 56, DI_NEXTENTS = 76, DI_FLAGS = 90;
const int FMT_BTREE = 3;        // di_format
const char DIFLAG_REALTIME = 1; // in the low byte of di_flags

size_t
inode_offset(TSK_INUM_T inum)
{
    return (inum >> 4) * V4_BLOCK + (inum & 15) * 256;
}

class XfsFileTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(XfsFileTest);
    CPPUNIT_TEST(testExtentTreeFileReadsAroundHoles);
    CPPUNIT_TEST(testExtentTreeFileHolesAreSparse);
    CPPUNIT_TEST(testIstatShowsExtentTree);
    CPPUNIT_TEST(testRealtimeExtentTreeNotRead);
    CPPUNIT_TEST(testReadsAcrossAndAbove4GiB);
    CPPUNIT_TEST(testReadsInsideUnwrittenExtent);
    CPPUNIT_TEST(testSharedBlocksReadInEachFile);
    CPPUNIT_TEST_SUITE_END();

  public:
    // /fragmented.bin, whose extent map is a B+tree (on v4-512 with a node
    // level), reads as the generator wrote it, with reads that start and
    // end inside holes and in the middle of blocks: around each chunk, inside
    // each hole, and across the end of the file.
    void testExtentTreeFileReadsAroundHoles()
    {
        for (const Fixture &f : ALL_FIXTURES) {
            const std::string v = f.variant;
            OpenedFs o(image_path(f));
            fstest::FilePtr file = open_path(o.fs.get(), "/fragmented.bin");
            CPPUNIT_ASSERT_EQUAL_MESSAGE(v, FRAG_SIZE, file->meta->size);
            for (int i = 0; i < FRAG_EXTENTS; ++i) {
                const std::string msg = v + ": chunk " + std::to_string(i);
                const std::string chunk =
                    pattern("fragment-" + std::to_string(i), FRAG_CHUNK);
                const TSK_OFF_T at = 2 * i * FRAG_UNIT;
                if (i == 0) {
                    CPPUNIT_ASSERT_MESSAGE(
                        msg, read_at(file.get(), 0, 150) == chunk + zeros(50));
                }
                else {
                    CPPUNIT_ASSERT_MESSAGE(msg,
                        read_at(file.get(), at - 50, 200) ==
                            zeros(50) + chunk + zeros(50));
                }
                CPPUNIT_ASSERT_MESSAGE(msg,
                    read_at(file.get(), at + 17, 30) == chunk.substr(17, 30));
                if (i + 1 < FRAG_EXTENTS) {
                    CPPUNIT_ASSERT_MESSAGE(msg + ": hole",
                        read_at(file.get(), at + FRAG_UNIT + 333, 5000) ==
                            zeros(5000));
                }
            }
            CPPUNIT_ASSERT_MESSAGE(v + ": end",
                read_at(file.get(), FRAG_SIZE - 10, 100, 10) == zeros(10));
        }
    }

    // A file walk of /fragmented.bin reads each chunk's blocks from disk and
    // reports every other block as sparse: blocks read are in the units that
    // hold a chunk, one at the start of each.
    void testExtentTreeFileHolesAreSparse()
    {
        for (const Fixture &f : ALL_FIXTURES) {
            const std::string v = f.variant;
            OpenedFs o(image_path(f));
            fstest::FilePtr file = open_path(o.fs.get(), "/fragmented.bin");
            std::vector<WalkedBlock> blocks;
            CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": " + error_text(), 0,
                static_cast<int>(tsk_fs_file_walk(file.get(),
                    TSK_FS_FILE_WALK_FLAG_AONLY, collect_blocks, &blocks)));
            CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": blocks",
                static_cast<size_t>(
                    (FRAG_SIZE + f.block_size - 1) / f.block_size),
                blocks.size());
            std::vector<bool> chunk_start(FRAG_EXTENTS, false);
            for (const WalkedBlock &b : blocks) {
                const std::string msg =
                    v + ": block at " + std::to_string(b.off);
                CPPUNIT_ASSERT_MESSAGE(msg, b.raw != b.sparse);
                if (b.raw) {
                    CPPUNIT_ASSERT_MESSAGE(msg, (b.off / FRAG_UNIT) % 2 == 0);
                    if (b.off % FRAG_UNIT == 0) {
                        chunk_start[b.off / FRAG_UNIT / 2] = true;
                    }
                }
                else {
                    CPPUNIT_ASSERT_MESSAGE(msg,
                        b.off % FRAG_UNIT != 0 || (b.off / FRAG_UNIT) % 2 == 1);
                }
            }
            CPPUNIT_ASSERT_MESSAGE(v + ": a chunk without a block",
                std::vector<bool>(FRAG_EXTENTS, true) == chunk_start);
        }
    }

    // istat shows the whole extent map of /fragmented.bin: a block address
    // (0 for a hole) for every block of the file, the addresses the file
    // walk reads; and with TSK_FS_ISTAT_RUNLIST one run per extent and one
    // sparse run per hole. The extents (xfs_db bmap): 300, on v4-512 8
    // blocks each from block 105 to 76649, on v5 one block each from block
    // 42 to 9610.
    void testIstatShowsExtentTree()
    {
        struct Want {
            const Fixture &f;
            const char *first_run;
            const char *last_run;
            TSK_DADDR_T first, last;
        };
        const Want wants[] = {
            {fstest::V4_512, "  Starting address: 105, length: 8  \n",
                "  Starting address: 76649, length: 8  \n", 105, 76656},
            {fstest::V5, "  Starting address: 42, length: 1  \n",
                "  Starting address: 9610, length: 1  \n", 42, 9610},
        };
        for (const Want &w : wants) {
            const std::string v = w.f.variant;
            OpenedFs o(image_path(w.f));
            fstest::FilePtr file = open_path(o.fs.get(), "/fragmented.bin");
            const TSK_INUM_T inum = file->meta->addr;

            const std::string text = fstest::istat_text(o.fs.get(), inum);
            const size_t at = text.find("\nDirect Blocks:\n");
            CPPUNIT_ASSERT_MESSAGE(std::string(v).append(": ").append(text),
                at != std::string::npos);
            std::istringstream in(text.substr(at + 16));
            std::vector<TSK_DADDR_T> listed;
            size_t all = 0;
            TSK_DADDR_T addr = 0;
            while (in >> addr) {
                ++all;
                if (addr != 0) {
                    listed.push_back(addr);
                }
            }
            CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": addresses",
                static_cast<size_t>(
                    (FRAG_SIZE + w.f.block_size - 1) / w.f.block_size),
                all);
            const std::vector<TSK_DADDR_T> walked =
                fstest::file_blocks(file.get());
            CPPUNIT_ASSERT_MESSAGE(v + ": blocks", walked == listed);
            CPPUNIT_ASSERT_EQUAL(w.first, listed.front());
            CPPUNIT_ASSERT_EQUAL(w.last, listed.back());

            const std::string runs =
                fstest::istat_text(o.fs.get(), inum, TSK_FS_ISTAT_RUNLIST);
            size_t data = 0, sparse = 0;
            std::istringstream lines(runs.substr(runs.find("Direct Blocks:")));
            std::string line;
            while (std::getline(lines, line)) {
                if (line.find("Starting address: X,") != std::string::npos &&
                    line.find("Sparse") != std::string::npos) {
                    ++sparse;
                }
                else if (line.find("Starting address: ") != std::string::npos) {
                    ++data;
                }
            }
            CPPUNIT_ASSERT_EQUAL_MESSAGE(
                v + ": data runs", size_t{FRAG_EXTENTS}, data);
            CPPUNIT_ASSERT_EQUAL_MESSAGE(
                v + ": sparse runs", size_t{FRAG_EXTENTS}, sparse);
            CPPUNIT_ASSERT_MESSAGE(
                std::string(v).append(": first run\n").append(runs),
                runs.find(std::string("Direct Blocks:\n") + w.first_run) !=
                    std::string::npos);
            CPPUNIT_ASSERT_MESSAGE(
                std::string(v).append(": last run\n").append(runs),
                runs.find(w.last_run) != std::string::npos);
        }
    }

    // A file on the realtime device has extents that are not in the image,
    // whether its extent map is a list (XfsInodeTest) or a B+tree: reading
    // it is not supported, rather than reading the data device's blocks of
    // the same numbers. /fragmented.bin with XFS_DIFLAG_REALTIME.
    void testRealtimeExtentTreeNotRead()
    {
        std::vector<char> image = v4_start();
        const size_t inode = inode_offset(186);
        CPPUNIT_ASSERT_EQUAL_MESSAGE("not the B+tree file xfs_db shows",
            FMT_BTREE, static_cast<int>(image[inode + DI_FORMAT]));
        image[inode + DI_FLAGS + 1] =
            static_cast<char>(image[inode + DI_FLAGS + 1] | DIFLAG_REALTIME);
        OpenedFs o(write_image("realtime-btree", image), TSK_IMG_TYPE_RAW);
        fstest::FilePtr file(tsk_fs_file_open_meta(o.fs.get(), nullptr, 186));
        CPPUNIT_ASSERT_MESSAGE(error_text(), file != nullptr);
        char buf[16];
        tsk_error_reset();
        CPPUNIT_ASSERT_EQUAL(ssize_t{-1},
            tsk_fs_file_read(
                file.get(), 0, buf, sizeof(buf), TSK_FS_FILE_READ_FLAG_NONE));
        CPPUNIT_ASSERT_EQUAL_MESSAGE(error_text(),
            static_cast<uint32_t>(TSK_ERR_FS_UNSUPFUNC), tsk_error_get_errno());
        tsk_error_reset();
    }

    // Files larger than 4 GiB read right below, across and above 4 GiB,
    // and far above it. Patched copy: /multiblock.bin's extent moved to file
    // block 2^20 - 2, so it spans 4 GiB, and /sparse.bin's second extent
    // ('sparse-b') moved to 1 TiB, the sizes to match.
    void testReadsAcrossAndAbove4GiB()
    {
        std::vector<char> image = v4_start();
        const TSK_OFF_T mb_start = GIB_4 - 2 * V4_BLOCK;
        const size_t mb_size = 14336;
        set_extent(image, 133, 0, mb_start / V4_BLOCK, 13, 4, false);
        put64(image, inode_offset(133) + DI_SIZE, mb_start + mb_size);
        const TSK_OFF_T tib = TSK_OFF_T{1} << 40;
        set_extent(image, 134, 1, tib / V4_BLOCK, 65, 1, false);
        put64(image, inode_offset(134) + DI_SIZE, tib + 65536);
        OpenedFs o(write_image("4gib", image), TSK_IMG_TYPE_RAW);

        const std::string mb = pattern("multiblock", mb_size);
        fstest::FilePtr big(tsk_fs_file_open_meta(o.fs.get(), nullptr, 133));
        CPPUNIT_ASSERT_MESSAGE(error_text(), big != nullptr);
        CPPUNIT_ASSERT_EQUAL(mb_start + TSK_OFF_T{14336}, big->meta->size);
        CPPUNIT_ASSERT_MESSAGE("hole below",
            read_at(big.get(), GIB_4 / 2 + 3, 4096) == zeros(4096));
        CPPUNIT_ASSERT_MESSAGE("below",
            read_at(big.get(), mb_start - 10, 20) ==
                zeros(10) + mb.substr(0, 10));
        CPPUNIT_ASSERT_MESSAGE("across",
            read_at(big.get(), GIB_4 - 50, 100) == mb.substr(8192 - 50, 100));
        CPPUNIT_ASSERT_MESSAGE(
            "whole extent", read_at(big.get(), mb_start, mb_size) == mb);
        CPPUNIT_ASSERT_MESSAGE("above",
            read_at(big.get(), GIB_4 + 4096 + 7, 2000) ==
                mb.substr(12288 + 7, 2000));
        CPPUNIT_ASSERT_MESSAGE("past the end",
            read_at(big.get(), GIB_4 + 6000, 1000, 144) ==
                mb.substr(14336 - 144));

        fstest::FilePtr huge(tsk_fs_file_open_meta(o.fs.get(), nullptr, 134));
        CPPUNIT_ASSERT_MESSAGE(error_text(), huge != nullptr);
        CPPUNIT_ASSERT_EQUAL(tib + 65536, huge->meta->size);
        const std::string a = pattern("sparse-a", 1000);
        const std::string b = pattern("sparse-b", 1000);
        CPPUNIT_ASSERT_MESSAGE("first extent",
            read_at(huge.get(), 32 * 4096 - 24, 1024) == zeros(24) + a);
        CPPUNIT_ASSERT_MESSAGE("hole across 4 GiB",
            read_at(huge.get(), GIB_4 - 4000, 8000) == zeros(8000));
        CPPUNIT_ASSERT_MESSAGE(
            "1 TiB", read_at(huge.get(), tib - 500, 1500) == zeros(500) + b);
        CPPUNIT_ASSERT_MESSAGE("1 TiB, inside",
            read_at(huge.get(), tib + 1000, 4000) == zeros(4000));
    }

    // A read that starts or ends inside an unwritten extent returns zeros
    // for that part, at any byte offset. Patched copy: /multiblock.bin's
    // extent split in three, file block 0 written (block 13), 1-2 unwritten
    // (blocks 14-15, which hold its data) and 3 written (block 16).
    void testReadsInsideUnwrittenExtent()
    {
        std::vector<char> image = v4_start();
        set_extent(image, 133, 0, 0, 13, 1, false);
        set_extent(image, 133, 1, 1, 14, 2, true);
        set_extent(image, 133, 2, 3, 16, 1, false);
        put32(image, inode_offset(133) + DI_NEXTENTS, 3);
        OpenedFs o(write_image("unwritten-split", image), TSK_IMG_TYPE_RAW);
        fstest::FilePtr file(tsk_fs_file_open_meta(o.fs.get(), nullptr, 133));
        CPPUNIT_ASSERT_MESSAGE(error_text(), file != nullptr);
        const std::string mb = pattern("multiblock", 14336);
        CPPUNIT_ASSERT_MESSAGE("written into unwritten",
            read_at(file.get(), 100, 8192) ==
                mb.substr(100, 4096 - 100) + zeros(8192 - 3996));
        CPPUNIT_ASSERT_MESSAGE(
            "inside unwritten", read_at(file.get(), 5000, 1000) == zeros(1000));
        CPPUNIT_ASSERT_MESSAGE("unwritten into written",
            read_at(file.get(), 12288 - 300, 1000) ==
                zeros(300) + mb.substr(12288, 700));
        CPPUNIT_ASSERT_MESSAGE("whole file",
            read_at(file.get(), 0, 14336) ==
                mb.substr(0, 4096) + zeros(8192) + mb.substr(12288));
        const std::vector<TSK_DADDR_T> blocks = fstest::file_blocks(file.get());
        CPPUNIT_ASSERT_MESSAGE(
            "blocks read", blocks == std::vector<TSK_DADDR_T>({13, 16}));
    }

    // Blocks shared by several files (reflinks) read the same in each.
    // Patched copy: /setuid maps all of /multiblock.bin's blocks (13-16),
    // /small.txt only its second (14).
    void testSharedBlocksReadInEachFile()
    {
        std::vector<char> image = v4_start();
        set_extent(image, 135, 0, 0, 13, 4, false);
        put64(image, inode_offset(135) + DI_SIZE, 14336);
        set_extent(image, 132, 0, 0, 14, 1, false);
        put64(image, inode_offset(132) + DI_SIZE, 4096);
        OpenedFs o(write_image("shared", image), TSK_IMG_TYPE_RAW);
        const std::string mb = pattern("multiblock", 14336);
        fstest::FilePtr source(tsk_fs_file_open_meta(o.fs.get(), nullptr, 133));
        fstest::FilePtr clone(tsk_fs_file_open_meta(o.fs.get(), nullptr, 135));
        fstest::FilePtr part(tsk_fs_file_open_meta(o.fs.get(), nullptr, 132));
        CPPUNIT_ASSERT_MESSAGE(error_text(),
            source != nullptr && clone != nullptr && part != nullptr);
        CPPUNIT_ASSERT_MESSAGE("source", read_at(source.get(), 0, 14336) == mb);
        CPPUNIT_ASSERT_MESSAGE("clone", read_at(clone.get(), 0, 14336) == mb);
        CPPUNIT_ASSERT_MESSAGE(
            "part", read_at(part.get(), 0, 4096) == mb.substr(4096, 4096));
        CPPUNIT_ASSERT_MESSAGE("clone blocks",
            fstest::file_blocks(clone.get()) ==
                fstest::file_blocks(source.get()));
        CPPUNIT_ASSERT_MESSAGE("part blocks",
            fstest::file_blocks(part.get()) == std::vector<TSK_DADDR_T>({14}));
    }

  private:
    // Writes extent record `index` of v4 inode `inum`'s data fork
    // (xfs_bmbt_rec: the unwritten flag, the file offset, the start block
    // and the length, 1, 54, 52 and 21 bits from the top).
    static void set_extent(std::vector<char> &image, TSK_INUM_T inum,
        size_t index, uint64_t startoff, uint64_t fsblock, uint64_t count,
        bool unwritten)
    {
        const size_t rec = inode_offset(inum) + CORE_V2 + 16 * index;
        put64(image, rec,
            (uint64_t{unwritten} << 63) | (startoff << 9) | (fsblock >> 43));
        put64(image, rec + 8, (fsblock << 21) | count);
    }

    static void put64(std::vector<char> &b, size_t off, uint64_t v)
    {
        for (size_t i = 0; i < 8; ++i) {
            b[off + i] = static_cast<char>(v >> (56 - 8 * i));
        }
    }

    static void put32(std::vector<char> &b, size_t off, uint32_t v)
    {
        for (size_t i = 0; i < 4; ++i) {
            b[off + i] = static_cast<char>(v >> (24 - 8 * i));
        }
    }

    // The first MiB of the v4 Fixture image.
    static std::vector<char> v4_start()
    {
        const std::string path = image_path(fstest::V4);
        std::ifstream in(path, std::ios::binary);
        std::vector<char> b(1 << 20);
        in.read(b.data(), static_cast<std::streamsize>(b.size()));
        CPPUNIT_ASSERT_MESSAGE(path + " is short", in.good());
        return b;
    }

    // Writes `bytes` as xfs-file-<name>.img in the test data directory and
    // returns its path. The file system is cut short, which libtsk accepts.
    static std::string write_image(
        const std::string &name, const std::vector<char> &bytes)
    {
        const std::string path = fstest::data_path("xfs-file-" + name + ".img");
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        CPPUNIT_ASSERT_MESSAGE("cannot write " + path, out.good());
        return path;
    }
};

CPPUNIT_TEST_SUITE_REGISTRATION(XfsFileTest);

} // namespace
