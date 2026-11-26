/*
 * Self-test of the inode walk and block walk checks (walkcheck.h) and their
 * check-tool commands, linked into manifest-selftest and run in gate 1. The
 * walk records are trimmed from what the Validation VM's guest printed
 * (xfsprogs 6.4.0, e2fsprogs 1.46.5, 2026-11-26), with the numbers changed
 * where a test needs a smaller or a particular case; the tool output is that
 * of ils, ifind and blkls of release 4.15.0 and the fork.
 */
#include "toolcheck_cli.h"
#include "walkcheck.h"

#include <cppunit/extensions/HelperMacros.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <sys/stat.h> // mkdir
#include <unistd.h>   // rmdir

using namespace tskval;

namespace {

// Two AGs of 2^18 blocks of 4 KiB, 8 inodes per block. AG 0: the root chunk,
// full, and a chunk with its last 5 inodes free. AG 1: a two-level inode
// B+tree (an interior block with keys, then two leaves); a sparse chunk whose
// last 16 inodes are a hole (XFS marks holes free too), and a chunk with one
// free inode. Three spot-check paths: one extent, holes and two AGs, none.
const char XFS_RECORD[] =
    "== xfs_db sb\n"
    "blocksize = 4096\n"
    "agcount = 2\n"
    "agblocks = 262144\n"
    "dblocks = 524288\n"
    "agblklog = 18\n"
    "inopblog = 3\n"
    "rbmino = 129\n"
    "rsumino = 130\n"
    "uquotino = null\n"
    "gquotino = null\n"
    "pquotino = 0\n"
    "== xfs_db agi 0\n"
    "count = 128\n"
    "level = 1\n"
    "freecount = 5\n"
    "inobt level 0 block 1 daddr 24\n"
    "recs[1-2] = [startino,holemask,count,freecount,free] \n"
    "1:[128,0,64,0,0] \n"
    "2:[1051968,0,64,5,0xf800000000000000]\n"
    "== xfs_db agi 1\n"
    "count = 112\n"
    "level = 2\n"
    "freecount = 1\n"
    "inobt level 1 block 3 daddr 2097176\n"
    "keys[1-2] = [startino] \n"
    "1:[576] \n"
    "2:[640]\n"
    "ptrs[1-2] = 1:4 2:5\n"
    "inobt level 0 block 4 daddr 2097184\n"
    "recs[1] = [startino,holemask,count,freecount,free] \n"
    "1:[576,0xf000,48,0,0xffff000000000000]\n"
    "inobt level 0 block 5 daddr 2097192\n"
    "recs[1] = [startino,holemask,count,freecount,free] \n"
    "1:[640,0,64,1,0x1]\n"
    "== xfs_db bnobt 0\n"
    "bnobt level 0 block 1 daddr 8\n"
    "recs[1-2] = [startblock,blockcount] \n"
    "1:[131381,115] \n"
    "2:[131509,130635]\n"
    "== xfs_db bnobt 1\n"
    "bnobt level 0 block 1 daddr 2097160\n"
    "recs[1] = [startblock,blockcount] \n"
    "1:[131239,221]\n"
    "== xfs_bmap 135 /corpus/files/multiblock.bin\n"
    "/data/corpus/files/multiblock.bin:\n"
    " EXT: FILE-OFFSET      BLOCK-RANGE      AG AG-OFFSET        TOTAL\n"
    "   0: [0..31]:         464..495          0 (464..495)          32\n"
    "== xfs_bmap 2097921 /corpus/sparse/holes.bin\n"
    "/data/corpus/sparse/holes.bin:\n"
    " EXT: FILE-OFFSET      BLOCK-RANGE      AG AG-OFFSET          TOTAL\n"
    "   0: [0..7]:          hole                                       8\n"
    "   1: [8..15]:         3150568..3150575  1 (1053416..1053423)     8\n"
    "   2: [16..23]:        hole                                       8\n"
    "   3: [24..39]:        600..615          0 (600..615)            16\n"
    "== xfs_bmap 131 /corpus\n"
    "/data/corpus: no extents\n"
    "== inode-ctime\n"
    "1052027 0000000000000000\n"
    "1052028 6b0703b62c3a1d80\n"
    "1052029 6b0703b62c3a1d80\n"
    "1052030 0000000000000000\n"
    "1052031 6b0703b62c3a1d80\n"
    "2097792 00000000ffffffff\n";

// Two groups of 64 inodes and 32768 blocks. Group 0 has used inodes up to 24
// (its unused tail starts at 25); group 1 up to 68. The reserved inodes 3-6,
// 9 and 10 were never used.
const char EXT_RECORD[] =
    "== dumpe2fs\n"
    "dumpe2fs 1.46.5 (30-Dec-2021)\n"
    "Filesystem volume name:   control\n"
    "Inode count:              128\n"
    "Block count:              65536\n"
    "First block:              0\n"
    "Block size:               4096\n"
    "Blocks per group:         32768\n"
    "Inodes per group:         64\n"
    "First inode:              11\n"
    "\n"
    "\n"
    "Group 0: (Blocks 0-32767) csum 0x78d4 [ITABLE_ZEROED]\n"
    "  Primary superblock at 0, Group descriptors at 1-1\n"
    "  Inode table at 145-656 (+145)\n"
    "  27775 free blocks, 45 free inodes, 3 directories, 40 unused inodes\n"
    "  Free blocks: 4262-4319, 4352-4383\n"
    "  Free inodes: 20-64\n"
    "Group 1: (Blocks 32768-65535) csum 0x3281 [ITABLE_ZEROED]\n"
    "  Backup superblock at 32768, Group descriptors at 32769-32769\n"
    "  31445 free blocks, 63 free inodes, 0 directories, 60 unused inodes\n"
    "  Free blocks: 32899-33023, 33792-35327\n"
    "  Free inodes: 65-70, 72-128\n"
    "== debugfs 2 /\n"
    "Inode: 2   Type: directory    Mode:  0755   Flags: 0x80000\n"
    " ctime: 0x6b0703b5:ac31ed38 -- Wed Nov 25 15:50:13 2026\n"
    "Inode checksum: 0x985cca06\n"
    "EXTENTS:\n"
    "(0):4241\n"
    "== debugfs 13 /corpus/fragmented/extents.bin\n"
    "Inode: 13   Type: regular    Mode:  0644   Flags: 0x80000\n"
    "EXTENTS:\n"
    "(ETB0):32898, (ETB1):32897, (0):156160, (2):156162, "
    "(4-5):156164-156165\n"
    "== debugfs 12 /corpus/files/multiblock.bin\n"
    "Inode: 12   Type: regular    Mode:  0644   Flags: 0x0\n"
    "BLOCKS:\n"
    "(0-1):35719-35720, (IND):35721, (2):35722\n"
    "TOTAL: 4\n"
    "\n"
    "== debugfs 14 /corpus/links/short-symlink\n"
    "Inode: 14   Type: symlink    Mode:  0777   Flags: 0x0\n"
    "Fast link dest: \"target.txt\"\n"
    "== inode-ctime\n"
    "1 6b03fd8a\n"
    "2 6b0703b5\n"
    "3 00000000\n"
    "4 00000000\n"
    "5 00000000\n"
    "6 00000000\n"
    "7 6b03fd8a\n"
    "8 6b03fd8a\n"
    "9 00000000\n"
    "10 00000000\n"
    "20 6b0703b6\n"
    "21 00000000\n"
    "22 6b0703b6\n"
    "23 6b0703b6\n"
    "24 6b0703b6\n"
    "65 6b0703b6\n"
    "66 00000000\n"
    "67 00000000\n"
    "68 00000000\n";

std::string
replace(std::string s, const std::string &from, const std::string &to)
{
    const size_t at = s.find(from);
    if (at == std::string::npos) {
        CPPUNIT_FAIL("not in the sample: " + from);
    }
    return s.replace(at, from.size(), to);
}

WalkTruth
truth(const std::string &record)
{
    WalkTruth t;
    std::string err;
    if (!parse_walk_truth(record, t, err)) {
        CPPUNIT_FAIL("parse_walk_truth failed: " + err);
    }
    return t;
}

std::vector<uint64_t>
plan_inodes(const std::string &record)
{
    std::vector<uint64_t> v;
    std::string err;
    if (!walk_plan_inodes(record, v, err)) {
        CPPUNIT_FAIL("walk_plan_inodes failed: " + err);
    }
    return v;
}

// "subject block inode" per expectation, one per line.
std::string
ifind_text(const WalkTruth &t)
{
    std::ostringstream s;
    for (const IfindExpectation &e : t.ifind) {
        s << e.subject << ' ' << e.block << ' ' << e.inode << '\n';
    }
    return s.str();
}

std::string
used_free_text(const WalkTruth &t)
{
    std::ostringstream s;
    for (uint64_t i : t.used_free) {
        s << i << ' ';
    }
    return s.str();
}

bool
parse_fails(const std::string &record, const std::string &why)
{
    WalkTruth t;
    std::string err;
    if (parse_walk_truth(record, t, err)) {
        return false;
    }
    if (err.find(why) == std::string::npos) {
        CPPUNIT_FAIL("error '" + err + "' doesn't say '" + why + "'");
    }
    return true;
}

} // namespace

class WalkRecordTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(WalkRecordTest);
    CPPUNIT_TEST(testXfsCountsAndInternalInodes);
    CPPUNIT_TEST(testXfsPlanSkipsSparseHoles);
    CPPUNIT_TEST(testXfsUsedFreeInodes);
    CPPUNIT_TEST(testXfsIfindExpectations);
    CPPUNIT_TEST(testXfsQuotaInodesAreInternal);
    CPPUNIT_TEST(testXfsInodeBtreeLevelNotes);
    CPPUNIT_TEST(testXfsErrors);
    CPPUNIT_TEST(testExtCountsAndReservedInodes);
    CPPUNIT_TEST(testExtPlanStopsAtUnusedTail);
    CPPUNIT_TEST(testExtPlanOfGroupNotZeroed);
    CPPUNIT_TEST(testExtUsedFreeInodes);
    CPPUNIT_TEST(testExtIfindExpectations);
    CPPUNIT_TEST(testExtErrors);
    CPPUNIT_TEST(testPlanBlocks);
    CPPUNIT_TEST(testUnknownRecord);
    CPPUNIT_TEST_SUITE_END();

  public:
    void testXfsCountsAndInternalInodes()
    {
        const WalkTruth t = truth(XFS_RECORD);
        CPPUNIT_ASSERT_EQUAL(std::string("xfs"), t.fs_type);
        CPPUNIT_ASSERT_EQUAL(uint64_t(524288), t.block_count);
        CPPUNIT_ASSERT_EQUAL(uint64_t(240), t.inode_count);
        CPPUNIT_ASSERT_EQUAL(uint64_t(0), t.virtual_inode);
        CPPUNIT_ASSERT_EQUAL(size_t(2), t.internal_inodes.size());
        CPPUNIT_ASSERT_EQUAL(uint64_t(129), t.internal_inodes[0].first);
        CPPUNIT_ASSERT_EQUAL(
            std::string("rbmino"), t.internal_inodes[0].second);
        CPPUNIT_ASSERT_EQUAL(uint64_t(130), t.internal_inodes[1].first);
        CPPUNIT_ASSERT_EQUAL(
            std::string("rsumino"), t.internal_inodes[1].second);
    }

    void testXfsPlanSkipsSparseHoles()
    {
        // AG 1 starts at inode 1 << (18 + 3); its hole's free bits are no
        // inodes.
        const std::vector<uint64_t> want = {
            1052027, 1052028, 1052029, 1052030, 1052031, 2097152 + 640};
        CPPUNIT_ASSERT(want == plan_inodes(XFS_RECORD));
    }

    void testXfsUsedFreeInodes()
    {
        // Any bit of the 64-bit ctime counts.
        CPPUNIT_ASSERT_EQUAL(std::string("1052028 1052029 1052031 2097792 "),
            used_free_text(truth(XFS_RECORD)));
    }

    void testXfsIfindExpectations()
    {
        // Blocks are linear: daddr * 512 / 4096. holes.bin: blocks 393821
        // (AG 1) then 75 and 76 (AG 0), in file order. /corpus has none.
        // The free block is AG 0's first free extent; the header block AG 1's
        // first block.
        CPPUNIT_ASSERT_EQUAL(
            std::string("/corpus/files/multiblock.bin@first 58 135\n"
                        "/corpus/files/multiblock.bin@middle 60 135\n"
                        "/corpus/files/multiblock.bin@last 61 135\n"
                        "/corpus/sparse/holes.bin@first 393821 2097921\n"
                        "/corpus/sparse/holes.bin@middle 75 2097921\n"
                        "/corpus/sparse/holes.bin@last 76 2097921\n"
                        "/free-block 131381 0\n"
                        "/header-block 262144 0\n"),
            ifind_text(truth(XFS_RECORD)));
    }

    void testXfsQuotaInodesAreInternal()
    {
        const WalkTruth t =
            truth(replace(XFS_RECORD, "gquotino = null", "gquotino = 131"));
        CPPUNIT_ASSERT_EQUAL(size_t(3), t.internal_inodes.size());
        CPPUNIT_ASSERT_EQUAL(uint64_t(131), t.internal_inodes[2].first);
        CPPUNIT_ASSERT_EQUAL(
            std::string("gquotino"), t.internal_inodes[2].second);
    }

    void testXfsInodeBtreeLevelNotes()
    {
        const WalkTruth t = truth(XFS_RECORD);
        std::string notes;
        for (const std::string &n : t.notes) {
            notes += n + '\n';
        }
        CPPUNIT_ASSERT_MESSAGE(notes,
            notes.find("AG 0: inode B+tree level 1, 2 chunk records, 128 "
                       "inodes") != std::string::npos);
        CPPUNIT_ASSERT_MESSAGE(notes,
            notes.find("AG 1: inode B+tree level 2, 2 chunk records, 112 "
                       "inodes") != std::string::npos);
    }

    void testXfsErrors()
    {
        CPPUNIT_ASSERT(parse_fails(
            replace(XFS_RECORD, "== xfs_db sb\n", "== other\n"), "xfs_db sb"));
        // The records' counts don't add up to agi_count.
        CPPUNIT_ASSERT(parse_fails(
            replace(XFS_RECORD, "count = 112", "count = 113"), "AG 1"));
        CPPUNIT_ASSERT(parse_fails(
            replace(XFS_RECORD, "1:[640,0,64,1,0x1]", "1:[640,0,64,1"),
            "1:[640,0,64,1"));
        // A free inode without its ctime.
        CPPUNIT_ASSERT(parse_fails(
            replace(XFS_RECORD, "2097792 00000000ffffffff\n", ""), "2097792"));
        CPPUNIT_ASSERT(
            parse_fails(replace(XFS_RECORD, "== xfs_db agi 1\n", ""), "agi"));
        CPPUNIT_ASSERT(
            parse_fails(replace(XFS_RECORD, "   0: [0..31]:         464..495",
                            "   0: [0..31]:         464..495x"),
                "multiblock.bin"));
    }

    void testExtCountsAndReservedInodes()
    {
        // Reserved and in use: allocated (not in a group's free inodes) with
        // a ctime. 2 is also a path of the Manifest.
        const WalkTruth t = truth(EXT_RECORD);
        CPPUNIT_ASSERT_EQUAL(std::string("ext"), t.fs_type);
        CPPUNIT_ASSERT_EQUAL(uint64_t(65536), t.block_count);
        CPPUNIT_ASSERT_EQUAL(uint64_t(128), t.inode_count);
        CPPUNIT_ASSERT_EQUAL(uint64_t(129), t.virtual_inode);
        std::string internal;
        for (const auto &i : t.internal_inodes) {
            internal += std::to_string(i.first) + ' ';
        }
        CPPUNIT_ASSERT_EQUAL(std::string("1 2 7 8 "), internal);
    }

    void testExtPlanStopsAtUnusedTail()
    {
        // The reserved inodes, then each group's free inodes below its
        // unused tail: 20-24 (tail 25-64) and 65-68 (tail 69-128).
        const std::vector<uint64_t> want = {
            1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 20, 21, 22, 23, 24, 65, 66, 67, 68};
        CPPUNIT_ASSERT(want == plan_inodes(EXT_RECORD));
    }

    void testExtPlanOfGroupNotZeroed()
    {
        // An inode table not known to be zeroed: every free inode of the
        // group, whatever the unused count says.
        const std::vector<uint64_t> got = plan_inodes(replace(EXT_RECORD,
            "csum 0x3281 [ITABLE_ZEROED]", "csum 0x3281 [INODE_UNINIT]"));
        CPPUNIT_ASSERT_EQUAL(size_t(15 + 63), got.size());
        CPPUNIT_ASSERT_EQUAL(uint64_t(128), got.back());
        // No unused count at all (no uninit_bg): the same.
        const std::vector<uint64_t> none = plan_inodes(replace(
            EXT_RECORD, "0 directories, 60 unused inodes", "0 directories"));
        CPPUNIT_ASSERT_EQUAL(size_t(15 + 63), none.size());
    }

    void testExtUsedFreeInodes()
    {
        CPPUNIT_ASSERT_EQUAL(
            std::string("20 22 23 24 65 "), used_free_text(truth(EXT_RECORD)));
    }

    void testExtIfindExpectations()
    {
        // Extent tree blocks (ETB) and indirect blocks (IND) aren't data. A
        // fast symlink has none. The free block is the first free one; the
        // header block group 1's first.
        CPPUNIT_ASSERT_EQUAL(
            std::string("/@first 4241 2\n"
                        "/@middle 4241 2\n"
                        "/@last 4241 2\n"
                        "/corpus/fragmented/extents.bin@first 156160 13\n"
                        "/corpus/fragmented/extents.bin@middle 156164 13\n"
                        "/corpus/fragmented/extents.bin@last 156165 13\n"
                        "/corpus/files/multiblock.bin@first 35719 12\n"
                        "/corpus/files/multiblock.bin@middle 35720 12\n"
                        "/corpus/files/multiblock.bin@last 35722 12\n"
                        "/free-block 4262 0\n"
                        "/header-block 32768 0\n"),
            ifind_text(truth(EXT_RECORD)));
    }

    void testExtErrors()
    {
        // Inodes per group times groups must be the inode count.
        CPPUNIT_ASSERT(
            parse_fails(replace(EXT_RECORD, "Inode count:              128",
                            "Inode count:              129"),
                "inode count"));
        CPPUNIT_ASSERT(
            parse_fails(replace(EXT_RECORD, "20 6b0703b6\n", ""), "20"));
        CPPUNIT_ASSERT(parse_fails(
            replace(EXT_RECORD, "Free inodes: 65-70", "Free inodes: 65-x"),
            "65-x"));
        CPPUNIT_ASSERT(
            parse_fails(replace(EXT_RECORD, "(0):4241", "(0):4241x"), "4241x"));
    }

    void testPlanBlocks()
    {
        std::vector<uint64_t> got;
        std::string err;
        // Without the inode-ctime section: the guest plans the blocks before
        // it reads the ctimes.
        const std::string rec = XFS_RECORD;
        CPPUNIT_ASSERT_MESSAGE(err,
            walk_plan_blocks(
                rec.substr(0, rec.find("== inode-ctime")), got, err));
        const std::vector<uint64_t> want = {
            58, 60, 61, 75, 76, 131381, 262144, 393821};
        CPPUNIT_ASSERT(want == got);
    }

    void testUnknownRecord()
    {
        CPPUNIT_ASSERT(parse_fails("", "neither"));
        std::vector<uint64_t> v;
        std::string err;
        CPPUNIT_ASSERT(!walk_plan_inodes("garbage\n", v, err));
        CPPUNIT_ASSERT(!walk_plan_blocks("== dumpe2fs\n", v, err));
    }
};
CPPUNIT_TEST_SUITE_REGISTRATION(WalkRecordTest);

namespace {

const char ILS_HEADER[] =
    "class|host|device|start_time\n"
    "ils|f5485a880cb2||1795695345\n"
    "st_ino|st_alloc|st_uid|st_gid|st_mtime|st_atime|st_ctime|st_crtime|"
    "st_mode|st_nlink|st_size\n";

// ils output with one line per inode.
std::string
ils_text(const std::vector<uint64_t> &inodes, char alloc)
{
    std::string s = ILS_HEADER;
    for (uint64_t i : inodes) {
        s += std::to_string(i) + '|' + alloc +
            "|0|0|1795621814|1795621814|1795621814|0|644|1|13\n";
    }
    return s;
}

std::vector<uint64_t>
inodes_1_to(uint64_t n)
{
    std::vector<uint64_t> v;
    for (uint64_t i = 1; i <= n; ++i) {
        v.push_back(i);
    }
    return v;
}

// Ground truth of EXT_RECORD's file system: the root, lost+found, and three
// files, one with two names.
Manifest
ext_ground_truth()
{
    Manifest m;
    const struct {
        const char *path;
        uint64_t inode;
    } entries[] = {{"/", 2}, {"/lost+found", 11}, {"/a", 12}, {"/b", 13},
        {"/c", 14}, {"/d", 12}};
    for (const auto &e : entries) {
        Entry x;
        x.path = e.path;
        x.inode = e.inode;
        m.entries.push_back(x);
    }
    return m;
}

// ils -a, ils -e and ils of EXT_RECORD's file system as they should be.
std::vector<uint64_t>
ext_allocated()
{
    return {1, 2, 7, 8, 11, 12, 13, 14, 129};
}

std::vector<uint64_t>
ext_used_free()
{
    return {20, 22, 23, 24, 65};
}

std::string
paths(const Manifest &m)
{
    std::string s;
    for (const Entry &e : m.entries) {
        s += e.path + (e.size != 0 ? "=" + std::to_string(e.size) : "") + ' ';
    }
    return s;
}

std::string
findings(const CompareResult &r)
{
    std::string s;
    for (const Finding &f : r.findings) {
        s += f.message + '\n';
    }
    return s;
}

CompareResult
compare(const Manifest &expected, const Manifest &run)
{
    return compare_manifests(expected, run, CompareOptions());
}

ToolOutput
not_recognised()
{
    return {1, "", "Cannot determine file system type\n"};
}

} // namespace

class IlsTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(IlsTest);
    CPPUNIT_TEST(testParsesIls);
    CPPUNIT_TEST(testMalformedIls);
    CPPUNIT_TEST(testExpected);
    CPPUNIT_TEST(testMatchingRunPasses);
    CPPUNIT_TEST(testDroppedAllocatedInode);
    CPPUNIT_TEST(testWrongInodeCount);
    CPPUNIT_TEST(testUsedFreeInodeNotListed);
    CPPUNIT_TEST(testCorpusDeletedMustBeUsedFree);
    CPPUNIT_TEST(testUnreadableRecord);
    CPPUNIT_TEST(testFailedIls);
    CPPUNIT_TEST_SUITE_END();

  public:
    void testParsesIls()
    {
        std::vector<IlsLine> v;
        std::string err;
        CPPUNIT_ASSERT_MESSAGE(err,
            parse_ils(std::string(ILS_HEADER) +
                    "92|f|0|0|1005250226|1014022236|1795621770|1795617662|0|"
                    "0|0\n"
                    "65537|a|0|0|0|0|0|0|0|1|0\n",
                v, err));
        CPPUNIT_ASSERT_EQUAL(size_t(2), v.size());
        CPPUNIT_ASSERT_EQUAL(uint64_t(92), v[0].inode);
        CPPUNIT_ASSERT_EQUAL('f', v[0].alloc);
        CPPUNIT_ASSERT_EQUAL(uint64_t(65537), v[1].inode);
        CPPUNIT_ASSERT_EQUAL('a', v[1].alloc);
        // No inodes at all.
        CPPUNIT_ASSERT(parse_ils(ILS_HEADER, v, err));
        CPPUNIT_ASSERT(v.empty());
    }

    void testMalformedIls()
    {
        std::vector<IlsLine> v;
        std::string err;
        CPPUNIT_ASSERT(!parse_ils("", v, err));
        CPPUNIT_ASSERT(!parse_ils("st_ino|st_alloc\n92|f\n", v, err));
        CPPUNIT_ASSERT(
            !parse_ils(std::string(ILS_HEADER) + "92|f|0|0\n", v, err));
        CPPUNIT_ASSERT(!parse_ils(
            std::string(ILS_HEADER) + "x|f|0|0|0|0|0|0|0|0|0\n", v, err));
        CPPUNIT_ASSERT(!parse_ils(
            std::string(ILS_HEADER) + "9|q|0|0|0|0|0|0|0|0|0\n", v, err));
        CPPUNIT_ASSERT_MESSAGE(err, err.find("9|q") != std::string::npos);
    }

    void testExpected()
    {
        // The Manifest's inodes (12 once), the reserved inodes in use, the
        // virtual $OrphanFiles, the free inodes once in use, and the inode
        // count plus the virtual inode.
        const Manifest m =
            ils_expected(ext_ground_truth(), EXT_RECORD, "22\n24\n");
        CPPUNIT_ASSERT_MESSAGE(m.error_detail, !m.has_error);
        CPPUNIT_ASSERT_EQUAL(
            std::string("/allocated/1 /allocated/2 /allocated/7 /allocated/8 "
                        "/allocated/11 /allocated/12 /allocated/13 "
                        "/allocated/14 /allocated/129 /used-free/20 "
                        "/used-free/22 /used-free/23 /used-free/24 "
                        "/used-free/65 /inode-count=129 "),
            paths(m));
    }

    void testMatchingRunPasses()
    {
        const Manifest run = ils_run({0, ils_text(ext_allocated(), 'a'), ""},
            {0, ils_text(inodes_1_to(129), 'a'), ""},
            {0, ils_text(ext_used_free(), 'f'), ""});
        const CompareResult r =
            compare(ils_expected(ext_ground_truth(), EXT_RECORD, ""), run);
        CPPUNIT_ASSERT_MESSAGE(findings(r), r.verdict() == Verdict::Pass);
    }

    void testDroppedAllocatedInode()
    {
        std::vector<uint64_t> allocated = ext_allocated();
        allocated.erase(allocated.begin() + 2); // 7
        const Manifest run = ils_run({0, ils_text(allocated, 'a'), ""},
            {0, ils_text(inodes_1_to(129), 'a'), ""},
            {0, ils_text(ext_used_free(), 'f'), ""});
        const CompareResult r =
            compare(ils_expected(ext_ground_truth(), EXT_RECORD, ""), run);
        CPPUNIT_ASSERT_EQUAL(size_t(1), r.failures);
        CPPUNIT_ASSERT_MESSAGE(findings(r),
            findings(r).find("/allocated/7: in Ground truth, missing") !=
                std::string::npos);
    }

    void testWrongInodeCount()
    {
        const Manifest run = ils_run({0, ils_text(ext_allocated(), 'a'), ""},
            {0, ils_text(inodes_1_to(128), 'a'), ""},
            {0, ils_text(ext_used_free(), 'f'), ""});
        const CompareResult r =
            compare(ils_expected(ext_ground_truth(), EXT_RECORD, ""), run);
        CPPUNIT_ASSERT_MESSAGE(findings(r),
            findings(r).find("/inode-count: size differs: Ground truth 129, "
                             "run 128") != std::string::npos);
    }

    void testUsedFreeInodeNotListed()
    {
        // A never-used free inode listed, a once-used one not.
        const Manifest run = ils_run({0, ils_text(ext_allocated(), 'a'), ""},
            {0, ils_text(inodes_1_to(129), 'a'), ""},
            {0, ils_text({20, 21, 22, 23, 24}, 'f'), ""});
        const std::string f = findings(
            compare(ils_expected(ext_ground_truth(), EXT_RECORD, ""), run));
        CPPUNIT_ASSERT_MESSAGE(f,
            f.find("/used-free/21: in the run, not in Ground truth") !=
                std::string::npos);
        CPPUNIT_ASSERT_MESSAGE(f,
            f.find("/used-free/65: in Ground truth, missing") !=
                std::string::npos);
    }

    void testCorpusDeletedMustBeUsedFree()
    {
        CPPUNIT_ASSERT(!ils_expected(ext_ground_truth(), EXT_RECORD, "22\n24\n")
                .has_error);
        // 21 is free but was never used: the corpus can't have deleted it.
        Manifest m = ils_expected(ext_ground_truth(), EXT_RECORD, "22\n21\n");
        CPPUNIT_ASSERT(m.has_error);
        CPPUNIT_ASSERT_EQUAL(std::string("corpus-deleted"), m.error_code);
        CPPUNIT_ASSERT_MESSAGE(
            m.error_detail, m.error_detail.find("21") != std::string::npos);
        // Nor an allocated one.
        CPPUNIT_ASSERT(
            ils_expected(ext_ground_truth(), EXT_RECORD, "12\n").has_error);
        CPPUNIT_ASSERT(
            ils_expected(ext_ground_truth(), EXT_RECORD, "x\n").has_error);
        // Nothing deleted (no churn): nothing to check.
        CPPUNIT_ASSERT(
            !ils_expected(ext_ground_truth(), EXT_RECORD, "").has_error);
    }

    void testUnreadableRecord()
    {
        const Manifest m =
            ils_expected(ext_ground_truth(), "== dumpe2fs\n", "");
        CPPUNIT_ASSERT(m.has_error);
        CPPUNIT_ASSERT_EQUAL(std::string("walk-record"), m.error_code);
    }

    void testFailedIls()
    {
        const ToolOutput ok{0, ils_text(ext_allocated(), 'a'), ""};
        Manifest run = ils_run(ok, not_recognised(), ok);
        CPPUNIT_ASSERT(run.has_error);
        CPPUNIT_ASSERT_EQUAL(std::string("fs-open"), run.error_code);
        run = ils_run(ok, ok, {0, "nonsense\n", ""});
        CPPUNIT_ASSERT(run.has_error);
        CPPUNIT_ASSERT_EQUAL(std::string("tool-output"), run.error_code);
    }
};
CPPUNIT_TEST_SUITE_REGISTRATION(IlsTest);

namespace {

// ifind -d's answers for EXT_RECORD's blocks: block -> output.
std::map<uint64_t, ToolOutput>
ext_ifind_answers()
{
    return {{4241, {0, "2\n", ""}}, {156160, {0, "13\n", ""}},
        {156164, {0, "13\n", ""}}, {156165, {0, "13\n", ""}},
        {35719, {0, "12\n", ""}}, {35720, {0, "12\n", ""}},
        {35722, {0, "12\n", ""}}, {4262, {0, "Inode not found\n", ""}},
        {32768, {0, "Meta Data\n", ""}}};
}

} // namespace

class IfindTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(IfindTest);
    CPPUNIT_TEST(testExpected);
    CPPUNIT_TEST(testMatchingRunPasses);
    CPPUNIT_TEST(testWrongAnswer);
    CPPUNIT_TEST(testBlockWithoutAnswer);
    CPPUNIT_TEST(testUnreadableAnswer);
    CPPUNIT_TEST(testFailedIfind);
    CPPUNIT_TEST_SUITE_END();

  public:
    void testExpected()
    {
        const Manifest m = ifind_expected(EXT_RECORD);
        CPPUNIT_ASSERT_MESSAGE(m.error_detail, !m.has_error);
        CPPUNIT_ASSERT_EQUAL(size_t(11), m.entries.size());
        CPPUNIT_ASSERT_EQUAL(
            std::string("/corpus/fragmented/extents.bin@middle"),
            m.entries[4].path);
        CPPUNIT_ASSERT_EQUAL(uint64_t(13), m.entries[4].inode);
        CPPUNIT_ASSERT_EQUAL(uint64_t(156164), m.entries[4].size);
        CPPUNIT_ASSERT_EQUAL(std::string("/header-block"), m.entries[10].path);
        CPPUNIT_ASSERT_EQUAL(uint64_t(0), m.entries[10].inode);
        CPPUNIT_ASSERT_EQUAL(
            std::string("walk-record"), ifind_expected("").error_code);
    }

    void testMatchingRunPasses()
    {
        // "Inode not found" and "Meta Data" both mean no inode.
        const Manifest expected = ifind_expected(EXT_RECORD);
        const CompareResult r =
            compare(expected, ifind_run(expected, ext_ifind_answers()));
        CPPUNIT_ASSERT_MESSAGE(findings(r), r.verdict() == Verdict::Pass);
    }

    void testWrongAnswer()
    {
        const Manifest expected = ifind_expected(EXT_RECORD);
        auto answers = ext_ifind_answers();
        answers[4262] = {0, "17\n", ""};
        answers[35720] = {0, "Inode not found\n", ""};
        const std::string f =
            findings(compare(expected, ifind_run(expected, answers)));
        CPPUNIT_ASSERT_MESSAGE(f,
            f.find("/free-block: inode differs: Ground truth 0, run 17") !=
                std::string::npos);
        CPPUNIT_ASSERT_MESSAGE(f,
            f.find("/corpus/files/multiblock.bin@middle: inode differs: "
                   "Ground truth 12, run 0") != std::string::npos);
    }

    void testBlockWithoutAnswer()
    {
        // No output, or a failed ifind, for a block: its subjects are missing.
        const Manifest expected = ifind_expected(EXT_RECORD);
        auto answers = ext_ifind_answers();
        answers.erase(4241);
        answers[4262] = {1, "", "Error reading block\n"};
        const CompareResult r = compare(expected, ifind_run(expected, answers));
        CPPUNIT_ASSERT_EQUAL(size_t(4), r.failures);
        CPPUNIT_ASSERT_MESSAGE(findings(r),
            findings(r).find("/@middle: in Ground truth, missing") !=
                std::string::npos);
    }

    void testUnreadableAnswer()
    {
        const Manifest expected = ifind_expected(EXT_RECORD);
        auto answers = ext_ifind_answers();
        answers[4241] = {0, "2-128-1\n", ""};
        const Manifest run = ifind_run(expected, answers);
        CPPUNIT_ASSERT(run.has_error);
        CPPUNIT_ASSERT_EQUAL(std::string("tool-output"), run.error_code);
    }

    void testFailedIfind()
    {
        const Manifest expected = ifind_expected(EXT_RECORD);
        auto answers = ext_ifind_answers();
        answers[4241] = not_recognised();
        const Manifest run = ifind_run(expected, answers);
        CPPUNIT_ASSERT(run.has_error);
        CPPUNIT_ASSERT_EQUAL(std::string("fs-open"), run.error_code);
    }
};
CPPUNIT_TEST_SUITE_REGISTRATION(IfindTest);

namespace {

// What run-tools.sh keeps of blkls -e -l: the header and the last line.
const char BLKLS_KEPT[] = "class|host|image|first_time|unit\n"
                          "blkls|f5485a880cb2||1795695345|Block\n"
                          "addr|alloc\n"
                          "65535|a\n";

std::string
summary_field(const Summary &s, const std::string &name)
{
    for (const auto &f : s.fields) {
        if (f.first == name) {
            return f.second;
        }
    }
    return "(none)";
}

} // namespace

class BlklsTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(BlklsTest);
    CPPUNIT_TEST(testExpected);
    CPPUNIT_TEST(testRunCountsLines);
    CPPUNIT_TEST(testBadOutput);
    CPPUNIT_TEST(testFailedBlkls);
    CPPUNIT_TEST_SUITE_END();

  public:
    void testExpected()
    {
        CPPUNIT_ASSERT_EQUAL(std::string("65536"),
            summary_field(blkls_expected(EXT_RECORD), "block-count"));
        const Summary bad = blkls_expected("");
        CPPUNIT_ASSERT(bad.has_error);
        CPPUNIT_ASSERT_EQUAL(std::string("walk-record"), bad.error_code);
    }

    void testRunCountsLines()
    {
        const Summary s = blkls_run({0, BLKLS_KEPT, ""}, "65539\n");
        CPPUNIT_ASSERT(!s.has_error);
        CPPUNIT_ASSERT_EQUAL(
            std::string("65536"), summary_field(s, "block-count"));
        const CompareResult r =
            compare_summaries(blkls_expected(EXT_RECORD), s, CompareOptions());
        CPPUNIT_ASSERT(r.verdict() == Verdict::Pass);
        CPPUNIT_ASSERT(
            compare_summaries(blkls_expected(EXT_RECORD),
                blkls_run({0, BLKLS_KEPT, ""}, "65538\n"), CompareOptions())
                .verdict() == Verdict::Fail);
    }

    void testBadOutput()
    {
        CPPUNIT_ASSERT_EQUAL(std::string("tool-output"),
            blkls_run({0, "nonsense\n", ""}, "65539\n").error_code);
        CPPUNIT_ASSERT_EQUAL(std::string("tool-output"),
            blkls_run({0, BLKLS_KEPT, ""}, "many\n").error_code);
        CPPUNIT_ASSERT_EQUAL(std::string("tool-output"),
            blkls_run({0, BLKLS_KEPT, ""}, "2\n").error_code);
    }

    void testFailedBlkls()
    {
        CPPUNIT_ASSERT_EQUAL(std::string("fs-open"),
            blkls_run(not_recognised(), "0\n").error_code);
        CPPUNIT_ASSERT_EQUAL(std::string("tool-failed"),
            blkls_run({1, "", "Read error\n"}, "0\n").error_code);
    }
};
CPPUNIT_TEST_SUITE_REGISTRATION(BlklsTest);

namespace {

// A temporary directory tree, removed afterwards.
class Scratch {
  public:
    Scratch()
    {
        const char *tmp = std::getenv("TMPDIR");
        std::string tmpl =
            std::string(tmp != nullptr ? tmp : "/tmp") + "/walkcheck.XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        CPPUNIT_ASSERT_MESSAGE(
            "mkdtemp failed", mkdtemp(buf.data()) != nullptr);
        root_ = buf.data();
        dirs_.push_back(root_);
    }
    ~Scratch()
    {
        for (auto f = files_.rbegin(); f != files_.rend(); ++f) {
            std::remove(f->c_str());
        }
        for (auto d = dirs_.rbegin(); d != dirs_.rend(); ++d) {
            rmdir(d->c_str());
        }
    }
    Scratch(const Scratch &) = delete;
    Scratch &operator=(const Scratch &) = delete;

    void dir(const std::string &name)
    {
        const std::string path = root_ + "/" + name;
        CPPUNIT_ASSERT_MESSAGE(path, mkdir(path.c_str(), 0700) == 0);
        dirs_.push_back(path);
    }
    std::string write(const std::string &name, const std::string &content)
    {
        const std::string path = root_ + "/" + name;
        std::ofstream out(path, std::ios::binary);
        out << content;
        files_.push_back(path);
        return path;
    }
    void tool(const std::string &name, const ToolOutput &t)
    {
        write(name + ".out", t.out);
        write(name + ".err", t.err);
        write(name + ".status", std::to_string(t.status) + "\n");
    }
    const std::string &path() const
    {
        return root_;
    }

  private:
    std::string root_;
    std::vector<std::string> dirs_;
    std::vector<std::string> files_;
};

struct Cli {
    int status;
    std::string out;
    std::string err;
};

Cli
run_cli(const std::vector<std::string> &args)
{
    std::ostringstream out;
    std::ostringstream err;
    const int status = check_tool_cli(args, out, err);
    return {status, out.str(), err.str()};
}

// Ground truth of EXT_RECORD's file system, and runs of ils, ifind and
// blkls that agree with it.
void
write_walks(Scratch &t)
{
    std::ostringstream m;
    write_manifest(m, ext_ground_truth());
    t.dir("gt");
    t.write("gt/ground-truth.manifest", m.str());
    t.write("gt/walk-record.txt", EXT_RECORD);
    t.write("gt/corpus-deleted.txt", "22\n24\n");
    t.dir("run");
    t.tool("run/ils-a", {0, ils_text(ext_allocated(), 'a'), ""});
    t.tool("run/ils-e", {0, ils_text(inodes_1_to(129), 'a'), ""});
    t.tool("run/ils", {0, ils_text(ext_used_free(), 'f'), ""});
    t.dir("run/ifind");
    for (const auto &a : ext_ifind_answers()) {
        t.tool("run/ifind/" + std::to_string(a.first), a.second);
    }
    t.tool("run/blkls", {0, BLKLS_KEPT, ""});
    t.write("run/blkls.lines", "65539\n");
}

std::vector<std::string>
walk_args(const Scratch &t, const std::string &tool,
    const std::vector<std::string> &more = {})
{
    std::vector<std::string> a = {tool, "--ground-truth-dir", t.path() + "/gt",
        "--run-dir", t.path() + "/run"};
    a.insert(a.end(), more.begin(), more.end());
    return a;
}

bool
has(const std::string &text, const std::string &what)
{
    return text.find(what) != std::string::npos;
}

} // namespace

class WalkCliTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(WalkCliTest);
    CPPUNIT_TEST(testEveryWalkToolPasses);
    CPPUNIT_TEST(testDroppedInodeFailsIls);
    CPPUNIT_TEST(testWrongAnswerFailsIfind);
    CPPUNIT_TEST(testWrongCountFailsBlkls);
    CPPUNIT_TEST(testNotesOfTheRecord);
    CPPUNIT_TEST(testPlans);
    CPPUNIT_TEST(testReadBytes);
    CPPUNIT_TEST(testUsageAndInputErrors);
    CPPUNIT_TEST_SUITE_END();

  public:
    void testEveryWalkToolPasses()
    {
        Scratch t;
        write_walks(t);
        for (const char *tool : {"ils", "ifind", "blkls"}) {
            const Cli r = run_cli(walk_args(t, tool));
            CPPUNIT_ASSERT_EQUAL_MESSAGE(
                tool + std::string(": ") + r.err + r.out, 0, r.status);
            CPPUNIT_ASSERT_MESSAGE(r.out, has(r.out, "Result: PASS"));
        }
    }

    void testDroppedInodeFailsIls()
    {
        // The test hook: an inode dropped from the expected allocated set.
        Scratch t;
        write_walks(t);
        const Cli r =
            run_cli(walk_args(t, "ils", {"--override", "/allocated/7=drop"}));
        CPPUNIT_ASSERT_EQUAL_MESSAGE(r.out + r.err, 1, r.status);
        CPPUNIT_ASSERT_MESSAGE(
            r.out, has(r.out, "/allocated/7: in the run, not in Ground truth"));
        CPPUNIT_ASSERT_MESSAGE(r.out, has(r.out, "test hook"));
    }

    void testWrongAnswerFailsIfind()
    {
        Scratch t;
        write_walks(t);
        const std::string items = t.write("items", "");
        const Cli r = run_cli(walk_args(t, "ifind",
            {"--override", "/corpus/files/multiblock.bin@first=99", "--check",
                "ifind-guest", "--partition", "sda6", "--items", items}));
        CPPUNIT_ASSERT_EQUAL_MESSAGE(r.out + r.err, 1, r.status);
        std::ifstream in(items);
        std::ostringstream written;
        written << in.rdbuf();
        CPPUNIT_ASSERT_MESSAGE(written.str(),
            has(written.str(),
                "fail\tifind-guest sda6: /corpus/files/multiblock.bin@first: "
                "inode differs: Ground truth 99, run 12"));
        CPPUNIT_ASSERT_MESSAGE(written.str(), has(written.str(), "warn\t"));
    }

    void testWrongCountFailsBlkls()
    {
        Scratch t;
        write_walks(t);
        const Cli r =
            run_cli(walk_args(t, "blkls", {"--override", "block-count=1"}));
        CPPUNIT_ASSERT_EQUAL(1, r.status);
        CPPUNIT_ASSERT_MESSAGE(r.out,
            has(r.out, "block-count differs: Ground truth 1, run 65536"));
    }

    void testNotesOfTheRecord()
    {
        // The record's facts go to the report and, as notes, to the items.
        Scratch t;
        write_walks(t);
        const std::string items = t.write("items", "");
        const Cli r = run_cli(walk_args(t, "ils",
            {"--check", "ils-host", "--partition", "sda6", "--items", items}));
        CPPUNIT_ASSERT_EQUAL(0, r.status);
        CPPUNIT_ASSERT_MESSAGE(r.out, has(r.out, "2 groups of 64 inodes"));
        CPPUNIT_ASSERT_MESSAGE(r.out,
            has(r.out,
                "5 free inodes once in use, 2 of them deleted by the Test "
                "corpus"));
    }

    void testPlans()
    {
        Scratch t;
        const std::string rec = t.write("record", EXT_RECORD);
        Cli r = run_cli({"plan-inodes", rec});
        CPPUNIT_ASSERT_EQUAL_MESSAGE(r.err, 0, r.status);
        CPPUNIT_ASSERT_EQUAL(
            std::string("1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n20\n21\n22\n23\n24\n"
                        "65\n66\n67\n68\n"),
            r.out);
        r = run_cli({"plan-blocks", rec});
        CPPUNIT_ASSERT_EQUAL_MESSAGE(r.err, 0, r.status);
        CPPUNIT_ASSERT_EQUAL(std::string("4241\n4262\n32768\n35719\n35720\n"
                                         "35722\n156160\n156164\n156165\n"),
            r.out);
        const std::string bad = t.write("bad", "nothing\n");
        r = run_cli({"plan-inodes", bad});
        CPPUNIT_ASSERT_EQUAL(2, r.status);
        CPPUNIT_ASSERT_MESSAGE(r.err, has(r.err, "neither"));
        CPPUNIT_ASSERT_EQUAL(2, run_cli({"plan-blocks"}).status);
        CPPUNIT_ASSERT_EQUAL(
            2, run_cli({"plan-blocks", t.path() + "/none"}).status);
    }

    void testReadBytes()
    {
        // The guest reads each free XFS inode's di_ctime this way: "<label>
        // <byte offset>" in, "<label> <hex of LENGTH bytes at offset + SKIP>"
        // out.
        Scratch t;
        std::string dev(64, '\0');
        dev[48] = '\x6b';
        dev[55] = '\x01';
        dev[56] = '\xff';
        const std::string path = t.write("dev", dev);
        const std::string list = t.write("list", "190 0\n191 8\n\n");
        Cli r = run_cli({"read-bytes", path, "48", "8", list});
        CPPUNIT_ASSERT_EQUAL_MESSAGE(r.err, 0, r.status);
        CPPUNIT_ASSERT_EQUAL(
            std::string("190 6b00000000000001\n191 ff00000000000000\n"), r.out);
        // Past the end of the device, a bad line, bad numbers.
        CPPUNIT_ASSERT_EQUAL(2,
            run_cli({"read-bytes", path, "48", "8", t.write("past", "1 16\n")})
                .status);
        CPPUNIT_ASSERT_EQUAL(2,
            run_cli({"read-bytes", path, "48", "8", t.write("bad", "1\n")})
                .status);
        CPPUNIT_ASSERT_EQUAL(
            2, run_cli({"read-bytes", path, "x", "8", list}).status);
        CPPUNIT_ASSERT_EQUAL(
            2, run_cli({"read-bytes", path, "48", "0", list}).status);
        CPPUNIT_ASSERT_EQUAL(2,
            run_cli({"read-bytes", t.path() + "/none", "48", "8", list})
                .status);
    }

    void testUsageAndInputErrors()
    {
        Scratch t;
        write_walks(t);
        // Hook values: drop for ils, an inode for ifind, a known subject.
        CPPUNIT_ASSERT_EQUAL(2,
            run_cli(walk_args(t, "ils", {"--override", "/allocated/7=3"}))
                .status);
        CPPUNIT_ASSERT_EQUAL(2,
            run_cli(walk_args(t, "ils", {"--override", "/allocated/77=drop"}))
                .status);
        CPPUNIT_ASSERT_EQUAL(2,
            run_cli(walk_args(t, "ifind", {"--override", "/free-block=x"}))
                .status);
        CPPUNIT_ASSERT_EQUAL(
            2, run_cli(walk_args(t, "fls", {"--override", "/x=drop"})).status);
        // The corpus's deleted inodes are part of Ground truth.
        Scratch nodel;
        write_walks(nodel);
        std::remove((nodel.path() + "/gt/corpus-deleted.txt").c_str());
        const Cli r = run_cli(walk_args(nodel, "ils"));
        CPPUNIT_ASSERT_EQUAL(2, r.status);
        CPPUNIT_ASSERT_MESSAGE(r.err, has(r.err, "corpus-deleted.txt"));
        // blkls without its line count.
        Scratch nolines;
        write_walks(nolines);
        std::remove((nolines.path() + "/run/blkls.lines").c_str());
        CPPUNIT_ASSERT_EQUAL(2, run_cli(walk_args(nolines, "blkls")).status);
    }
};
CPPUNIT_TEST_SUITE_REGISTRATION(WalkCliTest);
