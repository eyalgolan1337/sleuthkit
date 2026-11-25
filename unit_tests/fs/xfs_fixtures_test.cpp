/*
 * The XFS Fixture images (test/vm/fixtures/xfs/, unpacked by make check into
 * xfs/), read through the public API. Each image is recognised as XFS, by
 * autodetection and by the type name "xfs", and its superblock facts are the
 * ones mkfs.xfs was given (test/vm/README.md, "XFS fixture images"). The XFS
 * tickets of #11 add the walk tests as they go.
 */
#include "fs_test_data.h"
#include "fs_test_util.h"
#include "manifest_check.h"
#include "xfs_fixture_data.h"

#include "manifest.h"

#include <tsk/libtsk.h>

#include <cppunit/extensions/HelperMacros.h>

#include <cstdio>
#include <fstream>
#include <string>

namespace {

using fstest::error_text;
using fstest::FsPtr;
using fstest::fsstat_text;
using fstest::fsstat_value;
using fstest::ImgPtr;
using fstest::open_image;

using fstest::Fixture;
using fstest::image_path;
using fstest::read_manifest;
using fstest::V4;
using fstest::V4_512;
using fstest::V5;
using fstest::V5_1K;
using fstest::V5_64K;

// The volume ID as a canonical UUID ("" unless it has 16 bytes).
std::string
volume_id(const TSK_FS_INFO *fs)
{
    if (fs->fs_id_used != 16) {
        return "";
    }
    std::string out;
    char hex[3];
    for (size_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out += '-';
        }
        snprintf(hex, sizeof(hex), "%02x", fs->fs_id[i]);
        out += hex;
    }
    return out;
}

uint64_t
root_inode(const tskval::Manifest &m)
{
    for (const tskval::Entry &e : m.entries) {
        if (e.path == "/") {
            return e.inode;
        }
    }
    CPPUNIT_FAIL("the Manifest has no / entry");
    return 0;
}

class XfsFixturesTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(XfsFixturesTest);
    CPPUNIT_TEST(testDetectedAsXfs);
    CPPUNIT_TEST(testOpenedByTypeName);
    CPPUNIT_TEST(testGeometry);
    CPPUNIT_TEST(testFsstat);
    CPPUNIT_TEST(testLaterStepsFailWithTskErrors);
    CPPUNIT_TEST(testWalkStopsAtTheRootDirectory);
    CPPUNIT_TEST_SUITE_END();

  public:
    // Autodetection recognises every variant as XFS, and only as XFS (a
    // second match would fail with TSK_ERR_FS_MULTTYPE), with the root
    // directory's inode that the Manifest records for "/".
    void testDetectedAsXfs()
    {
        for (const Fixture &f : {V4, V4_512, V5_1K, V5_64K, V5}) {
            const std::string v = f.variant;
            const tskval::Manifest manifest = read_manifest(f);
            ImgPtr img = open_image(image_path(f));
            CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": image type",
                static_cast<int>(TSK_IMG_TYPE_RAW),
                static_cast<int>(img->itype));
            FsPtr fs(tsk_fs_open_img(img.get(), 0, TSK_FS_TYPE_DETECT));
            CPPUNIT_ASSERT_MESSAGE(
                v + ": autodetection failed: " + error_text(), fs != nullptr);
            CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": ftype",
                static_cast<int>(TSK_FS_TYPE_XFS), static_cast<int>(fs->ftype));
            CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": root inode",
                root_inode(manifest), static_cast<uint64_t>(fs->root_inum));
        }
    }

    // "xfs" is a type name (what tools' -f takes), and the explicit type
    // opens every variant.
    void testOpenedByTypeName()
    {
        const TSK_FS_TYPE_ENUM type = tsk_fs_type_toid_utf8("xfs");
        CPPUNIT_ASSERT_EQUAL(
            static_cast<int>(TSK_FS_TYPE_XFS), static_cast<int>(type));
        CPPUNIT_ASSERT_EQUAL(std::string("xfs"),
            std::string(tsk_fs_type_toname(TSK_FS_TYPE_XFS)));
        CPPUNIT_ASSERT(TSK_FS_TYPE_ISXFS(TSK_FS_TYPE_XFS));
        CPPUNIT_ASSERT(TSK_FS_TYPE_ISXFS(TSK_FS_TYPE_XFS_DETECT));
        CPPUNIT_ASSERT(!TSK_FS_TYPE_ISXFS(TSK_FS_TYPE_EXT4));
        for (const Fixture &f : {V4, V4_512, V5_1K, V5_64K, V5}) {
            const std::string v = f.variant;
            ImgPtr img = open_image(image_path(f));
            FsPtr fs(tsk_fs_open_img(img.get(), 0, type));
            CPPUNIT_ASSERT_MESSAGE(
                v + ": open as xfs failed: " + error_text(), fs != nullptr);
            CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": ftype",
                static_cast<int>(TSK_FS_TYPE_XFS), static_cast<int>(fs->ftype));
        }
    }

    // The geometry in the public TSK_FS_INFO fields: blocks are the data
    // section's, the volume ID is the UUID, and the metadata address range
    // runs from 0 to $OrphanFiles, one past the largest possible inode.
    void testGeometry()
    {
        for (const Fixture &f : {V4, V4_512, V5_1K, V5_64K, V5}) {
            const std::string v = f.variant;
            ImgPtr img = open_image(image_path(f));
            FsPtr fs(tsk_fs_open_img(img.get(), 0, TSK_FS_TYPE_XFS));
            CPPUNIT_ASSERT_MESSAGE(v + ": " + error_text(), fs != nullptr);
            CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": block size", f.block_size,
                static_cast<unsigned>(fs->block_size));
            CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": block count", f.blocks,
                static_cast<uint64_t>(fs->block_count));
            CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": first block", uint64_t{0},
                static_cast<uint64_t>(fs->first_block));
            CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": last block", f.blocks - 1,
                static_cast<uint64_t>(fs->last_block));
            CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": last block in the image",
                f.blocks - 1, static_cast<uint64_t>(fs->last_block_act));
            CPPUNIT_ASSERT_EQUAL_MESSAGE(
                v + ": volume ID", std::string(f.uuid), volume_id(fs.get()));
            CPPUNIT_ASSERT_MESSAGE(v + ": root inode outside the range",
                fs->root_inum >= fs->first_inum &&
                    fs->root_inum < fs->last_inum);
            CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": first inode", uint64_t{0},
                static_cast<uint64_t>(fs->first_inum));
            CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": $OrphanFiles past every inode",
                f.max_inode + 1, static_cast<uint64_t>(fs->last_inum));
        }
    }

    // fsstat shows the superblock facts that xfs_info shows.
    void testFsstat()
    {
        for (const Fixture &f : {V4, V4_512, V5_1K, V5_64K, V5}) {
            const std::string v = f.variant;
            ImgPtr img = open_image(image_path(f));
            FsPtr fs(tsk_fs_open_img(img.get(), 0, TSK_FS_TYPE_XFS));
            CPPUNIT_ASSERT_MESSAGE(v + ": " + error_text(), fs != nullptr);
            const std::string text = fsstat_text(fs.get());
            const auto expect = [&](const std::string &key,
                                    const std::string &want) {
                std::string msg = v;
                msg.append(": fsstat ").append(key).append("\n").append(text);
                CPPUNIT_ASSERT_EQUAL_MESSAGE(
                    msg, want, fsstat_value(text, key));
            };
            expect("File System Type", "XFS");
            expect("Volume Name", "tsk-" + v);
            expect("Volume ID", f.uuid);
            expect("Version", std::to_string(f.version));
            expect("Block Size", std::to_string(f.block_size));
            expect("Block Count", std::to_string(f.blocks));
            expect("Block Range", "0 - " + std::to_string(f.blocks - 1));
            expect("Total Range in Image", "(no line)");
            expect("Inode Size", std::to_string(f.inode_size));
            expect(
                "Inode per Block", std::to_string(f.block_size / f.inode_size));
            expect("Inode Range", "0 - " + std::to_string(f.max_inode + 1));
            expect("Allocation Group Count", std::to_string(f.ag_count));
            expect("Allocation Group Block Size", std::to_string(f.ag_blocks));
            expect("Sector Size", "512");
            expect("Root Inode", std::to_string(fs->root_inum));
            expect("Inode max persentage", "25");
            // xfs_db version: the sb_versionnum flags, then sb_features2
            // (v4 0x28a, v5 0x18a).
            const std::string flags =
                "nlink, align, logv2, extflg, dirv2, morebits, lazy-count, "
                "attr2, projid32bit, ";
            expect("Flags", flags + (f.version == 5 ? "crc" : "ftype"));
            if (f.version == 5) {
                // xfs_info: finobt=1 rmapbt=1 reflink=1 inobtcount=1;
                // ftype=1 sparse=1 bigtime=1 nrext64=1.
                expect("Read Only Compat Features",
                    "finobt, rmapbt, reflink, inobtcount");
                expect("InCompat Features", "ftype, sparse, bigtime, nrext64");
            }
            else {
                expect("Read Only Compat Features", "(no line)");
                expect("InCompat Features", "(no line)");
            }
        }
    }

    // What later steps of #11 implement fails with a TSK error, not a crash:
    // directories (#40), the inode and block walks (#44).
    void testLaterStepsFailWithTskErrors()
    {
        for (const Fixture &f : {V4, V5}) {
            const std::string v = f.variant;
            ImgPtr img = open_image(image_path(f));
            FsPtr fs(tsk_fs_open_img(img.get(), 0, TSK_FS_TYPE_DETECT));
            CPPUNIT_ASSERT_MESSAGE(v + ": " + error_text(), fs != nullptr);
            const auto failed = [&v](const std::string &what) {
                std::string msg = v;
                msg.append(": ").append(what).append(" without a TSK error");
                CPPUNIT_ASSERT_MESSAGE(
                    msg, tsk_error_get_errno() != 0 && !error_text().empty());
                tsk_error_reset();
            };

            tsk_error_reset();
            TSK_FS_DIR *dir = tsk_fs_dir_open_meta(fs.get(), fs->root_inum);
            CPPUNIT_ASSERT_MESSAGE(v + ": directory open", dir == nullptr);
            failed("directory open");

            CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": directory walk", 1,
                static_cast<int>(tsk_fs_dir_walk(fs.get(), fs->root_inum,
                    TSK_FS_DIR_WALK_FLAG_RECURSE, ignore_name, nullptr)));
            failed("directory walk");

            CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": inode walk", 1,
                static_cast<int>(
                    tsk_fs_meta_walk(fs.get(), fs->first_inum, fs->last_inum,
                        TSK_FS_META_FLAG_ALLOC, ignore_meta, nullptr)));
            failed("inode walk");

            CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": block walk", 1,
                static_cast<int>(
                    tsk_fs_block_walk(fs.get(), fs->first_block, fs->last_block,
                        TSK_FS_BLOCK_WALK_FLAG_ALLOC, ignore_block, nullptr)));
            failed("block walk");
        }
    }

    // The validate command's walk of each image opens the file system and
    // the root directory's inode, which matches the Manifest's "/", and stops
    // at the root's entries (#40 reads directories): every other entry of
    // the Manifest is missing.
    void testWalkStopsAtTheRootDirectory()
    {
        for (const Fixture &f : {V4, V4_512, V5_1K, V5_64K, V5}) {
            const std::string v = f.variant;
            const tskval::Manifest manifest = read_manifest(f);
            const fstest::WalkCheck walk =
                fstest::check_walk(image_path(f), manifest);
            CPPUNIT_ASSERT_MESSAGE(v + ": " + walk.report, !walk.passed);
            const fstest::WalkCheck tolerated =
                fstest::check_walk(image_path(f), manifest,
                    "* missing XFS directories come in #40\n");
            CPPUNIT_ASSERT_MESSAGE(
                v + ": " + tolerated.report, tolerated.passed);
            CPPUNIT_ASSERT_EQUAL_MESSAGE(v + ": entries walked", size_t{1},
                tolerated.result.run_entries);
        }
    }

  private:
    static TSK_WALK_RET_ENUM ignore_name(TSK_FS_FILE *, const char *, void *)
    {
        return TSK_WALK_CONT;
    }
    static TSK_WALK_RET_ENUM ignore_meta(TSK_FS_FILE *, void *)
    {
        return TSK_WALK_CONT;
    }
    static TSK_WALK_RET_ENUM ignore_block(const TSK_FS_BLOCK *, void *)
    {
        return TSK_WALK_CONT;
    }
};

CPPUNIT_TEST_SUITE_REGISTRATION(XfsFixturesTest);

} // namespace
