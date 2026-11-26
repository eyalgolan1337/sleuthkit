/*
 * XFS on damaged images (#45), through the public API, on copies of the
 * Fixture images with targeted damage.
 *
 * - v5 checksums: an undamaged Fixture gives no checksum warning; a wrong
 *   checksum in otherwise valid metadata (superblock, AGI, inode B+tree
 *   block, inode, extent B+tree block, directory block, symlink block) gives
 *   a warning in the verbose output, and the file system still walks as its
 *   Manifest says.
 * - Corrupt images: a bad magic, an image cut short, block and inode numbers
 *   out of range, a directory cycle, a huge extent count and B+tree loops
 *   each end quickly, with a TSK error where the damage makes the data
 *   unreadable.
 */
#include "fs_test_util.h"
#include "manifest_check.h"
#include "xfs_fixture_data.h"

#include "manifest.h"

#include <tsk/libtsk.h>

#include <cppunit/extensions/HelperMacros.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <set>
#include <sstream>
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
using fstest::V4;
using fstest::V4_512;
using fstest::V5;
using fstest::V5_1K;

using Bytes = std::vector<uint8_t>;

// A file descriptor, closed at the end of the scope.
struct Fd {
    int fd;
    explicit Fd(int f) : fd(f) {}
    ~Fd()
    {
        if (fd >= 0) {
            close(fd);
        }
    }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
};

// A whole copy of a Fixture image, xfs-corrupt-<name>.img in the test data
// directory, written sparse (the holes of the unpacked Fixture stay holes).
class ImageCopy {
  public:
    ImageCopy(const Fixture &f, const std::string &name)
        : m_path(fstest::data_path("xfs-corrupt-" + name + ".img"))
    {
        const Fd in(open(image_path(f).c_str(), O_RDONLY));
        const Fd out(open(
            m_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
        CPPUNIT_ASSERT_MESSAGE(
            "cannot copy to " + m_path, in.fd >= 0 && out.fd >= 0);
        const off_t size = lseek(in.fd, 0, SEEK_END);
        CPPUNIT_ASSERT(size > 0 && ftruncate(out.fd, size) == 0);
        std::vector<char> buf(1 << 20);
        off_t data = 0;
        while ((data = lseek(in.fd, data, SEEK_DATA)) >= 0 && data < size) {
            const off_t hole = lseek(in.fd, data, SEEK_HOLE);
            CPPUNIT_ASSERT(hole > data);
            while (data < hole) {
                const size_t want = static_cast<size_t>(
                    hole - data < static_cast<off_t>(buf.size())
                        ? hole - data
                        : static_cast<off_t>(buf.size()));
                const ssize_t got = pread(in.fd, buf.data(), want, data);
                CPPUNIT_ASSERT(got > 0);
                CPPUNIT_ASSERT_EQUAL(got,
                    pwrite(out.fd, buf.data(), static_cast<size_t>(got), data));
                data += got;
            }
        }
    }

    const std::string &path() const
    {
        return m_path;
    }

    Bytes read(uint64_t off, size_t n) const
    {
        const Fd in(open(m_path.c_str(), O_RDONLY));
        Bytes b(n);
        CPPUNIT_ASSERT_EQUAL_MESSAGE(m_path + " is short",
            static_cast<ssize_t>(n),
            pread(in.fd, b.data(), n, static_cast<off_t>(off)));
        return b;
    }

    void patch(uint64_t off, const Bytes &bytes) const
    {
        const Fd out(open(m_path.c_str(), O_WRONLY));
        CPPUNIT_ASSERT_EQUAL_MESSAGE("cannot patch " + m_path,
            static_cast<ssize_t>(bytes.size()),
            pwrite(
                out.fd, bytes.data(), bytes.size(), static_cast<off_t>(off)));
    }

    // A big-endian number of `n` bytes at `off`.
    uint64_t get(uint64_t off, size_t n) const
    {
        uint64_t v = 0;
        for (const uint8_t b : read(off, n)) {
            v = (v << 8) | b;
        }
        return v;
    }

    void put(uint64_t off, size_t n, uint64_t v) const
    {
        Bytes b(n);
        for (size_t i = 0; i < n; ++i) {
            b[n - 1 - i] = static_cast<uint8_t>(v >> (8 * i));
        }
        patch(off, b);
    }

    // Inverts the byte at `off`.
    void flip(uint64_t off) const
    {
        Bytes b = read(off, 1);
        b[0] = static_cast<uint8_t>(~b[0]);
        patch(off, b);
    }

    void cut(uint64_t size) const
    {
        CPPUNIT_ASSERT_EQUAL(
            0, truncate(m_path.c_str(), static_cast<off_t>(size)));
    }

  private:
    std::string m_path;
};

// An image and the XFS file system in it.
struct Opened {
    ImgPtr img;
    FsPtr fs;
    explicit Opened(const std::string &path)
        : img(open_image(path, TSK_IMG_TYPE_RAW)),
          fs(tsk_fs_open_img(img.get(), 0, TSK_FS_TYPE_XFS))
    {
        CPPUNIT_ASSERT_MESSAGE(path + ": " + error_text(), fs != nullptr);
    }
};

// --- Where things are in a Fixture image (its geometry, not libtsk) ---

unsigned
inopblog(const Fixture &f)
{
    unsigned log = 0;
    while ((f.inode_size << log) < f.block_size) {
        ++log;
    }
    return log;
}

// The byte offset of inode `inum`.
uint64_t
inode_offset(const Fixture &f, uint64_t inum)
{
    const uint64_t agino = inum & ((uint64_t{1} << f.ag_inode_bits) - 1);
    const uint64_t block =
        (inum >> f.ag_inode_bits) * f.ag_blocks + (agino >> inopblog(f));
    return block * f.block_size +
        (agino & ((1u << inopblog(f)) - 1)) * f.inode_size;
}

// The linear block number (libtsk's block address) of XFS block `fsblock`.
uint64_t
linear_block(const Fixture &f, uint64_t fsblock)
{
    const unsigned agblklog = f.ag_inode_bits - inopblog(f);
    return (fsblock >> agblklog) * f.ag_blocks +
        (fsblock & ((uint64_t{1} << agblklog) - 1));
}

// The inode core's size, and so the data fork's offset in the inode.
unsigned
fork_offset(const Fixture &f)
{
    return f.version == 5 ? 176 : 100;
}

// The size of the data fork of the inode at byte `ino`: up to the attr fork
// (di_forkoff, in 8-byte units), or the rest of the inode.
unsigned
fork_size(const Fixture &f, const ImageCopy &c, uint64_t ino)
{
    const unsigned forkoff = static_cast<unsigned>(c.get(ino + 82, 1));
    return forkoff != 0 ? forkoff * 8 : f.inode_size - fork_offset(f);
}

// The byte offset of pointer `i` of the extent B+tree root in the data fork
// of the inode at byte `ino`: after the 4-byte header and room for the most
// 8-byte keys the fork holds.
uint64_t
bmbt_root_ptr(const Fixture &f, const ImageCopy &c, uint64_t ino, unsigned i)
{
    const unsigned maxrecs = (fork_size(f, c, ino) - 4) / 16;
    return ino + fork_offset(f) + 4 + maxrecs * 8 + i * 8;
}

// AG 0's AGI: the third sector (sb_sectsize is at byte 102 of the
// superblock).
uint64_t
agi_offset(const ImageCopy &c)
{
    return 2 * c.get(102, 2);
}

// The blocks of the file at `path` in the undamaged Fixture.
std::vector<TSK_DADDR_T>
blocks_of(const Fixture &f, const tskval::Manifest &m, const std::string &path)
{
    Opened o(image_path(f));
    FilePtr file(
        tsk_fs_file_open_meta(o.fs.get(), nullptr, entry_of(m, path).inode));
    CPPUNIT_ASSERT_MESSAGE(path + ": " + error_text(), file != nullptr);
    const std::vector<TSK_DADDR_T> blocks = fstest::file_blocks(file.get());
    CPPUNIT_ASSERT_MESSAGE(path + ": no blocks", !blocks.empty());
    return blocks;
}

// --- Verbose output ---

// What libtsk writes to standard error, with tsk_verbose set (or, without
// `verbose`, not set), while `run` runs.
std::string
verbose_output(const std::function<void()> &run, bool verbose = true)
{
    struct Redirect {
        int saved;
        Redirect(FILE *to, bool verbose) : saved(dup(STDERR_FILENO))
        {
            fflush(stderr);
            dup2(fileno(to), STDERR_FILENO);
            tsk_verbose = verbose ? 1 : 0;
        }
        ~Redirect()
        {
            tsk_verbose = 0;
            fflush(stderr);
            dup2(saved, STDERR_FILENO);
            close(saved);
        }
    };
    bool ok = false;
    return fstest::printed_text(
        [&](FILE *f) {
            const Redirect redirect(f, verbose);
            run();
            return 0;
        },
        ok);
}

// The different lines of `text` that report a failed checksum.
std::set<std::string>
checksum_lines(const std::string &text)
{
    std::set<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.find("checksum failed") != std::string::npos) {
            lines.insert(line);
        }
    }
    return lines;
}

std::string
show(const std::set<std::string> &lines)
{
    std::string out;
    for (const std::string &l : lines) {
        out += "[" + l + "]";
    }
    return out.empty() ? "(none)" : out;
}

// Inode walk flags: allocated and free inodes, used or not.
const TSK_FS_META_FLAG_ENUM EVERY_INODE =
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): flags
    static_cast<TSK_FS_META_FLAG_ENUM>(
        TSK_FS_META_FLAG_ALLOC | TSK_FS_META_FLAG_UNALLOC);

TSK_WALK_RET_ENUM
count_inode(TSK_FS_FILE *, void *ptr)
{
    ++*static_cast<size_t *>(ptr);
    return TSK_WALK_CONT;
}

// --- Damage that must end in a TSK error, quickly ---

// Runs `run`, which must end within two minutes: the damage in these tests
// is found in a handful of reads, well under a second (the limit leaves room
// for valgrind), and a loop would run for hours.
void
quickly(const std::string &what, const std::function<void()> &run)
{
    const auto start = std::chrono::steady_clock::now();
    run();
    const auto took = std::chrono::steady_clock::now() - start;
    CPPUNIT_ASSERT_MESSAGE(
        what + ": took too long", took < std::chrono::seconds(120));
}

// Asserts that the current TSK error is `code`.
void
check_errno(uint32_t code, const std::string &what)
{
    CPPUNIT_ASSERT_EQUAL_MESSAGE(what + ": " + error_text(), code,
        static_cast<uint32_t>(tsk_error_get_errno()));
}

// Opening the copy as XFS fails with `code`.
void
open_fails(const ImageCopy &c, uint32_t code, const std::string &what)
{
    quickly(what, [&]() {
        const ImgPtr img = open_image(c.path(), TSK_IMG_TYPE_RAW);
        tsk_error_reset();
        const FsPtr fs(tsk_fs_open_img(img.get(), 0, TSK_FS_TYPE_XFS));
        CPPUNIT_ASSERT_MESSAGE(what + ": opened", fs == nullptr);
        check_errno(code, what);
    });
}

// Opening inode `inum` of the copy fails with `code`.
void
lookup_fails(
    const ImageCopy &c, uint64_t inum, uint32_t code, const std::string &what)
{
    quickly(what, [&]() {
        Opened o(c.path());
        tsk_error_reset();
        const FilePtr file(tsk_fs_file_open_meta(o.fs.get(), nullptr, inum));
        CPPUNIT_ASSERT_MESSAGE(what + ": opened", file == nullptr);
        check_errno(code, what);
    });
}

// Inode `inum` of the copy opens, and reading its content fails with `code`.
void
read_fails(
    const ImageCopy &c, uint64_t inum, uint32_t code, const std::string &what)
{
    quickly(what, [&]() {
        Opened o(c.path());
        const FilePtr file(tsk_fs_file_open_meta(o.fs.get(), nullptr, inum));
        CPPUNIT_ASSERT_MESSAGE(what + ": " + error_text(), file != nullptr);
        char buf[512];
        tsk_error_reset();
        const ssize_t cnt = tsk_fs_file_read(
            file.get(), 0, buf, sizeof(buf), TSK_FS_FILE_READ_FLAG_NONE);
        CPPUNIT_ASSERT_EQUAL_MESSAGE(what + ": read", ssize_t{-1}, cnt);
        check_errno(code, what);
    });
}

// Opening directory `inum` of the copy fails with `code`.
void
dir_fails(
    const ImageCopy &c, uint64_t inum, uint32_t code, const std::string &what)
{
    quickly(what, [&]() {
        Opened o(c.path());
        tsk_error_reset();
        TSK_FS_DIR *dir = tsk_fs_dir_open_meta(o.fs.get(), inum);
        if (dir != nullptr) {
            tsk_fs_dir_close(dir);
            CPPUNIT_FAIL(what + ": opened");
        }
        check_errno(code, what);
    });
}

// The walk of AG 0's inodes of the copy fails with `code`.
void
inode_walk_fails(const ImageCopy &c, const Fixture &f, uint32_t code,
    const std::string &what)
{
    quickly(what, [&]() {
        Opened o(c.path());
        size_t inodes = 0;
        tsk_error_reset();
        const uint8_t rc = tsk_fs_meta_walk(o.fs.get(), 0,
            (uint64_t{1} << f.ag_inode_bits) - 1, EVERY_INODE, count_inode,
            &inodes);
        CPPUNIT_ASSERT_EQUAL_MESSAGE(what + ": walk", 1, int{rc});
        check_errno(code, what);
    });
}

// One piece of damage: `bytes` at `off`. Applied to a copy for the length
// of a check, then undone.
struct Damage {
    const ImageCopy &c;
    uint64_t off;
    Bytes saved;
    Damage(const ImageCopy &copy, uint64_t at, const Bytes &bytes)
        : c(copy), off(at), saved(copy.read(at, bytes.size()))
    {
        c.patch(off, bytes);
    }
    ~Damage()
    {
        c.patch(off, saved);
    }
    Damage(const Damage &) = delete;
    Damage &operator=(const Damage &) = delete;
};

// `n` big-endian bytes of `v`.
Bytes
be(uint64_t v, size_t n)
{
    Bytes b(n);
    for (size_t i = 0; i < n; ++i) {
        b[n - 1 - i] = static_cast<uint8_t>(v >> (8 * i));
    }
    return b;
}

// An extent record: file block `startoff`, XFS block `fsblock`, `count`
// blocks.
Bytes
extent(uint64_t startoff, uint64_t fsblock, uint64_t count)
{
    Bytes rec = be((startoff << 9) | (fsblock >> 43), 8);
    const Bytes l1 = be((fsblock << 21) | count, 8);
    rec.insert(rec.end(), l1.begin(), l1.end());
    return rec;
}

// Superblock and AGI fields (byte offsets).
const uint64_t SB_DBLOCKS = 8;
const uint64_t SB_AGBLOCKS = 84;
const uint64_t SB_AGCOUNT = 88;
const uint64_t SB_SECTSIZE = 102;
const uint64_t SB_SECTLOG = 121;
const uint64_t SB_AGBLKLOG = 124;
const uint64_t AGI_ROOT = 20;
const uint64_t AGI_LEVEL = 24;
// Inode fields.
const uint64_t DI_NEXTENTS = 76;

struct DirWalkCount {
    size_t names = 0;
    static TSK_WALK_RET_ENUM cb(TSK_FS_FILE *, const char *, void *ptr)
    {
        ++static_cast<DirWalkCount *>(ptr)->names;
        return TSK_WALK_CONT;
    }
};

class XfsCorruptTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(XfsCorruptTest);
    CPPUNIT_TEST(testNoChecksumWarningOnFixtures);
    CPPUNIT_TEST(testWrongChecksumWarnsAndStillReads);
    CPPUNIT_TEST(testBadMagic);
    CPPUNIT_TEST(testImpossibleGeometry);
    CPPUNIT_TEST(testImageCutShort);
    CPPUNIT_TEST(testBlockNumbersOutOfRange);
    CPPUNIT_TEST(testInodeNumbersOutOfRange);
    CPPUNIT_TEST(testDirectoryCycle);
    CPPUNIT_TEST(testHugeExtentCount);
    CPPUNIT_TEST(testExtentTreeLoops);
    CPPUNIT_TEST(testInodeTreeLoops);
    CPPUNIT_TEST_SUITE_END();

  public:
    // A test that hangs fails: the process is killed (after an hour: the
    // tests take seconds, minutes under valgrind).
    void setUp() override
    {
        alarm(3600);
    }
    void tearDown() override
    {
        alarm(0);
    }

    // What mkfs.xfs wrote has good checksums: the walk of every Fixture
    // (every name, inode and file content) and the walk of all its inodes,
    // free ones too, report no failed checksum. (v4 has no checksums.)
    void testNoChecksumWarningOnFixtures()
    {
        for (const Fixture &f : ALL_FIXTURES) {
            const std::string v = f.variant;
            const tskval::Manifest m = read_manifest(f);
            fstest::WalkCheck walk;
            size_t inodes = 0;
            uint8_t rc = 1;
            const std::string text = verbose_output([&]() {
                walk = fstest::check_walk(
                    image_path(f), m, fstest::inode_known_failures(f));
                Opened o(image_path(f));
                rc = tsk_fs_meta_walk(o.fs.get(), o.fs->first_inum,
                    o.fs->last_inum, EVERY_INODE, count_inode, &inodes);
            });
            CPPUNIT_ASSERT_MESSAGE(v + ": " + walk.report, walk.passed);
            CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": inode walk", 0, int{rc});
            // Allocated (named and internal) and free inodes.
            CPPUNIT_ASSERT_MESSAGE(v + ": few inodes", inodes > f.free_inodes);
            CPPUNIT_ASSERT_MESSAGE(v +
                    ": the verbose output has no "
                    "xfs_dir_open_meta line",
                text.find("xfs_dir_open_meta") != std::string::npos);
            CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": checksum warnings",
                std::string("(none)"), show(checksum_lines(text)));
        }
    }

    // One wrong checksum (a byte of the stored checksum inverted, so the
    // metadata itself is intact) per kind of v5 metadata: the walk of the
    // file system still matches the Manifest, the inode walk still visits
    // every inode, and the verbose output of each has the warning for that
    // structure and no other. Without tsk_verbose nothing is reported: the
    // walk passes and prints nothing.
    void testWrongChecksumWarnsAndStillReads()
    {
        const Fixture &f = V5;
        const tskval::Manifest m = read_manifest(f);
        const ImageCopy c(f, "checksum");
        const auto block_line = [](uint64_t block) {
            return "xfs: Block " + std::to_string(block) + " checksum failed";
        };

        const uint64_t file = entry_of(m, "/multiblock.bin").inode;
        const uint64_t inobt_root = c.get(agi_offset(c) + 20, 4);
        const uint64_t frag =
            inode_offset(f, entry_of(m, "/fragmented.bin").inode);
        const uint64_t bmbt =
            linear_block(f, c.get(bmbt_root_ptr(f, c, frag, 0), 8));
        const uint64_t dir = blocks_of(f, m, "/dirs/block").front();
        const uint64_t data = blocks_of(f, m, "/dirs/leaf").front();
        const uint64_t link = blocks_of(f, m, "/symlink-long").front();

        const struct {
            const char *name;
            uint64_t checksum; // byte offset of the stored checksum
            std::string line;
        } cases[] = {
            {"superblock", 224, "xfs_open: superblock checksum failed"},
            {"agi", agi_offset(c) + 312, block_line(0)},
            {"inode-btree", inobt_root * f.block_size + 52,
                block_line(inobt_root)},
            {"inode", inode_offset(f, file) + 100,
                "xfs_dinode_load: Inode " + std::to_string(file) +
                    " checksum failed"},
            {"extent-btree", bmbt * f.block_size + 64, block_line(bmbt)},
            {"directory", dir * f.block_size + 4, block_line(dir)},
            {"directory-data", data * f.block_size + 4, block_line(data)},
            {"symlink", link * f.block_size + 12, block_line(link)},
        };
        const std::string known = fstest::inode_known_failures(f);
        const auto count_inodes = [&c]() {
            Opened o(c.path());
            size_t inodes = 0;
            CPPUNIT_ASSERT_EQUAL(0,
                int{tsk_fs_meta_walk(o.fs.get(), o.fs->first_inum,
                    o.fs->last_inum, EVERY_INODE, count_inode, &inodes)});
            return inodes;
        };
        const size_t all_inodes = count_inodes();
        for (const auto &k : cases) {
            const std::string name = k.name;
            c.flip(k.checksum);
            fstest::WalkCheck walk;
            fstest::WalkCheck quiet;
            size_t inodes = 0;
            const std::string text = verbose_output(
                [&]() { walk = fstest::check_walk(c.path(), m, known); });
            const std::string walk_text =
                verbose_output([&]() { inodes = count_inodes(); });
            const std::string quiet_text = verbose_output(
                [&]() { quiet = fstest::check_walk(c.path(), m, known); },
                false);
            c.flip(k.checksum);
            CPPUNIT_ASSERT_MESSAGE(name + ": " + walk.report, walk.passed);
            CPPUNIT_ASSERT_EQUAL_MESSAGE(name + ": warnings",
                "[" + k.line + "]", show(checksum_lines(text)));
            CPPUNIT_ASSERT_EQUAL_MESSAGE(
                name + ": inodes walked", all_inodes, inodes);
            // The inode walk reads no directory and no extent map.
            const bool walked =
                name.compare(0, 9, "directory") != 0 && name != "extent-btree";
            CPPUNIT_ASSERT_EQUAL_MESSAGE(name + ": inode walk warnings",
                walked ? "[" + k.line + "]" : std::string("(none)"),
                show(checksum_lines(walk_text)));
            CPPUNIT_ASSERT_MESSAGE(name + ": " + quiet.report, quiet.passed);
            CPPUNIT_ASSERT_EQUAL_MESSAGE(name + ": output without tsk_verbose",
                std::string(), quiet_text);
        }
    }

    // A structure whose magic is not its own: the superblock (no file
    // system), the AGI and an inode (no inode), a directory block (no
    // directory), an extent B+tree block (no content).
    void testBadMagic()
    {
        const Fixture &f = V4;
        const tskval::Manifest m = read_manifest(f);
        const ImageCopy c(f, "magic");
        const uint64_t file = entry_of(m, "/multiblock.bin").inode;
        const uint64_t dir = entry_of(m, "/dirs/block").inode;
        const uint64_t frag = entry_of(m, "/fragmented.bin").inode;
        const Bytes bad = {'N', 'O', 'P', 'E'};
        {
            const Damage d(c, 0, bad);
            open_fails(c, TSK_ERR_FS_MAGIC, "superblock");
        }
        {
            const Damage d(c, agi_offset(c), bad);
            lookup_fails(c, file, TSK_ERR_FS_CORRUPT, "AGI");
        }
        {
            const Damage d(c, inode_offset(f, file), {'N', 'O'});
            lookup_fails(c, file, TSK_ERR_FS_INODE_COR, "inode");
        }
        {
            const Damage d(
                c, blocks_of(f, m, "/dirs/block").front() * f.block_size, bad);
            dir_fails(c, dir, TSK_ERR_FS_CORRUPT, "directory block");
        }
        {
            const uint64_t block = linear_block(
                f, c.get(bmbt_root_ptr(f, c, inode_offset(f, frag), 0), 8));
            const Damage d(c, block * f.block_size, bad);
            read_fails(c, frag, TSK_ERR_FS_INODE_COR, "extent B+tree block");
        }
    }

    // A superblock with a geometry no XFS has, beyond those of
    // xfs_superblock_test.cpp: sectors larger than blocks, and more blocks
    // than byte offsets can address.
    void testImpossibleGeometry()
    {
        {
            const Fixture &f = V4_512;
            const ImageCopy c(f, "sector");
            const Damage size(c, SB_SECTSIZE, be(1024, 2));
            const Damage log(c, SB_SECTLOG, {10});
            open_fails(c, TSK_ERR_FS_CORRUPT, "sector larger than block");
        }
        {
            // 2^32 - 1 AGs of 2^27 blocks of 4 KiB: 2^71 bytes.
            const Fixture &f = V4;
            const ImageCopy c(f, "bytes");
            const Damage agblocks(c, SB_AGBLOCKS, be(uint64_t{1} << 27, 4));
            const Damage agblklog(c, SB_AGBLKLOG, {27});
            const Damage agcount(c, SB_AGCOUNT, be(0xffffffff, 4));
            const Damage dblocks(
                c, SB_DBLOCKS, be(uint64_t{0xffffffff} << 27, 8));
            open_fails(c, TSK_ERR_FS_CORRUPT, "byte offsets overflow");
        }
    }

    // An image cut in the middle of the second AG still opens. The inode
    // walk fails when it reaches what is missing (TSK_ERR_FS_READ), the
    // recursive walk of the names ends, and each file either reads to its
    // end or fails with a TSK error; some do fail.
    void testImageCutShort()
    {
        const Fixture &f = V4;
        const tskval::Manifest m = read_manifest(f);
        const ImageCopy c(f, "cut");
        c.cut(uint64_t{f.ag_blocks} * f.block_size * 3 / 2);
        quickly("cut", [&]() {
            Opened o(c.path());
            size_t inodes = 0;
            tsk_error_reset();
            CPPUNIT_ASSERT_EQUAL(1,
                int{tsk_fs_meta_walk(o.fs.get(), o.fs->first_inum,
                    o.fs->last_inum, TSK_FS_META_FLAG_ALLOC, count_inode,
                    &inodes)});
            check_errno(TSK_ERR_FS_READ, "inode walk");
            CPPUNIT_ASSERT_MESSAGE("no inode of the first AG", inodes > 0);

            DirWalkCount names;
            tsk_fs_dir_walk(o.fs.get(), o.fs->root_inum,
                TSK_FS_DIR_WALK_FLAG_RECURSE, DirWalkCount::cb, &names);
            CPPUNIT_ASSERT_MESSAGE("no names", names.names > 0);

            size_t failed = 0;
            size_t read = 0;
            std::vector<char> buf(1 << 16);
            for (const tskval::Entry &e : m.entries) {
                if (e.type != tskval::EntryType::File) {
                    continue;
                }
                tsk_error_reset();
                const FilePtr file(
                    tsk_fs_file_open_meta(o.fs.get(), nullptr, e.inode));
                bool good = file != nullptr;
                for (TSK_OFF_T at = 0; good && at < file->meta->size;) {
                    const ssize_t cnt = tsk_fs_file_read(file.get(), at,
                        buf.data(), buf.size(), TSK_FS_FILE_READ_FLAG_NONE);
                    good = cnt > 0;
                    at += cnt;
                }
                if (good) {
                    ++read;
                }
                else {
                    ++failed;
                    CPPUNIT_ASSERT_MESSAGE(e.path + ": failed with no error",
                        tsk_error_get_errno() != 0);
                }
            }
            CPPUNIT_ASSERT_MESSAGE("no file read", read > 0);
            CPPUNIT_ASSERT_MESSAGE("no file failed", failed > 0);
        });
    }

    // Block numbers that no block has: an extent in an AG past the last, an
    // extent that runs past the end of its AG, an extent B+tree block past
    // the last AG, an inode B+tree root past its AG.
    void testBlockNumbersOutOfRange()
    {
        const Fixture &f = V4;
        const tskval::Manifest m = read_manifest(f);
        const ImageCopy c(f, "blocks");
        const unsigned agblklog = f.ag_inode_bits - inopblog(f);
        const uint64_t file = entry_of(m, "/multiblock.bin").inode;
        const uint64_t fork = inode_offset(f, file) + fork_offset(f);
        CPPUNIT_ASSERT_EQUAL_MESSAGE("/multiblock.bin: extents format",
            uint64_t{2}, c.get(inode_offset(f, file) + 5, 1));
        {
            const Damage d(
                c, fork, extent(0, uint64_t{f.ag_count} << agblklog, 1));
            read_fails(c, file, TSK_ERR_FS_INODE_COR, "extent past last AG");
        }
        {
            const Damage d(c, fork, extent(0, f.ag_blocks - 1, 2));
            read_fails(c, file, TSK_ERR_FS_INODE_COR, "extent past its AG");
        }
        {
            const uint64_t frag = entry_of(m, "/fragmented.bin").inode;
            const Damage d(c, bmbt_root_ptr(f, c, inode_offset(f, frag), 0),
                be(uint64_t{f.ag_count} << agblklog, 8));
            read_fails(c, frag, TSK_ERR_FS_INODE_COR, "B+tree block");
        }
        {
            const Damage d(c, agi_offset(c) + AGI_ROOT, be(f.ag_blocks, 4));
            lookup_fails(c, file, TSK_ERR_FS_CORRUPT, "inode B+tree root");
            inode_walk_fails(c, f, TSK_ERR_FS_CORRUPT, "inode B+tree root");
        }
    }

    // A directory entry with an inode number that no inode has (0, or in no
    // AG) makes the directory corrupt: a short-form entry and parent, and an
    // entry of a directory block.
    void testInodeNumbersOutOfRange()
    {
        const Fixture &f = V4;
        const tskval::Manifest m = read_manifest(f);
        const ImageCopy c(f, "inodes");
        const uint64_t sf = entry_of(m, "/dirs/shortform").inode;
        const uint64_t fork = inode_offset(f, sf) + fork_offset(f);
        // The header: entry count, 8-byte inode count (0: 4-byte inode
        // numbers), parent. The first entry: name length, offset (2), name,
        // file type, inode number.
        CPPUNIT_ASSERT_EQUAL(uint64_t{0}, c.get(fork + 1, 1));
        const uint64_t first_ino = fork + 6 + 3 + c.get(fork + 6, 1) + 1;
        CPPUNIT_ASSERT_MESSAGE("not the first entry's inode number",
            c.get(first_ino, 4) > sf && c.get(first_ino, 4) < sf + 64);
        {
            const Damage d(c, first_ino, be(0xffffffff, 4));
            dir_fails(c, sf, TSK_ERR_FS_CORRUPT, "short-form entry, no AG");
        }
        {
            const Damage d(c, first_ino, be(0, 4));
            dir_fails(c, sf, TSK_ERR_FS_CORRUPT, "short-form entry, 0");
        }
        {
            const Damage d(c, fork + 2, be(0xffffffff, 4));
            dir_fails(c, sf, TSK_ERR_FS_CORRUPT, "short-form parent");
        }
        // The entries of a directory block start after its 16-byte header,
        // each with its inode number (8 bytes).
        const uint64_t dir = entry_of(m, "/dirs/block").inode;
        const uint64_t block =
            blocks_of(f, m, "/dirs/block").front() * f.block_size;
        CPPUNIT_ASSERT_EQUAL(dir, c.get(block + 16, 8)); // "."
        {
            const Damage d(c, block + 16, be(uint64_t{1} << 40, 8));
            dir_fails(c, dir, TSK_ERR_FS_CORRUPT, "block entry");
        }
    }

    // A directory entry that names an ancestor as a subdirectory (the root,
    // or the directory itself): the recursive walk doesn't follow the cycle
    // and ends without an error (libtsk's walk keeps to directories it is
    // not inside already; it lists the root once more, not below it).
    void testDirectoryCycle()
    {
        const Fixture &f = V4;
        const tskval::Manifest m = read_manifest(f);
        const ImageCopy c(f, "cycle");
        const uint64_t sf = entry_of(m, "/dirs/shortform").inode;
        const uint64_t fork = inode_offset(f, sf) + fork_offset(f);
        const uint64_t ftype = fork + 6 + 3 + c.get(fork + 6, 1);
        const auto walk = [&]() {
            Opened o(c.path());
            DirWalkCount names;
            tsk_error_reset();
            const uint8_t rc = tsk_fs_dir_walk(o.fs.get(), o.fs->root_inum,
                TSK_FS_DIR_WALK_FLAG_RECURSE, DirWalkCount::cb, &names);
            CPPUNIT_ASSERT_EQUAL_MESSAGE(error_text(), 0, int{rc});
            return names.names;
        };
        const size_t whole = walk();
        CPPUNIT_ASSERT_MESSAGE("few names", whole > m.entries.size());
        for (const uint64_t target : {entry_of(m, "/").inode, sf}) {
            const Damage type(c, ftype, {2}); // a directory
            const Damage ino(c, ftype + 1, be(target, 4));
            quickly("cycle", [&]() {
                const size_t names = walk();
                CPPUNIT_ASSERT_MESSAGE(std::to_string(names) +
                        " names, not about " + std::to_string(whole),
                    names >= whole && names < whole + 64);
            });
        }
    }

    // An extent count far beyond what the inode or its B+tree holds: in an
    // inode with an extent list (a file, a directory), in one with an extent
    // B+tree, and as the record count of a B+tree block. No content; nothing
    // is allocated for the count.
    void testHugeExtentCount()
    {
        const Fixture &f = V4;
        const tskval::Manifest m = read_manifest(f);
        const ImageCopy c(f, "count");
        const uint64_t file = entry_of(m, "/multiblock.bin").inode;
        const uint64_t dir = entry_of(m, "/dirs/leaf").inode;
        const uint64_t frag = entry_of(m, "/fragmented.bin").inode;
        const Bytes huge = be(0xffffffff, 4);
        {
            const Damage d(c, inode_offset(f, file) + DI_NEXTENTS, huge);
            read_fails(c, file, TSK_ERR_FS_INODE_COR, "extent list");
        }
        {
            const Damage d(c, inode_offset(f, dir) + DI_NEXTENTS, huge);
            dir_fails(c, dir, TSK_ERR_FS_INODE_COR, "directory");
        }
        {
            const Damage d(c, inode_offset(f, frag) + DI_NEXTENTS, huge);
            read_fails(c, frag, TSK_ERR_FS_INODE_COR, "extent B+tree");
        }
        {
            // bb_numrecs, after the magic and the level.
            const uint64_t block = linear_block(
                f, c.get(bmbt_root_ptr(f, c, inode_offset(f, frag), 0), 8));
            const Damage d(c, block * f.block_size + 6, be(0xffff, 2));
            read_fails(c, frag, TSK_ERR_FS_INODE_COR, "B+tree block records");
        }
    }

    // An extent B+tree that is no tree: /fragmented.bin of the 512-byte-block
    // Fixture has three levels (the root in the inode, a node, leaves). A
    // node that points to itself, and a node whose pointers all lead to one
    // leaf, are corrupt.
    void testExtentTreeLoops()
    {
        const Fixture &f = V4_512;
        const tskval::Manifest m = read_manifest(f);
        const ImageCopy c(f, "bmbt-loop");
        const uint64_t frag = entry_of(m, "/fragmented.bin").inode;
        const uint64_t ino = inode_offset(f, frag);
        CPPUNIT_ASSERT_EQUAL_MESSAGE(
            "root level", uint64_t{2}, c.get(ino + fork_offset(f), 2));
        const uint64_t node_fsb = c.get(bmbt_root_ptr(f, c, ino, 0), 8);
        const uint64_t node = linear_block(f, node_fsb) * f.block_size;
        CPPUNIT_ASSERT_EQUAL_MESSAGE(
            "node level", uint64_t{1}, c.get(node + 4, 2));
        const uint64_t numrecs = c.get(node + 6, 2);
        CPPUNIT_ASSERT_MESSAGE("node has one pointer", numrecs >= 2);
        // After the 24-byte header: room for the most keys, then pointers.
        const uint64_t ptrs = node + 24 + (f.block_size - 24) / 16 * 8;
        {
            const Damage d(c, ptrs, be(node_fsb, 8));
            read_fails(c, frag, TSK_ERR_FS_INODE_COR, "node points to itself");
        }
        {
            Bytes same;
            const Bytes first = c.read(ptrs, 8);
            for (uint64_t i = 0; i < numrecs; ++i) {
                same.insert(same.end(), first.begin(), first.end());
            }
            const Damage d(c, ptrs, same);
            read_fails(c, frag, TSK_ERR_FS_INODE_COR, "one leaf many times");
        }
    }

    // An inode B+tree that is no tree, built in unused blocks at the end of
    // AG 0: a root node that points to itself, and nine levels of nodes whose
    // every pointer leads to the same node below, down to a leaf without
    // records (510^8 paths). Both are corrupt, at once.
    void testInodeTreeLoops()
    {
        const Fixture &f = V4;
        const tskval::Manifest m = read_manifest(f);
        const ImageCopy c(f, "inobt-loop");
        const uint64_t file = entry_of(m, "/multiblock.bin").inode;
        const uint64_t agi = agi_offset(c);
        // A block: the magic, the level, the record count and no siblings,
        // then the keys (0) and after room for the most keys the pointers.
        const size_t maxrecs = (f.block_size - 16) / 8;
        const auto block = [&](unsigned level, size_t numrecs, uint32_t ptr) {
            Bytes b(f.block_size);
            const Bytes head = {'I', 'A', 'B', 'T', 0,
                static_cast<uint8_t>(level), static_cast<uint8_t>(numrecs >> 8),
                static_cast<uint8_t>(numrecs), 0xff, 0xff, 0xff, 0xff, 0xff,
                0xff, 0xff, 0xff};
            std::copy(head.begin(), head.end(), b.begin());
            for (size_t i = 0; level > 0 && i < numrecs; ++i) {
                const Bytes p = be(ptr, 4);
                std::copy(
                    p.begin(), p.end(), b.begin() + 16 + maxrecs * 4 + i * 4);
            }
            return b;
        };
        const uint32_t top = f.ag_blocks - 16;
        {
            const Damage node(
                c, uint64_t{top} * f.block_size, block(1, 1, top));
            const Damage root(c, agi + AGI_ROOT, be(top, 4));
            const Damage level(c, agi + AGI_LEVEL, be(2, 4));
            lookup_fails(c, file, TSK_ERR_FS_CORRUPT, "self, lookup");
            inode_walk_fails(c, f, TSK_ERR_FS_CORRUPT, "self, walk");
        }
        {
            Bytes blocks;
            for (unsigned level = 8; level >= 1; --level) {
                const Bytes b = block(level, maxrecs, top + 9 - level);
                blocks.insert(blocks.end(), b.begin(), b.end());
            }
            const Bytes leaf = block(0, 0, 0);
            blocks.insert(blocks.end(), leaf.begin(), leaf.end());
            const Damage nodes(c, uint64_t{top} * f.block_size, blocks);
            const Damage root(c, agi + AGI_ROOT, be(top, 4));
            const Damage level(c, agi + AGI_LEVEL, be(9, 4));
            inode_walk_fails(c, f, TSK_ERR_FS_CORRUPT, "paths, walk");
            // A lookup goes down one path and finds no chunk: no inode.
            lookup_fails(c, file, TSK_ERR_FS_INODE_NUM, "paths, lookup");
        }
    }
};

CPPUNIT_TEST_SUITE_REGISTRATION(XfsCorruptTest);

} // namespace
