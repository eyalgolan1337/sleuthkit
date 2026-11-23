/*
 * Self-test of the harness (harness.h): its command line, the mapping from
 * libtsk metadata to Manifest entries, and the !error Manifests written when
 * nothing can be read, and the walk over a small ext4 image made with
 * mkfs.ext4 -d (needs e2fsprogs and fdisk, as in the devcontainer). The full
 * comparison with Ground truth is test/vm/harness/selftest.sh.
 */
#include "harness.h"

#include "manifest.h"

#include <cppunit/CompilerOutputter.h>
#include <cppunit/extensions/HelperMacros.h>
#include <cppunit/extensions/TestFactoryRegistry.h>
#include <cppunit/ui/text/TestRunner.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace tskval;

namespace {

TSK_FS_META
blank_meta()
{
    TSK_FS_META m;
    std::memset(&m, 0, sizeof(m));
    return m;
}

Manifest
parse(const std::string &text)
{
    std::istringstream in(text);
    Manifest m;
    std::string err;
    if (!read_manifest(in, m, err)) {
        CPPUNIT_FAIL("not a Manifest: " + err + "\n" + text);
    }
    return m;
}

} // namespace

class HarnessArgsTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(HarnessArgsTest);
    CPPUNIT_TEST(image_only);
    CPPUNIT_TEST(offset_and_output);
    CPPUNIT_TEST(partition);
    CPPUNIT_TEST(split_image_segments);
    CPPUNIT_TEST(help);
    CPPUNIT_TEST(bad_usage);
    CPPUNIT_TEST_SUITE_END();

    static bool fails(const std::vector<std::string> &args)
    {
        HarnessOptions o;
        bool help = false;
        std::string error;
        bool ok = parse_harness_args(args, o, help, error);
        if (!ok) {
            CPPUNIT_ASSERT_MESSAGE("error message expected", !error.empty());
        }
        return !ok;
    }

  public:
    void image_only()
    {
        HarnessOptions o;
        bool help = true;
        std::string error;
        CPPUNIT_ASSERT(parse_harness_args({"disk.img"}, o, help, error));
        CPPUNIT_ASSERT(!help);
        CPPUNIT_ASSERT(o.images == std::vector<std::string>{"disk.img"});
        CPPUNIT_ASSERT(!o.has_offset);
        CPPUNIT_ASSERT_EQUAL(0u, o.partition);
        CPPUNIT_ASSERT(o.output.empty());
    }

    void offset_and_output()
    {
        HarnessOptions o;
        bool help = false;
        std::string error;
        CPPUNIT_ASSERT(parse_harness_args(
            {"--offset", "1048576", "-o", "out.manifest", "d.vmdk"}, o, help,
            error));
        CPPUNIT_ASSERT(o.has_offset);
        CPPUNIT_ASSERT_EQUAL(uint64_t(1048576), o.offset);
        CPPUNIT_ASSERT_EQUAL(std::string("out.manifest"), o.output);
        CPPUNIT_ASSERT(o.images == std::vector<std::string>{"d.vmdk"});
    }

    void partition()
    {
        HarnessOptions o;
        bool help = false;
        std::string error;
        CPPUNIT_ASSERT(
            parse_harness_args({"d.img", "--partition", "4"}, o, help, error));
        CPPUNIT_ASSERT_EQUAL(4u, o.partition);
        CPPUNIT_ASSERT(!o.has_offset);
    }

    void split_image_segments()
    {
        HarnessOptions o;
        bool help = false;
        std::string error;
        CPPUNIT_ASSERT(parse_harness_args({"d.001", "d.002"}, o, help, error));
        CPPUNIT_ASSERT_EQUAL(size_t(2), o.images.size());
    }

    void help()
    {
        HarnessOptions o;
        bool help = false;
        std::string error;
        CPPUNIT_ASSERT(parse_harness_args({"--help"}, o, help, error));
        CPPUNIT_ASSERT(help);
    }

    void bad_usage()
    {
        CPPUNIT_ASSERT(fails({}));                           // no image
        CPPUNIT_ASSERT(fails({"--offset"}));                 // no value
        CPPUNIT_ASSERT(fails({"--offset", "12k", "d.img"})); // not a number
        CPPUNIT_ASSERT(fails({"--offset", "-1", "d.img"}));  // negative
        CPPUNIT_ASSERT(fails({"--offset", "99999999999999999999", "d.img"}));
        CPPUNIT_ASSERT(fails({"--partition", "0", "d.img"})); // 1-based
        CPPUNIT_ASSERT(fails({"--partition", "x", "d.img"}));
        CPPUNIT_ASSERT(fails({"--partition", "1", "--offset", "0", "d.img"}));
        CPPUNIT_ASSERT(fails({"--bogus", "d.img"}));
        CPPUNIT_ASSERT(fails({"-o"}));
    }
};
CPPUNIT_TEST_SUITE_REGISTRATION(HarnessArgsTest);

class EntryFromMetaTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(EntryFromMetaTest);
    CPPUNIT_TEST(regular_file_fields);
    CPPUNIT_TEST(types);
    CPPUNIT_TEST(crtime_zero_means_absent);
    CPPUNIT_TEST(symlink_target);
    CPPUNIT_TEST_SUITE_END();

  public:
    void regular_file_fields()
    {
        TSK_FS_META m = blank_meta();
        m.type = TSK_FS_META_TYPE_REG;
        m.addr = 12;
        // libtsk stores OR-ed permission bits in this enum.
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        m.mode = static_cast<TSK_FS_META_MODE_ENUM>(04751);
        m.nlink = 2;
        m.size = 3;
        m.uid = 1000;
        m.gid = 100;
        m.mtime = -2;
        m.mtime_nano = 500000000;
        m.ctime = 1700000000;
        m.ctime_nano = 123456789;
        m.atime = 5;
        m.atime_nano = 6;
        m.crtime = 1600000000;
        m.crtime_nano = 1;
        Entry e = entry_from_meta("/a/b", m);
        // The Manifest line, as documented in test/vm/README.md.
        CPPUNIT_ASSERT_EQUAL(
            std::string(
                "/a/b\tfile\t12\t4751\t1000\t100\t3\t2\t-\t-\t"
                "-1500000000\t1700000000123456789\t1600000000000000001\t"
                "5000000006"),
            format_entry(e));
    }

    void types()
    {
        const struct {
            TSK_FS_META_TYPE_ENUM tsk;
            const char *manifest;
        } cases[] = {
            {TSK_FS_META_TYPE_REG, "file"},
            {TSK_FS_META_TYPE_DIR, "dir"},
            {TSK_FS_META_TYPE_LNK, "symlink"},
            {TSK_FS_META_TYPE_CHR, "chr"},
            {TSK_FS_META_TYPE_BLK, "blk"},
            {TSK_FS_META_TYPE_FIFO, "fifo"},
            {TSK_FS_META_TYPE_SOCK, "sock"},
            {TSK_FS_META_TYPE_UNDEF, "other"},
            {TSK_FS_META_TYPE_WHT, "other"},
            {TSK_FS_META_TYPE_SHAD, "other"},
        };
        for (const auto &c : cases) {
            TSK_FS_META m = blank_meta();
            m.type = c.tsk;
            std::string line = format_entry(entry_from_meta("/x", m));
            CPPUNIT_ASSERT_EQUAL(std::string("/x\t") + c.manifest + "\t",
                line.substr(0, 4 + std::strlen(c.manifest)));
        }
    }

    void crtime_zero_means_absent()
    {
        TSK_FS_META m = blank_meta();
        m.type = TSK_FS_META_TYPE_REG;
        Entry e = entry_from_meta("/x", m);
        CPPUNIT_ASSERT(!e.has_crtime);
        CPPUNIT_ASSERT(e.has_atime);
        m.crtime_nano = 1; // 1 ns after the epoch is a real crtime
        CPPUNIT_ASSERT(entry_from_meta("/x", m).has_crtime);
    }

    void symlink_target()
    {
        TSK_FS_META m = blank_meta();
        m.type = TSK_FS_META_TYPE_LNK;
        m.size = 9;
        char target[] = "../t a\xe9";
        m.link = target;
        Entry e = entry_from_meta("/l", m);
        CPPUNIT_ASSERT(e.has_target);
        CPPUNIT_ASSERT_EQUAL(std::string("../t a\xe9"), e.target);
        CPPUNIT_ASSERT(!e.has_sha256);
        m.link = nullptr;
        CPPUNIT_ASSERT(!entry_from_meta("/l", m).has_target);
    }
};
CPPUNIT_TEST_SUITE_REGISTRATION(EntryFromMetaTest);

// Images that hold no file system: every run writes an !error Manifest.
class HarnessErrorTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(HarnessErrorTest);
    CPPUNIT_TEST(missing_image);
    CPPUNIT_TEST(no_file_system);
    CPPUNIT_TEST(no_volume_system);
    CPPUNIT_TEST(offset_past_the_end);
    CPPUNIT_TEST(cli_writes_the_output_file);
    CPPUNIT_TEST_SUITE_END();

    std::string m_dir;
    std::string m_zeros; // 1 MiB of zeros

    Manifest run(const HarnessOptions &o, int expectedExit)
    {
        std::ostringstream out, err;
        int rc = run_harness(o, out, err);
        CPPUNIT_ASSERT_EQUAL_MESSAGE(err.str(), expectedExit, rc);
        return parse(out.str());
    }

  public:
    void setUp() override
    {
        char tmpl[] = "/tmp/harness-selftest-XXXXXX";
        CPPUNIT_ASSERT(mkdtemp(tmpl) != nullptr);
        m_dir = tmpl;
        m_zeros = m_dir + "/zeros.img";
        std::ofstream f(m_zeros, std::ios::binary);
        f << std::string(1 << 20, '\0');
    }

    void tearDown() override
    {
        std::remove(m_zeros.c_str());
        std::remove((m_dir + "/out").c_str());
        rmdir(m_dir.c_str());
    }

    void missing_image()
    {
        HarnessOptions o;
        o.images = {m_dir + "/nonexistent.img"};
        Manifest m = run(o, HARNESS_INCOMPLETE);
        CPPUNIT_ASSERT(m.has_error);
        CPPUNIT_ASSERT_EQUAL(std::string("img-open"), m.error_code);
        CPPUNIT_ASSERT(!m.error_detail.empty());
        CPPUNIT_ASSERT(m.entries.empty());
    }

    void no_file_system()
    {
        HarnessOptions o;
        o.images = {m_zeros};
        Manifest m = run(o, HARNESS_INCOMPLETE);
        CPPUNIT_ASSERT_EQUAL(std::string("fs-open"), m.error_code);
    }

    void no_volume_system()
    {
        HarnessOptions o;
        o.images = {m_zeros};
        o.partition = 1;
        Manifest m = run(o, HARNESS_INCOMPLETE);
        CPPUNIT_ASSERT_EQUAL(std::string("vs-open"), m.error_code);
    }

    void offset_past_the_end()
    {
        HarnessOptions o;
        o.images = {m_zeros};
        o.has_offset = true;
        o.offset = 2 << 20;
        Manifest m = run(o, HARNESS_INCOMPLETE);
        CPPUNIT_ASSERT_EQUAL(std::string("fs-open"), m.error_code);
    }

    void cli_writes_the_output_file()
    {
        std::ostringstream out, err;
        const std::string file = m_dir + "/out";
        CPPUNIT_ASSERT_EQUAL(int(HARNESS_INCOMPLETE),
            harness_cli({"-o", file, m_zeros}, out, err));
        CPPUNIT_ASSERT(out.str().empty());
        std::ifstream in(file, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        CPPUNIT_ASSERT_EQUAL(
            std::string("fs-open"), parse(text.str()).error_code);

        std::ostringstream out2, err2;
        CPPUNIT_ASSERT_EQUAL(int(HARNESS_ERROR),
            harness_cli({"-o", m_dir + "/no/such/dir", m_zeros}, out2, err2));
        CPPUNIT_ASSERT_EQUAL(int(HARNESS_ERROR), harness_cli({}, out2, err2));
        std::ostringstream helpOut, helpErr;
        CPPUNIT_ASSERT_EQUAL(
            int(HARNESS_OK), harness_cli({"-h"}, helpOut, helpErr));
        CPPUNIT_ASSERT(helpOut.str().find("Usage:") != std::string::npos);
    }
};
CPPUNIT_TEST_SUITE_REGISTRATION(HarnessErrorTest);

namespace {

int
remove_one(const char *path, const struct stat *, int, struct FTW *)
{
    return std::remove(path);
}

void
sh(const std::string &cmd)
{
    const std::string full = "PATH=\"$PATH:/usr/sbin:/sbin\"; " + cmd;
    // The fixture runs mkfs.ext4, sfdisk and dd through the shell, on paths
    // it made itself.
    // NOLINTNEXTLINE(bugprone-command-processor)
    const int rc = std::system(full.c_str());
    CPPUNIT_ASSERT_EQUAL_MESSAGE("command failed: " + cmd, 0, rc);
}

} // namespace

// The walk over a real file system: a small ext4 image, bare and inside a
// GPT disk image.
class HarnessWalkTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(HarnessWalkTest);
    CPPUNIT_TEST(records_every_allocated_entry_in_order);
    CPPUNIT_TEST(content_links_and_types);
    CPPUNIT_TEST(partition_found_through_the_volume_system);
    CPPUNIT_TEST(partition_by_offset);
    CPPUNIT_TEST(missing_partition);
    CPPUNIT_TEST_SUITE_END();

    std::string m_dir;
    std::string m_fs;   // bare ext4 image
    std::string m_disk; // GPT disk, the ext4 image at 1 MiB

    std::string run_text(const HarnessOptions &o, int expectedExit)
    {
        std::ostringstream out, err;
        CPPUNIT_ASSERT_EQUAL_MESSAGE(
            err.str(), expectedExit, run_harness(o, out, err));
        return out.str();
    }

    std::string fs_manifest()
    {
        HarnessOptions o;
        o.images = {m_fs};
        return run_text(o, HARNESS_OK);
    }

  public:
    void setUp() override
    {
        char tmpl[] = "/tmp/harness-walk-XXXXXX";
        CPPUNIT_ASSERT(mkdtemp(tmpl) != nullptr);
        m_dir = tmpl;
        m_fs = m_dir + "/fs.img";
        m_disk = m_dir + "/disk.img";
        const std::string t = m_dir + "/tree";
        sh("mkdir -p '" + t + "/d' && printf abc >'" + t + "/d/f' && : >'" + t +
            "/empty' && ln '" + t + "/d/f' '" + t + "/d/h' && ln -s d/f '" + t +
            "/link' && mkfifo '" + t + "/fifo'");
        sh("mkfs.ext4 -q -F -I 256 -d '" + t + "' '" + m_fs + "' 4M");
        sh("truncate -s 6M '" + m_disk +
            "' && printf 'label: gpt\\nstart=2048, "
            "size=8192\\n' | sfdisk -q '" +
            m_disk + "' >/dev/null && dd if='" + m_fs + "' of='" + m_disk +
            "' bs=1M seek=1 conv=notrunc status=none");
    }

    void tearDown() override
    {
        nftw(m_dir.c_str(), remove_one, 16, FTW_DEPTH | FTW_PHYS);
    }

    // Only allocated names, no ".", ".." or $OrphanFiles, each directory's
    // names sorted bytewise, a directory before its contents.
    void records_every_allocated_entry_in_order()
    {
        Manifest m = parse(fs_manifest());
        std::vector<std::string> paths;
        paths.reserve(m.entries.size());
        for (const Entry &e : m.entries) {
            paths.push_back(e.path);
        }
        const std::vector<std::string> expected = {"/", "/d", "/d/f", "/d/h",
            "/empty", "/fifo", "/link", "/lost+found"};
        CPPUNIT_ASSERT(expected == paths);
    }

    void content_links_and_types()
    {
        Manifest m = parse(fs_manifest());
        auto find = [&m](const std::string &path) {
            for (const Entry &e : m.entries) {
                if (e.path == path) {
                    return e;
                }
            }
            CPPUNIT_FAIL("no entry " + path);
            return Entry();
        };
        const Entry root = find("/");
        CPPUNIT_ASSERT(root.type == EntryType::Dir);
        CPPUNIT_ASSERT_EQUAL(uint64_t(2), root.inode); // ext4's root inode
        const Entry f = find("/d/f");
        const Entry h = find("/d/h");
        CPPUNIT_ASSERT(f.type == EntryType::File);
        CPPUNIT_ASSERT_EQUAL(std::string("ba7816bf8f01cfea414140de5dae2223"
                                         "b00361a396177a9cb410ff61f20015ad"),
            f.sha256);
        CPPUNIT_ASSERT_EQUAL(uint64_t(3), f.size);
        CPPUNIT_ASSERT_EQUAL(f.inode, h.inode);
        CPPUNIT_ASSERT_EQUAL(uint64_t(2), f.nlink);
        CPPUNIT_ASSERT_EQUAL(f.sha256, h.sha256);
        const Entry empty = find("/empty");
        CPPUNIT_ASSERT_EQUAL(std::string("e3b0c44298fc1c149afbf4c8996fb924"
                                         "27ae41e4649b934ca495991b7852b855"),
            empty.sha256);
        const Entry link = find("/link");
        CPPUNIT_ASSERT(link.type == EntryType::Symlink);
        CPPUNIT_ASSERT(link.has_target);
        CPPUNIT_ASSERT_EQUAL(std::string("d/f"), link.target);
        CPPUNIT_ASSERT(!link.has_sha256);
        const Entry fifo = find("/fifo");
        CPPUNIT_ASSERT(fifo.type == EntryType::Fifo);
        CPPUNIT_ASSERT(!fifo.has_sha256);
        CPPUNIT_ASSERT(find("/d").type == EntryType::Dir);
    }

    void partition_found_through_the_volume_system()
    {
        HarnessOptions o;
        o.images = {m_disk};
        o.partition = 1;
        CPPUNIT_ASSERT_EQUAL(fs_manifest(), run_text(o, HARNESS_OK));
    }

    void partition_by_offset()
    {
        HarnessOptions o;
        o.images = {m_disk};
        o.has_offset = true;
        o.offset = 1 << 20;
        CPPUNIT_ASSERT_EQUAL(fs_manifest(), run_text(o, HARNESS_OK));
    }

    void missing_partition()
    {
        HarnessOptions o;
        o.images = {m_disk};
        o.partition = 2;
        Manifest m = parse(run_text(o, HARNESS_INCOMPLETE));
        CPPUNIT_ASSERT(m.has_error);
        CPPUNIT_ASSERT_EQUAL(std::string("partition"), m.error_code);
        CPPUNIT_ASSERT(m.entries.empty());
    }
};
CPPUNIT_TEST_SUITE_REGISTRATION(HarnessWalkTest);

int
main()
{
    CppUnit::TextUi::TestRunner runner;
    runner.addTest(CppUnit::TestFactoryRegistry::getRegistry().makeTest());
    runner.setOutputter(
        new CppUnit::CompilerOutputter(&runner.result(), std::cerr));
    return runner.run() ? 0 : 1;
}
