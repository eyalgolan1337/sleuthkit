/*
 * XFS inode and block walks (#44), on the Fixture images through the public
 * API: the inode walk visits the inodes of the expected Manifest plus the
 * file system's internal inodes, each once, with the walk flags and ranges
 * honoured; ifind -d finds the owner of a file's block; the block walk covers
 * every block once, each reported as allocated.
 */
#include "fs_test_util.h"
#include "manifest_check.h"
#include "xfs_fixture_data.h"

#include "manifest.h"

#include <tsk/libtsk.h>

#include <cppunit/extensions/HelperMacros.h>

#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
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
using fstest::read_manifest;
using fstest::V4;
using fstest::V4_512;
using fstest::V5;
using fstest::V5_64K;

// An inode the walk visited: its number and its flags.
struct Visited {
    TSK_INUM_T inum;
    unsigned flags;
};

// What a meta walk callback records, and when it stops.
struct MetaWalk {
    std::vector<Visited> visited;
    size_t stop_after = 0; // 0: never
    TSK_WALK_RET_ENUM stop_with = TSK_WALK_STOP;
};

TSK_WALK_RET_ENUM
record_inode(TSK_FS_FILE *file, void *ptr)
{
    MetaWalk &w = *static_cast<MetaWalk *>(ptr);
    w.visited.push_back({file->meta->addr, file->meta->flags});
    return w.visited.size() == w.stop_after ? w.stop_with : TSK_WALK_CONT;
}

// tsk_fs_meta_walk from `start` to `end` with `flags`; asserts that it
// succeeded and returns the visited inodes.
std::vector<Visited>
walk_inodes(TSK_FS_INFO *fs, TSK_INUM_T start, TSK_INUM_T end, unsigned flags,
    const std::string &what)
{
    MetaWalk w;
    tsk_error_reset();
    const uint8_t rc = tsk_fs_meta_walk(fs, start, end,
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): flags
        static_cast<TSK_FS_META_FLAG_ENUM>(flags), record_inode, &w);
    CPPUNIT_ASSERT_EQUAL_MESSAGE(
        what + ": " + error_text(), 0, static_cast<int>(rc));
    return w.visited;
}

// The inode numbers of `visited`, as a set.
std::set<uint64_t>
numbers(const std::vector<Visited> &visited)
{
    std::set<uint64_t> out;
    for (const Visited &v : visited) {
        out.insert(v.inum);
    }
    return out;
}

// The Fixture's allocated inodes: the Manifest's and its internal inodes.
std::set<uint64_t>
allocated_inodes(const Fixture &f)
{
    std::set<uint64_t> out(
        std::begin(f.internal_inodes), std::end(f.internal_inodes));
    for (const tskval::Entry &e : read_manifest(f).entries) {
        out.insert(e.inode);
    }
    return out;
}

// The members of `s` from `lo` to `hi`.
std::set<uint64_t>
in_range(const std::set<uint64_t> &s, uint64_t lo, uint64_t hi)
{
    return std::set<uint64_t>(s.lower_bound(lo), s.upper_bound(hi));
}

std::string
show(const std::set<uint64_t> &s)
{
    std::string out;
    for (const uint64_t n : s) {
        out += (out.empty() ? "" : " ") + std::to_string(n);
    }
    return "{" + out + "}";
}

// The first `n` bytes of the Fixture image.
std::vector<char>
image_start(const Fixture &f, size_t n)
{
    std::ifstream in(image_path(f), std::ios::binary);
    std::vector<char> b(n);
    in.read(b.data(), static_cast<std::streamsize>(n));
    CPPUNIT_ASSERT_MESSAGE(image_path(f) + " is short", in.good());
    return b;
}

// Writes `image` as xfs-walk-<name>.img in the test data directory and opens
// it as XFS; `img` keeps the image open. The file system is cut short, which
// libtsk accepts (last_block_act).
FsPtr
open_copy(const std::string &name, const std::vector<char> &image, ImgPtr &img)
{
    const std::string path = fstest::data_path("xfs-walk-" + name + ".img");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(image.data(), static_cast<std::streamsize>(image.size()));
        CPPUNIT_ASSERT_MESSAGE("cannot write " + path, out.good());
    }
    img = open_image(path, TSK_IMG_TYPE_RAW);
    FsPtr fs(tsk_fs_open_img(img.get(), 0, TSK_FS_TYPE_XFS));
    CPPUNIT_ASSERT_MESSAGE(name + ": " + error_text(), fs != nullptr);
    return fs;
}

// Big-endian stores in an image copy.
void
put16(std::vector<char> &b, size_t off, uint16_t v)
{
    b[off] = static_cast<char>(v >> 8);
    b[off + 1] = static_cast<char>(v);
}
void
put32(std::vector<char> &b, size_t off, uint32_t v)
{
    put16(b, off, static_cast<uint16_t>(v >> 16));
    put16(b, off + 2, static_cast<uint16_t>(v));
}
void
put64(std::vector<char> &b, size_t off, uint64_t v)
{
    put32(b, off, static_cast<uint32_t>(v >> 32));
    put32(b, off + 4, static_cast<uint32_t>(v));
}

// What tsk_fs_ifind_data prints to standard output for `block` (ifind -d),
// and whether it succeeded. POSIX: standard output goes to printed_text's
// temporary file while it runs.
std::string
ifind_data(TSK_FS_INFO *fs, TSK_DADDR_T block, bool &ok)
{
    return fstest::printed_text(
        [&](FILE *f) {
            fflush(stdout);
            const int saved = dup(STDOUT_FILENO);
            if (saved < 0 || dup2(fileno(f), STDOUT_FILENO) < 0) {
                CPPUNIT_FAIL("cannot redirect standard output");
                return 1;
            }
            tsk_error_reset();
            const int rc = tsk_fs_ifind_data(fs, TSK_FS_IFIND_NONE, block);
            fflush(stdout);
            dup2(saved, STDOUT_FILENO);
            close(saved);
            return rc;
        },
        ok);
}

// A block the walk visited: its address and its flags.
struct BlockVisit {
    TSK_DADDR_T addr;
    unsigned flags;
};

// What a block walk callback records (and, with `image`, checks), and when
// it stops.
struct BlockWalk {
    std::vector<BlockVisit> visited;
    size_t stop_after = 0; // 0: never
    TSK_WALK_RET_ENUM stop_with = TSK_WALK_STOP;
    std::ifstream *image = nullptr; // compare each block's bytes with it
    size_t differ = 0;              // blocks whose bytes differ
};

TSK_WALK_RET_ENUM
record_block(const TSK_FS_BLOCK *block, void *ptr)
{
    BlockWalk &w = *static_cast<BlockWalk *>(ptr);
    w.visited.push_back({block->addr, block->flags});
    if (w.image != nullptr) {
        const size_t size = block->fs_info->block_size;
        std::vector<char> want(size);
        w.image->seekg(static_cast<std::streamoff>(block->addr * size));
        w.image->read(want.data(), static_cast<std::streamsize>(size));
        if (!w.image->good() ||
            std::string(want.data(), size) != std::string(block->buf, size)) {
            ++w.differ;
        }
    }
    return w.visited.size() == w.stop_after ? w.stop_with : TSK_WALK_CONT;
}

// tsk_fs_block_walk from `start` to `end` with `flags`; returns its result.
uint8_t
walk_blocks(TSK_FS_INFO *fs, TSK_DADDR_T start, TSK_DADDR_T end, unsigned flags,
    BlockWalk &w)
{
    tsk_error_reset();
    return tsk_fs_block_walk(fs, start, end,
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): flags
        static_cast<TSK_FS_BLOCK_WALK_FLAG_ENUM>(flags), record_block, &w);
}

// The allocation and category flags of a block.
unsigned
kind(unsigned flags)
{
    return flags &
        (TSK_FS_BLOCK_FLAG_ALLOC | TSK_FS_BLOCK_FLAG_UNALLOC |
            TSK_FS_BLOCK_FLAG_META | TSK_FS_BLOCK_FLAG_CONT);
}

const unsigned ALLOC = TSK_FS_META_FLAG_ALLOC;
const unsigned UNALLOC = TSK_FS_META_FLAG_UNALLOC;
const unsigned USED = TSK_FS_META_FLAG_USED;
const unsigned UNUSED = TSK_FS_META_FLAG_UNUSED;

class XfsWalkTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(XfsWalkTest);
    CPPUNIT_TEST(testInodeWalkVisitsManifestInodes);
    CPPUNIT_TEST(testInodeWalkFlags);
    CPPUNIT_TEST(testInodeWalkRanges);
    CPPUNIT_TEST(testInodeWalkRangeErrors);
    CPPUNIT_TEST(testInodeWalkCallbackStops);
    CPPUNIT_TEST(testInodeWalkUsedFreeInode);
    CPPUNIT_TEST(testInodeWalkSparseChunk);
    CPPUNIT_TEST(testInodeWalkTreeWithNodes);
    CPPUNIT_TEST(testInodeWalkDamage);
    CPPUNIT_TEST(testIfindData);
    CPPUNIT_TEST(testBlockWalkCoversEveryBlock);
    CPPUNIT_TEST(testBlockWalkFlags);
    CPPUNIT_TEST(testBlockWalkContentAndRanges);
    CPPUNIT_TEST(testBlockWalkRangeErrors);
    CPPUNIT_TEST(testBlockWalkCallbackStops);
    CPPUNIT_TEST(testBlockWalkImageCutShort);
    CPPUNIT_TEST_SUITE_END();

  public:
    // The walk of every allocated inode visits the Manifest's inodes and the
    // realtime bitmap and summary inodes (xfs_db), in ascending order, so
    // each once; each Manifest inode has the Manifest's metadata and
    // content.
    void testInodeWalkVisitsManifestInodes()
    {
        for (const Fixture &f : ALL_FIXTURES) {
            const std::string v = f.variant;
            const tskval::Manifest manifest = read_manifest(f);
            const fstest::MetaWalkCheck walk = fstest::check_meta_walk(
                image_path(f), manifest, fstest::inode_known_failures(f));
            CPPUNIT_ASSERT_MESSAGE(v + ": " + walk.report, walk.passed);

            std::set<uint64_t> expected(
                std::begin(f.internal_inodes), std::end(f.internal_inodes));
            for (const tskval::Entry &e : manifest.entries) {
                expected.insert(e.inode);
            }
            for (size_t i = 1; i < walk.visited.size(); ++i) {
                CPPUNIT_ASSERT_MESSAGE(v + ": inode " +
                        std::to_string(walk.visited[i]) + " after " +
                        std::to_string(walk.visited[i - 1]),
                    walk.visited[i - 1] < walk.visited[i]);
            }
            const std::set<uint64_t> visited(
                walk.visited.begin(), walk.visited.end());
            CPPUNIT_ASSERT_EQUAL_MESSAGE(
                v + ": inodes visited", expected.size(), visited.size());
            CPPUNIT_ASSERT_MESSAGE(v + ": visited set", expected == visited);
        }
    }

    // The walk flags select by allocation and use: the free inodes of the
    // allocated chunks (xfs_db's count, with the Fixture's free inode) are
    // UNALLOC and, never used (ctime 0), UNUSED; allocated inodes are
    // ALLOC | USED. No ALLOC/UNALLOC or no USED/UNUSED flag means both.
    void testInodeWalkFlags()
    {
        for (const Fixture &f : ALL_FIXTURES) {
            const std::string v = f.variant;
            ImgPtr img = open_image(image_path(f));
            FsPtr fs(tsk_fs_open_img(img.get(), 0, TSK_FS_TYPE_DETECT));
            CPPUNIT_ASSERT_MESSAGE(v + ": " + error_text(), fs != nullptr);
            const auto walk = [&](unsigned flags, const std::string &what) {
                return walk_inodes(fs.get(), fs->first_inum, fs->last_inum,
                    flags, std::string(v).append(": ").append(what));
            };
            const std::set<uint64_t> allocated = allocated_inodes(f);

            const std::vector<Visited> free = walk(UNALLOC, "UNALLOC");
            CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": free inodes",
                static_cast<size_t>(f.free_inodes), numbers(free).size());
            CPPUNIT_ASSERT_EQUAL_MESSAGE(
                v + ": free inodes once", free.size(), numbers(free).size());
            CPPUNIT_ASSERT_MESSAGE(v + ": the Fixture's free inode",
                numbers(free).count(f.free_inode));
            for (const Visited &x : free) {
                CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": " + std::to_string(x.inum),
                    UNALLOC | UNUSED, x.flags);
                CPPUNIT_ASSERT_MESSAGE(
                    v + ": " + std::to_string(x.inum) + " is allocated",
                    allocated.count(x.inum) == 0);
            }
            CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": UNALLOC | UNUSED", free.size(),
                walk(UNALLOC | UNUSED, "UNALLOC | UNUSED").size());
            CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": UNALLOC | USED", size_t{0},
                walk(UNALLOC | USED, "UNALLOC | USED").size());
            CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": ALLOC | UNUSED", size_t{0},
                walk(ALLOC | UNUSED, "ALLOC | UNUSED").size());

            const std::vector<Visited> used =
                walk(ALLOC | USED, "ALLOC | USED");
            CPPUNIT_ASSERT_MESSAGE(
                v + ": ALLOC | USED", numbers(used) == allocated);
            for (const Visited &x : used) {
                CPPUNIT_ASSERT_EQUAL_MESSAGE(
                    v + ": " + std::to_string(x.inum), ALLOC | USED, x.flags);
            }

            std::set<uint64_t> every = allocated;
            const std::set<uint64_t> free_set = numbers(free);
            every.insert(free_set.begin(), free_set.end());
            for (const unsigned flags : {0u, ALLOC | UNALLOC, USED | UNUSED}) {
                const std::string what = "flags " + std::to_string(flags);
                CPPUNIT_ASSERT_MESSAGE(std::string(v).append(": ").append(what),
                    numbers(walk(flags, what)) == every);
            }
            // Orphans: free inodes in use that no name refers to; none here.
            CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": ORPHAN", size_t{0},
                walk(TSK_FS_META_FLAG_ORPHAN, "ORPHAN").size());
        }
    }

    // A walk over part of the inode numbers visits only that part: inside one
    // chunk, across AGs, single inodes, numbers that are no inode, and
    // last_inum ($OrphanFiles, which XFS doesn't have) alone or at the end.
    void testInodeWalkRanges()
    {
        for (const Fixture &f : ALL_FIXTURES) {
            const std::string v = f.variant;
            ImgPtr img = open_image(image_path(f));
            FsPtr fs(tsk_fs_open_img(img.get(), 0, TSK_FS_TYPE_DETECT));
            CPPUNIT_ASSERT_MESSAGE(v + ": " + error_text(), fs != nullptr);
            const std::set<uint64_t> allocated = allocated_inodes(f);
            const uint64_t root = fs->root_inum;
            const uint64_t ag1 = uint64_t{1} << f.ag_inode_bits;
            const struct {
                uint64_t lo, hi;
            } ranges[] = {
                {root, root},
                {root + 3, root + 20},
                {0, root - 1},
                {root + 1, ag1 + 5000},
                {ag1, 3 * ag1 - 1},
                {ag1 + 1, ag1 + 1},
                {f.max_inode - 100, fs->last_inum},
                {fs->last_inum, fs->last_inum},
                {f.free_inode, f.free_inode},
            };
            for (const auto &r : ranges) {
                std::string what = v;
                what.append(": range ")
                    .append(std::to_string(r.lo))
                    .append("-")
                    .append(std::to_string(r.hi));
                const std::vector<Visited> got =
                    walk_inodes(fs.get(), r.lo, r.hi, ALLOC, what);
                const std::set<uint64_t> want = in_range(allocated, r.lo, r.hi);
                CPPUNIT_ASSERT_EQUAL_MESSAGE(
                    what, show(want), show(numbers(got)));
                CPPUNIT_ASSERT_EQUAL_MESSAGE(
                    what + " once", want.size(), got.size());
            }
            // The free inode, alone.
            const std::vector<Visited> one = walk_inodes(fs.get(), f.free_inode,
                f.free_inode, UNALLOC, v + ": free inode");
            CPPUNIT_ASSERT_EQUAL_MESSAGE(
                v + ": free inode", size_t{1}, one.size());
            CPPUNIT_ASSERT_EQUAL(
                static_cast<TSK_INUM_T>(f.free_inode), one[0].inum);
        }
    }

    // A range outside the file system's, or backwards, is
    // TSK_ERR_FS_WALK_RNG with a text, and the action is never called.
    void testInodeWalkRangeErrors()
    {
        ImgPtr img = open_image(image_path(V5));
        FsPtr fs(tsk_fs_open_img(img.get(), 0, TSK_FS_TYPE_DETECT));
        CPPUNIT_ASSERT_MESSAGE(error_text(), fs != nullptr);
        const TSK_INUM_T last = fs->last_inum;
        const struct {
            TSK_INUM_T start, end;
        } ranges[] = {{last + 1, last + 1}, {128, last + 1}, {200, 128}};
        for (const auto &r : ranges) {
            MetaWalk w;
            tsk_error_reset();
            CPPUNIT_ASSERT_EQUAL(1,
                static_cast<int>(tsk_fs_meta_walk(fs.get(), r.start, r.end,
                    TSK_FS_META_FLAG_ALLOC, record_inode, &w)));
            CPPUNIT_ASSERT_EQUAL(static_cast<uint32_t>(TSK_ERR_FS_WALK_RNG),
                tsk_error_get_errno());
            CPPUNIT_ASSERT(!error_text().empty());
            CPPUNIT_ASSERT(w.visited.empty());
            tsk_error_reset();
        }
    }

    // An action that returns TSK_WALK_STOP ends the walk, which succeeds;
    // TSK_WALK_ERROR ends it, and it fails.
    void testInodeWalkCallbackStops()
    {
        ImgPtr img = open_image(image_path(V5));
        FsPtr fs(tsk_fs_open_img(img.get(), 0, TSK_FS_TYPE_DETECT));
        CPPUNIT_ASSERT_MESSAGE(error_text(), fs != nullptr);
        for (const TSK_WALK_RET_ENUM ret : {TSK_WALK_STOP, TSK_WALK_ERROR}) {
            MetaWalk w;
            w.stop_after = 3;
            w.stop_with = ret;
            tsk_error_reset();
            const uint8_t rc = tsk_fs_meta_walk(fs.get(), fs->first_inum,
                fs->last_inum, TSK_FS_META_FLAG_ALLOC, record_inode, &w);
            CPPUNIT_ASSERT_EQUAL(
                ret == TSK_WALK_STOP ? 0 : 1, static_cast<int>(rc));
            CPPUNIT_ASSERT_EQUAL(size_t{3}, w.visited.size());
            CPPUNIT_ASSERT_EQUAL(TSK_INUM_T{128}, w.visited[0].inum);
            tsk_error_reset();
        }
    }

    // A free inode whose ctime is not 0 was in use once (as on ext): it is
    // UNALLOC | USED, and an orphan, since no name refers to it. A copy of
    // the start of v4 gets a ctime on free inode 191.
    void testInodeWalkUsedFreeInode()
    {
        std::vector<char> image = v4_start();
        put32(image, V4_INODE_191 + DI_CTIME, 1600000000);
        ImgPtr img;
        FsPtr fs = open_copy("used-free-inode", image, img);
        const std::vector<Visited> used =
            walk_inodes(fs.get(), 0, V4_AG0_LAST, UNALLOC | USED, "USED");
        CPPUNIT_ASSERT_EQUAL(size_t{1}, used.size());
        CPPUNIT_ASSERT_EQUAL(TSK_INUM_T{191}, used[0].inum);
        CPPUNIT_ASSERT_EQUAL(UNALLOC | USED, used[0].flags);
        const std::vector<Visited> orphans = walk_inodes(
            fs.get(), 0, V4_AG0_LAST, TSK_FS_META_FLAG_ORPHAN, "ORPHAN");
        CPPUNIT_ASSERT_EQUAL(size_t{1}, orphans.size());
        CPPUNIT_ASSERT_EQUAL(TSK_INUM_T{191}, orphans[0].inum);
        CPPUNIT_ASSERT_EQUAL(size_t{4},
            walk_inodes(fs.get(), 0, V4_AG0_LAST, UNALLOC | UNUSED, "UNUSED")
                .size());
    }

    // The holes of a sparse inode chunk are not inodes: not visited, free or
    // not. A copy of the start of v5 (sparse inodes on) makes the last 4
    // inodes of AG 0's chunk (188-191) a hole: hole mask bit 15, their free
    // bits set (as the kernel sets them), 60 inodes of which none is free
    // (189-191 were the free ones; holes are not counted).
    void testInodeWalkSparseChunk()
    {
        std::vector<char> image = image_start(V5, 1 << 20);
        const size_t rec = 3 * 4096 + 56; // AG 0's leaf, block 3: 1 record
        put16(image, rec + 4, 0x8000);    // hole mask
        image[rec + 6] = 60;              // inodes
        image[rec + 7] = 0;               // free
        put64(image, rec + 8, 0xf000000000000000ULL);
        ImgPtr img;
        FsPtr fs = open_copy("sparse-chunk", image, img);
        const std::set<uint64_t> every =
            numbers(walk_inodes(fs.get(), 0, V5_AG0_LAST, 0, "every"));
        CPPUNIT_ASSERT_EQUAL(size_t{60}, every.size());
        CPPUNIT_ASSERT_EQUAL(uint64_t{128}, *every.begin());
        CPPUNIT_ASSERT_EQUAL(uint64_t{187}, *every.rbegin());
        CPPUNIT_ASSERT_EQUAL(size_t{0},
            walk_inodes(fs.get(), 0, V5_AG0_LAST, UNALLOC, "free").size());
    }

    // An inode B+tree of two levels (a node above the leaf) is walked
    // through the node. A copy of the start of v4 gets a node in free block
    // 34 (xfs_db: AG 0's free space starts there) with one key, 128,
    // pointing to the leaf (block 3), and the AGI says two levels with the
    // node as root. A node with no records, or whose child is not one level
    // down, is corrupt.
    void testInodeWalkTreeWithNodes()
    {
        ImgPtr base_img;
        FsPtr base = open_copy("one-level", v4_start(), base_img);
        const std::set<uint64_t> want =
            numbers(walk_inodes(base.get(), 0, V4_AG0_LAST, 0, "one level"));
        CPPUNIT_ASSERT_EQUAL(size_t{64}, want.size());

        std::vector<char> image = v4_start();
        const size_t node = 34 * 4096;
        put32(image, node, 0x49414254);     // "IABT"
        put16(image, node + 4, 1);          // level
        put16(image, node + 6, 1);          // one key
        put32(image, node + 8, 0xffffffff); // no siblings
        put32(image, node + 12, 0xffffffff);
        put32(image, node + 16, 128); // key: the chunk's first inode
        const size_t maxrecs = (4096 - 16) / 8;
        put32(image, node + 16 + maxrecs * 4, 3); // pointer: the leaf
        put32(image, V4_AGI + AGI_ROOT, 34);
        put32(image, V4_AGI + AGI_LEVEL, 2);
        ImgPtr img;
        FsPtr fs = open_copy("two-level", image, img);
        CPPUNIT_ASSERT_MESSAGE("two levels",
            want ==
                numbers(
                    walk_inodes(fs.get(), 0, V4_AG0_LAST, 0, "two levels")));

        put16(image, node + 6, 0);
        ImgPtr empty_img;
        FsPtr empty = open_copy("two-level-empty-node", image, empty_img);
        expect_walk_error(
            empty.get(), V4_AG0_LAST, TSK_ERR_FS_CORRUPT, "empty node");

        put16(image, node + 6, 1);
        put16(image, node + 4, 2);
        ImgPtr bad_img;
        FsPtr bad = open_copy("two-level-bad", image, bad_img);
        expect_walk_error(bad.get(), V4_AG0_LAST, TSK_ERR_FS_CORRUPT, "level");
    }

    // A damaged AGI or inode B+tree fails the walk (TSK_ERR_FS_CORRUPT), as
    // does a chunk out of order or outside its AG; a root leaf with no
    // records is an AG without inodes; a damaged inode is skipped and the
    // walk goes on. Copies of the start of v4; AG 0's one
    // leaf (block 3) has one record, chunk 128.
    void testInodeWalkDamage()
    {
        const size_t leaf = 3 * 4096;
        const size_t rec = leaf + 16;
        const struct {
            const char *name;
            std::function<void(std::vector<char> &)> damage;
        } fails[] = {
            {"agi-magic", [](std::vector<char> &b) { put32(b, V4_AGI, 0); }},
            {"agi-root-past-ag",
                [](std::vector<char> &b) {
                    put32(b, V4_AGI + AGI_ROOT, 20480);
                }},
            {"inobt-magic", [=](std::vector<char> &b) { put32(b, leaf, 0); }},
            {"inobt-too-many-records",
                [=](std::vector<char> &b) { put16(b, leaf + 6, 300); }},
            {"chunk-past-ag",
                [=](std::vector<char> &b) { put32(b, rec, 20480 * 16 - 32); }},
            {"chunks-overlap",
                [=](std::vector<char> &b) {
                    put16(b, leaf + 6, 2);
                    for (size_t i = 0; i < 16; ++i) {
                        b[rec + 16 + i] = b[rec + i];
                    }
                    put32(b, rec + 16, 160);
                }},
        };
        for (const auto &c : fails) {
            std::vector<char> image = v4_start();
            c.damage(image);
            ImgPtr img;
            FsPtr fs = open_copy(c.name, image, img);
            expect_walk_error(
                fs.get(), V4_AG0_LAST, TSK_ERR_FS_CORRUPT, c.name);
        }

        // A leaf with no records is a valid root: an AG without inodes.
        {
            std::vector<char> image = v4_start();
            put16(image, leaf + 6, 0);
            ImgPtr img;
            FsPtr fs = open_copy("inobt-empty-leaf", image, img);
            CPPUNIT_ASSERT_EQUAL(size_t{0},
                walk_inodes(fs.get(), 0, V4_AG0_LAST, 0, "empty leaf").size());
        }

        std::vector<char> image = v4_start();
        put16(image, V4_INODE_133, 0); // magic
        ImgPtr img;
        FsPtr fs = open_copy("inode-magic", image, img);
        const std::set<uint64_t> got =
            numbers(walk_inodes(fs.get(), 0, V4_AG0_LAST, 0, "inode-magic"));
        CPPUNIT_ASSERT_EQUAL(size_t{63}, got.size());
        CPPUNIT_ASSERT_EQUAL(size_t{0}, got.count(133));
    }

    // ifind -d (tsk_fs_ifind_data) finds the inode (the Manifest's) of the
    // file that holds a block: the first and last block of a small file, of
    // /fragmented.bin (an extent B+tree) and of directories (leaf; B+tree).
    // A free block (the last of the file system, in AG 3's free space by
    // xfs_db) and the superblock have no owner: with no META flag on any
    // block, ifind says "Inode not found", not "Meta Data".
    void testIfindData()
    {
        for (const Fixture &f : ALL_FIXTURES) {
            const std::string v = f.variant;
            const tskval::Manifest manifest = read_manifest(f);
            ImgPtr img = open_image(image_path(f));
            FsPtr fs(tsk_fs_open_img(img.get(), 0, TSK_FS_TYPE_DETECT));
            CPPUNIT_ASSERT_MESSAGE(v + ": " + error_text(), fs != nullptr);
            for (const char *path : {"/multiblock.bin", "/fragmented.bin",
                     "/dirs/leaf", "/dirs/btree"}) {
                fstest::FilePtr file(tsk_fs_file_open(fs.get(), nullptr, path));
                CPPUNIT_ASSERT_MESSAGE(v + path + ": " + error_text(), file);
                const std::vector<TSK_DADDR_T> blocks =
                    fstest::file_blocks(file.get());
                CPPUNIT_ASSERT_MESSAGE(
                    v + path + ": no blocks", !blocks.empty());
                const std::string want =
                    std::to_string(fstest::entry_of(manifest, path).inode) +
                    "\n";
                for (const TSK_DADDR_T b : {blocks.front(), blocks.back()}) {
                    bool ok = false;
                    const std::string got = ifind_data(fs.get(), b, ok);
                    const std::string what =
                        v + path + " block " + std::to_string(b);
                    CPPUNIT_ASSERT_MESSAGE(what + ": " + error_text(), ok);
                    CPPUNIT_ASSERT_EQUAL_MESSAGE(what, want, got);
                }
            }
            for (const TSK_DADDR_T b : {fs->last_block, TSK_DADDR_T{0}}) {
                bool ok = false;
                const std::string got = ifind_data(fs.get(), b, ok);
                const std::string what = v + " block " + std::to_string(b);
                CPPUNIT_ASSERT_MESSAGE(what + ": " + error_text(), ok);
                CPPUNIT_ASSERT_EQUAL_MESSAGE(
                    what, std::string("Inode not found\n"), got);
            }
        }
    }

    // The block walk visits every block of the file system, from 0 to the
    // last (sb_dblocks - 1, mkfs.xfs's block count), once and in order, each
    // only allocated: the free-space B+trees are not read, and metadata and
    // content blocks are not told apart.
    void testBlockWalkCoversEveryBlock()
    {
        for (const Fixture &f : ALL_FIXTURES) {
            const std::string v = f.variant;
            ImgPtr img = open_image(image_path(f));
            FsPtr fs(tsk_fs_open_img(img.get(), 0, TSK_FS_TYPE_DETECT));
            CPPUNIT_ASSERT_MESSAGE(v + ": " + error_text(), fs != nullptr);
            CPPUNIT_ASSERT_EQUAL_MESSAGE(
                v + ": last block", TSK_DADDR_T{f.blocks - 1}, fs->last_block);
            BlockWalk w;
            CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": " + error_text(), 0,
                static_cast<int>(walk_blocks(fs.get(), fs->first_block,
                    fs->last_block, TSK_FS_BLOCK_WALK_FLAG_AONLY, w)));
            CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": blocks",
                static_cast<size_t>(f.blocks), w.visited.size());
            for (size_t i = 0; i < w.visited.size(); ++i) {
                if (w.visited[i].addr != i ||
                    kind(w.visited[i].flags) != TSK_FS_BLOCK_FLAG_ALLOC) {
                    CPPUNIT_FAIL(v + ": visit " + std::to_string(i) +
                        " is block " + std::to_string(w.visited[i].addr) +
                        " with flags " + std::to_string(w.visited[i].flags));
                }
            }
            // A single block's flags say the same.
            fstest::BlockPtr one(
                tsk_fs_block_get(fs.get(), nullptr, f.blocks / 2));
            CPPUNIT_ASSERT_MESSAGE(v + ": " + error_text(), one != nullptr);
            CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": block_get",
                static_cast<unsigned>(TSK_FS_BLOCK_FLAG_ALLOC),
                kind(one->flags));
        }
    }

    // As every block is allocated, an UNALLOC-only walk visits none; with
    // no META or CONT flag on any block, a META-only or CONT-only walk keeps
    // every block.
    void testBlockWalkFlags()
    {
        ImgPtr img = open_image(image_path(V5));
        FsPtr fs(tsk_fs_open_img(img.get(), 0, TSK_FS_TYPE_DETECT));
        CPPUNIT_ASSERT_MESSAGE(error_text(), fs != nullptr);
        const unsigned AONLY = TSK_FS_BLOCK_WALK_FLAG_AONLY;
        const struct {
            unsigned flags;
            size_t blocks;
        } cases[] = {
            {TSK_FS_BLOCK_WALK_FLAG_UNALLOC, 0},
            {TSK_FS_BLOCK_WALK_FLAG_ALLOC, 81920},
            {TSK_FS_BLOCK_WALK_FLAG_ALLOC | TSK_FS_BLOCK_WALK_FLAG_UNALLOC,
                81920},
            {TSK_FS_BLOCK_WALK_FLAG_META, 81920},
            {TSK_FS_BLOCK_WALK_FLAG_CONT, 81920},
            {TSK_FS_BLOCK_WALK_FLAG_UNALLOC | TSK_FS_BLOCK_WALK_FLAG_META, 0},
        };
        for (const auto &c : cases) {
            BlockWalk w;
            const std::string what = "flags " + std::to_string(c.flags);
            CPPUNIT_ASSERT_EQUAL_MESSAGE(what + ": " + error_text(), 0,
                static_cast<int>(walk_blocks(fs.get(), fs->first_block,
                    fs->last_block, c.flags | AONLY, w)));
            CPPUNIT_ASSERT_EQUAL_MESSAGE(what, c.blocks, w.visited.size());
        }
    }

    // Without AONLY, each block's buffer holds the block's bytes in the
    // image. A walk over part of the blocks visits only that part.
    void testBlockWalkContentAndRanges()
    {
        for (const Fixture &f : {V4_512, V5_64K, V5}) {
            const std::string v = f.variant;
            ImgPtr img = open_image(image_path(f));
            FsPtr fs(tsk_fs_open_img(img.get(), 0, TSK_FS_TYPE_DETECT));
            CPPUNIT_ASSERT_MESSAGE(v + ": " + error_text(), fs != nullptr);
            std::ifstream raw(image_path(f), std::ios::binary);
            const TSK_DADDR_T mid = f.blocks / 2;
            const struct {
                TSK_DADDR_T lo, hi;
            } ranges[] = {{0, 63}, {mid - 3, mid + 3}, {mid, mid},
                {f.blocks - 2, f.blocks - 1}};
            for (const auto &r : ranges) {
                const std::string what = v + ": blocks " +
                    std::to_string(r.lo) + "-" + std::to_string(r.hi);
                BlockWalk w;
                w.image = &raw;
                CPPUNIT_ASSERT_EQUAL_MESSAGE(what + ": " + error_text(), 0,
                    static_cast<int>(walk_blocks(fs.get(), r.lo, r.hi, 0, w)));
                CPPUNIT_ASSERT_EQUAL_MESSAGE(what,
                    static_cast<size_t>(r.hi - r.lo + 1), w.visited.size());
                for (size_t i = 0; i < w.visited.size(); ++i) {
                    CPPUNIT_ASSERT_EQUAL_MESSAGE(
                        what, r.lo + i, w.visited[i].addr);
                }
                CPPUNIT_ASSERT_EQUAL_MESSAGE(
                    what + ": bytes", size_t{0}, w.differ);
            }
        }
    }

    // A range outside the file system's, or backwards, is
    // TSK_ERR_FS_WALK_RNG, and the action is never called.
    void testBlockWalkRangeErrors()
    {
        ImgPtr img = open_image(image_path(V5));
        FsPtr fs(tsk_fs_open_img(img.get(), 0, TSK_FS_TYPE_DETECT));
        CPPUNIT_ASSERT_MESSAGE(error_text(), fs != nullptr);
        const TSK_DADDR_T last = fs->last_block;
        const struct {
            TSK_DADDR_T start, end;
        } ranges[] = {{last + 1, last + 1}, {0, last + 1}, {20, 10}};
        for (const auto &r : ranges) {
            BlockWalk w;
            CPPUNIT_ASSERT_EQUAL(1,
                static_cast<int>(walk_blocks(fs.get(), r.start, r.end,
                    TSK_FS_BLOCK_WALK_FLAG_AONLY, w)));
            CPPUNIT_ASSERT_EQUAL(static_cast<uint32_t>(TSK_ERR_FS_WALK_RNG),
                tsk_error_get_errno());
            CPPUNIT_ASSERT(w.visited.empty());
            tsk_error_reset();
        }
    }

    // An action that returns TSK_WALK_STOP ends the walk, which succeeds;
    // TSK_WALK_ERROR ends it, and it fails.
    void testBlockWalkCallbackStops()
    {
        ImgPtr img = open_image(image_path(V5));
        FsPtr fs(tsk_fs_open_img(img.get(), 0, TSK_FS_TYPE_DETECT));
        CPPUNIT_ASSERT_MESSAGE(error_text(), fs != nullptr);
        for (const TSK_WALK_RET_ENUM ret : {TSK_WALK_STOP, TSK_WALK_ERROR}) {
            BlockWalk w;
            w.stop_after = 5;
            w.stop_with = ret;
            CPPUNIT_ASSERT_EQUAL(ret == TSK_WALK_STOP ? 0 : 1,
                static_cast<int>(walk_blocks(
                    fs.get(), fs->first_block, fs->last_block, 0, w)));
            CPPUNIT_ASSERT_EQUAL(size_t{5}, w.visited.size());
            tsk_error_reset();
        }
    }

    // An image shorter than the file system: the walk visits the blocks in
    // the image, then fails with a read error at the first block past its
    // end (TSK_ERR_FS_READ), as ext's does. A copy of the first MiB of v4
    // (256 blocks).
    void testBlockWalkImageCutShort()
    {
        ImgPtr img;
        FsPtr fs = open_copy("cut-short", v4_start(), img);
        for (const unsigned flags :
            {0u, unsigned{TSK_FS_BLOCK_WALK_FLAG_AONLY}}) {
            BlockWalk w;
            CPPUNIT_ASSERT_EQUAL(1,
                static_cast<int>(walk_blocks(
                    fs.get(), fs->first_block, fs->last_block, flags, w)));
            CPPUNIT_ASSERT_EQUAL(
                static_cast<uint32_t>(TSK_ERR_FS_READ), tsk_error_get_errno());
            CPPUNIT_ASSERT_EQUAL(size_t{256}, w.visited.size());
            tsk_error_reset();
        }
    }

  private:
    // The v4 Fixture (xfs_db): 4 KiB blocks, 512-byte sectors, 256-byte
    // inodes, 2^19 inode numbers per AG; AG 0's AGI in its third sector,
    // its inode B+tree one leaf in block 3 with the chunk of inodes 128-191
    // (blocks 8-11), of which 187-191 are free.
    static const size_t V4_AGI = 2 * 512;
    static const size_t AGI_ROOT = 20, AGI_LEVEL = 24;
    static const size_t V4_INODE_133 = 8 * 4096 + 5 * 256;
    static const size_t V4_INODE_191 = 11 * 4096 + 15 * 256;
    static const size_t DI_CTIME = 48;
    static const TSK_INUM_T V4_AG0_LAST = (TSK_INUM_T{1} << 19) - 1;
    // The v5 Fixture: 2^18 inode numbers per AG.
    static const TSK_INUM_T V5_AG0_LAST = (TSK_INUM_T{1} << 18) - 1;

    // The first MiB of v4: AG 0's headers and inodes.
    static std::vector<char> v4_start()
    {
        return image_start(V4, 1 << 20);
    }

    static void expect_walk_error(
        TSK_FS_INFO *fs, TSK_INUM_T end, uint32_t err, const std::string &what)
    {
        MetaWalk w;
        tsk_error_reset();
        CPPUNIT_ASSERT_EQUAL_MESSAGE(what, 1,
            static_cast<int>(tsk_fs_meta_walk(
                fs, 0, end, TSK_FS_META_FLAG_ALLOC, record_inode, &w)));
        CPPUNIT_ASSERT_EQUAL_MESSAGE(
            what + ": " + error_text(), err, tsk_error_get_errno());
        tsk_error_reset();
    }
};

CPPUNIT_TEST_SUITE_REGISTRATION(XfsWalkTest);

} // namespace
