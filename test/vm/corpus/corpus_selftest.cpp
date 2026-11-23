/*
 * Self-test of the Test corpus generator (corpus.h): it writes the corpus
 * into temporary directories and checks the result through the kernel
 * (stat, readlink, SEEK_DATA) and through Ground truth capture, not through
 * the generator's own code. Determinism is checked the way the acceptance
 * criterion states it: two runs on empty targets give the same Manifest,
 * apart from inode numbers, ctime and crtime (and atime, never compared).
 */
#include "corpus.h"

#include "capture.h"
#include "manifest.h"

#include <cppunit/extensions/HelperMacros.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace tskval;

namespace {

int
remove_one(const char *path, const struct stat *, int, struct FTW *)
{
    // Directories made unwritable or unreadable by the corpus (mode 0000)
    // must be removable.
    chmod(path, 0700);
    return remove(path);
}

// Small enough to be quick, large enough for every feature.
CorpusOptions
small_options()
{
    CorpusOptions o;
    o.large_size = 3 * 1024 * 1024 + 17;
    o.btree_entries = 1500;
    return o;
}

std::string
read_file(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

// The names in a directory, without "." and "..".
std::vector<std::string>
list_names(const std::string &dir)
{
    std::vector<std::string> names;
    DIR *d = opendir(dir.c_str());
    if (d == nullptr) {
        CPPUNIT_FAIL("opendir " + dir + ": " + std::strerror(errno));
        return names;
    }
    while (struct dirent *e = readdir(d))
        if (std::strcmp(e->d_name, ".") != 0 &&
            std::strcmp(e->d_name, "..") != 0)
            names.emplace_back(e->d_name);
    closedir(d);
    return names;
}

unsigned
count_entries(const std::string &dir)
{
    return static_cast<unsigned>(list_names(dir).size());
}

const RecordLine *
find_line(const std::vector<RecordLine> &r, const std::string &feature,
    RecordLine::Kind kind)
{
    for (const RecordLine &l : r)
        if (l.feature == feature && l.kind == kind)
            return &l;
    return nullptr;
}

// Every field the comparison step compares, except the ones the kernel
// chooses: inode, ctime and crtime.
std::string
reproducible_fields(const Entry &e)
{
    std::ostringstream s;
    s << static_cast<int>(e.type) << ' ' << e.mode << ' ' << e.uid << ' '
      << e.gid << ' ' << e.size << ' ' << e.nlink << ' '
      << (e.has_target ? encode_bytes(e.target) : "-") << ' '
      << (e.has_sha256 ? e.sha256 : "-") << ' ' << format_timestamp(e.mtime);
    return s.str();
}

} // namespace

class CorpusTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(CorpusTest);
    CPPUNIT_TEST(prng_is_seeded_and_pinned);
    CPPUNIT_TEST(two_runs_give_the_same_tree);
    CPPUNIT_TEST(another_seed_gives_other_content);
    CPPUNIT_TEST(directories_have_their_entry_counts);
    CPPUNIT_TEST(node_and_btree_directories_use_hard_links);
    CPPUNIT_TEST(files_and_large_file);
    CPPUNIT_TEST(sparse_file_has_three_holes);
    CPPUNIT_TEST(fragmented_file_has_2000_extents);
    CPPUNIT_TEST(reflinked_or_copied_content);
    CPPUNIT_TEST(links_and_symlinks);
    CPPUNIT_TEST(utf8_and_255_byte_names);
    CPPUNIT_TEST(timestamps_before_1970_and_after_2038);
    CPPUNIT_TEST(every_entry_has_seeded_nanosecond_times);
    CPPUNIT_TEST(owners_and_modes);
    CPPUNIT_TEST(record_has_each_feature);
    CPPUNIT_TEST(refuses_a_non_empty_target);
    CPPUNIT_TEST(cli_writes_the_record);
    CPPUNIT_TEST(cli_usage_errors);
    CPPUNIT_TEST_SUITE_END();

    std::string m_tmp;
    // One corpus shared by the read-only tests, generated once.
    static std::string s_dir;
    static std::vector<RecordLine> s_record;

    std::string fresh(const std::string &name) const
    {
        return m_tmp + "/" + name;
    }

    static std::string c(const std::string &rel)
    {
        return s_dir + "/" + rel;
    }

    static struct stat lst(const std::string &rel)
    {
        struct stat st;
        CPPUNIT_ASSERT_MESSAGE(rel, lstat(c(rel).c_str(), &st) == 0);
        return st;
    }

  public:
    void setUp() override
    {
        char tmpl[] = "/tmp/corpus-selftest-XXXXXX";
        CPPUNIT_ASSERT(mkdtemp(tmpl) != nullptr);
        m_tmp = tmpl;
        if (s_dir.empty()) {
            static char shared[] = "/tmp/corpus-selftest-shared-XXXXXX";
            CPPUNIT_ASSERT(mkdtemp(shared) != nullptr);
            s_dir = std::string(shared) + "/corpus";
            CPPUNIT_ASSERT_MESSAGE("generate the shared corpus",
                generate_corpus(s_dir, small_options(), s_record));
            std::atexit([] {
                nftw(s_dir.substr(0, s_dir.rfind('/')).c_str(), remove_one, 16,
                    FTW_DEPTH | FTW_PHYS);
            });
        }
    }

    void tearDown() override
    {
        nftw(m_tmp.c_str(), remove_one, 16, FTW_DEPTH | FTW_PHYS);
    }

    void prng_is_seeded_and_pinned()
    {
        Prng a(42), b(42), d(43);
        std::vector<uint64_t> va, vb, vd;
        for (int i = 0; i < 100; ++i) {
            va.push_back(a.next());
            vb.push_back(b.next());
            vd.push_back(d.next());
        }
        CPPUNIT_ASSERT(va == vb);
        CPPUNIT_ASSERT(va != vd);
        // xoshiro256** seeded from splitmix64(0): pinned, because changing
        // the stream changes every corpus.
        Prng z(0);
        CPPUNIT_ASSERT_EQUAL(
            static_cast<uint64_t>(0x99ec5f36cb75f2b4ULL), z.next());
        CPPUNIT_ASSERT(derive_seed(1, "a") != derive_seed(1, "b"));
        CPPUNIT_ASSERT(derive_seed(1, "a") != derive_seed(2, "a"));
        CPPUNIT_ASSERT_EQUAL(derive_seed(7, "x"), derive_seed(7, "x"));
        Prng r(5);
        for (int i = 0; i < 1000; ++i) {
            uint64_t v = r.range(3, 9);
            CPPUNIT_ASSERT(v >= 3 && v <= 9);
        }
        // fill() is next() in little-endian order, tail included.
        Prng f1(9), f2(9);
        unsigned char buf[11];
        f1.fill(buf, sizeof buf);
        uint64_t w0 = f2.next(), w1 = f2.next();
        for (int i = 0; i < 8; ++i)
            CPPUNIT_ASSERT_EQUAL(
                static_cast<unsigned char>(w0 >> (8 * i)), buf[i]);
        for (int i = 0; i < 3; ++i)
            CPPUNIT_ASSERT_EQUAL(
                static_cast<unsigned char>(w1 >> (8 * i)), buf[8 + i]);
    }

    void two_runs_give_the_same_tree()
    {
        std::vector<RecordLine> r1, r2;
        CPPUNIT_ASSERT(generate_corpus(fresh("one"), small_options(), r1));
        CPPUNIT_ASSERT(generate_corpus(fresh("two"), small_options(), r2));
        std::vector<Entry> e1, e2;
        std::vector<std::string> p1, p2;
        CPPUNIT_ASSERT(capture_tree(fresh("one"), e1, p1));
        CPPUNIT_ASSERT(capture_tree(fresh("two"), e2, p2));
        // As an ordinary user, the mode-0000 file can't be read (no SHA-256
        // on either side); root, as in the guest, reads everything.
        CPPUNIT_ASSERT_EQUAL(p1.size(), p2.size());
        for (const std::string &p : p1)
            CPPUNIT_ASSERT_MESSAGE(p,
                geteuid() != 0 &&
                    p.find("/owners/no-permissions-0000") != std::string::npos);
        CPPUNIT_ASSERT_EQUAL(e1.size(), e2.size());
        CPPUNIT_ASSERT(e1.size() > 1500 + 600 + 300);
        for (size_t i = 0; i < e1.size(); ++i) {
            CPPUNIT_ASSERT_EQUAL(
                encode_bytes(e1[i].path), encode_bytes(e2[i].path));
            CPPUNIT_ASSERT_EQUAL_MESSAGE(encode_bytes(e1[i].path),
                reproducible_fields(e1[i]), reproducible_fields(e2[i]));
        }
        CPPUNIT_ASSERT_EQUAL(r1.size(), r2.size());
        for (size_t i = 0; i < r1.size(); ++i)
            CPPUNIT_ASSERT_EQUAL(
                format_record_line(r1[i]), format_record_line(r2[i]));
    }

    void another_seed_gives_other_content()
    {
        CorpusOptions o = small_options();
        o.seed = 1;
        std::vector<RecordLine> r;
        CPPUNIT_ASSERT(generate_corpus(fresh("seed1"), o, r));
        CPPUNIT_ASSERT(read_file(fresh("seed1") + "/files/small.txt") !=
            read_file(c("files/small.txt")));
        CPPUNIT_ASSERT_EQUAL(read_file(c("links/target.txt")).size(),
            read_file(fresh("seed1") + "/links/target.txt").size());
    }

    void directories_have_their_entry_counts()
    {
        CPPUNIT_ASSERT_EQUAL(3u, count_entries(c("dirs/shortform")));
        CPPUNIT_ASSERT_EQUAL(40u, count_entries(c("dirs/block")));
        CPPUNIT_ASSERT_EQUAL(300u, count_entries(c("dirs/leaf")));
        CPPUNIT_ASSERT_EQUAL(600u, count_entries(c("dirs/node")));
        CPPUNIT_ASSERT_EQUAL(1500u, count_entries(c("dirs/btree")));
        CPPUNIT_ASSERT_EQUAL(50u, count_entries(c("dirs/link-targets")));
    }

    void node_and_btree_directories_use_hard_links()
    {
        // Every entry is a link to one of the 50 targets, and their link
        // counts add up to 50 + the entries.
        std::set<ino_t> inodes;
        for (const char *dir : {"dirs/node/", "dirs/btree/"}) {
            for (const std::string &name : list_names(c(dir))) {
                struct stat st = lst(dir + name);
                CPPUNIT_ASSERT(S_ISREG(st.st_mode));
                inodes.insert(st.st_ino);
            }
        }
        nlink_t links = 0;
        for (int i = 0; i < 50; ++i) {
            char name[64];
            snprintf(name, sizeof name, "dirs/link-targets/target-%02d", i);
            struct stat st = lst(name);
            links += st.st_nlink;
            inodes.erase(st.st_ino);
            CPPUNIT_ASSERT_EQUAL(static_cast<off_t>(i + 1), st.st_size);
        }
        // Each target's own name, the node directory's 600 and the B+tree
        // directory's 1500 entries.
        CPPUNIT_ASSERT_EQUAL(static_cast<nlink_t>(50 + 600 + 1500), links);
        CPPUNIT_ASSERT(inodes.empty()); // no other inode behind them
    }

    void files_and_large_file()
    {
        CPPUNIT_ASSERT_EQUAL(static_cast<off_t>(0), lst("files/empty").st_size);
        CPPUNIT_ASSERT_EQUAL(static_cast<off_t>(3 * 1024 * 1024 + 17),
            lst("files/large.bin").st_size);
        CPPUNIT_ASSERT_EQUAL(
            static_cast<off_t>(14459), lst("files/multiblock.bin").st_size);
        CPPUNIT_ASSERT_EQUAL(
            static_cast<off_t>(1 << 20), lst("files/preallocated.bin").st_size);
        CPPUNIT_ASSERT(read_file(c("files/preallocated.bin")) ==
            std::string(1 << 20, '\0'));
        // Seeded content, not zeros or a repeated pattern.
        std::string large = read_file(c("files/large.bin"));
        CPPUNIT_ASSERT(large.substr(0, 4096) != large.substr(4096, 4096));
        CPPUNIT_ASSERT(large.find(std::string(64, '\0')) == std::string::npos);
    }

    void sparse_file_has_three_holes()
    {
        const off_t u = 64 * 1024;
        std::string s = read_file(c("sparse/holes.bin"));
        CPPUNIT_ASSERT_EQUAL(static_cast<size_t>(6 * u), s.size());
        CPPUNIT_ASSERT(s.substr(0, u) == std::string(u, '\0'));
        CPPUNIT_ASSERT(s.substr(2 * u, 2 * u) == std::string(2 * u, '\0'));
        CPPUNIT_ASSERT(s.substr(5 * u) == std::string(u, '\0'));
        CPPUNIT_ASSERT(s.substr(u, 64) != std::string(64, '\0'));
        if (find_line(s_record, "sparse file", RecordLine::Made) == nullptr)
            return; // this file system doesn't punch holes; recorded
        int fd = open(c("sparse/holes.bin").c_str(), O_RDONLY);
        CPPUNIT_ASSERT(fd >= 0);
        CPPUNIT_ASSERT_EQUAL(u, lseek(fd, 0, SEEK_DATA));
        CPPUNIT_ASSERT_EQUAL(2 * u, lseek(fd, u, SEEK_HOLE));
        CPPUNIT_ASSERT_EQUAL(4 * u, lseek(fd, 2 * u, SEEK_DATA));
        CPPUNIT_ASSERT_EQUAL(5 * u, lseek(fd, 4 * u, SEEK_HOLE));
        CPPUNIT_ASSERT_EQUAL(
            static_cast<off_t>(-1), lseek(fd, 5 * u, SEEK_DATA));
        close(fd);
    }

    void fragmented_file_has_2000_extents()
    {
        CPPUNIT_ASSERT_EQUAL(static_cast<off_t>(3999 * 4096),
            lst("fragmented/extents.bin").st_size);
        if (find_line(s_record, "extent B+tree file", RecordLine::Made) ==
            nullptr)
            return;
        int fd = open(c("fragmented/extents.bin").c_str(), O_RDONLY);
        CPPUNIT_ASSERT(fd >= 0);
        unsigned runs = 0;
        off_t pos = 0, d;
        while ((d = lseek(fd, pos, SEEK_DATA)) >= 0) {
            CPPUNIT_ASSERT_EQUAL(static_cast<off_t>(2 * runs * 4096), d);
            pos = lseek(fd, d, SEEK_HOLE);
            CPPUNIT_ASSERT_EQUAL(d + 4096, pos);
            ++runs;
        }
        close(fd);
        CPPUNIT_ASSERT_EQUAL(2000u, runs);
    }

    void reflinked_or_copied_content()
    {
        std::string src = read_file(c("reflink/source.bin"));
        std::string clone = read_file(c("reflink/clone.bin"));
        std::string partial = read_file(c("reflink/partial.bin"));
        CPPUNIT_ASSERT_EQUAL(static_cast<size_t>(1 << 20), src.size());
        CPPUNIT_ASSERT(src == clone);
        CPPUNIT_ASSERT_EQUAL(src.size(), partial.size());
        const size_t at = 256 * 1024;
        CPPUNIT_ASSERT(src.substr(0, at) == partial.substr(0, at));
        CPPUNIT_ASSERT(src.substr(at + 4096) == partial.substr(at + 4096));
        CPPUNIT_ASSERT(src.substr(at, 4096) != partial.substr(at, 4096));
        // Made or not, it is in the record.
        CPPUNIT_ASSERT(
            find_line(s_record, "reflinks", RecordLine::Made) != nullptr ||
            find_line(s_record, "reflinks", RecordLine::NotMade) != nullptr);
    }

    void links_and_symlinks()
    {
        struct stat t = lst("links/target.txt");
        CPPUNIT_ASSERT_EQUAL(static_cast<nlink_t>(3), t.st_nlink);
        CPPUNIT_ASSERT_EQUAL(t.st_ino, lst("links/hard-1").st_ino);
        CPPUNIT_ASSERT_EQUAL(t.st_ino, lst("links/sub/hard-2").st_ino);
        const std::map<std::string, size_t> lens = {{"short-symlink", 10},
            {"long-symlink-300", 300}, {"max-symlink-1023", 1023},
            {"dangling-symlink", 14}, {"absolute-symlink", 13}};
        for (const auto &l : lens) {
            char buf[2048];
            ssize_t n =
                readlink(c("links/" + l.first).c_str(), buf, sizeof buf);
            CPPUNIT_ASSERT_EQUAL_MESSAGE(
                l.first, static_cast<ssize_t>(l.second), n);
        }
    }

    void utf8_and_255_byte_names()
    {
        CPPUNIT_ASSERT(S_ISREG(lst("names/caf\xc3\xa9.txt").st_mode));
        CPPUNIT_ASSERT(S_ISREG(lst("names/new\nline").st_mode));
        unsigned long255 = 0, dirs255 = 0;
        for (const std::string &name : list_names(c("names")))
            if (name.size() == 255) {
                ++long255;
                if (S_ISDIR(lst("names/" + name).st_mode))
                    ++dirs255;
            }
        CPPUNIT_ASSERT_EQUAL(3u, long255);
        CPPUNIT_ASSERT_EQUAL(1u, dirs255);
    }

    void timestamps_before_1970_and_after_2038()
    {
        // tmpfs and ext4 (the test's likely file systems) store these.
        struct stat st = lst("times/before-1970/int32-min");
        CPPUNIT_ASSERT_EQUAL(
            static_cast<time_t>(-2147483648LL), st.st_mtim.tv_sec);
        st = lst("times/before-1970/epoch-minus-1ns");
        CPPUNIT_ASSERT_EQUAL(static_cast<time_t>(-1), st.st_mtim.tv_sec);
        CPPUNIT_ASSERT_EQUAL(999999999L, st.st_mtim.tv_nsec);
        st = lst("times/after-2038/2100");
        CPPUNIT_ASSERT_EQUAL(
            static_cast<time_t>(4102444800LL), st.st_mtim.tv_sec);
        CPPUNIT_ASSERT_EQUAL(123456789L, st.st_mtim.tv_nsec);
        CPPUNIT_ASSERT(
            find_line(s_record, "timestamps before 1970", RecordLine::Made));
        CPPUNIT_ASSERT(
            find_line(s_record, "timestamps after 2038", RecordLine::Made));
    }

    void every_entry_has_seeded_nanosecond_times()
    {
        std::vector<Entry> e;
        std::vector<std::string> problems;
        CPPUNIT_ASSERT(capture_tree(s_dir, e, problems));
        unsigned with_ns = 0;
        for (const Entry &x : e) {
            // Within the seeded range (2001 to 2033), or under /times.
            bool times = x.path.compare(0, 7, "/times/") == 0 &&
                x.type == EntryType::File;
            if (!times) {
                CPPUNIT_ASSERT_MESSAGE(x.path,
                    x.mtime.sec >= 1000000000 && x.mtime.sec <= 2000000000);
            }
            if (x.mtime.nsec != 0)
                ++with_ns;
        }
        CPPUNIT_ASSERT(with_ns > e.size() * 9 / 10);
    }

    void owners_and_modes()
    {
        CPPUNIT_ASSERT_EQUAL(static_cast<mode_t>(04755),
            lst("owners/setuid-4755").st_mode & 07777);
        CPPUNIT_ASSERT_EQUAL(static_cast<mode_t>(02711),
            lst("owners/setgid-2711").st_mode & 07777);
        CPPUNIT_ASSERT_EQUAL(static_cast<mode_t>(01777),
            lst("owners/sticky-dir-1777").st_mode & 07777);
        CPPUNIT_ASSERT_EQUAL(static_cast<mode_t>(0),
            lst("owners/no-permissions-0000").st_mode & 07777);
        if (geteuid() == 0) {
            CPPUNIT_ASSERT_EQUAL(
                static_cast<uid_t>(1000), lst("owners/user-0640").st_uid);
            CPPUNIT_ASSERT(
                find_line(s_record, "owners and modes", RecordLine::Made));
        }
        else {
            CPPUNIT_ASSERT(
                find_line(s_record, "owners and modes", RecordLine::NotMade));
        }
    }

    void record_has_each_feature()
    {
        // Each feature has one key: at most one made line (what was made)
        // and at most one not-made line (what wasn't, and why), at least
        // one of the two. No other key appears.
        const std::set<std::string> features = {"directories",
            "B+tree directory", "empty file", "large file",
            "preallocated extents", "sparse file", "extent B+tree file",
            "reflinks", "hard links", "symlinks", "UTF-8 names", "other names",
            "255-byte names", "timestamps before 1970",
            "timestamps at the epoch and the end of 32-bit time",
            "timestamps after 2038", "special files", "owners and modes",
            "timestamps"};
        for (const std::string &f : features) {
            unsigned made = 0, not_made = 0;
            for (const RecordLine &l : s_record)
                if (l.feature == f)
                    ++(l.kind == RecordLine::Made ? made : not_made);
            CPPUNIT_ASSERT_MESSAGE(
                f, made <= 1 && not_made <= 1 && made + not_made >= 1);
        }
        for (const RecordLine &l : s_record) {
            CPPUNIT_ASSERT(l.kind != RecordLine::Failed);
            CPPUNIT_ASSERT_MESSAGE(l.feature, features.count(l.feature) == 1);
            CPPUNIT_ASSERT_MESSAGE(l.feature, !l.detail.empty());
        }
        // Device nodes need CAP_MKNOD: recorded with the reason otherwise.
        if (geteuid() != 0) {
            const RecordLine *l =
                find_line(s_record, "special files", RecordLine::NotMade);
            CPPUNIT_ASSERT(
                l != nullptr && l->detail.find("mknod") != std::string::npos);
        }
        CPPUNIT_ASSERT_EQUAL(std::string("not-made\treflinks\tFICLONE: x"),
            format_record_line(
                {RecordLine::NotMade, "reflinks", "FICLONE: x"}));
    }

    void refuses_a_non_empty_target()
    {
        const std::string d = fresh("full");
        CPPUNIT_ASSERT_EQUAL(0, mkdir(d.c_str(), 0755));
        std::ofstream(d + "/x") << "x";
        std::vector<RecordLine> r;
        CPPUNIT_ASSERT(!generate_corpus(d, small_options(), r));
        CPPUNIT_ASSERT_EQUAL(static_cast<size_t>(1), r.size());
        CPPUNIT_ASSERT_EQUAL(RecordLine::Failed, r[0].kind);
        CPPUNIT_ASSERT(r[0].detail.find("not empty") != std::string::npos);
        CPPUNIT_ASSERT_EQUAL(1u, count_entries(d)); // nothing written

        r.clear();
        CPPUNIT_ASSERT(
            !generate_corpus(fresh("no/parent"), small_options(), r));
        CPPUNIT_ASSERT_EQUAL(RecordLine::Failed, r.back().kind);
    }

    void cli_writes_the_record()
    {
        std::ostringstream out, err;
        int rc = corpus_cli({"--seed", "0x75b0c0de", "--large-size", "1K",
                                "--btree-entries", "100", fresh("cli")},
            out, err);
        CPPUNIT_ASSERT_EQUAL_MESSAGE(
            err.str(), static_cast<int>(CORPUS_OK), rc);
        CPPUNIT_ASSERT_EQUAL(static_cast<off_t>(1024), [&] {
            struct stat st;
            stat((fresh("cli") + "/files/large.bin").c_str(), &st);
            return st.st_size;
        }());
        const std::string o = out.str();
        CPPUNIT_ASSERT(o.find("# tsk-corpus seed=1974517982 large-size=1024 "
                              "btree-entries=100\n") == 0);
        CPPUNIT_ASSERT(
            o.find("\nmade\tB+tree directory\t/dirs/btree: 100 entries") !=
            std::string::npos);

        std::ostringstream out2, err2;
        rc = corpus_cli({"--large-size", "1M", fresh("cli")}, out2, err2);
        CPPUNIT_ASSERT_EQUAL(static_cast<int>(CORPUS_USAGE), rc);
        CPPUNIT_ASSERT(err2.str().find("not empty") != std::string::npos);
        CPPUNIT_ASSERT(
            out2.str().find("failed\ttarget\t") != std::string::npos);
    }

    void cli_usage_errors()
    {
        const std::vector<std::vector<std::string>> bad = {
            {},
            {fresh("u")}, // no --large-size
            {"--large-size", "1X", fresh("u")},
            {"--large-size", "-1", fresh("u")},
            {"--large-size"},
            {"--btree-entries", "0", "--large-size", "1", fresh("u")},
            {"--seed", "abc", "--large-size", "1", fresh("u")},
            {"--bogus", "--large-size", "1", fresh("u")},
            {"--large-size", "1", fresh("u"), fresh("v")},
        };
        for (const auto &args : bad) {
            std::ostringstream out, err;
            CPPUNIT_ASSERT_EQUAL(
                static_cast<int>(CORPUS_USAGE), corpus_cli(args, out, err));
            CPPUNIT_ASSERT(!err.str().empty());
        }
        CPPUNIT_ASSERT(access(fresh("u").c_str(), F_OK) != 0);
        std::ostringstream out, err;
        CPPUNIT_ASSERT_EQUAL(
            static_cast<int>(CORPUS_OK), corpus_cli({"--help"}, out, err));
        CPPUNIT_ASSERT(out.str().find("usage: tsk-corpus") == 0);
    }
};

std::string CorpusTest::s_dir;
std::vector<RecordLine> CorpusTest::s_record;

CPPUNIT_TEST_SUITE_REGISTRATION(CorpusTest);
