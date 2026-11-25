/*
 * XFS inodes and file content (#39), on the Fixture images through the
 * public API: every inode of the expected Manifest opens by its number with
 * the metadata xfs_db reads, regular files read byte for byte, and inode
 * numbers that are no allocated inode give a TSK error.
 */
#include "fs_test_util.h"
#include "manifest_check.h"
#include "xfs_fixture_data.h"

#include "manifest.h"
#include "toolcheck.h"

#include <tsk/libtsk.h>

#include <cppunit/extensions/HelperMacros.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
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

// Differences that later tickets of #11 remove.
const char LATER[] = "/symlink-short target symlink targets come in #43\n"
                     "/symlink-long target symlink targets come in #43\n";

// The harness records a crtime of 0 as no crtime (libtsk has no other way to
// say there is none), so a v5 inode created at the epoch differs.
const char CRTIME_0[] = "/times/epoch crtime crtime 0 reads as none\n";

// The differences a Fixture's comparison tolerates.
std::string
known_failures(const Fixture &f)
{
    return std::string(LATER) + (f.version == 5 ? CRTIME_0 : "");
}

// The Manifest's entry for `path`.
const tskval::Entry &
entry_of(const tskval::Manifest &m, const std::string &path)
{
    for (const tskval::Entry &e : m.entries) {
        if (e.path == path) {
            return e;
        }
    }
    CPPUNIT_FAIL("the Manifest has no " + path);
    return m.entries.front();
}

// The inode number of `path` in the Manifest.
uint64_t
inode_of(const tskval::Manifest &m, const std::string &path)
{
    return entry_of(m, path).inode;
}

// Opens the Fixture's entry at `path` by its inode number in the Manifest
// (paths can't be resolved before #40); asserts that it opened.
TSK_FS_FILE *
open_path(const Fixture &f, TSK_FS_INFO *fs, const std::string &path)
{
    const TSK_INUM_T inum = inode_of(read_manifest(f), path);
    TSK_FS_FILE *file = tsk_fs_file_open_meta(fs, nullptr, inum);
    CPPUNIT_ASSERT_MESSAGE(path + ": " + error_text(), file != nullptr);
    return file;
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

// Writes `bytes` as the image xfs-inode-<name>.img in the test data
// directory and returns its path. The file system is cut short, which libtsk
// accepts (last_block_act).
std::string
write_image(const std::string &name, const std::vector<char> &bytes)
{
    const std::string path = fstest::data_path("xfs-inode-" + name + ".img");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    CPPUNIT_ASSERT_MESSAGE("cannot write " + path, out.good());
    return path;
}

// tsk_fs_file_walk callback: the file offsets of the blocks read from disk.
TSK_WALK_RET_ENUM
collect_raw_offsets(TSK_FS_FILE *, TSK_OFF_T off, TSK_DADDR_T, char *, size_t,
    TSK_FS_BLOCK_FLAG_ENUM flags, void *ptr)
{
    if (flags & TSK_FS_BLOCK_FLAG_RAW) {
        static_cast<std::vector<TSK_OFF_T> *>(ptr)->push_back(off);
    }
    return TSK_WALK_CONT;
}

class XfsInodeTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(XfsInodeTest);
    CPPUNIT_TEST(testEveryEntryOpensByInode);
    CPPUNIT_TEST(testLaterAllocationGroups);
    CPPUNIT_TEST(testNoInodeGivesError);
    CPPUNIT_TEST(testOrphanFilesAddress);
    CPPUNIT_TEST(testHolesAreSparse);
    CPPUNIT_TEST(testUnwrittenExtentReadsAsZeros);
    CPPUNIT_TEST(testLocalDataIsResident);
    CPPUNIT_TEST(testExtentTreeNotReadYet);
    CPPUNIT_TEST(testIstatMetadata);
    CPPUNIT_TEST(testIstatBlocks);
    CPPUNIT_TEST(testIstatNoInodeGivesError);
    CPPUNIT_TEST_SUITE_END();

  public:
    // Every entry of the expected Manifest, opened by its inode number, has
    // the Manifest's type, mode, uid, gid, size, link count, mtime, ctime and
    // (v5) crtime, to the nanosecond; every regular file's content has the
    // Manifest's SHA-256.
    void testEveryEntryOpensByInode()
    {
        for (const Fixture &f : ALL_FIXTURES) {
            const std::string v = f.variant;
            const fstest::WalkCheck check = fstest::check_by_inode(
                image_path(f), read_manifest(f), known_failures(f));
            CPPUNIT_ASSERT_MESSAGE(v + ": " + check.report, check.passed);
        }
    }

    // Inode numbers carry their AG above the AG-relative bits, and data
    // block numbers theirs above sb_agblklog bits: in AGs other than the
    // first, both must be decoded (testEveryEntryOpensByInode compares them).
    // Each Fixture has inodes there, and a regular file there whose blocks
    // (as TSK addresses) are in its AG too.
    void testLaterAllocationGroups()
    {
        for (const Fixture &f : ALL_FIXTURES) {
            const std::string v = f.variant;
            const tskval::Manifest manifest = read_manifest(f);
            ImgPtr img = open_image(image_path(f));
            FsPtr fs(tsk_fs_open_img(img.get(), 0, TSK_FS_TYPE_XFS));
            CPPUNIT_ASSERT_MESSAGE(v + ": " + error_text(), fs != nullptr);
            size_t later = 0;
            bool file_checked = false;
            for (const tskval::Entry &e : manifest.entries) {
                const uint64_t ag = e.inode >> f.ag_inode_bits;
                if (ag == 0) {
                    continue;
                }
                ++later;
                if (file_checked || e.type != tskval::EntryType::File ||
                    e.size == 0) {
                    continue;
                }
                fstest::FilePtr file(
                    tsk_fs_file_open_meta(fs.get(), nullptr, e.inode));
                CPPUNIT_ASSERT_MESSAGE(
                    v + ": " + error_text(), file != nullptr);
                const std::vector<TSK_DADDR_T> blocks =
                    fstest::file_blocks(file.get());
                CPPUNIT_ASSERT_MESSAGE(v + ": no blocks", !blocks.empty());
                for (const TSK_DADDR_T b : blocks) {
                    CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": block " +
                            std::to_string(b) + " of inode " +
                            std::to_string(e.inode) + " in AG",
                        ag, static_cast<uint64_t>(b / f.ag_blocks));
                }
                file_checked = true;
            }
            CPPUNIT_ASSERT_MESSAGE(v + ": no inode in a later AG", later > 0);
            CPPUNIT_ASSERT_MESSAGE(
                v + ": no regular file in a later AG", file_checked);
        }
    }

    // A number that is no allocated inode gives a TSK error, never a file:
    // above the largest inode, a block number past the end of the AG (the AG
    // block bits are rounded up), the superblock's bytes (no inode chunk),
    // and a free inode in an allocated chunk.
    void testNoInodeGivesError()
    {
        for (const Fixture &f : ALL_FIXTURES) {
            const std::string v = f.variant;
            ImgPtr img = open_image(image_path(f));
            FsPtr fs(tsk_fs_open_img(img.get(), 0, TSK_FS_TYPE_XFS));
            CPPUNIT_ASSERT_MESSAGE(v + ": " + error_text(), fs != nullptr);
            const unsigned inopblog = f.ag_inode_bits - ceil_log2(f.ag_blocks);
            const struct {
                const char *what;
                TSK_INUM_T inum;
            } cases[] = {
                {"past the last inode", f.max_inode + 2},
                {"far past the last inode", ~TSK_INUM_T{0}},
                {"past the end of AG 0", TSK_INUM_T{f.ag_blocks} << inopblog},
                {"the superblock", 0},
                {"a free inode", f.free_inode},
            };
            for (const auto &c : cases) {
                const std::string what =
                    v + ": " + c.what + " (" + std::to_string(c.inum) + ")";
                tsk_error_reset();
                fstest::FilePtr file(
                    tsk_fs_file_open_meta(fs.get(), nullptr, c.inum));
                CPPUNIT_ASSERT_MESSAGE(what + " opened", file == nullptr);
                CPPUNIT_ASSERT_EQUAL_MESSAGE(what + ": " + error_text(),
                    static_cast<uint32_t>(TSK_ERR_FS_INODE_NUM),
                    tsk_error_get_errno());
            }
            tsk_error_reset();
        }
    }

    // last_inum is not an inode: like other file systems, it is the virtual
    // $OrphanFiles directory.
    void testOrphanFilesAddress()
    {
        for (const Fixture &f : {fstest::V4, fstest::V5}) {
            const std::string v = f.variant;
            ImgPtr img = open_image(image_path(f));
            FsPtr fs(tsk_fs_open_img(img.get(), 0, TSK_FS_TYPE_XFS));
            CPPUNIT_ASSERT_MESSAGE(v + ": " + error_text(), fs != nullptr);
            fstest::FilePtr file(
                tsk_fs_file_open_meta(fs.get(), nullptr, fs->last_inum));
            CPPUNIT_ASSERT_MESSAGE(v + ": " + error_text(), file != nullptr);
            CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": type",
                static_cast<int>(TSK_FS_META_TYPE_VIRT_DIR),
                static_cast<int>(file->meta->type));
        }
    }

    // /sparse.bin (512 KiB with holes at the start, in the middle and at the
    // end; its content is compared in testEveryEntryOpensByInode) has blocks
    // only where xfs_db's bmap has data: two blocks, at file blocks 32 and
    // 80 (4 KiB blocks). The holes are sparse, not blocks.
    void testHolesAreSparse()
    {
        for (const Fixture &f : {fstest::V4, fstest::V5}) {
            const std::string v = f.variant;
            ImgPtr img = open_image(image_path(f));
            FsPtr fs(tsk_fs_open_img(img.get(), 0, TSK_FS_TYPE_XFS));
            CPPUNIT_ASSERT_MESSAGE(v + ": " + error_text(), fs != nullptr);
            fstest::FilePtr file(open_path(f, fs.get(), "/sparse.bin"));
            CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": data blocks", size_t{2},
                fstest::file_blocks(file.get()).size());
            std::vector<TSK_OFF_T> offsets;
            const uint8_t rc = tsk_fs_file_walk(file.get(),
                TSK_FS_FILE_WALK_FLAG_AONLY, collect_raw_offsets, &offsets);
            CPPUNIT_ASSERT_EQUAL(0, static_cast<int>(rc));
            CPPUNIT_ASSERT_EQUAL_MESSAGE(
                v + ": data blocks with offsets", size_t{2}, offsets.size());
            CPPUNIT_ASSERT_EQUAL(TSK_OFF_T{32 * 4096}, offsets[0]);
            CPPUNIT_ASSERT_EQUAL(TSK_OFF_T{80 * 4096}, offsets[1]);
        }
    }

    // An extent with the unwritten flag (preallocated, never written) reads
    // as zeros, though its blocks hold data. A copy of the start of the v4
    // image (no inode CRCs) with the flag set on /multiblock.bin's only
    // extent (xfs_db: inode 133, file blocks 0-3 at block 13).
    void testUnwrittenExtentReadsAsZeros()
    {
        const Fixture &f = fstest::V4;
        const TSK_INUM_T inum = 133;
        const size_t size = 14336;
        std::vector<char> image = image_start(f, 1 << 20);
        const size_t inode_off =
            (inum >> 4) * f.block_size + (inum & 15) * f.inode_size;
        const size_t fork_off = inode_off + 100; // after a v2 inode core
        CPPUNIT_ASSERT_EQUAL_MESSAGE("not the extent xfs_db shows", 0,
            static_cast<int>(image[fork_off]));
        image[fork_off] = static_cast<char>(0x80); // the flag: bit 127

        for (const bool unwritten : {false, true}) {
            const std::string path =
                write_image(unwritten ? "unwritten" : "written",
                    unwritten ? image : image_start(f, 1 << 20));
            ImgPtr img = open_image(path, TSK_IMG_TYPE_RAW);
            FsPtr fs(tsk_fs_open_img(img.get(), 0, TSK_FS_TYPE_XFS));
            CPPUNIT_ASSERT_MESSAGE(error_text(), fs != nullptr);
            fstest::FilePtr file(
                tsk_fs_file_open_meta(fs.get(), nullptr, inum));
            CPPUNIT_ASSERT_MESSAGE(error_text(), file != nullptr);
            CPPUNIT_ASSERT_EQUAL(
                static_cast<TSK_OFF_T>(size), file->meta->size);
            std::vector<char> buf(size, 'x');
            CPPUNIT_ASSERT_EQUAL(static_cast<ssize_t>(size),
                tsk_fs_file_read(file.get(), 0, buf.data(), size,
                    TSK_FS_FILE_READ_FLAG_NONE));
            const bool zeros = buf == std::vector<char>(size, 0);
            CPPUNIT_ASSERT_EQUAL_MESSAGE(
                unwritten ? "unwritten: zeros" : "written: data", unwritten,
                zeros);
            CPPUNIT_ASSERT_EQUAL_MESSAGE("blocks", size_t{unwritten ? 0u : 4u},
                fstest::file_blocks(file.get()).size());
        }
    }

    // Data stored in the inode (data fork "local"), such as a short-form
    // directory, is the resident default attribute: the directory's raw
    // entries, di_size bytes. /dirs/shortform (xfs_db): 54 bytes, a header
    // of 3 entries, all with 4-byte inode numbers, parent /dirs.
    void testLocalDataIsResident()
    {
        for (const Fixture &f : {fstest::V4, fstest::V5}) {
            const std::string v = f.variant;
            ImgPtr img = open_image(image_path(f));
            FsPtr fs(tsk_fs_open_img(img.get(), 0, TSK_FS_TYPE_XFS));
            CPPUNIT_ASSERT_MESSAGE(v + ": " + error_text(), fs != nullptr);
            const tskval::Manifest manifest = read_manifest(f);
            fstest::FilePtr file(open_path(f, fs.get(), "/dirs/shortform"));
            const TSK_FS_ATTR *attr = tsk_fs_file_attr_get(file.get());
            CPPUNIT_ASSERT_MESSAGE(v + ": " + error_text(), attr != nullptr);
            CPPUNIT_ASSERT_MESSAGE(
                v + ": resident", (attr->flags & TSK_FS_ATTR_RES) != 0);
            CPPUNIT_ASSERT_EQUAL_MESSAGE(
                v + ": size", TSK_OFF_T{54}, attr->size);
            char buf[54];
            CPPUNIT_ASSERT_EQUAL(ssize_t{54},
                tsk_fs_file_read(file.get(), 0, buf, sizeof(buf),
                    TSK_FS_FILE_READ_FLAG_NONE));
            CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": count", 3, int{buf[0]});
            CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": i8count", 0, int{buf[1]});
            const auto *u = reinterpret_cast<const uint8_t *>(buf);
            const uint64_t parent = (uint64_t{u[2]} << 24) |
                (uint64_t{u[3]} << 16) | (uint64_t{u[4]} << 8) | u[5];
            CPPUNIT_ASSERT_EQUAL_MESSAGE(
                v + ": parent", inode_of(manifest, "/dirs"), parent);
        }
    }

    // A data fork that is the root of an extent B+tree is not read yet
    // (#42): reading its content fails with TSK_ERR_FS_UNSUPFUNC, while its
    // metadata opens (testEveryEntryOpensByInode). /dirs/btree is one.
    void testExtentTreeNotReadYet()
    {
        for (const Fixture &f : {fstest::V4, fstest::V5}) {
            const std::string v = f.variant;
            ImgPtr img = open_image(image_path(f));
            FsPtr fs(tsk_fs_open_img(img.get(), 0, TSK_FS_TYPE_XFS));
            CPPUNIT_ASSERT_MESSAGE(v + ": " + error_text(), fs != nullptr);
            fstest::FilePtr file(open_path(f, fs.get(), "/dirs/btree"));
            char buf[16];
            tsk_error_reset();
            CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": read", ssize_t{-1},
                tsk_fs_file_read(file.get(), 0, buf, sizeof(buf),
                    TSK_FS_FILE_READ_FLAG_NONE));
            CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": " + error_text(),
                static_cast<uint32_t>(TSK_ERR_FS_UNSUPFUNC),
                tsk_error_get_errno());
            tsk_error_reset();
        }
    }

    // istat shows each inode's metadata as the validate command's istat
    // check reads it (its parser, with TZ=UTC as there): inode, type, mode,
    // uid, gid, size, link count, mtime, ctime and (v5) crtime match the
    // Manifest, for files of each data fork format and special files.
    void testIstatMetadata()
    {
        const UtcZone utc;
        for (const Fixture &f : ALL_FIXTURES) {
            const std::string v = f.variant;
            const tskval::Manifest manifest = read_manifest(f);
            ImgPtr img = open_image(image_path(f));
            FsPtr fs(tsk_fs_open_img(img.get(), 0, TSK_FS_TYPE_XFS));
            CPPUNIT_ASSERT_MESSAGE(v + ": " + error_text(), fs != nullptr);
            for (const char *path : {"/", "/multiblock.bin", "/sparse.bin",
                     "/empty", "/setuid", "/dirs/shortform", "/dirs/btree",
                     "/fifo", "/chardev", "/symlink-short"}) {
                const tskval::Entry want = entry_of(manifest, path);
                const std::string text =
                    fstest::istat_text(fs.get(), want.inode);
                tskval::Entry got;
                std::string error;
                // "<variant>: <path>: <field>", and istat's output if asked.
                const auto msg = [&](const char *field, bool output) {
                    std::string m = v;
                    m.append(": ").append(path).append(": ").append(field);
                    if (output) {
                        m.append("\n").append(text);
                    }
                    return m;
                };
                const bool parsed = tskval::parse_istat(text, got, error);
                CPPUNIT_ASSERT_MESSAGE(msg(error.c_str(), true), parsed);
                CPPUNIT_ASSERT_EQUAL_MESSAGE(
                    msg("inode", false), want.inode, got.inode);
                CPPUNIT_ASSERT_MESSAGE(
                    msg("type", true), want.type == got.type);
                CPPUNIT_ASSERT_EQUAL_MESSAGE(
                    msg("mode", false), want.mode, got.mode);
                CPPUNIT_ASSERT_EQUAL_MESSAGE(
                    msg("uid", false), want.uid, got.uid);
                CPPUNIT_ASSERT_EQUAL_MESSAGE(
                    msg("gid", false), want.gid, got.gid);
                CPPUNIT_ASSERT_EQUAL_MESSAGE(
                    msg("size", false), want.size, got.size);
                CPPUNIT_ASSERT_EQUAL_MESSAGE(
                    msg("nlink", false), want.nlink, got.nlink);
                CPPUNIT_ASSERT_MESSAGE(
                    msg("mtime", true), want.mtime == got.mtime);
                CPPUNIT_ASSERT_MESSAGE(
                    msg("ctime", true), want.ctime == got.ctime);
                CPPUNIT_ASSERT_EQUAL_MESSAGE(
                    msg("has crtime", true), want.has_crtime, got.has_crtime);
                CPPUNIT_ASSERT_MESSAGE(
                    msg("crtime", true), want.crtime == got.crtime);
            }
        }
    }

    // istat lists a file's blocks, as ext's istat does ("Direct Blocks:",
    // one address per block, 0 for a hole), or with TSK_FS_ISTAT_RUNLIST
    // its runs. /multiblock.bin is 3.5 blocks in one extent (xfs_db: blocks
    // 13-16 on v4, 24-27 on v5).
    void testIstatBlocks()
    {
        for (const Fixture &f : {fstest::V4, fstest::V5}) {
            const std::string v = f.variant;
            // The ends of istat's output without and with -r.
            const std::string blocks = f.version == 5
                ? "\nDirect Blocks:\n24 25 26 27 \n"
                : "\nDirect Blocks:\n13 14 15 16 \n";
            const std::string runs_end = f.version == 5
                ? "\nDirect Blocks:\n  Starting address: 24, length: 4  \n"
                : "\nDirect Blocks:\n  Starting address: 13, length: 4  \n";
            const tskval::Manifest manifest = read_manifest(f);
            ImgPtr img = open_image(image_path(f));
            FsPtr fs(tsk_fs_open_img(img.get(), 0, TSK_FS_TYPE_XFS));
            CPPUNIT_ASSERT_MESSAGE(v + ": " + error_text(), fs != nullptr);
            const uint64_t inum = inode_of(manifest, "/multiblock.bin");

            const std::string text = fstest::istat_text(fs.get(), inum);
            CPPUNIT_ASSERT_MESSAGE(
                std::string(v).append(": blocks\n").append(text),
                ends_with(text, blocks));

            const std::string runs =
                fstest::istat_text(fs.get(), inum, TSK_FS_ISTAT_RUNLIST);
            CPPUNIT_ASSERT_MESSAGE(
                std::string(v).append(": runs\n").append(runs),
                ends_with(runs, runs_end));

            // Data in the inode has no blocks to list.
            const std::string local = fstest::istat_text(
                fs.get(), inode_of(manifest, "/dirs/shortform"));
            CPPUNIT_ASSERT_MESSAGE(
                std::string(v).append(": local\n").append(local),
                local.find("Direct Blocks:") == std::string::npos);
        }
    }

    // istat of a number that is no allocated inode fails with the lookup's
    // TSK error.
    void testIstatNoInodeGivesError()
    {
        for (const Fixture &f : {fstest::V4, fstest::V5}) {
            ImgPtr img = open_image(image_path(f));
            FsPtr fs(tsk_fs_open_img(img.get(), 0, TSK_FS_TYPE_XFS));
            CPPUNIT_ASSERT_MESSAGE(error_text(), fs != nullptr);
            for (const TSK_INUM_T inum : {TSK_INUM_T{0}, f.free_inode}) {
                bool ok = true;
                tsk_error_reset();
                fstest::printed_text(
                    [&](FILE *out) {
                        return fs->istat(
                            fs.get(), TSK_FS_ISTAT_NONE, out, inum, 0, 0);
                    },
                    ok);
                CPPUNIT_ASSERT_MESSAGE(
                    std::to_string(inum) + " succeeded", !ok);
                CPPUNIT_ASSERT_EQUAL(
                    static_cast<uint32_t>(TSK_ERR_FS_INODE_NUM),
                    tsk_error_get_errno());
                tsk_error_reset();
            }
        }
    }

  private:
    // TZ=UTC while it lives (istat prints local time).
    class UtcZone {
      public:
        UtcZone()
        {
            const char *tz = getenv("TZ");
            m_had = tz != nullptr;
            m_old = m_had ? tz : "";
            setenv("TZ", "UTC", 1);
            tzset();
        }
        ~UtcZone()
        {
            if (m_had) {
                setenv("TZ", m_old.c_str(), 1);
            }
            else {
                unsetenv("TZ");
            }
            tzset();
        }
        UtcZone(const UtcZone &) = delete;
        UtcZone &operator=(const UtcZone &) = delete;

      private:
        bool m_had = false;
        std::string m_old;
    };

    static bool ends_with(const std::string &s, const std::string &end)
    {
        return s.size() >= end.size() &&
            s.compare(s.size() - end.size(), end.size(), end) == 0;
    }

    // ceil(log2(n)), as mkfs computes sb_agblklog.
    static unsigned ceil_log2(uint64_t n)
    {
        unsigned log = 0;
        while ((uint64_t{1} << log) < n) {
            ++log;
        }
        return log;
    }
};

CPPUNIT_TEST_SUITE_REGISTRATION(XfsInodeTest);

} // namespace
