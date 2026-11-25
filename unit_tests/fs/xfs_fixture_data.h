/*
 * The XFS Fixture images (test/vm/fixtures/xfs/, unpacked by make check into
 * xfs/) and what mkfs.xfs made of each, for the XFS unit tests. Test-only.
 * C++14.
 */
#ifndef TSK_UNIT_TESTS_FS_XFS_FIXTURE_DATA_H
#define TSK_UNIT_TESTS_FS_XFS_FIXTURE_DATA_H

#include "fs_test_data.h"

#include "manifest.h"

#include <cppunit/extensions/HelperMacros.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>

namespace fstest {

// One Fixture image and what mkfs.xfs made of it: the mkfs.xfs options and
// the xfs_info output in the comment at the top of its Manifest.
struct Fixture {
    const char *variant;
    size_t entries;   // test/vm/README.md, "XFS fixture images"
    const char *uuid; // -m uuid=
    unsigned version; // crc=1: 5, crc=0: 4
    unsigned block_size;
    uint64_t blocks;
    unsigned inode_size;
    unsigned ag_count;
    unsigned ag_blocks;
    // From xfs_db: bits of the AG-relative part of an inode number
    // (sb_agblklog + sb_inopblog), so inode >> ag_inode_bits is its AG.
    unsigned ag_inode_bits;
    // From xfs_db on the image, independently of libtsk: the largest inode
    // number the geometry allows (convert agno <last> agbno <last block of
    // the last AG> inoidx <inodes per block - 1> inode).
    uint64_t max_inode;
    // From xfs_db: a free inode in an allocated inode chunk of AG 0 (its bit
    // is set in ir_free of AG 0's inode B+tree record; its mode is 0).
    uint64_t free_inode;
    const char *label; // -L
    // Directory entries record the file type (mkfs.xfs -n ftype=1, the
    // default; v5 always).
    bool ftype;
};

const Fixture V4 = {"v4", 1006, "5f4b0c1e-0000-4000-8000-000000000004", 4, 4096,
    81920, 256, 4, 20480, 19, 0x1cffff, 191, "tsk-v4", true};
const Fixture V4_512 = {"v4-512", 1006, "5f4b0c1e-0000-4000-8000-000000000512",
    4, 512, 655360, 256, 4, 163840, 19, 0x1cffff, 95, "tsk-v4-512", true};
const Fixture V4_NOFTYPE = {"v4-noftype", 1006,
    "5f4b0c1e-0000-4000-8000-0000000000f0", 4, 4096, 81920, 256, 4, 20480, 19,
    0x1cffff, 191, "tsk-v4-noft", false};
const Fixture V5_1K = {"v5-1k", 1181, "5f4b0c1e-0000-4000-8000-000000001024", 5,
    1024, 327680, 512, 4, 81920, 18, 0xe7fff, 127, "tsk-v5-1k", true};
const Fixture V5_64K = {"v5-64k", 18821, "5f4b0c1e-0000-4000-8000-000000065536",
    5, 65536, 8192, 512, 4, 2048, 18, 0xfffff, 4671, "tsk-v5-64k", true};
const Fixture V5 = {"v5", 1181, "5f4b0c1e-0000-4000-8000-000000000005", 5, 4096,
    81920, 512, 4, 20480, 18, 0xe7fff, 191, "tsk-v5", true};

const Fixture ALL_FIXTURES[] = {V4, V4_512, V4_NOFTYPE, V5_1K, V5_64K, V5};

inline std::string
image_path(const Fixture &f)
{
    return data_path(std::string("xfs/") + f.variant + ".img");
}

// The Fixture's expected Manifest; asserts that it has the documented number
// of entries.
inline tskval::Manifest
read_manifest(const Fixture &f)
{
    const std::string path =
        data_path(std::string("xfs/") + f.variant + ".manifest");
    std::ifstream in(path, std::ios::binary);
    CPPUNIT_ASSERT_MESSAGE(path + ": no unpacked Manifest", in.good());
    tskval::Manifest manifest;
    std::string error;
    const bool read = tskval::read_manifest(in, manifest, error);
    CPPUNIT_ASSERT_MESSAGE(path + ": " + error, read);
    CPPUNIT_ASSERT_MESSAGE(path + ": Manifest has !error", !manifest.has_error);
    CPPUNIT_ASSERT_EQUAL_MESSAGE(
        path + ": Manifest entries", f.entries, manifest.entries.size());
    return manifest;
}

// The differences from a Fixture's Manifest that its comparison tolerates,
// in check_walk's Known failure format: symlink targets are not read yet
// (#43), and the harness records a crtime of 0 as no crtime (libtsk has no
// other way to say there is none), so a v5 inode created at the epoch
// differs (#50).
inline std::string
inode_known_failures(const Fixture &f)
{
    std::string out = "/symlink-short target symlink targets come in #43\n"
                      "/symlink-long target symlink targets come in #43\n";
    if (f.version == 5) {
        out += "/times/epoch crtime crtime 0 reads as none (#50)\n";
    }
    return out;
}

// The Manifest's entry for `path`; asserts that there is one.
inline const tskval::Entry &
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

} // namespace fstest

#endif
