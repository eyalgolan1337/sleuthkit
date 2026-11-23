/*
 * Self-test of Ground truth capture (capture.h) on a directory tree built in
 * a temporary directory: symlinks, hard links, empty files, odd names, set
 * timestamps. Expected values come from what the test created, not from the
 * capture code.
 */
#include "capture.h"

#include "manifest.h"

#include <cppunit/extensions/HelperMacros.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace tskval;

namespace {

int
remove_one(const char *path, const struct stat *, int, struct FTW *)
{
    return remove(path);
}

// "abc" and the empty string, as `printf ... | sha256sum` prints them.
const char SHA_ABC[] =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
const char SHA_EMPTY[] =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

} // namespace

class CaptureTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(CaptureTest);
    CPPUNIT_TEST(root_is_a_directory_entry);
    CPPUNIT_TEST(regular_file);
    CPPUNIT_TEST(empty_file);
    CPPUNIT_TEST(nested_directories);
    CPPUNIT_TEST(hard_links_share_inode_and_link_count);
    CPPUNIT_TEST(symlinks_are_recorded_not_followed);
    CPPUNIT_TEST(odd_names_are_kept_byte_for_byte);
    CPPUNIT_TEST(nanosecond_and_pre_1970_timestamps);
    CPPUNIT_TEST(fifo);
    CPPUNIT_TEST(permission_bits);
    CPPUNIT_TEST(crtime_when_the_kernel_reports_it);
    CPPUNIT_TEST(walk_order_is_sorted);
    CPPUNIT_TEST(unreadable_file_is_a_problem);
    CPPUNIT_TEST(missing_root_fails);
    CPPUNIT_TEST(round_trips_through_the_reader);
    CPPUNIT_TEST(cli_writes_a_manifest);
    CPPUNIT_TEST(cli_reports_incomplete_capture);
    CPPUNIT_TEST(cli_usage_errors);
    CPPUNIT_TEST_SUITE_END();

    std::string m_dir;

    std::string p(const std::string &rel) const
    {
        return m_dir + rel;
    }

    void write_file(const std::string &rel, const std::string &content)
    {
        std::ofstream f(p(rel), std::ios::binary);
        f << content;
        CPPUNIT_ASSERT_MESSAGE("write " + rel, f.good());
    }

    void make_dir(const std::string &rel)
    {
        CPPUNIT_ASSERT_EQUAL_MESSAGE(
            "mkdir " + rel, 0, mkdir(p(rel).c_str(), 0755));
    }

    void set_mtime(const std::string &rel, int64_t sec, long nsec)
    {
        struct timespec ts[2];
        ts[0].tv_sec = 1000000000; // atime
        ts[0].tv_nsec = 5;
        ts[1].tv_sec = sec;
        ts[1].tv_nsec = nsec;
        CPPUNIT_ASSERT_EQUAL_MESSAGE("utimensat " + rel, 0,
            utimensat(AT_FDCWD, p(rel).c_str(), ts, AT_SYMLINK_NOFOLLOW));
    }

    struct stat lst(const std::string &rel) const
    {
        struct stat st;
        CPPUNIT_ASSERT_EQUAL(0, lstat(p(rel).c_str(), &st));
        return st;
    }

    std::map<std::string, Entry> capture() const
    {
        std::vector<Entry> entries;
        std::vector<std::string> problems;
        CPPUNIT_ASSERT(capture_tree(m_dir, entries, problems));
        std::string all;
        for (const std::string &s : problems) {
            all += s + "\n";
        }
        CPPUNIT_ASSERT_MESSAGE(
            "unexpected problems:\n" + all, problems.empty());
        std::map<std::string, Entry> byPath;
        for (const Entry &e : entries) {
            CPPUNIT_ASSERT_MESSAGE(
                "duplicate " + e.path, byPath.emplace(e.path, e).second);
        }
        return byPath;
    }

    // By value: GCC's -Wdangling-reference can't tell that the returned
    // reference doesn't point into the temporary `path`.
    static Entry at(
        const std::map<std::string, Entry> &m, const std::string &path)
    {
        auto it = m.find(path);
        if (it == m.end()) {
            CPPUNIT_FAIL("no entry for '" + encode_bytes(path) + "'");
        }
        return it->second;
    }

  public:
    void setUp() override
    {
        char tmpl[] = "/tmp/capture-selftest-XXXXXX";
        CPPUNIT_ASSERT(mkdtemp(tmpl) != nullptr);
        m_dir = tmpl;
    }

    void tearDown() override
    {
        // Undo chmod 000 so the tree can be removed.
        chmod(p("/locked").c_str(), 0644);
        nftw(m_dir.c_str(), remove_one, 16, FTW_DEPTH | FTW_PHYS);
    }

    void root_is_a_directory_entry()
    {
        auto m = capture();
        CPPUNIT_ASSERT_EQUAL(size_t(1), m.size());
        const Entry root = at(m, "/");
        CPPUNIT_ASSERT(root.type == EntryType::Dir);
        struct stat st = lst("");
        CPPUNIT_ASSERT_EQUAL(uint64_t(st.st_ino), root.inode);
        CPPUNIT_ASSERT(!root.has_sha256);
        CPPUNIT_ASSERT(!root.has_target);
    }

    void regular_file()
    {
        write_file("/f", "abc");
        auto m = capture();
        const Entry e = at(m, "/f");
        struct stat st = lst("/f");
        CPPUNIT_ASSERT(e.type == EntryType::File);
        CPPUNIT_ASSERT_EQUAL(uint64_t(st.st_ino), e.inode);
        CPPUNIT_ASSERT_EQUAL(uint64_t(3), e.size);
        CPPUNIT_ASSERT_EQUAL(uint64_t(1), e.nlink);
        CPPUNIT_ASSERT_EQUAL(uint32_t(getuid()), e.uid);
        CPPUNIT_ASSERT_EQUAL(uint32_t(st.st_gid), e.gid);
        CPPUNIT_ASSERT(e.has_sha256);
        CPPUNIT_ASSERT_EQUAL(std::string(SHA_ABC), e.sha256);
        CPPUNIT_ASSERT(!e.has_target);
        CPPUNIT_ASSERT(e.has_atime);
    }

    void empty_file()
    {
        write_file("/empty", "");
        auto m = capture();
        const Entry e = at(m, "/empty");
        CPPUNIT_ASSERT_EQUAL(uint64_t(0), e.size);
        CPPUNIT_ASSERT(e.has_sha256);
        CPPUNIT_ASSERT_EQUAL(std::string(SHA_EMPTY), e.sha256);
    }

    void nested_directories()
    {
        make_dir("/a");
        make_dir("/a/b");
        write_file("/a/b/c", "abc");
        auto m = capture();
        CPPUNIT_ASSERT_EQUAL(size_t(4), m.size());
        CPPUNIT_ASSERT(at(m, "/a").type == EntryType::Dir);
        CPPUNIT_ASSERT(at(m, "/a/b").type == EntryType::Dir);
        CPPUNIT_ASSERT_EQUAL(uint64_t(3), at(m, "/a").nlink); // ., .., b
        CPPUNIT_ASSERT_EQUAL(std::string(SHA_ABC), at(m, "/a/b/c").sha256);
    }

    void hard_links_share_inode_and_link_count()
    {
        write_file("/one", "abc");
        make_dir("/d");
        CPPUNIT_ASSERT_EQUAL(0, link(p("/one").c_str(), p("/d/two").c_str()));
        auto m = capture();
        const Entry a = at(m, "/one");
        const Entry b = at(m, "/d/two");
        CPPUNIT_ASSERT_EQUAL(a.inode, b.inode);
        CPPUNIT_ASSERT_EQUAL(uint64_t(2), a.nlink);
        CPPUNIT_ASSERT_EQUAL(uint64_t(2), b.nlink);
        CPPUNIT_ASSERT_EQUAL(std::string(SHA_ABC), b.sha256);
    }

    void symlinks_are_recorded_not_followed()
    {
        make_dir("/d");
        write_file("/d/f", "abc");
        const std::string longTarget(300, 'x');
        const std::string oddTarget = std::string("a b\n\xe9%-");
        CPPUNIT_ASSERT_EQUAL(0, symlink("d", p("/to-dir").c_str()));
        CPPUNIT_ASSERT_EQUAL(
            0, symlink(longTarget.c_str(), p("/long").c_str()));
        CPPUNIT_ASSERT_EQUAL(0, symlink(oddTarget.c_str(), p("/odd").c_str()));
        auto m = capture();
        const Entry dirLink = at(m, "/to-dir");
        CPPUNIT_ASSERT(dirLink.type == EntryType::Symlink);
        CPPUNIT_ASSERT(dirLink.has_target);
        CPPUNIT_ASSERT_EQUAL(std::string("d"), dirLink.target);
        CPPUNIT_ASSERT_EQUAL(uint64_t(1), dirLink.size);
        CPPUNIT_ASSERT(!dirLink.has_sha256);
        // Not followed: nothing recorded below the link.
        CPPUNIT_ASSERT(m.find("/to-dir/f") == m.end());
        CPPUNIT_ASSERT_EQUAL(longTarget, at(m, "/long").target);
        CPPUNIT_ASSERT_EQUAL(uint64_t(300), at(m, "/long").size);
        CPPUNIT_ASSERT_EQUAL(oddTarget, at(m, "/odd").target);
    }

    void odd_names_are_kept_byte_for_byte()
    {
        const std::vector<std::string> names = {"with space", "tab\there",
            "new\nline", "latin1-\xe9", "utf8-\xc3\xa9", "-", "%", "%41",
            std::string(255, 'n')};
        for (const std::string &n : names) {
            write_file("/" + n, "abc");
        }
        auto m = capture();
        CPPUNIT_ASSERT_EQUAL(names.size() + 1, m.size());
        for (const std::string &n : names) {
            CPPUNIT_ASSERT(at(m, "/" + n).type == EntryType::File);
        }
    }

    void nanosecond_and_pre_1970_timestamps()
    {
        write_file("/new", "");
        write_file("/old", "");
        set_mtime("/new", 1700000000, 123456789);
        set_mtime("/old", -2, 500000000); // 1969-12-31T23:59:58.5Z
        auto m = capture();
        Timestamp newT;
        newT.sec = 1700000000;
        newT.nsec = 123456789;
        Timestamp oldT;
        oldT.sec = -2;
        oldT.nsec = 500000000;
        Timestamp atimeT;
        atimeT.sec = 1000000000;
        atimeT.nsec = 5;
        CPPUNIT_ASSERT_EQUAL(
            format_timestamp(newT), format_timestamp(at(m, "/new").mtime));
        CPPUNIT_ASSERT_EQUAL(
            format_timestamp(oldT), format_timestamp(at(m, "/old").mtime));
        CPPUNIT_ASSERT_EQUAL(
            format_timestamp(atimeT), format_timestamp(at(m, "/new").atime));
        // ctime is set by the kernel: it must match stat's.
        struct stat st = lst("/new");
        Timestamp ctimeT;
        ctimeT.sec = st.st_ctim.tv_sec;
        ctimeT.nsec = static_cast<uint32_t>(st.st_ctim.tv_nsec);
        CPPUNIT_ASSERT_EQUAL(
            format_timestamp(ctimeT), format_timestamp(at(m, "/new").ctime));
    }

    void fifo()
    {
        CPPUNIT_ASSERT_EQUAL(0, mkfifo(p("/pipe").c_str(), 0600));
        auto m = capture();
        const Entry e = at(m, "/pipe");
        CPPUNIT_ASSERT(e.type == EntryType::Fifo);
        CPPUNIT_ASSERT(!e.has_sha256);
        CPPUNIT_ASSERT_EQUAL(uint32_t(0600), e.mode);
    }

    void permission_bits()
    {
        write_file("/x", "");
        make_dir("/sticky");
        CPPUNIT_ASSERT_EQUAL(0, chmod(p("/x").c_str(), 04751));
        CPPUNIT_ASSERT_EQUAL(0, chmod(p("/sticky").c_str(), 01777));
        auto m = capture();
        CPPUNIT_ASSERT_EQUAL(uint32_t(04751), at(m, "/x").mode);
        CPPUNIT_ASSERT_EQUAL(uint32_t(01777), at(m, "/sticky").mode);
    }

    // Whether a birth time exists depends on the file system under /tmp, so
    // the expectation comes from statx itself.
    void crtime_when_the_kernel_reports_it()
    {
        write_file("/f", "");
        struct statx stx;
        CPPUNIT_ASSERT_EQUAL(0,
            statx(AT_FDCWD, p("/f").c_str(), AT_SYMLINK_NOFOLLOW, STATX_BTIME,
                &stx));
        auto m = capture();
        const Entry e = at(m, "/f");
        const bool kernelHasBtime = (stx.stx_mask & STATX_BTIME) != 0;
        CPPUNIT_ASSERT_EQUAL(kernelHasBtime, e.has_crtime);
        if (kernelHasBtime) {
            CPPUNIT_ASSERT_EQUAL(int64_t(stx.stx_btime.tv_sec), e.crtime.sec);
            CPPUNIT_ASSERT_EQUAL(stx.stx_btime.tv_nsec, e.crtime.nsec);
        }
    }

    void walk_order_is_sorted()
    {
        make_dir("/b");
        write_file("/b/z", "");
        write_file("/b/a", "");
        write_file("/a", "");
        write_file("/C", "");
        std::vector<Entry> entries;
        std::vector<std::string> problems;
        CPPUNIT_ASSERT(capture_tree(m_dir, entries, problems));
        std::vector<std::string> paths;
        paths.reserve(entries.size());
        for (const Entry &e : entries) {
            paths.push_back(e.path);
        }
        const std::vector<std::string> expected = {
            "/", "/C", "/a", "/b", "/b/a", "/b/z"};
        CPPUNIT_ASSERT(expected == paths);
    }

    void unreadable_file_is_a_problem()
    {
        if (geteuid() == 0) {
            return; // root reads it anyway
        }
        write_file("/locked", "abc");
        CPPUNIT_ASSERT_EQUAL(0, chmod(p("/locked").c_str(), 0));
        std::vector<Entry> entries;
        std::vector<std::string> problems;
        CPPUNIT_ASSERT(capture_tree(m_dir, entries, problems));
        CPPUNIT_ASSERT_EQUAL(size_t(1), problems.size());
        CPPUNIT_ASSERT(problems[0].find("/locked") != std::string::npos);
        CPPUNIT_ASSERT_EQUAL(size_t(2), entries.size());
        const Entry &e = entries[1];
        CPPUNIT_ASSERT_EQUAL(std::string("/locked"), e.path);
        CPPUNIT_ASSERT(e.type == EntryType::File);
        CPPUNIT_ASSERT_EQUAL(uint64_t(3), e.size);
        CPPUNIT_ASSERT(!e.has_sha256);
    }

    void missing_root_fails()
    {
        std::vector<Entry> entries;
        std::vector<std::string> problems;
        CPPUNIT_ASSERT(!capture_tree(p("/nonexistent"), entries, problems));
        CPPUNIT_ASSERT_EQUAL(size_t(1), problems.size());
        CPPUNIT_ASSERT(entries.empty());
    }

    // A tree with every kind of entry above, written and read back.
    void round_trips_through_the_reader()
    {
        make_dir("/d");
        write_file("/d/f", "abc");
        write_file("/empty", "");
        CPPUNIT_ASSERT_EQUAL(0, link(p("/d/f").c_str(), p("/hard").c_str()));
        CPPUNIT_ASSERT_EQUAL(0, symlink("d/f", p("/sym").c_str()));
        CPPUNIT_ASSERT_EQUAL(0, symlink("-", p("/dash-target").c_str()));
        CPPUNIT_ASSERT_EQUAL(0, mkfifo(p("/pipe").c_str(), 0644));
        write_file("/new\nline and space\t\xff", "x");
        write_file("/" + std::string(255, 'L'), "");
        set_mtime("/empty", -1500000000, 1);

        std::vector<Entry> entries;
        std::vector<std::string> problems;
        CPPUNIT_ASSERT(capture_tree(m_dir, entries, problems));
        CPPUNIT_ASSERT(problems.empty());
        Manifest written;
        written.entries = entries;
        std::ostringstream out;
        write_manifest(out, written);

        std::istringstream in(out.str());
        Manifest back;
        std::string err;
        CPPUNIT_ASSERT_MESSAGE(err, read_manifest(in, back, err));
        CPPUNIT_ASSERT(!back.has_error);
        CPPUNIT_ASSERT_EQUAL(entries.size(), back.entries.size());
        for (size_t i = 0; i < entries.size(); ++i) {
            CPPUNIT_ASSERT_EQUAL(
                format_entry(entries[i]), format_entry(back.entries[i]));
            CPPUNIT_ASSERT_EQUAL(entries[i].path, back.entries[i].path);
            CPPUNIT_ASSERT_EQUAL(entries[i].target, back.entries[i].target);
        }
    }

    void cli_writes_a_manifest()
    {
        write_file("/f", "abc");
        const std::string outFile = p("-out.manifest"); // beside, not inside
        std::ostringstream out, err;
        int rc = capture_cli({"-o", outFile, m_dir}, out, err);
        std::ifstream in(outFile, std::ios::binary);
        Manifest m;
        std::string readErr;
        const bool ok = read_manifest(in, m, readErr);
        remove(outFile.c_str());
        CPPUNIT_ASSERT_EQUAL_MESSAGE(err.str(), int(CAPTURE_OK), rc);
        CPPUNIT_ASSERT_MESSAGE(readErr, ok);
        CPPUNIT_ASSERT_EQUAL(size_t(2), m.entries.size());
        CPPUNIT_ASSERT(out.str().empty());

        // Without -o, the Manifest goes to standard output.
        std::ostringstream out2, err2;
        CPPUNIT_ASSERT_EQUAL(int(CAPTURE_OK), capture_cli({m_dir}, out2, err2));
        CPPUNIT_ASSERT_EQUAL(
            0, out2.str().compare(0, 17, "#tsk-manifest v1\n"));
        CPPUNIT_ASSERT(out2.str().find("\n/f\tfile\t") != std::string::npos);
    }

    void cli_reports_incomplete_capture()
    {
        if (geteuid() == 0) {
            return;
        }
        write_file("/locked", "abc");
        CPPUNIT_ASSERT_EQUAL(0, chmod(p("/locked").c_str(), 0));
        std::ostringstream out, err;
        CPPUNIT_ASSERT_EQUAL(
            int(CAPTURE_INCOMPLETE), capture_cli({m_dir}, out, err));
        CPPUNIT_ASSERT(err.str().find("/locked") != std::string::npos);
        CPPUNIT_ASSERT(
            out.str().find("\n/locked\tfile\t") != std::string::npos);
    }

    void cli_usage_errors()
    {
        std::ostringstream out, err;
        CPPUNIT_ASSERT_EQUAL(int(CAPTURE_ERROR), capture_cli({}, out, err));
        CPPUNIT_ASSERT_EQUAL(
            int(CAPTURE_ERROR), capture_cli({m_dir, m_dir}, out, err));
        CPPUNIT_ASSERT_EQUAL(int(CAPTURE_ERROR), capture_cli({"-o"}, out, err));
        CPPUNIT_ASSERT_EQUAL(
            int(CAPTURE_ERROR), capture_cli({"--bogus", m_dir}, out, err));
        CPPUNIT_ASSERT_EQUAL(
            int(CAPTURE_ERROR), capture_cli({p("/nonexistent")}, out, err));
        CPPUNIT_ASSERT_EQUAL(int(CAPTURE_ERROR),
            capture_cli({"-o", p("/no/such/dir/out"), m_dir}, out, err));
        CPPUNIT_ASSERT(out.str().empty());
        std::ostringstream helpOut, helpErr;
        CPPUNIT_ASSERT_EQUAL(
            int(CAPTURE_OK), capture_cli({"-h"}, helpOut, helpErr));
        CPPUNIT_ASSERT(helpOut.str().find("Usage:") != std::string::npos);
    }
};
CPPUNIT_TEST_SUITE_REGISTRATION(CaptureTest);
