/*
 * XFS short-form and block directories (#40), on the Fixture images through
 * the public API: each directory lists the names, inode numbers and types the
 * expected Manifest has, with "." and ".." as ext reports them, and walks and
 * path lookups go through them.
 */
#include "fs_test_util.h"
#include "xfs_fixture_data.h"

#include "manifest.h"

#include <tsk/libtsk.h>

#include <cppunit/extensions/HelperMacros.h>

#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

using fstest::ALL_FIXTURES;
using fstest::entry_of;
using fstest::error_text;
using fstest::Fixture;
using fstest::FsPtr;
using fstest::image_path;
using fstest::ImgPtr;
using fstest::open_image;
using fstest::read_manifest;

struct DirCloser {
    void operator()(TSK_FS_DIR *dir) const
    {
        tsk_fs_dir_close(dir);
    }
};
using DirPtr = std::unique_ptr<TSK_FS_DIR, DirCloser>;

// The name type a directory entry with ftype records for a Manifest type.
TSK_FS_NAME_TYPE_ENUM
name_type(tskval::EntryType t)
{
    switch (t) {
    case tskval::EntryType::File:
        return TSK_FS_NAME_TYPE_REG;
    case tskval::EntryType::Dir:
        return TSK_FS_NAME_TYPE_DIR;
    case tskval::EntryType::Symlink:
        return TSK_FS_NAME_TYPE_LNK;
    case tskval::EntryType::CharDev:
        return TSK_FS_NAME_TYPE_CHR;
    case tskval::EntryType::BlockDev:
        return TSK_FS_NAME_TYPE_BLK;
    case tskval::EntryType::Fifo:
        return TSK_FS_NAME_TYPE_FIFO;
    case tskval::EntryType::Socket:
        return TSK_FS_NAME_TYPE_SOCK;
    case tskval::EntryType::Other:
        break;
    }
    return TSK_FS_NAME_TYPE_UNDEF;
}

// One listed name: inode number and name type.
struct Listed {
    uint64_t inode;
    TSK_FS_NAME_TYPE_ENUM type;
    TSK_FS_NAME_FLAG_ENUM flags;
};

// What a directory should list, by name: "." and ".." (always
// TSK_FS_NAME_TYPE_DIR in a short-form directory, which doesn't store them;
// a block directory stores them, with a type only if the file system has
// ftype), then the Manifest's entries directly below `dir`, typed by the
// Manifest with ftype, else TSK_FS_NAME_TYPE_UNDEF. All allocated.
std::map<std::string, Listed>
expected_names(const Fixture &f, const tskval::Manifest &m,
    const std::string &dir, bool dots_stored)
{
    const size_t slash = dir.rfind('/');
    const std::string parent = slash == 0 ? "/" : dir.substr(0, slash);
    const TSK_FS_NAME_TYPE_ENUM dot_type =
        !dots_stored || f.ftype ? TSK_FS_NAME_TYPE_DIR : TSK_FS_NAME_TYPE_UNDEF;
    std::map<std::string, Listed> want;
    want["."] = {entry_of(m, dir).inode, dot_type, TSK_FS_NAME_FLAG_ALLOC};
    want[".."] = {entry_of(m, parent).inode, dot_type, TSK_FS_NAME_FLAG_ALLOC};
    const std::string prefix = dir == "/" ? "/" : dir + "/";
    for (const tskval::Entry &e : m.entries) {
        if (e.path.size() <= prefix.size() ||
            e.path.compare(0, prefix.size(), prefix) != 0 ||
            e.path.find('/', prefix.size()) != std::string::npos) {
            continue;
        }
        want[e.path.substr(prefix.size())] = {e.inode,
            f.ftype ? name_type(e.type) : TSK_FS_NAME_TYPE_UNDEF,
            TSK_FS_NAME_FLAG_ALLOC};
    }
    return want;
}

// What tsk_fs_dir_open_meta lists for the directory at `dir`, opened by its
// inode number in the Manifest; asserts that it opened, and that no name is
// listed twice.
std::map<std::string, Listed>
listed_names(const Fixture &f, TSK_FS_INFO *fs, const tskval::Manifest &m,
    const std::string &dir)
{
    const std::string what = std::string(f.variant) + " " + dir;
    tsk_error_reset();
    DirPtr d(tsk_fs_dir_open_meta(fs, entry_of(m, dir).inode));
    CPPUNIT_ASSERT_MESSAGE(what + ": " + error_text(), d != nullptr);
    std::map<std::string, Listed> got;
    for (size_t i = 0; i < tsk_fs_dir_getsize(d.get()); ++i) {
        const TSK_FS_NAME *n = tsk_fs_dir_get_name(d.get(), i);
        CPPUNIT_ASSERT_MESSAGE(
            what + ": no name " + std::to_string(i), n != nullptr);
        const bool added =
            got.emplace(n->name, Listed{n->meta_addr, n->type, n->flags})
                .second;
        CPPUNIT_ASSERT_MESSAGE(what + ": " + n->name + " twice", added);
    }
    return got;
}

// Asserts that the directory at `dir` lists exactly what the Manifest says.
void
check_listing(const Fixture &f, TSK_FS_INFO *fs, const tskval::Manifest &m,
    const std::string &dir, bool dots_stored)
{
    const std::string what = std::string(f.variant) + " " + dir;
    const std::map<std::string, Listed> want =
        expected_names(f, m, dir, dots_stored);
    const std::map<std::string, Listed> got = listed_names(f, fs, m, dir);
    for (const auto &w : want) {
        const auto g = got.find(w.first);
        CPPUNIT_ASSERT_MESSAGE(
            what + ": " + w.first + " not listed", g != got.end());
        CPPUNIT_ASSERT_EQUAL_MESSAGE(
            what + ": inode of " + w.first, w.second.inode, g->second.inode);
        CPPUNIT_ASSERT_EQUAL_MESSAGE(what + ": type of " + w.first,
            static_cast<int>(w.second.type), static_cast<int>(g->second.type));
        CPPUNIT_ASSERT_EQUAL_MESSAGE(what + ": flags of " + w.first,
            static_cast<int>(w.second.flags),
            static_cast<int>(g->second.flags));
    }
    CPPUNIT_ASSERT_EQUAL_MESSAGE(what + ": names", want.size(), got.size());
}

// What a recursive tsk_fs_dir_walk reports: each "." and ".." as a name
// with its flags, name type and metadata type and flags, and every other
// path (the walk's path plus the name).
struct WalkSeen {
    std::set<std::string> dots;
    std::vector<std::string> paths;
    size_t dot_count = 0;

    static TSK_WALK_RET_ENUM cb(TSK_FS_FILE *file, const char *path, void *ptr)
    {
        WalkSeen *seen = static_cast<WalkSeen *>(ptr);
        const TSK_FS_NAME *n = file->name;
        if (TSK_FS_ISDOT(n->name)) {
            ++seen->dot_count;
            std::string d = std::string(n->name) + " name flags " +
                std::to_string(n->flags) + " type " + std::to_string(n->type);
            if (file->meta != nullptr) {
                d += " meta type " + std::to_string(file->meta->type) +
                    " flags " + std::to_string(file->meta->flags);
            }
            seen->dots.insert(d);
        }
        else {
            seen->paths.push_back(std::string(path) + n->name);
        }
        return TSK_WALK_CONT;
    }
};

// A recursive walk of the root with allocated and unallocated names;
// asserts that it succeeded.
WalkSeen
walk_all(TSK_FS_INFO *fs, const std::string &what)
{
    WalkSeen seen;
    tsk_error_reset();
    const uint8_t rc = tsk_fs_dir_walk(fs, fs->root_inum,
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): flags
        static_cast<TSK_FS_DIR_WALK_FLAG_ENUM>(TSK_FS_DIR_WALK_FLAG_RECURSE |
            TSK_FS_DIR_WALK_FLAG_ALLOC | TSK_FS_DIR_WALK_FLAG_UNALLOC),
        WalkSeen::cb, &seen);
    CPPUNIT_ASSERT_EQUAL_MESSAGE(
        what + ": walk failed: " + error_text(), 0, static_cast<int>(rc));
    return seen;
}

// --- Patched copies of the v4 Fixture ---

// The byte offset of inode `inum` in Fixture `f`: its AG, its block in the
// AG and its index in the block, as the inode number holds them.
uint64_t
inode_offset(const Fixture &f, uint64_t inum)
{
    const unsigned per_block = f.block_size / f.inode_size;
    unsigned inopblog = 0;
    while ((1u << inopblog) < per_block) {
        ++inopblog;
    }
    const uint64_t agino = inum & ((uint64_t{1} << f.ag_inode_bits) - 1);
    const uint64_t block =
        (inum >> f.ag_inode_bits) * f.ag_blocks + (agino >> inopblog);
    return block * f.block_size + (agino & (per_block - 1)) * f.inode_size;
}

// A copy of Fixture `f`, the image xfs-dir-<name>.img in the test data
// directory, that holds only the first 64 blocks of each AG (the AG headers
// and the inode B+tree roots) and the blocks that hold the bytes at
// `offsets` (the inodes and directory blocks a test patches); the rest reads
// as zeros. Patched with `patch(offset, bytes)`.
class PatchedImage {
  public:
    PatchedImage(const Fixture &f, const std::string &name,
        const std::vector<uint64_t> &offsets)
        : m_path(fstest::data_path("xfs-dir-" + name + ".img"))
    {
        std::ifstream in(image_path(f), std::ios::binary);
        std::ofstream out(m_path, std::ios::binary | std::ios::trunc);
        const uint64_t chunk = uint64_t{64} * f.block_size;
        std::vector<char> buf(chunk);
        for (unsigned ag = 0; ag < f.ag_count; ++ag) {
            const uint64_t off = uint64_t{ag} * f.ag_blocks * f.block_size;
            in.seekg(static_cast<std::streamoff>(off));
            in.read(buf.data(), static_cast<std::streamsize>(chunk));
            out.seekp(static_cast<std::streamoff>(off));
            out.write(buf.data(), static_cast<std::streamsize>(chunk));
        }
        for (const uint64_t at : offsets) {
            const uint64_t off = at / f.block_size * f.block_size;
            in.seekg(static_cast<std::streamoff>(off));
            in.read(buf.data(), f.block_size);
            out.seekp(static_cast<std::streamoff>(off));
            out.write(buf.data(), f.block_size);
        }
        // The full size, so no block is past the end of the image.
        out.seekp(static_cast<std::streamoff>(f.blocks * f.block_size - 1));
        out.put('\0');
        CPPUNIT_ASSERT_MESSAGE("cannot copy to " + m_path, in && out.good());
    }

    const std::string &path() const
    {
        return m_path;
    }

    // `n` bytes at `off`.
    std::vector<uint8_t> read(uint64_t off, size_t n) const
    {
        std::ifstream in(m_path, std::ios::binary);
        in.seekg(static_cast<std::streamoff>(off));
        std::vector<uint8_t> b(n);
        in.read(reinterpret_cast<char *>(b.data()),
            static_cast<std::streamsize>(n));
        CPPUNIT_ASSERT_MESSAGE(m_path + " is short", in.good());
        return b;
    }

    void patch(uint64_t off, const std::vector<uint8_t> &bytes) const
    {
        std::fstream io(
            m_path, std::ios::binary | std::ios::in | std::ios::out);
        io.seekp(static_cast<std::streamoff>(off));
        io.write(reinterpret_cast<const char *>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        CPPUNIT_ASSERT_MESSAGE("cannot patch " + m_path, io.good());
    }

    // Replaces the bytes `from` (which occur once in the `n` bytes at
    // `off`) with `to`, of the same length.
    void replace(uint64_t off, size_t n, const std::string &from,
        const std::string &to) const
    {
        CPPUNIT_ASSERT_EQUAL(from.size(), to.size());
        const std::vector<uint8_t> b = read(off, n);
        const std::string region(b.begin(), b.end());
        const size_t at = region.find(from);
        CPPUNIT_ASSERT_MESSAGE(from + " not found", at != std::string::npos);
        CPPUNIT_ASSERT_MESSAGE(
            from + " twice", region.find(from, at + 1) == std::string::npos);
        patch(off + at, std::vector<uint8_t>(to.begin(), to.end()));
    }

  private:
    std::string m_path;
};

void
put_be16(std::vector<uint8_t> &b, size_t at, uint16_t v)
{
    b[at] = static_cast<uint8_t>(v >> 8);
    b[at + 1] = static_cast<uint8_t>(v);
}

void
put_be32(std::vector<uint8_t> &b, size_t at, uint32_t v)
{
    put_be16(b, at, static_cast<uint16_t>(v >> 16));
    put_be16(b, at + 2, static_cast<uint16_t>(v));
}

// Opens the file system in `image`; asserts that it opened.
struct OpenedFs {
    ImgPtr img;
    FsPtr fs;
    explicit OpenedFs(const std::string &image) : img(open_image(image))
    {
        fs.reset(tsk_fs_open_img(img.get(), 0, TSK_FS_TYPE_XFS));
        CPPUNIT_ASSERT_MESSAGE(image + ": " + error_text(), fs != nullptr);
    }
};

// Asserts that directory `inum` doesn't open, with TSK error `errno_want`.
void
check_open_fails(TSK_FS_INFO *fs, TSK_INUM_T inum, uint32_t errno_want,
    const std::string &what)
{
    tsk_error_reset();
    DirPtr d(tsk_fs_dir_open_meta(fs, inum));
    CPPUNIT_ASSERT_MESSAGE(what + ": opened", d == nullptr);
    CPPUNIT_ASSERT_EQUAL_MESSAGE(what + ": " + error_text(), errno_want,
        static_cast<uint32_t>(tsk_error_get_errno()));
}

// Where a v4 inode's data fork starts: after the 100-byte core of a v2
// inode (the v4 Fixtures' inodes are v2).
const size_t V4_FORK = 100;

class XfsDirTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(XfsDirTest);
    CPPUNIT_TEST(testShortformDirectories);
    CPPUNIT_TEST(testBlockDirectories);
    CPPUNIT_TEST(testRecursiveWalkAsOnExt);
    CPPUNIT_TEST(testOpenByPath);
    CPPUNIT_TEST(testLargerFormatsNotReadYet);
    CPPUNIT_TEST(testOpenErrors);
    CPPUNIT_TEST(testNamesAsStored);
    CPPUNIT_TEST(testShortformEightByteInodes);
    CPPUNIT_TEST(testFileTypes);
    CPPUNIT_TEST(testCorruptShortform);
    CPPUNIT_TEST(testCorruptBlock);
    CPPUNIT_TEST_SUITE_END();

  public:
    // Short-form directories (data in the inode): /dirs/shortform, /dirs and
    // /times, with "." (the directory) and ".." (its parent) added, as the
    // kernel's readdir reports them.
    void testShortformDirectories()
    {
        for (const Fixture &f : ALL_FIXTURES) {
            const tskval::Manifest m = read_manifest(f);
            OpenedFs o(image_path(f));
            const FsPtr &fs = o.fs;
            for (const char *dir : {"/dirs/shortform", "/dirs", "/times"}) {
                check_listing(f, fs.get(), m, dir, false);
            }
        }
    }

    // Block directories (one directory block): the root, whose ".." is
    // itself and which has the 255-byte name, and /dirs/block. They store
    // "." and "..", with a file type only with ftype.
    void testBlockDirectories()
    {
        for (const Fixture &f : ALL_FIXTURES) {
            const tskval::Manifest m = read_manifest(f);
            OpenedFs o(image_path(f));
            const FsPtr &fs = o.fs;
            for (const char *dir : {"/", "/dirs/block"}) {
                check_listing(f, fs.get(), m, dir, true);
            }
            const std::map<std::string, Listed> root =
                listed_names(f, fs.get(), m, "/");
            CPPUNIT_ASSERT_MESSAGE(std::string(f.variant) + ": 255-byte name",
                root.count(std::string(255, 'x')) == 1);
        }
    }

    // A recursive walk of the root with allocated and unallocated names
    // finishes (the larger directory formats fail on their own, #41), lists
    // every path once and no $OrphanFiles (XFS doesn't add it), and reports
    // "." and ".." of each directory it read exactly as on ext: the same
    // name flags and types, the same metadata types and flags.
    void testRecursiveWalkAsOnExt()
    {
        ImgPtr ext_img = open_image(fstest::data_path("ext-small.img"));
        FsPtr ext(tsk_fs_open_img(ext_img.get(), 0, TSK_FS_TYPE_DETECT));
        CPPUNIT_ASSERT_MESSAGE(error_text(), ext != nullptr);
        const WalkSeen on_ext = walk_all(ext.get(), "ext");
        CPPUNIT_ASSERT_MESSAGE("ext: no dots", !on_ext.dots.empty());

        for (const Fixture &f : ALL_FIXTURES) {
            const std::string v = f.variant;
            OpenedFs o(image_path(f));
            const FsPtr &fs = o.fs;
            const WalkSeen seen = walk_all(fs.get(), v);
            const std::set<std::string> unique(
                seen.paths.begin(), seen.paths.end());
            CPPUNIT_ASSERT_EQUAL_MESSAGE(
                v + ": a path twice", seen.paths.size(), unique.size());
            for (const std::string &p : seen.paths) {
                CPPUNIT_ASSERT_MESSAGE(std::string(v).append(": ").append(p),
                    p.find("$OrphanFiles") == std::string::npos);
            }
            // The root, /dirs, /times, /dirs/shortform and /dirs/block.
            CPPUNIT_ASSERT_EQUAL_MESSAGE(
                v + ": dots", size_t{2 * 5}, seen.dot_count);
            if (f.ftype) {
                CPPUNIT_ASSERT_MESSAGE(
                    v + ": dots differ from ext's", on_ext.dots == seen.dots);
            }
        }
    }

    // tsk_fs_file_open finds every path of the Manifest that short-form and
    // block directories lead to (the 255-byte name too) at its inode. A path
    // below a leaf directory isn't found yet (#41).
    void testOpenByPath()
    {
        for (const Fixture &f : ALL_FIXTURES) {
            const std::string v = f.variant;
            const tskval::Manifest m = read_manifest(f);
            OpenedFs o(image_path(f));
            const FsPtr &fs = o.fs;
            size_t opened = 0;
            for (const tskval::Entry &e : m.entries) {
                if (fstest::below_larger_dir(e.path)) {
                    continue;
                }
                tsk_error_reset();
                fstest::FilePtr file(
                    tsk_fs_file_open(fs.get(), nullptr, e.path.c_str()));
                CPPUNIT_ASSERT_MESSAGE(
                    v + ": " + e.path + ": " + error_text(), file != nullptr);
                CPPUNIT_ASSERT_MESSAGE(
                    v + ": " + e.path + ": no metadata", file->meta != nullptr);
                CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": inode of " + e.path,
                    e.inode, static_cast<uint64_t>(file->meta->addr));
                ++opened;
            }
            CPPUNIT_ASSERT_MESSAGE(v + ": few paths", opened > 30);

            std::string leaf;
            for (const tskval::Entry &e : m.entries) {
                if (e.path.compare(0, 11, "/dirs/leaf/") == 0) {
                    leaf = e.path;
                    break;
                }
            }
            tsk_error_reset();
            fstest::FilePtr file(
                tsk_fs_file_open(fs.get(), nullptr, leaf.c_str()));
            const std::string what = std::string(v).append(": ").append(leaf);
            CPPUNIT_ASSERT_MESSAGE(what, file == nullptr);
            CPPUNIT_ASSERT_EQUAL_MESSAGE(
                std::string(what).append(": ").append(error_text()),
                static_cast<uint32_t>(TSK_ERR_FS_UNSUPFUNC),
                static_cast<uint32_t>(tsk_error_get_errno()));
        }
    }

    // Leaf, node and B+tree directories are not read yet (#41): opening one
    // fails with TSK_ERR_FS_UNSUPFUNC. So does a single-block directory
    // whose block is a leaf directory's data block (a leaf directory can
    // shrink to one data block).
    void testLargerFormatsNotReadYet()
    {
        for (const Fixture &f : ALL_FIXTURES) {
            const tskval::Manifest m = read_manifest(f);
            OpenedFs o(image_path(f));
            for (const char *dir : fstest::LARGER_DIRS) {
                check_open_fails(o.fs.get(), entry_of(m, dir).inode,
                    TSK_ERR_FS_UNSUPFUNC, std::string(f.variant) + " " + dir);
            }
        }
        // v4 and v5 magics (directory block CRCs are not checked, #45).
        for (const Fixture &f : {fstest::V4, fstest::V5}) {
            const std::string v = f.variant;
            const tskval::Manifest m = read_manifest(f);
            const TSK_INUM_T inum = entry_of(m, "/dirs/block").inode;
            const uint64_t block = dir_block_offset(f, m, "/dirs/block");
            PatchedImage img(
                f, "leaf-block-" + v, {inode_offset(f, inum), block});
            if (f.version == 5) {
                img.replace(block, 4, "XDB3", "XDD3");
            }
            else {
                img.replace(block, 4, "XD2B", "XD2D");
            }
            OpenedFs o(img.path());
            check_open_fails(
                o.fs.get(), inum, TSK_ERR_FS_UNSUPFUNC, v + ": data magic");
        }
    }

    // A number past $OrphanFiles, a file that is not a directory and
    // $OrphanFiles itself (orphans need the inode walk, #44) don't open as
    // directories.
    void testOpenErrors()
    {
        const Fixture &f = fstest::V5;
        const tskval::Manifest m = read_manifest(f);
        OpenedFs o(image_path(f));
        TSK_FS_INFO *fs = o.fs.get();
        check_open_fails(
            fs, fs->last_inum + 1, TSK_ERR_FS_WALK_RNG, "past the range");
        check_open_fails(fs, entry_of(m, "/small.txt").inode, TSK_ERR_FS_ARG,
            "regular file");
        check_open_fails(fs, TSK_FS_ORPHANDIR_INUM(fs), TSK_ERR_FS_UNSUPFUNC,
            "$OrphanFiles");
        tsk_error_reset();
    }

    // Names are listed byte for byte as stored: UTF-8 and a byte that is
    // not UTF-8, in a short-form and in a block directory.
    void testNamesAsStored()
    {
        const Fixture &f = fstest::V4;
        const tskval::Manifest m = read_manifest(f);
        const std::string utf8 = "caf\xc3\xa9\xe2\x82\xac"; // café€
        const std::string latin1 = "b\xe9" + std::string(13, 'b') + "0";
        const uint64_t sf =
            inode_offset(f, entry_of(m, "/dirs/shortform").inode);
        const uint64_t block = dir_block_offset(f, m, "/dirs/block");
        PatchedImage img(f, "names", {sf, block});
        img.replace(sf, f.inode_size, "s000000s", utf8);
        img.replace(block, f.block_size, "b000000bbbbbbbbb", latin1);
        OpenedFs o(img.path());
        const std::map<std::string, Listed> s =
            listed_names(f, o.fs.get(), m, "/dirs/shortform");
        CPPUNIT_ASSERT_EQUAL(size_t{1}, s.count(utf8));
        CPPUNIT_ASSERT_EQUAL(
            entry_of(m, "/dirs/shortform/s000000s").inode, s.at(utf8).inode);
        const std::map<std::string, Listed> b =
            listed_names(f, o.fs.get(), m, "/dirs/block");
        CPPUNIT_ASSERT_EQUAL(size_t{1}, b.count(latin1));
        CPPUNIT_ASSERT_EQUAL(entry_of(m, "/dirs/block/b000000bbbbbbbbb").inode,
            b.at(latin1).inode);
    }

    // A short-form directory with 8-byte inode numbers (i8count not 0,
    // which inode numbers above 2^32 need): the parent and every entry are
    // 8 bytes, and are listed as stored. The top byte is not part of the
    // number (the kernel masks it off).
    void testShortformEightByteInodes()
    {
        const Fixture &f = fstest::V4;
        const tskval::Manifest m = read_manifest(f);
        const uint64_t ino =
            inode_offset(f, entry_of(m, "/dirs/shortform").inode);
        PatchedImage img(f, "i8", {ino});
        const uint64_t parent = entry_of(m, "/dirs").inode;
        // count, i8count, parent; then namelen, offset, name, ftype, inode.
        std::vector<uint8_t> sf = {3, 1};
        const auto put_ino = [&sf](uint64_t v) {
            for (int shift = 56; shift >= 0; shift -= 8) {
                sf.push_back(static_cast<uint8_t>(v >> shift));
            }
        };
        put_ino(parent | (uint64_t{0xff} << 56));
        const uint64_t high = (uint64_t{1} << 40) + 5;
        const uint64_t inodes[] = {
            entry_of(m, "/dirs/shortform/s000000s").inode,
            entry_of(m, "/dirs/shortform/s000001s").inode, high};
        for (int i = 0; i < 3; ++i) {
            const std::string name = "e" + std::to_string(i);
            sf.push_back(static_cast<uint8_t>(name.size()));
            sf.push_back(0);
            sf.push_back(static_cast<uint8_t>(0x60 + 0x10 * i));
            sf.insert(sf.end(), name.begin(), name.end());
            sf.push_back(1); // XFS_DIR3_FT_REG_FILE
            put_ino(inodes[i]);
        }
        img.patch(ino + V4_FORK, sf);
        std::vector<uint8_t> size(8, 0);
        put_be32(size, 4, static_cast<uint32_t>(sf.size()));
        img.patch(ino + 56, size); // di_size
        OpenedFs o(img.path());
        const std::map<std::string, Listed> got =
            listed_names(f, o.fs.get(), m, "/dirs/shortform");
        CPPUNIT_ASSERT_EQUAL(size_t{5}, got.size());
        CPPUNIT_ASSERT_EQUAL(parent, got.at("..").inode);
        CPPUNIT_ASSERT_EQUAL(inodes[0], got.at("e0").inode);
        CPPUNIT_ASSERT_EQUAL(inodes[1], got.at("e1").inode);
        CPPUNIT_ASSERT_EQUAL(high, got.at("e2").inode);
        CPPUNIT_ASSERT_EQUAL(static_cast<int>(TSK_FS_NAME_TYPE_REG),
            static_cast<int>(got.at("e2").type));
    }

    // File types the Fixtures don't have: a socket, a whiteout, and a
    // value XFS doesn't define (unknown).
    void testFileTypes()
    {
        const Fixture &f = fstest::V4;
        const tskval::Manifest m = read_manifest(f);
        const uint64_t sf =
            inode_offset(f, entry_of(m, "/dirs/shortform").inode);
        PatchedImage img(f, "ftypes", {sf});
        // Each name is followed by its file type (1, a regular file).
        img.replace(sf, f.inode_size, std::string("s000000s\x01", 9),
            std::string("s000000s\x06", 9));
        img.replace(sf, f.inode_size, std::string("s000001s\x01", 9),
            std::string("s000001s\x08", 9));
        img.replace(sf, f.inode_size, std::string("s000002s\x01", 9),
            std::string("s000002s\x09", 9));
        OpenedFs o(img.path());
        const std::map<std::string, Listed> got =
            listed_names(f, o.fs.get(), m, "/dirs/shortform");
        CPPUNIT_ASSERT_EQUAL(static_cast<int>(TSK_FS_NAME_TYPE_SOCK),
            static_cast<int>(got.at("s000000s").type));
        CPPUNIT_ASSERT_EQUAL(static_cast<int>(TSK_FS_NAME_TYPE_WHT),
            static_cast<int>(got.at("s000001s").type));
        CPPUNIT_ASSERT_EQUAL(static_cast<int>(TSK_FS_NAME_TYPE_UNDEF),
            static_cast<int>(got.at("s000002s").type));
    }

    // A short-form directory whose entries don't fit in its size, or with
    // an empty name, or a size larger than the data fork, gives
    // TSK_ERR_FS_CORRUPT.
    void testCorruptShortform()
    {
        const Fixture &f = fstest::V4;
        const tskval::Manifest m = read_manifest(f);
        const TSK_INUM_T inum = entry_of(m, "/dirs/shortform").inode;
        const uint64_t ino = inode_offset(f, inum);
        const auto check = [&, f](const std::string &name, uint64_t off,
                               const std::vector<uint8_t> &bytes) {
            PatchedImage img(f, std::string("sf-").append(name), {ino});
            img.patch(off, bytes);
            OpenedFs o(img.path());
            check_open_fails(o.fs.get(), inum, TSK_ERR_FS_CORRUPT, name);
        };
        check("count", ino + V4_FORK, {4});       // one entry too many
        check("namelen", ino + V4_FORK + 6, {0}); // the first entry's
        check("namelen-long", ino + V4_FORK + 6, {200});
        check("size", ino + 56, {0, 0, 0, 0, 0, 0, 1, 0}); // 256 bytes
        check("header", ino + 56, {0, 0, 0, 0, 0, 0, 0, 5});
    }

    // A block directory with a magic that is no directory block's, more
    // leaf entries than the block holds, an unused region of no length or
    // past the entries, an entry past the entries, or a size under one
    // directory block, gives TSK_ERR_FS_CORRUPT.
    void testCorruptBlock()
    {
        const Fixture &f = fstest::V4;
        const tskval::Manifest m = read_manifest(f);
        const TSK_INUM_T inum = entry_of(m, "/dirs/block").inode;
        const uint64_t block = dir_block_offset(f, m, "/dirs/block");
        const uint64_t tail = block + f.block_size - 8;
        const auto check = [&, f](const std::string &name, uint64_t off,
                               const std::vector<uint8_t> &bytes) {
            PatchedImage img(f, std::string("block-").append(name),
                {inode_offset(f, inum), block});
            img.patch(off, bytes);
            OpenedFs o(img.path());
            check_open_fails(o.fs.get(), inum, TSK_ERR_FS_CORRUPT, name);
        };
        check("magic", block, {'X', 'D', '2', 'Q'});
        check("count", tail, {0, 0, 2, 0});
        // The entries start after the 16-byte header: make the first one an
        // unused region.
        check("free-0", block + 16, {0xff, 0xff, 0, 0});
        check("free-long", block + 16, {0xff, 0xff, 0x10, 0});
        check("free-odd", block + 16, {0xff, 0xff, 0, 12});
        check("namelen", block + 16 + 8, {0});
        // 100 more leaf entries than the 22 names: the data area ends
        // inside the last entry or unused region.
        check("count-past-entries", tail, {0, 0, 0, 122});
        // An extent-mapped directory smaller than one directory block.
        check("size", inode_offset(f, inum) + 56, {0, 0, 0, 0, 0, 0, 8, 0});
    }

  private:
    // The byte offset of the first (only) block of block directory `dir`,
    // from libtsk's own block map of it.
    static uint64_t dir_block_offset(
        const Fixture &f, const tskval::Manifest &m, const std::string &dir)
    {
        OpenedFs o(image_path(f));
        fstest::FilePtr file(
            tsk_fs_file_open_meta(o.fs.get(), nullptr, entry_of(m, dir).inode));
        CPPUNIT_ASSERT_MESSAGE(dir + ": " + error_text(), file != nullptr);
        const std::vector<TSK_DADDR_T> blocks = fstest::file_blocks(file.get());
        CPPUNIT_ASSERT_EQUAL_MESSAGE(
            dir + ": blocks", size_t{1}, blocks.size());
        return blocks.front() * f.block_size;
    }
};

CPPUNIT_TEST_SUITE_REGISTRATION(XfsDirTest);

} // namespace
