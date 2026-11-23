/*
 * Self-test of the CLI tool checks (toolcheck.h, toolcheck_cli.h), linked
 * into manifest-selftest and run in gate 1. Tool output as the tools print
 * it (captured from fls, icat, fsstat and istat of release 4.15.0 on a small
 * ext4 image made with mkfs.ext4 -d) in, Manifests, summaries and verdicts
 * out.
 */
#include "toolcheck.h"
#include "toolcheck_cli.h"

#include <cppunit/extensions/HelperMacros.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <utility>
#include <sstream>
#include <string>
#include <vector>

#include <sys/stat.h> // mkdir
#include <unistd.h>   // rmdir; mkdtemp is in <cstdlib> on POSIX

using namespace tskval;

namespace {

Manifest
parse_manifest(const std::string &text)
{
    std::istringstream in("#tsk-manifest v1\n" + text);
    Manifest m;
    std::string err;
    if (!read_manifest(in, m, err)) {
        CPPUNIT_FAIL("read_manifest failed: " + err);
    }
    return m;
}

const Entry *
find(const Manifest &m, const std::string &path)
{
    for (const Entry &e : m.entries) {
        if (e.path == path) {
            return &e;
        }
    }
    return nullptr;
}

// A copy: a reference would outlive the temporary path string in GCC's view
// (-Wdangling-reference).
Entry
get(const Manifest &m, const std::string &path)
{
    const Entry *e = find(m, path);
    if (e == nullptr) {
        CPPUNIT_FAIL("no entry " + path);
    }
    return *e;
}

// fls -r -p -u of the sample image, TZ=UTC, as printed (tabs after the colon).
const char FLS_SAMPLE[] = "d/d 11:\tlost+found\n"
                          "r/r 13:\tctl^name\n"
                          "d/d 14:\tempty-dir\n"
                          "r/r 15:\thello.txt\n"
                          "r/r 16:\tsp ace\n"
                          "d/d 17:\tsub\n"
                          "r/r 15:\tsub/hard\n"
                          "l/l 18:\tsymlink\n"
                          "V/V 2049:\t$OrphanFiles\n";

} // namespace

class FlsTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(FlsTest);
    CPPUNIT_TEST(testParsesPathsAndTypes);
    CPPUNIT_TEST(testSkipsEverythingBelowVirtualEntries);
    CPPUNIT_TEST(testDisagreeingTypesAreOther);
    CPPUNIT_TEST(testRejectsMalformedLine);
    CPPUNIT_TEST(testExpectedListingFromGroundTruth);
    CPPUNIT_TEST(testGroundTruthErrorCarriesOver);
    CPPUNIT_TEST(testRunOfAFailedTool);
    CPPUNIT_TEST_SUITE_END();

  public:
    void testParsesPathsAndTypes()
    {
        Manifest m;
        std::string err;
        CPPUNIT_ASSERT_MESSAGE(err, parse_fls(FLS_SAMPLE, m, err));
        CPPUNIT_ASSERT_EQUAL(size_t(8), m.entries.size());
        CPPUNIT_ASSERT(get(m, "/lost+found").type == EntryType::Dir);
        CPPUNIT_ASSERT(get(m, "/ctl^name").type == EntryType::File);
        CPPUNIT_ASSERT(get(m, "/sp ace").type == EntryType::File);
        CPPUNIT_ASSERT(get(m, "/sub/hard").type == EntryType::File);
        CPPUNIT_ASSERT(get(m, "/symlink").type == EntryType::Symlink);
        CPPUNIT_ASSERT(find(m, "/$OrphanFiles") == nullptr);
    }

    void testSkipsEverythingBelowVirtualEntries()
    {
        Manifest m;
        std::string err;
        CPPUNIT_ASSERT_MESSAGE(err,
            parse_fls("V/V 2049:\t$OrphanFiles\n"
                      "r/r 30:\t$OrphanFiles/OrphanFile-30\n"
                      "r/r 31:\t$OrphanFilesX\n",
                m, err));
        CPPUNIT_ASSERT_EQUAL(size_t(1), m.entries.size());
        CPPUNIT_ASSERT_EQUAL(std::string("/$OrphanFilesX"), m.entries[0].path);
    }

    void testDisagreeingTypesAreOther()
    {
        // A name whose metadata is missing (-) or of another type.
        Manifest m;
        std::string err;
        CPPUNIT_ASSERT_MESSAGE(
            err, parse_fls("r/- 12:\ta\nr/d 13:\tb\nh/h 14:\tc\n", m, err));
        CPPUNIT_ASSERT(get(m, "/a").type == EntryType::Other);
        CPPUNIT_ASSERT(get(m, "/b").type == EntryType::Other);
        CPPUNIT_ASSERT(get(m, "/c").type == EntryType::Other);
    }

    void testRejectsMalformedLine()
    {
        Manifest m;
        std::string err;
        CPPUNIT_ASSERT(!parse_fls("r/r 12:\ta\nnonsense\n", m, err));
        CPPUNIT_ASSERT_MESSAGE(err, err.find("line 2") != std::string::npos);
        CPPUNIT_ASSERT(!parse_fls("r/r 12:\t\n", m, err)); // no name
    }

    void testExpectedListingFromGroundTruth()
    {
        // Ground truth of the sample tree (fields after the type are
        // whatever capture wrote; only path and type may survive).
        const Manifest gt = parse_manifest(
            "/\tdir\t2\t0755\t0\t0\t1024\t5\t-\t-\t1\t1\t-\t-\n"
            "/ctl%01name\tfile\t13\t0644\t0\t0\t1\t1\t-\t"
            "a1fce4363854ff888cff4b8e7875d600c2682390412a8cf79b37d0b11148b0fa"
            "\t1\t1\t-\t-\n"
            "/del%7Fname\tfifo\t19\t0644\t0\t0\t0\t1\t-\t-\t1\t1\t-\t-\n"
            "/symlink\tsymlink\t18\t0777\t0\t0\t9\t1\thello.txt\t-\t1\t1"
            "\t-\t-\n");
        const Manifest want = fls_expected(gt);
        CPPUNIT_ASSERT(!want.has_error);
        CPPUNIT_ASSERT_EQUAL(size_t(3), want.entries.size());
        CPPUNIT_ASSERT(find(want, "/") == nullptr);
        const Entry ctl = get(want, "/ctl^name");
        CPPUNIT_ASSERT(ctl.type == EntryType::File);
        CPPUNIT_ASSERT(get(want, "/del^name").type == EntryType::Fifo);
        // Only path and type: the rest is the same as in a parsed listing.
        Manifest listed;
        std::string err;
        CPPUNIT_ASSERT(parse_fls("r/r 13:\tctl^name\n", listed, err));
        CPPUNIT_ASSERT_EQUAL(
            format_entry(listed.entries[0]), format_entry(ctl));
        CPPUNIT_ASSERT_EQUAL(
            std::string("/sym^link"), fls_sanitize(std::string("/sym\nlink")));
        CPPUNIT_ASSERT_EQUAL(std::string("/caf\xC3\xA9"),
            fls_sanitize(std::string("/caf\xC3\xA9")));
    }

    void testGroundTruthErrorCarriesOver()
    {
        Manifest gt;
        gt.has_error = true;
        gt.error_code = "capture";
        gt.error_detail = "boom";
        const Manifest want = fls_expected(gt);
        CPPUNIT_ASSERT(want.has_error);
        CPPUNIT_ASSERT_EQUAL(std::string("capture"), want.error_code);
    }

    void testRunOfAFailedTool()
    {
        // What fls prints on a file system libtsk doesn't recognise
        // (an XFS partition before XFS support).
        Manifest m = fls_run({1, "", "Cannot determine file system type\n"});
        CPPUNIT_ASSERT(m.has_error);
        CPPUNIT_ASSERT_EQUAL(std::string("fs-open"), m.error_code);
        CPPUNIT_ASSERT_EQUAL(
            std::string("Cannot determine file system type"), m.error_detail);

        // libtsk's other autodetection failures (tsk/fs/fs_open.c): the
        // file system wasn't recognised either. A partition full of random
        // data (high entropy) gets the first.
        for (const char *msg : {"Possible encryption detected (High entropy "
                                "(7.77))\n",
                 "Encryption detected (BitLocker)\n",
                 "Multiple file system types detected (ext4 and xfs)\n"}) {
            m = fls_run({1, "", msg});
            CPPUNIT_ASSERT_EQUAL_MESSAGE(
                msg, std::string("fs-open"), m.error_code);
        }
        // Only at the start of the message.
        m = fls_run({1, "",
            "Error reading: Cannot determine file system "
            "type of the volume\n"});
        CPPUNIT_ASSERT_EQUAL(std::string("tool-failed"), m.error_code);

        m = fls_run({1, "", "Error opening image file\nmore\n"});
        CPPUNIT_ASSERT_EQUAL(std::string("tool-failed"), m.error_code);
        CPPUNIT_ASSERT_EQUAL(
            std::string("exit status 1: Error opening image file"),
            m.error_detail);

        m = fls_run({0, "garbage\n", ""});
        CPPUNIT_ASSERT_EQUAL(std::string("tool-output"), m.error_code);

        m = fls_run({0, FLS_SAMPLE, ""});
        CPPUNIT_ASSERT(!m.has_error);
        CPPUNIT_ASSERT_EQUAL(size_t(8), m.entries.size());
    }
};
CPPUNIT_TEST_SUITE_REGISTRATION(FlsTest);

namespace {

const char HASH_HELLO[] =
    "5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03";
const char HASH_X[] =
    "2d711642b726b04401627ca9fbac32f5c8530fb1903cc4db02258717921a4881";

// Ground truth of a small tree: a directory, a file with two names (hard
// link), another file and a symlink.
Manifest
icat_ground_truth()
{
    return parse_manifest(
        std::string("/\tdir\t2\t0755\t0\t0\t1024\t3\t-\t-\t1\t1\t-\t-\n"
                    "/hello.txt\tfile\t15\t0644\t0\t0\t6\t2\t-\t") +
        HASH_HELLO +
        "\t1\t1\t-\t-\n"
        "/sub\tdir\t17\t0755\t0\t0\t1024\t2\t-\t-\t1\t1\t-\t-\n"
        "/sub/hard\tfile\t15\t0644\t0\t0\t6\t2\t-\t" +
        HASH_HELLO +
        "\t1\t1\t-\t-\n"
        "/x\tfile\t16\t0600\t0\t0\t1\t1\t-\t" +
        HASH_X +
        "\t1\t1\t-\t-\n"
        "/symlink\tsymlink\t18\t0777\t0\t0\t9\t1\thello.txt\t-\t1\t1\t-"
        "\t-\n");
}

} // namespace

class IcatTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(IcatTest);
    CPPUNIT_TEST(testExpectedHashesOfRegularFiles);
    CPPUNIT_TEST(testRunHashesByInode);
    CPPUNIT_TEST(testFailedFileHasNoHash);
    CPPUNIT_TEST(testFailedProbeFailsTheRun);
    CPPUNIT_TEST(testMalformedResults);
    CPPUNIT_TEST_SUITE_END();

  public:
    void testExpectedHashesOfRegularFiles()
    {
        const Manifest want = icat_expected(icat_ground_truth());
        CPPUNIT_ASSERT_EQUAL(size_t(3), want.entries.size());
        const Entry hard = get(want, "/sub/hard");
        CPPUNIT_ASSERT(hard.has_sha256);
        CPPUNIT_ASSERT_EQUAL(std::string(HASH_HELLO), hard.sha256);
        CPPUNIT_ASSERT(hard.type == EntryType::File);
        CPPUNIT_ASSERT_EQUAL(uint64_t(0), hard.inode); // not compared
        CPPUNIT_ASSERT(find(want, "/symlink") == nullptr);
    }

    void testRunHashesByInode()
    {
        const Manifest run = icat_run(icat_ground_truth(), {0, "", ""},
            std::string("15\t0\t") + HASH_HELLO + "\t\n16\t0\t" + HASH_X +
                "\t\n");
        CPPUNIT_ASSERT(!run.has_error);
        // Same fields as expected: the comparison finds no difference.
        const Manifest want = icat_expected(icat_ground_truth());
        CPPUNIT_ASSERT_EQUAL(want.entries.size(), run.entries.size());
        for (const Entry &w : want.entries) {
            CPPUNIT_ASSERT_EQUAL(
                format_entry(w), format_entry(get(run, w.path)));
        }
    }

    void testFailedFileHasNoHash()
    {
        // icat of inode 16 failed; inode 15 was not run at all.
        const Manifest run = icat_run(
            icat_ground_truth(), {0, "", ""}, "16\t1\t-\tError reading file\n");
        CPPUNIT_ASSERT(!run.has_error);
        CPPUNIT_ASSERT(!get(run, "/x").has_sha256);
        CPPUNIT_ASSERT(!get(run, "/hello.txt").has_sha256);
    }

    void testFailedProbeFailsTheRun()
    {
        const Manifest run = icat_run(icat_ground_truth(),
            {1, "", "Cannot determine file system type\n"}, "");
        CPPUNIT_ASSERT(run.has_error);
        CPPUNIT_ASSERT_EQUAL(std::string("fs-open"), run.error_code);
    }

    void testMalformedResults()
    {
        for (const char *bad :
            {"15\t0\n", "x\t0\t-\t\n", "15\t0\tABC\t\n", "15\tz\t-\t\n"}) {
            const Manifest run =
                icat_run(icat_ground_truth(), {0, "", ""}, bad);
            CPPUNIT_ASSERT_MESSAGE(bad, run.has_error);
            CPPUNIT_ASSERT_EQUAL(std::string("tool-output"), run.error_code);
        }
    }
};
CPPUNIT_TEST_SUITE_REGISTRATION(IcatTest);

namespace {

// istat of the sample image's /hello.txt (inode 15, setuid), TZ=UTC.
const char ISTAT_HELLO[] =
    "inode: 15\n"
    "Allocated\n"
    "Group: 0\n"
    "Generation Id: 0\n"
    "uid / gid: 1000 / 1000\n"
    "mode: rrwsr-xr-x\n"
    "Flags: Extents, \n"
    "size: 6\n"
    "num of links: 2\n"
    "\n"
    "Inode Times:\n"
    "Accessed:\t2021-02-03 04:05:06.000000000 (UTC)\n"
    "File Modified:\t2021-02-03 04:05:06.123456789 (UTC)\n"
    "Inode Modified:\t2026-11-23 13:31:39.000000000 (UTC)\n"
    "File Created:\t2026-11-23 13:31:39.620293209 (UTC)\n"
    "\n"
    "Direct Blocks:\n"
    "1652 \n";

// istat of its symlink (inode 18).
const char ISTAT_SYMLINK[] =
    "inode: 18\n"
    "Allocated\n"
    "Group: 0\n"
    "Generation Id: 0\n"
    "symbolic link to: hello.txt\n"
    "uid / gid: 1000 / 1000\n"
    "mode: lrwxrwxrwx\n"
    "size: 9\n"
    "num of links: 1\n"
    "\n"
    "Inode Times:\n"
    "Accessed:\t2026-11-23 13:31:39.000000000 (UTC)\n"
    "File Modified:\t2026-11-23 13:31:39.000000000 (UTC)\n"
    "Inode Modified:\t2026-11-23 13:31:39.000000000 (UTC)\n"
    "File Created:\t2026-11-23 13:31:39.000000000 (UTC)\n"
    "\n"
    "Direct Blocks:\n"
    "0 \n";

// The guest's stat records of the same two paths (the format
// tsk-validate.sh's capture-tools writes: stat --printf with TZ=UTC, then
// the symlink target or -).
const char STAT_RECORDS[] =
    "/hello.txt\t15\t89ed\t1000\t1000\t6\t2\t2021-02-03 04:05:06.123456789 "
    "+0000\t2026-11-23 13:31:39.000000000 +0000\t2026-11-23 "
    "13:31:39.620293209 +0000\t-\n"
    "/symlink\t18\ta1ff\t1000\t1000\t9\t1\t2026-11-23 13:31:39.000000000 "
    "+0000\t2026-11-23 13:31:39.000000000 +0000\t2026-11-23 13:31:39.000000000 "
    "+0000\thello.txt\n";

} // namespace

class IstatTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(IstatTest);
    CPPUNIT_TEST(testParsesStatRecords);
    CPPUNIT_TEST(testStatTimesBefore1970AndAfter2038);
    CPPUNIT_TEST(testMissingSpotCheckPathFailsGroundTruth);
    CPPUNIT_TEST(testMalformedStatRecords);
    CPPUNIT_TEST(testParsesIstat);
    CPPUNIT_TEST(testIstatZeroTimes);
    CPPUNIT_TEST(testMalformedIstat);
    CPPUNIT_TEST(testRunMatchesStat);
    CPPUNIT_TEST(testRunOfFailedIstat);
    CPPUNIT_TEST_SUITE_END();

  public:
    void testParsesStatRecords()
    {
        const Manifest m = istat_expected(STAT_RECORDS);
        CPPUNIT_ASSERT_MESSAGE(m.error_detail, !m.has_error);
        CPPUNIT_ASSERT_EQUAL(size_t(2), m.entries.size());
        const Entry h = get(m, "/hello.txt");
        CPPUNIT_ASSERT(h.type == EntryType::File);
        CPPUNIT_ASSERT_EQUAL(uint64_t(15), h.inode);
        CPPUNIT_ASSERT_EQUAL(uint32_t(04755), h.mode);
        CPPUNIT_ASSERT_EQUAL(uint32_t(1000), h.uid);
        CPPUNIT_ASSERT_EQUAL(uint32_t(1000), h.gid);
        CPPUNIT_ASSERT_EQUAL(uint64_t(6), h.size);
        CPPUNIT_ASSERT_EQUAL(uint64_t(2), h.nlink);
        CPPUNIT_ASSERT_EQUAL(int64_t(1612325106), h.mtime.sec);
        CPPUNIT_ASSERT_EQUAL(uint32_t(123456789), h.mtime.nsec);
        CPPUNIT_ASSERT_EQUAL(int64_t(1795440699), h.ctime.sec);
        CPPUNIT_ASSERT(h.has_crtime);
        CPPUNIT_ASSERT_EQUAL(uint32_t(620293209), h.crtime.nsec);
        CPPUNIT_ASSERT(!h.has_target);
        CPPUNIT_ASSERT(!h.has_sha256);
        CPPUNIT_ASSERT(!h.has_atime);
        const Entry l = get(m, "/symlink");
        CPPUNIT_ASSERT(l.type == EntryType::Symlink);
        CPPUNIT_ASSERT_EQUAL(uint32_t(0777), l.mode);
        CPPUNIT_ASSERT(l.has_target);
        CPPUNIT_ASSERT_EQUAL(std::string("hello.txt"), l.target);
    }

    void testStatTimesBefore1970AndAfter2038()
    {
        const Manifest m = istat_expected(
            "/d\t2\t41ed\t0\t0\t4096\t3\t1922-06-20 21:20:00.500000000 "
            "+0000\t2486-07-02 08:20:00.000000000 +0000\t-\t-\n");
        CPPUNIT_ASSERT_MESSAGE(m.error_detail, !m.has_error);
        const Entry d = get(m, "/d");
        CPPUNIT_ASSERT(d.type == EntryType::Dir);
        CPPUNIT_ASSERT_EQUAL(uint32_t(0755), d.mode);
        CPPUNIT_ASSERT_EQUAL(int64_t(-1500000000), d.mtime.sec);
        CPPUNIT_ASSERT_EQUAL(uint32_t(500000000), d.mtime.nsec);
        CPPUNIT_ASSERT_EQUAL(int64_t(16299217200), d.ctime.sec);
        CPPUNIT_ASSERT(!d.has_crtime); // %w is - without a birth time
    }

    void testMissingSpotCheckPathFailsGroundTruth()
    {
        const Manifest m = istat_expected(
            "/gone\t!missing\tstat: cannot statx '/control/gone': No such "
            "file or directory\n");
        CPPUNIT_ASSERT(m.has_error);
        CPPUNIT_ASSERT_EQUAL(std::string("spot-check"), m.error_code);
        CPPUNIT_ASSERT(m.error_detail.find("/gone") != std::string::npos);
    }

    void testMalformedStatRecords()
    {
        for (const char *bad :
            {"/x\t15\n", "x\t15\t89ed\t0\t0\t6\t2\t-\t-\t-\t-\n",
                "/x\t15\t89ed\t0\t0\t6\t2\t2021-02-03 04:05:06 "
                "+0000\t2021-02-03 "
                "04:05:06.000000000 +0000\t-\t-\n",
                "/x\t15\t89ed\t0\t0\t6\t2\t2021-02-03 04:05:06.000000000 "
                "+0200\t2021-02-03 04:05:06.000000000 +0000\t-\t-\n",
                "/x\t15\tzz\t0\t0\t6\t2\t2021-02-03 04:05:06.000000000 "
                "+0000\t2021-02-03 04:05:06.000000000 +0000\t-\t-\n"}) {
            const Manifest m = istat_expected(bad);
            CPPUNIT_ASSERT_MESSAGE(bad, m.has_error);
            CPPUNIT_ASSERT_EQUAL(std::string("stat-output"), m.error_code);
        }
    }

    void testParsesIstat()
    {
        Entry e;
        std::string err;
        CPPUNIT_ASSERT_MESSAGE(err, parse_istat(ISTAT_HELLO, e, err));
        CPPUNIT_ASSERT(e.type == EntryType::File);
        CPPUNIT_ASSERT_EQUAL(uint64_t(15), e.inode);
        CPPUNIT_ASSERT_EQUAL(uint32_t(04755), e.mode);
        CPPUNIT_ASSERT_EQUAL(uint32_t(1000), e.uid);
        CPPUNIT_ASSERT_EQUAL(uint64_t(6), e.size);
        CPPUNIT_ASSERT_EQUAL(uint64_t(2), e.nlink);
        CPPUNIT_ASSERT_EQUAL(int64_t(1612325106), e.mtime.sec);
        CPPUNIT_ASSERT_EQUAL(uint32_t(123456789), e.mtime.nsec);
        CPPUNIT_ASSERT_EQUAL(int64_t(1795440699), e.ctime.sec);
        CPPUNIT_ASSERT(e.has_crtime);
        CPPUNIT_ASSERT_EQUAL(uint32_t(620293209), e.crtime.nsec);
        CPPUNIT_ASSERT(!e.has_target);

        CPPUNIT_ASSERT_MESSAGE(err, parse_istat(ISTAT_SYMLINK, e, err));
        CPPUNIT_ASSERT(e.type == EntryType::Symlink);
        CPPUNIT_ASSERT_EQUAL(uint32_t(0777), e.mode);
        CPPUNIT_ASSERT(e.has_target);
        CPPUNIT_ASSERT_EQUAL(std::string("hello.txt"), e.target);

        // Other mode letters: sticky without x, setgid without x.
        CPPUNIT_ASSERT(
            parse_istat("inode: 3\nmode: drwxr-Sr-T\nuid / gid: 0 / "
                        "0\nsize: 1\nnum of links: 1\n\nInode "
                        "Times:\nFile Modified:\t0000-00-00 00:00:00 "
                        "(UTC)\nInode Modified:\t0000-00-00 00:00:00 "
                        "(UTC)\n",
                e, err));
        CPPUNIT_ASSERT(e.type == EntryType::Dir);
        CPPUNIT_ASSERT_EQUAL(uint32_t(03744), e.mode);
    }

    void testIstatZeroTimes()
    {
        // istat prints a time at or before the epoch as all zeros, and
        // prints no "File Created" line without a crtime (ext2/3 inodes).
        Entry e;
        std::string err;
        CPPUNIT_ASSERT_MESSAGE(err,
            parse_istat(
                "inode: 3\nmode: rrw-r--r--\nuid / gid: 0 / 0\nsize: "
                "1\nnum of links: 1\n\nInode Times:\nAccessed:\t0000-00-00 "
                "00:00:00 (UTC)\nFile Modified:\t0000-00-00 00:00:00 "
                "(UTC)\nInode Modified:\t2021-02-03 04:05:06 (UTC)\n",
                e, err));
        CPPUNIT_ASSERT_EQUAL(int64_t(0), e.mtime.sec);
        CPPUNIT_ASSERT_EQUAL(int64_t(1612325106), e.ctime.sec);
        CPPUNIT_ASSERT_EQUAL(uint32_t(0), e.ctime.nsec);
        CPPUNIT_ASSERT(!e.has_crtime);
    }

    void testMalformedIstat()
    {
        Entry e;
        std::string err;
        // No mode line.
        CPPUNIT_ASSERT(!parse_istat("inode: 3\nuid / gid: 0 / 0\nsize: 1\nnum "
                                    "of links: 1\n\nInode Times:\nFile "
                                    "Modified:\t2021-02-03 04:05:06 "
                                    "(UTC)\nInode Modified:\t2021-02-03 "
                                    "04:05:06 (UTC)\n",
            e, err));
        // A local time zone instead of UTC.
        CPPUNIT_ASSERT(
            !parse_istat("inode: 3\nmode: rrw-r--r--\nuid / gid: 0 / "
                         "0\nsize: 1\nnum of links: 1\n\nInode "
                         "Times:\nFile Modified:\t2021-02-03 "
                         "04:05:06 (CET)\nInode "
                         "Modified:\t2021-02-03 04:05:06 (UTC)\n",
                e, err));
        CPPUNIT_ASSERT_MESSAGE(err, err.find("UTC") != std::string::npos);
    }

    void testRunMatchesStat()
    {
        const Manifest want = istat_expected(STAT_RECORDS);
        const Manifest run = istat_run(
            want, {{15, {0, ISTAT_HELLO, ""}}, {18, {0, ISTAT_SYMLINK, ""}}});
        CPPUNIT_ASSERT_MESSAGE(run.error_detail, !run.has_error);
        CPPUNIT_ASSERT_EQUAL(format_entry(get(want, "/hello.txt")),
            format_entry(get(run, "/hello.txt")));
        // The symlink's istat times have no fraction in this sample but
        // stat's do too: equal.
        CPPUNIT_ASSERT_EQUAL(format_entry(get(want, "/symlink")),
            format_entry(get(run, "/symlink")));
    }

    void testRunOfFailedIstat()
    {
        const Manifest want = istat_expected(STAT_RECORDS);
        // One inode failed: that entry is missing from the run.
        Manifest run = istat_run(want,
            {{15, {0, ISTAT_HELLO, ""}},
                {18, {1, "", "Error reading inode\n"}}});
        CPPUNIT_ASSERT(!run.has_error);
        CPPUNIT_ASSERT(find(run, "/symlink") == nullptr);
        // Not recognised: the whole run failed.
        run = istat_run(want,
            {{15, {1, "", "Cannot determine file system type\n"}},
                {18, {1, "", "Cannot determine file system type\n"}}});
        CPPUNIT_ASSERT(run.has_error);
        CPPUNIT_ASSERT_EQUAL(std::string("fs-open"), run.error_code);
        // Output it can't parse.
        run = istat_run(want, {{15, {0, "garbage\n", ""}}});
        CPPUNIT_ASSERT_EQUAL(std::string("tool-output"), run.error_code);
        // An inode with no run at all is missing.
        run = istat_run(want, {{15, {0, ISTAT_HELLO, ""}}});
        CPPUNIT_ASSERT(!run.has_error);
        CPPUNIT_ASSERT_EQUAL(size_t(1), run.entries.size());
    }
};
CPPUNIT_TEST_SUITE_REGISTRATION(IstatTest);

namespace {

Summary
summary(std::initializer_list<std::pair<std::string, std::string>> fields)
{
    Summary s;
    s.fields.assign(fields.begin(), fields.end());
    return s;
}

Summary
control_summary()
{
    return summary({{"fs-type", "ext4"}, {"block-size", "4096"},
        {"inode-size", "256"}, {"group-count", "8"},
        {"uuid", "68ff9a08-03e0-4ef3-b3b1-5040914fff69"}, {"label", "control"},
        {"version", "1"}});
}

CompareOptions
known(const std::string &lines)
{
    CompareOptions o;
    o.check = "fsstat-guest";
    o.partition = "sda6";
    std::istringstream in(lines);
    std::string err;
    if (!read_known_failures(in, o.known_failures, err)) {
        CPPUNIT_FAIL("read_known_failures: " + err);
    }
    return o;
}

bool
has_finding(const CompareResult &r, Severity s, const std::string &difference)
{
    for (const Finding &f : r.findings) {
        if (f.severity == s && f.difference == difference) {
            return true;
        }
    }
    return false;
}

} // namespace

class SummaryCompareTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(SummaryCompareTest);
    CPPUNIT_TEST(testEqualSummariesPass);
    CPPUNIT_TEST(testDifferentValueFails);
    CPPUNIT_TEST(testFieldOnOneSideOnlyFails);
    CPPUNIT_TEST(testKnownFailureOfAField);
    CPPUNIT_TEST(testWholeRunError);
    CPPUNIT_TEST(testGroundTruthErrorFails);
    CPPUNIT_TEST(testSummaryFieldNeedsTheSubjectStar);
    CPPUNIT_TEST_SUITE_END();

  public:
    void testEqualSummariesPass()
    {
        const CompareResult r =
            compare_summaries(control_summary(), control_summary(), known(""));
        CPPUNIT_ASSERT(r.verdict() == Verdict::Pass);
        CPPUNIT_ASSERT_EQUAL(size_t(7), r.ground_truth_entries);
    }

    void testDifferentValueFails()
    {
        Summary run = control_summary();
        run.fields[4].second = "00000000-0000-0000-0000-000000000000";
        const CompareResult r =
            compare_summaries(control_summary(), run, known(""));
        CPPUNIT_ASSERT(r.verdict() == Verdict::Fail);
        CPPUNIT_ASSERT_EQUAL(size_t(1), r.failures);
        const Finding &f = r.findings[0];
        CPPUNIT_ASSERT_EQUAL(std::string("uuid"), f.difference);
        CPPUNIT_ASSERT_MESSAGE(f.message,
            f.message.find("68ff9a08-03e0-4ef3-b3b1-5040914fff69") !=
                    std::string::npos &&
                f.message.find("00000000-0000") != std::string::npos);
    }

    void testFieldOnOneSideOnlyFails()
    {
        Summary run = control_summary();
        run.fields.pop_back(); // no version
        run.fields.push_back({"extra-field", "1"});
        const CompareResult r =
            compare_summaries(control_summary(), run, known(""));
        CPPUNIT_ASSERT_EQUAL(size_t(2), r.failures);
        CPPUNIT_ASSERT(has_finding(r, Severity::Failure, "version"));
        CPPUNIT_ASSERT(has_finding(r, Severity::Failure, "extra-field"));
    }

    void testKnownFailureOfAField()
    {
        Summary run = control_summary();
        run.fields[1].second = "1024";
        CompareResult r = compare_summaries(control_summary(), run,
            known("fsstat-guest sda6 * block-size a comment\n"
                  "fsstat-host  sda6 * uuid       another check's\n"));
        CPPUNIT_ASSERT(r.verdict() == Verdict::Pass);
        CPPUNIT_ASSERT(has_finding(r, Severity::Known, "block-size"));
        // Stale once the difference is gone.
        r = compare_summaries(control_summary(), control_summary(),
            known("fsstat-guest sda6 * block-size a comment\n"));
        CPPUNIT_ASSERT(r.verdict() == Verdict::Fail);
        CPPUNIT_ASSERT(
            has_finding(r, Severity::Failure, "known-failure-passes"));
    }

    void testWholeRunError()
    {
        Summary run;
        run.has_error = true;
        run.error_code = "fs-open";
        run.error_detail = "Cannot determine file system type";
        CompareResult r = compare_summaries(control_summary(), run, known(""));
        CPPUNIT_ASSERT(r.verdict() == Verdict::Fail);
        r = compare_summaries(control_summary(), run,
            known("fsstat-guest sda6 * error:fs-open not recognised\n"));
        CPPUNIT_ASSERT(r.verdict() == Verdict::Pass);
        CPPUNIT_ASSERT(has_finding(r, Severity::Known, "error"));
    }

    void testGroundTruthErrorFails()
    {
        Summary gt;
        gt.has_error = true;
        gt.error_code = "summary";
        gt.error_detail = "no dumpe2fs output";
        const CompareResult r =
            compare_summaries(gt, control_summary(), known(""));
        CPPUNIT_ASSERT(r.verdict() == Verdict::Fail);
        CPPUNIT_ASSERT(has_finding(r, Severity::Failure, "ground-truth-error"));
    }

    void testSummaryFieldNeedsTheSubjectStar()
    {
        // A summary field is about the whole file system, never an entry.
        std::istringstream in("fsstat-guest sda6 /corpus uuid wrong\n");
        std::vector<KnownFailure> list;
        std::string err;
        CPPUNIT_ASSERT(!read_known_failures(in, list, err));
        CPPUNIT_ASSERT_MESSAGE(err,
            err.find("line 1") != std::string::npos &&
                err.find("uuid") != std::string::npos);
    }
};
CPPUNIT_TEST_SUITE_REGISTRATION(SummaryCompareTest);

namespace {

// What tsk-validate.sh capture-tools writes for the sample ext4 image:
// blkid -p -o export, then dumpe2fs -h (abridged: the lines it uses and
// some around them).
const char GUEST_EXT4[] =
    "== blkid\n"
    "DEVNAME=/dev/sda6\n"
    "LABEL=mylabel\n"
    "UUID=68ff9a08-03e0-4ef3-b3b1-5040914fff69\n"
    "VERSION=1.0\n"
    "FSBLOCKSIZE=1024\n"
    "BLOCK_SIZE=1024\n"
    "TYPE=ext4\n"
    "USAGE=filesystem\n"
    "== dumpe2fs\n"
    "Filesystem volume name:   mylabel\n"
    "Filesystem UUID:          68ff9a08-03e0-4ef3-b3b1-5040914fff69\n"
    "Filesystem revision #:    1 (dynamic)\n"
    "Inode count:              2048\n"
    "Block count:              8192\n"
    "First block:              1\n"
    "Block size:               1024\n"
    "Blocks per group:         8192\n"
    "Inode size:\t          256\n";

// xfs_info of a v5 file system, and blkid of it without a label.
const char GUEST_XFS[] =
    "== blkid\n"
    "DEVNAME=/dev/sda4\n"
    "UUID=58b65ad6-8935-4dc6-91b5-006c193f477d\n"
    "TYPE=xfs\n"
    "== xfs_info\n"
    "meta-data=/dev/sda4              isize=512    agcount=4, agsize=19200 "
    "blks\n"
    "         =                       sectsz=512   attr=2, projid32bit=1\n"
    "         =                       crc=1        finobt=1, sparse=1, "
    "rmapbt=1\n"
    "         =                       reflink=1    bigtime=1 inobtcount=1 "
    "nrext64=1\n"
    "data     =                       bsize=4096   blocks=76800, imaxpct=25\n"
    "         =                       sunit=0      swidth=0 blks\n"
    "naming   =version 2              bsize=4096   ascii-ci=0, ftype=1, "
    "parent=0\n"
    "log      =internal log           bsize=4096   blocks=16384, version=2\n";

// fsstat of the sample ext4 image (abridged after the first block group).
const char FSSTAT_EXT4[] =
    "FILE SYSTEM INFORMATION\n"
    "--------------------------------------------\n"
    "File System Type: Ext4\n"
    "Volume Name: mylabel\n"
    "Volume ID: 69ff4f914050b1b3f34ee003089aff68\n"
    "\n"
    "Last Written at: 2026-11-23 13:31:39 (UTC)\n"
    "Source OS: Linux\n"
    "Dynamic Structure\n"
    "Compat Features: Journal, Ext Attributes, Resize Inode, Dir Index\n"
    "\n"
    "METADATA INFORMATION\n"
    "--------------------------------------------\n"
    "Inode Range: 1 - 2049\n"
    "Root Directory: 2\n"
    "Free Inodes: 2030\n"
    "Inode Size: 256\n"
    "\n"
    "CONTENT INFORMATION\n"
    "--------------------------------------------\n"
    "Block Groups Per Flex Group: 16\n"
    "Block Range: 0 - 8191\n"
    "Block Size: 1024\n"
    "Free Blocks: 6537\n"
    "\n"
    "BLOCK GROUP INFORMATION\n"
    "--------------------------------------------\n"
    "Number of Block Groups: 1\n"
    "Inodes per group: 2048\n"
    "Blocks per group: 8192\n"
    "\n"
    "Group: 0:\n"
    "  Block Group Flags: []\n"
    "  Inode Range: 1 - 2048\n";

std::string
field(const Summary &s, const std::string &name)
{
    for (const auto &f : s.fields) {
        if (f.first == name) {
            return f.second;
        }
    }
    CPPUNIT_FAIL("no summary field " + name);
    return "";
}

std::string
replace(std::string s, const std::string &from, const std::string &to)
{
    const size_t at = s.find(from);
    CPPUNIT_ASSERT_MESSAGE(from, at != std::string::npos);
    return s.replace(at, from.size(), to);
}

} // namespace

class FsstatTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(FsstatTest);
    CPPUNIT_TEST(testGuestSummaryOfExt4);
    CPPUNIT_TEST(testGuestSummaryOfXfs);
    CPPUNIT_TEST(testGuestSummaryErrors);
    CPPUNIT_TEST(testFsstatOfExt4MatchesGuest);
    CPPUNIT_TEST(testVolumeIdWithoutLeadingZeros);
    CPPUNIT_TEST(testWrongVolumeId);
    CPPUNIT_TEST(testFsstatFailures);
    CPPUNIT_TEST_SUITE_END();

  public:
    void testGuestSummaryOfExt4()
    {
        const Summary s = guest_summary(GUEST_EXT4);
        CPPUNIT_ASSERT_MESSAGE(s.error_detail, !s.has_error);
        CPPUNIT_ASSERT_EQUAL(size_t(7), s.fields.size());
        CPPUNIT_ASSERT_EQUAL(std::string("ext4"), field(s, "fs-type"));
        CPPUNIT_ASSERT_EQUAL(std::string("1024"), field(s, "block-size"));
        CPPUNIT_ASSERT_EQUAL(std::string("256"), field(s, "inode-size"));
        // (8192 blocks - first block 1) / 8192 per group, rounded up
        CPPUNIT_ASSERT_EQUAL(std::string("1"), field(s, "group-count"));
        CPPUNIT_ASSERT_EQUAL(
            std::string("68ff9a08-03e0-4ef3-b3b1-5040914fff69"),
            field(s, "uuid"));
        CPPUNIT_ASSERT_EQUAL(std::string("mylabel"), field(s, "label"));
        CPPUNIT_ASSERT_EQUAL(std::string("1"), field(s, "version"));

        // 1 GiB with 4 KiB blocks: 262144 blocks, first block 0, 32768 per
        // group: 8 groups. One more block makes a ninth.
        std::string big = replace(GUEST_EXT4, "Block count:              8192",
            "Block count:              262145");
        big = replace(
            big, "First block:              1", "First block:              0");
        big = replace(big, "Blocks per group:         8192",
            "Blocks per group:         32768");
        CPPUNIT_ASSERT_EQUAL(
            std::string("9"), field(guest_summary(big), "group-count"));
    }

    void testGuestSummaryOfXfs()
    {
        const Summary s = guest_summary(GUEST_XFS);
        CPPUNIT_ASSERT_MESSAGE(s.error_detail, !s.has_error);
        CPPUNIT_ASSERT_EQUAL(std::string("xfs"), field(s, "fs-type"));
        CPPUNIT_ASSERT_EQUAL(std::string("4096"), field(s, "block-size"));
        CPPUNIT_ASSERT_EQUAL(std::string("512"), field(s, "inode-size"));
        CPPUNIT_ASSERT_EQUAL(std::string("4"), field(s, "group-count"));
        CPPUNIT_ASSERT_EQUAL(std::string(""), field(s, "label"));
        CPPUNIT_ASSERT_EQUAL(std::string("5"), field(s, "version"));
        const Summary v4 = guest_summary(replace(GUEST_XFS, "crc=1", "crc=0"));
        CPPUNIT_ASSERT_EQUAL(std::string("4"), field(v4, "version"));
    }

    void testGuestSummaryErrors()
    {
        for (const std::string &bad :
            {std::string("== blkid\nTYPE=vfat\nUUID=1234-ABCD\n"),
                replace(GUEST_EXT4, "Inode size:", "Inode sighs:"),
                std::string("== blkid\nTYPE=xfs\nUUID=x\n"), std::string("")}) {
            const Summary s = guest_summary(bad);
            CPPUNIT_ASSERT_MESSAGE(bad, s.has_error);
            CPPUNIT_ASSERT_EQUAL(std::string("summary"), s.error_code);
        }
    }

    void testFsstatOfExt4MatchesGuest()
    {
        const Summary want = guest_summary(GUEST_EXT4);
        const Summary run = fsstat_run({0, FSSTAT_EXT4, ""}, want);
        CPPUNIT_ASSERT_MESSAGE(run.error_detail, !run.has_error);
        CPPUNIT_ASSERT(want.fields == run.fields);
        // Static structure: revision 0.
        const Summary rev0 = fsstat_run(
            {0, replace(FSSTAT_EXT4, "Dynamic Structure", "Static Structure"),
                ""},
            want);
        CPPUNIT_ASSERT_EQUAL(std::string("0"), field(rev0, "version"));
        // An empty volume name.
        const Summary nolabel = fsstat_run(
            {0, replace(FSSTAT_EXT4, "Volume Name: mylabel", "Volume Name: "),
                ""},
            want);
        CPPUNIT_ASSERT_EQUAL(std::string(""), field(nolabel, "label"));
    }

    void testVolumeIdWithoutLeadingZeros()
    {
        // fsstat prints the UUID as two little-endian 64-bit numbers in hex,
        // last 8 bytes first, without leading zeros. This UUID's last byte
        // is 0x05, so the first number has 15 digits.
        const std::string uuid = "11223344-5566-7788-99aa-bbccddeeff05";
        const Summary want = guest_summary(replace(GUEST_EXT4,
            "UUID=68ff9a08-03e0-4ef3-b3b1-5040914fff69", "UUID=" + uuid));
        const Summary run = fsstat_run(
            {0,
                replace(FSSTAT_EXT4, "69ff4f914050b1b3f34ee003089aff68",
                    "5ffeeddccbbaa998877665544332211"),
                ""},
            want);
        CPPUNIT_ASSERT_EQUAL(uuid, field(run, "uuid"));
    }

    void testWrongVolumeId()
    {
        const Summary want = guest_summary(GUEST_EXT4);
        // 32 digits: read back as a UUID, which differs.
        Summary run = fsstat_run(
            {0,
                replace(FSSTAT_EXT4, "69ff4f914050b1b3f34ee003089aff68",
                    "00ff4f914050b1b3f34ee003089aff68"),
                ""},
            want);
        CPPUNIT_ASSERT_EQUAL(
            std::string("68ff9a08-03e0-4ef3-b3b1-5040914fff00"),
            field(run, "uuid"));
        // Fewer digits and not the expected one: shown as printed.
        run = fsstat_run({0,
                             replace(FSSTAT_EXT4,
                                 "69ff4f914050b1b3f34ee003089aff68", "1234"),
                             ""},
            want);
        CPPUNIT_ASSERT_EQUAL(std::string("Volume ID 1234"), field(run, "uuid"));
    }

    void testFsstatFailures()
    {
        const Summary want = guest_summary(GUEST_EXT4);
        Summary run =
            fsstat_run({1, "", "Cannot determine file system type\n"}, want);
        CPPUNIT_ASSERT(run.has_error);
        CPPUNIT_ASSERT_EQUAL(std::string("fs-open"), run.error_code);
        run = fsstat_run({1, "", "Error reading superblock\n"}, want);
        CPPUNIT_ASSERT_EQUAL(std::string("tool-failed"), run.error_code);
        // A file system type whose fsstat output this check can't read yet.
        run = fsstat_run(
            {0, "FILE SYSTEM INFORMATION\nFile System Type: XFS\n", ""}, want);
        CPPUNIT_ASSERT_EQUAL(std::string("tool-output"), run.error_code);
        CPPUNIT_ASSERT_MESSAGE(run.error_detail,
            run.error_detail.find("XFS") != std::string::npos);
        // ext4 output with a line missing.
        run = fsstat_run(
            {0, replace(FSSTAT_EXT4, "Block Size: 1024\n", ""), ""}, want);
        CPPUNIT_ASSERT_EQUAL(std::string("tool-output"), run.error_code);
    }
};
CPPUNIT_TEST_SUITE_REGISTRATION(FsstatTest);

namespace {

// A scratch directory tree, removed with everything written through it.
class Scratch {
  public:
    Scratch()
    {
        const char *base = std::getenv("TMPDIR");
        std::string tmpl = std::string(base && *base ? base : "/tmp") +
            "/toolcheck-selftest.XXXXXX";
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

    std::string dir(const std::string &name)
    {
        const std::string path = root_ + "/" + name;
        CPPUNIT_ASSERT_MESSAGE(path, mkdir(path.c_str(), 0700) == 0);
        dirs_.push_back(path);
        return path;
    }
    std::string write(const std::string &name, const std::string &content)
    {
        const std::string path = root_ + "/" + name;
        std::ofstream out(path, std::ios::binary);
        out << content;
        files_.push_back(path);
        return path;
    }
    // A tool's three files: <name>.out, <name>.err and <name>.status.
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
    std::string read(const std::string &name) const
    {
        std::ifstream in(root_ + "/" + name, std::ios::binary);
        std::ostringstream s;
        s << in.rdbuf();
        return s.str();
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

// Ground truth and stat records of the sample tree.
void
write_ground_truth(Scratch &t)
{
    std::ostringstream m;
    write_manifest(m, icat_ground_truth());
    t.dir("gt");
    t.write("gt/ground-truth.manifest", m.str());
    t.write("gt/fs-summary.txt", GUEST_EXT4);
    t.write("gt/stat.txt", STAT_RECORDS);
}

// fls output matching icat_ground_truth().
const char FLS_MATCHING[] = "r/r 15:\thello.txt\n"
                            "d/d 17:\tsub\n"
                            "r/r 15:\tsub/hard\n"
                            "r/r 16:\tx\n"
                            "l/l 18:\tsymlink\n"
                            "V/V 2049:\t$OrphanFiles\n";

// A run directory where every tool agrees with Ground truth.
void
write_matching_run(Scratch &t)
{
    t.dir("run");
    t.tool("run/fls", {0, FLS_MATCHING, ""});
    t.tool("run/icat-root", {0, "", ""});
    t.write("run/icat.tsv",
        std::string("15\t0\t") + HASH_HELLO + "\t\n16\t0\t" + HASH_X + "\t\n");
    t.tool("run/fsstat", {0, FSSTAT_EXT4, ""});
    t.dir("run/istat");
    t.tool("run/istat/15", {0, ISTAT_HELLO, ""});
    t.tool("run/istat/18", {0, ISTAT_SYMLINK, ""});
}

// check-tool's arguments for the gt/ and run/ directories of a scratch tree.
std::vector<std::string>
args(const Scratch &t, const std::string &tool,
    const std::vector<std::string> &more = {})
{
    std::vector<std::string> a = {tool, "--ground-truth-dir", t.path() + "/gt",
        "--run-dir", t.path() + "/run"};
    a.insert(a.end(), more.begin(), more.end());
    return a;
}

} // namespace

class CheckToolCliTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(CheckToolCliTest);
    CPPUNIT_TEST(testEveryToolPasses);
    CPPUNIT_TEST(testChangedByteFailsIcat);
    CPPUNIT_TEST(testWrongSummaryValueFailsFsstat);
    CPPUNIT_TEST(testNotRecognisedIsAKnownFailure);
    CPPUNIT_TEST(testIstatOfAMissingInode);
    CPPUNIT_TEST(testUsageAndInputErrors);
    CPPUNIT_TEST_SUITE_END();

  public:
    void testEveryToolPasses()
    {
        Scratch t;
        write_ground_truth(t);
        write_matching_run(t);
        for (const char *tool : {"fls", "icat", "fsstat", "istat"}) {
            const Cli r = run_cli(args(t, tool));
            CPPUNIT_ASSERT_EQUAL_MESSAGE(
                tool + std::string(": ") + r.err + r.out, 0, r.status);
            CPPUNIT_ASSERT_MESSAGE(
                r.out, r.out.find("Result: PASS") != std::string::npos);
        }
    }

    void testChangedByteFailsIcat()
    {
        // The deliberate break: one byte of /x changed after Ground truth.
        Scratch t;
        write_ground_truth(t);
        write_matching_run(t);
        const std::string other(HASH_X, 63);
        t.write("run/icat.tsv",
            std::string("15\t0\t") + HASH_HELLO + "\t\n16\t0\t" + other +
                "0\t\n");
        const std::string items = t.write("items", "");
        const Cli r = run_cli(args(t, "icat",
            {"--check", "icat-guest", "--partition", "sda6", "--items",
                items}));
        CPPUNIT_ASSERT_EQUAL(1, r.status);
        const std::string written = t.read("items");
        CPPUNIT_ASSERT_MESSAGE(written,
            written.find("fail\ticat-guest sda6: /x: sha256 differs") !=
                std::string::npos);
    }

    void testWrongSummaryValueFailsFsstat()
    {
        // The test hook: a deliberately wrong expected value.
        Scratch t;
        write_ground_truth(t);
        write_matching_run(t);
        const Cli r =
            run_cli(args(t, "fsstat", {"--override", "block-size=2048"}));
        CPPUNIT_ASSERT_EQUAL(1, r.status);
        CPPUNIT_ASSERT_MESSAGE(r.out,
            r.out.find("block-size differs: Ground truth 2048, run 1024") !=
                std::string::npos);
        CPPUNIT_ASSERT_MESSAGE(
            r.out, r.out.find("test hook") != std::string::npos);
        // Only known fields.
        const Cli bad =
            run_cli(args(t, "fsstat", {"--override", "colour=blue"}));
        CPPUNIT_ASSERT_EQUAL(2, bad.status);
    }

    void testNotRecognisedIsAKnownFailure()
    {
        Scratch t;
        write_ground_truth(t);
        t.dir("run");
        const ToolOutput unknown{1, "", "Cannot determine file system type\n"};
        t.tool("run/fls", unknown);
        t.tool("run/icat-root", unknown);
        t.write("run/icat.tsv", "");
        t.tool("run/fsstat", unknown);
        t.dir("run/istat");
        t.tool("run/istat/15", unknown);
        t.tool("run/istat/18", unknown);
        for (const char *tool : {"fls", "icat", "fsstat", "istat"}) {
            const std::string check = tool + std::string("-guest");
            const std::string kf = t.write(check + ".known",
                check + " sda4 * error:fs-open not recognised\n");
            Cli r = run_cli(args(t, tool,
                {"--known-failures", kf, "--check", check, "--partition",
                    "sda4"}));
            CPPUNIT_ASSERT_EQUAL_MESSAGE(r.out + r.err, 0, r.status);
            // Without the line, it fails.
            r = run_cli(args(t, tool));
            CPPUNIT_ASSERT_EQUAL_MESSAGE(r.out, 1, r.status);
        }
    }

    void testIstatOfAMissingInode()
    {
        // No istat files for the symlink's inode: its entry is missing.
        Scratch t;
        write_ground_truth(t);
        t.dir("run");
        t.dir("run/istat");
        t.tool("run/istat/15", {0, ISTAT_HELLO, ""});
        const Cli r = run_cli(args(t, "istat"));
        CPPUNIT_ASSERT_EQUAL(1, r.status);
        CPPUNIT_ASSERT_MESSAGE(r.out,
            r.out.find("/symlink: in Ground truth, missing from the run") !=
                std::string::npos);
    }

    void testUsageAndInputErrors()
    {
        Scratch t;
        write_ground_truth(t);
        write_matching_run(t);
        CPPUNIT_ASSERT_EQUAL(2, run_cli({}).status);
        CPPUNIT_ASSERT_EQUAL(2, run_cli(args(t, "ls")).status);
        CPPUNIT_ASSERT_EQUAL(2, run_cli({"fls", "--run-dir", t.path()}).status);
        CPPUNIT_ASSERT_EQUAL(
            2, run_cli(args(t, "fls", {"--known-failures", "x"})).status);
        CPPUNIT_ASSERT_EQUAL(0, run_cli({"--help"}).status);
        // A tool whose files are missing (its run never happened).
        Scratch empty;
        write_ground_truth(empty);
        empty.dir("run");
        const Cli r = run_cli(args(empty, "fls"));
        CPPUNIT_ASSERT_EQUAL(2, r.status);
        CPPUNIT_ASSERT_MESSAGE(
            r.err, r.err.find("fls.status") != std::string::npos);
    }
};
CPPUNIT_TEST_SUITE_REGISTRATION(CheckToolCliTest);
