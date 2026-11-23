/*
 * The Test corpus generator. See corpus.h and test/vm/README.md ("The Test
 * corpus").
 */
#include "corpus.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef FICLONE
#define FICLONE _IOW(0x94, 9, int)
#endif
#ifndef FALLOC_FL_KEEP_SIZE
#define FALLOC_FL_KEEP_SIZE 0x01
#endif
#ifndef FALLOC_FL_PUNCH_HOLE
#define FALLOC_FL_PUNCH_HOLE 0x02
#endif

namespace tskval {

// ---------------------------------------------------------------------------
// The seeded stream.

namespace {

uint64_t
splitmix64(uint64_t &x)
{
    uint64_t z = (x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

uint64_t
rotl(uint64_t x, int k)
{
    return (x << k) | (x >> (64 - k));
}

} // namespace

Prng::Prng(uint64_t seed)
{
    for (uint64_t &s : m_s)
        s = splitmix64(seed);
}

uint64_t
Prng::next()
{
    const uint64_t result = rotl(m_s[1] * 5, 7) * 9;
    const uint64_t t = m_s[1] << 17;
    m_s[2] ^= m_s[0];
    m_s[3] ^= m_s[1];
    m_s[1] ^= m_s[2];
    m_s[0] ^= m_s[3];
    m_s[2] ^= t;
    m_s[3] = rotl(m_s[3], 45);
    return result;
}

uint64_t
Prng::range(uint64_t lo, uint64_t hi)
{
    // The modulo bias is irrelevant here; only determinism matters.
    const uint64_t span = hi - lo + 1;
    return span == 0 ? next() : lo + next() % span;
}

void
Prng::fill(unsigned char *buf, size_t len)
{
    size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t v = next();
        for (int b = 0; b < 8; ++b)
            buf[i + b] = static_cast<unsigned char>(v >> (8 * b));
    }
    if (i < len) {
        uint64_t v = next();
        for (; i < len; ++i, v >>= 8)
            buf[i] = static_cast<unsigned char>(v);
    }
}

uint64_t
derive_seed(uint64_t seed, const std::string &key)
{
    // FNV-1a over the key, mixed with the seed.
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : key) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    uint64_t x = seed ^ h;
    return splitmix64(x);
}

std::string
format_record_line(const RecordLine &line)
{
    const char *kind = line.kind == RecordLine::Made ? "made"
        : line.kind == RecordLine::NotMade           ? "not-made"
                                                     : "failed";
    return std::string(kind) + "\t" + line.feature + "\t" + line.detail;
}

// ---------------------------------------------------------------------------
// The generator.

namespace {

// An error that stops the generator.
struct GenError : std::runtime_error {
    explicit GenError(const std::string &what) : std::runtime_error(what) {}
};

std::string
errno_text(int err)
{
    return std::strerror(err);
}

// "2038-01-19T03:14:08.5Z": a timestamp in UTC, with the nanoseconds only
// if there are any (trailing zeros dropped).
std::string
iso_time(const struct timespec &ts)
{
    time_t t = ts.tv_sec;
    struct tm tm;
    char buf[64];
    if (gmtime_r(&t, &tm) == nullptr ||
        strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm) == 0)
        return std::to_string(static_cast<long long>(ts.tv_sec)) + "s";
    std::string s = buf;
    if (ts.tv_nsec != 0) {
        char ns[16];
        snprintf(ns, sizeof ns, ".%09ld", static_cast<long>(ts.tv_nsec));
        std::string frac = ns;
        while (frac.back() == '0')
            frac.pop_back();
        s += frac;
    }
    return s + "Z";
}

bool
same_time(const struct timespec &a, const struct timespec &b)
{
    return a.tv_sec == b.tv_sec && a.tv_nsec == b.tv_nsec;
}

// Sizes of the directories, in entries. With 4 KiB directory blocks (both
// XFS partitions of the Validation VM, and the XFS default) and these name
// lengths, each has the XFS format in its name: short-form (in the inode),
// block (one directory block), leaf (several data blocks, one leaf block),
// node (more entries than one leaf block holds). The B+tree directory's
// size comes from the options.
//
// A directory whose entries are new files gets its blocks interleaved with
// the files' inode chunks and data blocks, so its extents multiply. The node
// directory must keep few enough to stay a list in the inode (12 on the
// Validation VM, where the SELinux label's attribute fork leaves 192 bytes),
// so its entries are hard links, which allocate nothing but the directory's
// own blocks. So are the B+tree directory's, so that ~50k entries don't use
// ~50k inodes (an ext4 file system of 1 GiB has 65536).
struct DirSpec {
    const char *name;
    unsigned entries;
    bool links; // hard links to the files of /dirs/link-targets
};
const DirSpec DIR_SPECS[] = {
    {"shortform", 3, false},
    {"block", 40, false},
    {"leaf", 300, false},
    {"node", 600, true},
};
const unsigned LINK_TARGETS = 50;

// The extent B+tree file: this many 4 KiB data extents, each followed by a
// 4 KiB hole (the last one isn't), so each extent is its own record however
// the allocator places them. Far more than an inode's data fork holds as a
// list (21 with 512-byte XFS inodes, 53 with 1 KiB ones).
const unsigned FRAGMENT_EXTENTS = 2000;
const off_t UNIT = 4096;

const char LONG_SYMLINK_PREFIX[] = "long-symlink-target/";

// The timestamps of /times: mtime and atime of each file.
struct TimeCase {
    const char *name;
    int64_t sec;
    long nsec;
};
const TimeCase BEFORE_1970[] = {
    {"int32-min", -2147483648LL, 0},    // 1901-12-13T20:45:52Z
    {"1910", -1893456000LL, 500000000}, // 1910-01-01
    {"1960", -315619200LL, 123456789},  // 1960-01-01
    {"epoch-minus-1ns", -1, 999999999}, // 1969-12-31T23:59:59.999999999Z
};
const TimeCase AROUND_EPOCH[] = {
    {"epoch", 0, 0},
    {"int32-max", 2147483647LL, 0}, // 2038-01-19T03:14:07Z
};
const TimeCase AFTER_2038[] = {
    {"int32-max-plus-1", 2147483648LL, 0}, // 2038-01-19T03:14:08Z
    {"2100", 4102444800LL, 123456789},     // 2100-01-01
    {"2262", 9223372037LL, 1},             // past 64-bit nanoseconds
    {"ext4-max", 15032385535LL, 0},        // 2446-05-10T22:38:55Z
    {"xfs-bigtime-max", 16299260424LL, 0}, // 2486-07-02T20:20:24Z
};

class Generator {
  public:
    Generator(const std::string &root, const CorpusOptions &opt,
        std::vector<RecordLine> &record)
        : m_root(root), m_opt(opt), m_record(record)
    {}

    void run();

  private:
    std::string m_root;
    const CorpusOptions &m_opt;
    std::vector<RecordLine> &m_record;
    int m_rootfd = -1;

    // Each created object's times, applied at the end, deepest first, so a
    // directory keeps its own mtime.
    struct Times {
        std::string rel;
        struct timespec atime, mtime;
    };
    std::vector<Times> m_times;

    void made(const std::string &feature, const std::string &detail)
    {
        m_record.push_back({RecordLine::Made, feature, detail});
    }
    void not_made(const std::string &feature, const std::string &reason)
    {
        m_record.push_back({RecordLine::NotMade, feature, reason});
    }
    [[noreturn]] void fail(const std::string &rel, const char *op, int err)
    {
        throw GenError("/" + rel + ": " + op + ": " + errno_text(err));
    }

    // Seeded times for an object: a date between 2001 and 2033 with
    // nanoseconds, atime a little later.
    void default_times(const std::string &rel)
    {
        Prng p(derive_seed(m_opt.seed, "times:" + rel));
        struct timespec m, a;
        m.tv_sec = static_cast<time_t>(p.range(1000000000, 2000000000));
        m.tv_nsec = static_cast<long>(p.range(0, 999999999));
        a.tv_sec = m.tv_sec + static_cast<time_t>(p.range(0, 86400 * 365));
        a.tv_nsec = static_cast<long>(p.range(0, 999999999));
        m_times.push_back({rel, a, m});
    }

    void make_dir(const std::string &rel, mode_t mode = 0755)
    {
        if (mkdirat(m_rootfd, rel.c_str(), mode) != 0)
            fail(rel, "mkdir", errno);
        if (fchmodat(m_rootfd, rel.c_str(), mode, 0) != 0)
            fail(rel, "chmod", errno);
        default_times(rel);
    }

    int create(const std::string &rel, mode_t mode = 0644)
    {
        int fd = openat(m_rootfd, rel.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
        if (fd < 0)
            fail(rel, "create", errno);
        default_times(rel);
        return fd;
    }

    void write_all(int fd, const std::string &rel, const unsigned char *buf,
        size_t len, off_t off)
    {
        while (len > 0) {
            ssize_t n = pwrite(fd, buf, len, off);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                fail(rel, "write", errno);
            }
            buf += n;
            len -= static_cast<size_t>(n);
            off += n;
        }
    }

    // Writes `size` bytes of the path's seeded content at offset 0.
    void write_content(int fd, const std::string &rel, uint64_t size)
    {
        Prng p(derive_seed(m_opt.seed, "content:" + rel));
        const size_t chunk = 1 << 20;
        std::unique_ptr<unsigned char[]> buf(new unsigned char[chunk]);
        off_t off = 0;
        while (static_cast<uint64_t>(off) < size) {
            size_t n = static_cast<size_t>(
                std::min<uint64_t>(chunk, size - static_cast<uint64_t>(off)));
            p.fill(buf.get(), n);
            write_all(fd, rel, buf.get(), n, off);
            off += static_cast<off_t>(n);
        }
    }

    void close_fd(int fd, const std::string &rel)
    {
        if (close(fd) != 0)
            fail(rel, "close", errno);
    }

    void file(const std::string &rel, uint64_t size, mode_t mode = 0644)
    {
        int fd = create(rel, mode);
        write_content(fd, rel, size);
        close_fd(fd, rel);
    }

    void hard_link(const std::string &target, const std::string &rel)
    {
        if (linkat(m_rootfd, target.c_str(), m_rootfd, rel.c_str(), 0) != 0)
            fail(rel, "link", errno);
    }

    // A name of `len` bytes after `prefix`, from [a-z0-9_-].
    static std::string seeded_name(
        Prng &p, const std::string &prefix, size_t len)
    {
        static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789_-";
        std::string s = prefix;
        for (size_t i = 0; i < len; ++i)
            s += chars[p.range(0, sizeof chars - 2)];
        return s;
    }

    // The data ranges of a file, from SEEK_DATA and SEEK_HOLE.
    std::vector<std::pair<off_t, off_t>> data_ranges(
        int fd, const std::string &rel)
    {
        std::vector<std::pair<off_t, off_t>> r;
        struct stat st;
        if (fstat(fd, &st) != 0)
            fail(rel, "stat", errno);
        off_t pos = 0;
        while (pos < st.st_size) {
            off_t d = lseek(fd, pos, SEEK_DATA);
            if (d < 0) {
                if (errno == ENXIO)
                    break;
                fail(rel, "lseek SEEK_DATA", errno);
            }
            off_t h = lseek(fd, d, SEEK_HOLE);
            if (h < 0)
                fail(rel, "lseek SEEK_HOLE", errno);
            r.push_back({d, h});
            pos = h;
        }
        return r;
    }

    // Writes the file's seeded content in full, then punches the holes
    // (ranges of [offset, length)). Returns an empty string if the file
    // system made each a hole, or why not.
    std::string sparse_file(const std::string &rel, uint64_t size,
        const std::vector<std::pair<off_t, off_t>> &holes)
    {
        int fd = create(rel);
        write_content(fd, rel, size);
        // The holes read as zeros, made or not.
        std::vector<unsigned char> zero(1 << 16, 0);
        for (const auto &h : holes)
            for (off_t o = 0; o < h.second;) {
                size_t n = static_cast<size_t>(std::min<off_t>(
                    h.second - o, static_cast<off_t>(zero.size())));
                write_all(fd, rel, zero.data(), n, h.first + o);
                o += static_cast<off_t>(n);
            }
        if (fsync(fd) != 0)
            fail(rel, "fsync", errno);
        std::string why;
        for (const auto &h : holes)
            if (fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                    h.first, h.second) != 0) {
                why = "fallocate(PUNCH_HOLE): " + errno_text(errno);
                break;
            }
        if (why.empty() && fsync(fd) != 0)
            fail(rel, "fsync", errno);
        if (why.empty()) {
            // Each hole must be a hole: no data range overlaps it.
            auto data = data_ranges(fd, rel);
            for (const auto &h : holes)
                for (const auto &d : data)
                    if (d.first < h.first + h.second && h.first < d.second)
                        why = "the file system kept data in a punched hole";
        }
        close_fd(fd, rel);
        return why;
    }

    void dirs();
    void files();
    void sparse();
    void fragmented();
    void reflinks();
    void links();
    void names();
    void times();
    void special();
    void owners();
    void apply_times();
};

void
Generator::dirs()
{
    make_dir("dirs");
    make_dir("dirs/link-targets");
    std::vector<std::string> targets;
    for (unsigned i = 0; i < LINK_TARGETS; ++i) {
        char name[32];
        snprintf(name, sizeof name, "dirs/link-targets/target-%02u", i);
        targets.push_back(name);
        file(name, 1 + i);
    }

    std::ostringstream summary;
    for (const DirSpec &d : DIR_SPECS) {
        const std::string dir = std::string("dirs/") + d.name;
        make_dir(dir);
        Prng p(derive_seed(m_opt.seed, "names:" + dir));
        for (unsigned i = 0; i < d.entries; ++i) {
            // Short names in the short-form directory, so it stays in the
            // inode; 4 to 20 random bytes after an index elsewhere.
            char idx[16];
            snprintf(idx, sizeof idx, "%04u-", i);
            const std::string name = d.entries <= 3
                ? seeded_name(p, std::string(1, static_cast<char>('a' + i)), 2)
                : seeded_name(p, idx, p.range(4, 20));
            std::string path = dir;
            path += '/';
            path += name;
            if (d.links)
                hard_link(targets[p.range(0, LINK_TARGETS - 1)], path);
            else
                file(path, p.range(0, 100));
        }
        summary << (summary.tellp() > 0 ? ", " : "") << d.name << " "
                << d.entries << (d.links ? " (hard links)" : " (files)");
    }
    made("directories", "/dirs: " + summary.str() + " entries");

    make_dir("dirs/btree");
    Prng p(derive_seed(m_opt.seed, "names:dirs/btree"));
    for (unsigned i = 0; i < m_opt.btree_entries; ++i) {
        char idx[16];
        snprintf(idx, sizeof idx, "%05u-", i);
        hard_link(targets[p.range(0, LINK_TARGETS - 1)],
            "dirs/btree/" + seeded_name(p, idx, p.range(8, 40)));
    }
    made("B+tree directory",
        "/dirs/btree: " + std::to_string(m_opt.btree_entries) +
            " entries, hard links to the " + std::to_string(LINK_TARGETS) +
            " files in /dirs/link-targets");
}

void
Generator::files()
{
    make_dir("files");
    file("files/empty", 0);
    file("files/small.txt", 13);
    file("files/multiblock.bin", 3 * 4096 + 2048 + 123);
    file("files/large.bin", m_opt.large_size);
    made("empty file", "/files/empty");
    made("large file",
        "/files/large.bin: " + std::to_string(m_opt.large_size) + " bytes");

    // Preallocated, never written: unwritten extents, read as zeros.
    const std::string rel = "files/preallocated.bin";
    int fd = create(rel);
    if (fallocate(fd, 0, 0, 1 << 20) == 0) {
        made("preallocated extents", "/" + rel + ": 1 MiB, never written");
    }
    else {
        const std::string why = "fallocate: " + errno_text(errno);
        if (ftruncate(fd, 1 << 20) != 0)
            fail(rel, "ftruncate", errno);
        not_made(
            "preallocated extents", why + "; /" + rel + " is a hole instead");
    }
    close_fd(fd, rel);
}

void
Generator::sparse()
{
    make_dir("sparse");
    // 64 KiB units: hole, data, hole, hole, data, hole.
    const off_t u = 64 * 1024;
    std::string why = sparse_file(
        "sparse/holes.bin", 6 * u, {{0, u}, {2 * u, 2 * u}, {5 * u, u}});
    if (why.empty())
        made("sparse file",
            "/sparse/holes.bin: holes at the start, in the "
            "middle and at the end");
    else
        not_made("sparse file", "/sparse/holes.bin: " + why);

    const std::string rel = "sparse/only-hole.bin";
    int fd = create(rel);
    if (ftruncate(fd, 1 << 20) != 0)
        fail(rel, "ftruncate", errno);
    close_fd(fd, rel);
}

void
Generator::fragmented()
{
    make_dir("fragmented");
    const std::string rel = "fragmented/extents.bin";
    std::vector<std::pair<off_t, off_t>> holes;
    for (unsigned i = 0; i + 1 < FRAGMENT_EXTENTS; ++i)
        holes.push_back({(2 * i + 1) * UNIT, UNIT});
    std::string why =
        sparse_file(rel, (2 * FRAGMENT_EXTENTS - 1) * UNIT, holes);
    if (why.empty())
        made("extent B+tree file",
            "/" + rel + ": " + std::to_string(FRAGMENT_EXTENTS) +
                " data extents of 4 KiB between holes");
    else
        not_made("extent B+tree file", "/" + rel + ": " + why);
}

void
Generator::reflinks()
{
    make_dir("reflink");
    const std::string src = "reflink/source.bin";
    const uint64_t size = 1 << 20;
    file(src, size);
    int in = openat(m_rootfd, src.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0)
        fail(src, "open", errno);

    std::string why;
    for (const char *name : {"reflink/clone.bin", "reflink/partial.bin"}) {
        const std::string rel = name;
        int out = create(rel);
        if (why.empty() && ioctl(out, FICLONE, in) != 0)
            why = "FICLONE: " + errno_text(errno);
        if (!why.empty()) {
            // A plain copy, so the content is the same either way.
            std::unique_ptr<unsigned char[]> buf(new unsigned char[size]);
            ssize_t n = pread(in, buf.get(), size, 0);
            if (n != static_cast<ssize_t>(size))
                fail(src, "read", n < 0 ? errno : EIO);
            write_all(out, rel, buf.get(), size, 0);
        }
        if (rel == "reflink/partial.bin") {
            // Overwrite 4 KiB in the middle: the rest stays shared.
            unsigned char block[UNIT];
            Prng p(derive_seed(m_opt.seed, "content:" + rel));
            p.fill(block, sizeof block);
            write_all(out, rel, block, sizeof block, 256 * 1024);
        }
        close_fd(out, rel);
    }
    close_fd(in, src);
    if (why.empty())
        made("reflinks",
            "/reflink/clone.bin shares all extents with "
            "/reflink/source.bin, /reflink/partial.bin all but "
            "4 KiB");
    else
        not_made("reflinks", why + "; plain copies instead");
}

void
Generator::links()
{
    make_dir("links");
    make_dir("links/sub");
    file("links/target.txt", 21);
    hard_link("links/target.txt", "links/hard-1");
    hard_link("links/target.txt", "links/sub/hard-2");
    made("hard links",
        "/links/target.txt, /links/hard-1, /links/sub/hard-2 "
        "(3 links); and the 50 files of /dirs/link-targets");

    struct Sym {
        const char *name;
        std::string target;
    };
    Prng p(derive_seed(m_opt.seed, "symlinks"));
    auto long_target = [&p](size_t len) {
        std::string t = LONG_SYMLINK_PREFIX;
        while (t.size() < len)
            t += (t.size() % 50 == 49)
                ? '/'
                : "abcdefghijklmnopqrstuvwxyz"[p.range(0, 25)];
        t.back() = 'x';
        return t;
    };
    const Sym syms[] = {
        {"short-symlink", "target.txt"},
        {"relative-symlink", "../files/small.txt"},
        {"absolute-symlink", "/etc/hostname"},
        {"dangling-symlink", "does-not-exist"},
        {"dir-symlink", "../dirs"},
        {"long-symlink-300", long_target(300)},
        // The longest target XFS stores (shorter than XFS_SYMLINK_MAXLEN,
        // 1024, which counts the terminating NUL).
        {"max-symlink-1023", long_target(1023)},
    };
    std::string skipped;
    for (const Sym &s : syms) {
        const std::string rel = std::string("links/") + s.name;
        if (symlinkat(s.target.c_str(), m_rootfd, rel.c_str()) != 0) {
            if (errno == ENAMETOOLONG) {
                skipped += " /" + rel;
                continue;
            }
            fail(rel, "symlink", errno);
        }
        default_times(rel);
    }
    made("symlinks",
        std::string("/links: targets of 10 bytes (short) up to 300") +
            (skipped.empty() ? " and 1023" : "") +
            " bytes (long), relative, absolute, dangling, to a directory");
    if (!skipped.empty())
        not_made(
            "symlinks", "symlink: " + errno_text(ENAMETOOLONG) + ":" + skipped);
}

void
Generator::names()
{
    make_dir("names");
    const std::vector<std::string> utf8 = {
        "caf\xc3\xa9.txt",                                  // é
        "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e.txt",         // 日本語
        "\xf0\x9f\x8c\xb2 tree.txt",                        // U+1F332, 4 bytes
        "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82", // Привет
    };
    for (const std::string &n : utf8)
        file("names/" + n, n.size());
    made("UTF-8 names", "/names: 2-, 3- and 4-byte UTF-8 sequences");

    for (const char *n : {"name with spaces", "-leading-dash", "100%",
             "tab\there", "new\nline", "trailing-space "})
        file(std::string("names/") + n, 7);
    made("other names", "/names: spaces, tab, newline, '%', leading '-'");

    // Names of 255 bytes, the longest XFS and ext4 store: ASCII, and 85
    // three-byte UTF-8 characters.
    Prng p(derive_seed(m_opt.seed, "names:255"));
    std::string ascii = seeded_name(p, "", 255);
    std::string wide;
    for (int i = 0; i < 85; ++i)
        wide += "\xe5\xad\x97"; // 字
    file("names/" + ascii, 255);
    file("names/" + wide, 85);
    make_dir("names/" + seeded_name(p, "dir-", 251));
    made("255-byte names",
        "/names: two files (ASCII; 85 three-byte UTF-8 "
        "characters) and a directory");
}

void
Generator::times()
{
    make_dir("times");
    make_dir("times/before-1970");
    make_dir("times/after-2038");
    struct Group {
        const char *dir;
        const TimeCase *cases;
        size_t n;
        const char *feature;
    };
    const Group groups[] = {
        {"times/before-1970", BEFORE_1970,
            sizeof BEFORE_1970 / sizeof BEFORE_1970[0],
            "timestamps before 1970"},
        {"times", AROUND_EPOCH, sizeof AROUND_EPOCH / sizeof AROUND_EPOCH[0],
            "timestamps at the epoch and the end of 32-bit time"},
        {"times/after-2038", AFTER_2038,
            sizeof AFTER_2038 / sizeof AFTER_2038[0], "timestamps after 2038"},
    };
    for (const Group &g : groups) {
        std::string ok, clamped;
        for (size_t i = 0; i < g.n; ++i) {
            const TimeCase &c = g.cases[i];
            const std::string rel = std::string(g.dir) + "/" + c.name;
            int fd = create(rel);
            write_content(fd, rel, 8);
            m_times.pop_back(); // not the default times
            struct timespec ts[2];
            ts[0].tv_sec = ts[1].tv_sec = static_cast<time_t>(c.sec);
            ts[0].tv_nsec = ts[1].tv_nsec = c.nsec;
            if (futimens(fd, ts) != 0)
                fail(rel, "futimens", errno);
            struct stat st;
            if (fstat(fd, &st) != 0)
                fail(rel, "stat", errno);
            close_fd(fd, rel);
            if (same_time(st.st_mtim, ts[1]))
                ok += std::string(ok.empty() ? "" : ", ") + iso_time(ts[1]);
            else
                clamped += std::string(clamped.empty() ? "" : ", ") +
                    iso_time(ts[1]) + " stored as " + iso_time(st.st_mtim);
        }
        if (!ok.empty())
            made(g.feature, "/" + std::string(g.dir) + ": " + ok);
        if (!clamped.empty())
            not_made(g.feature, "not stored as given: " + clamped);
    }
}

void
Generator::special()
{
    make_dir("special");
    std::string made_list, why;
    if (mkfifoat(m_rootfd, "special/fifo", 0644) != 0)
        fail("special/fifo", "mkfifo", errno);
    default_times("special/fifo");
    made_list = "fifo";

    // A socket: bind a Unix socket to the path, then close it.
    const std::string sock = m_root + "/special/socket";
    struct sockaddr_un sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    if (sock.size() < sizeof sa.sun_path) {
        std::memcpy(sa.sun_path, sock.c_str(), sock.size());
        int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (s < 0)
            fail("special/socket", "socket", errno);
        if (bind(s, reinterpret_cast<struct sockaddr *>(&sa), sizeof sa) != 0)
            fail("special/socket", "bind", errno);
        close(s);
        if (fchmodat(m_rootfd, "special/socket", 0755, 0) != 0)
            fail("special/socket", "chmod", errno);
        default_times("special/socket");
        made_list += ", socket";
    }
    else {
        why = "/special/socket: path longer than sun_path";
    }

    const struct {
        const char *rel;
        mode_t type;
        dev_t dev;
    } nodes[] = {
        {"special/chardev", S_IFCHR, makedev(1, 3)},
        {"special/blockdev", S_IFBLK, makedev(7, 0)},
    };
    for (const auto &n : nodes) {
        if (mknodat(m_rootfd, n.rel, n.type | 0640, n.dev) != 0) {
            if (errno != EPERM)
                fail(n.rel, "mknod", errno);
            why += std::string(why.empty() ? "" : "; ") + "/" + n.rel +
                ": mknod: " + errno_text(EPERM);
            continue;
        }
        default_times(n.rel);
        made_list += std::string(", ") +
            (n.type == S_IFCHR ? "character" : "block") + " device";
    }
    made("special files", "/special: " + made_list);
    if (!why.empty())
        not_made("special files", why);
}

void
Generator::owners()
{
    make_dir("owners");
    struct Owned {
        const char *rel;
        bool dir;
        uid_t uid;
        gid_t gid;
        mode_t mode;
    };
    const Owned owned[] = {
        {"owners/user-0640", false, 1000, 1000, 0640},
        {"owners/nobody-0600", false, 65534, 65534, 0600},
        {"owners/setuid-4755", false, 0, 0, 04755},
        {"owners/setgid-2711", false, 0, 100, 02711},
        {"owners/no-permissions-0000", false, 1000, 100, 0},
        {"owners/sticky-dir-1777", true, 0, 0, 01777},
        {"owners/group-dir-2770", true, 1000, 100, 02770},
    };
    std::string why;
    for (const Owned &o : owned) {
        if (o.dir)
            make_dir(o.rel, 0755);
        else
            file(o.rel, 11);
        // chown first: it clears setuid and setgid.
        if (fchownat(m_rootfd, o.rel, o.uid, o.gid, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno != EPERM)
                fail(o.rel, "chown", errno);
            if (why.empty())
                why = "chown: " + errno_text(EPERM) + "; owned by the caller";
        }
        if (fchmodat(m_rootfd, o.rel, o.mode, 0) != 0)
            fail(o.rel, "chmod", errno);
    }
    if (why.empty())
        made("owners and modes",
            "/owners: uid and gid 1000, 65534, 0 and 100; setuid, setgid, "
            "sticky, mode 0000");
    else
        not_made("owners and modes",
            why + " (modes set: setuid, setgid, sticky, 0000)");
}

void
Generator::apply_times()
{
    // Deepest first, so setting a child's times doesn't change its
    // directory's afterwards (it wouldn't anyway, but a directory's own
    // mtime must be the last thing set on it).
    std::stable_sort(
        m_times.begin(), m_times.end(), [](const Times &a, const Times &b) {
            return std::count(a.rel.begin(), a.rel.end(), '/') >
                std::count(b.rel.begin(), b.rel.end(), '/');
        });
    for (const Times &t : m_times) {
        struct timespec ts[2] = {t.atime, t.mtime};
        const char *path = t.rel.empty() ? "." : t.rel.c_str();
        if (utimensat(m_rootfd, path, ts, AT_SYMLINK_NOFOLLOW) != 0)
            fail(t.rel, "utimensat", errno);
    }
    made("timestamps",
        "every entry: a seeded mtime and atime with "
        "nanoseconds (ctime and crtime can't be set)");
}

void
Generator::run()
{
    m_rootfd = open(m_root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (m_rootfd < 0)
        throw GenError(m_root + ": open: " + errno_text(errno));
    struct RootFd {
        int fd;
        ~RootFd()
        {
            close(fd);
        }
    } guard{m_rootfd};

    if (fchmod(m_rootfd, 0755) != 0)
        throw GenError(m_root + ": chmod: " + errno_text(errno));
    default_times("");
    dirs();
    files();
    sparse();
    fragmented();
    reflinks();
    links();
    names();
    times();
    special();
    owners();
    make_dir("empty-dir");
    apply_times();
    if (syncfs(m_rootfd) != 0)
        throw GenError(m_root + ": syncfs: " + errno_text(errno));
}

// The target must be an empty directory, or not exist yet.
bool
prepare_target(const std::string &dir, std::string &why)
{
    struct stat st;
    if (lstat(dir.c_str(), &st) != 0) {
        if (errno != ENOENT) {
            why = dir + ": " + errno_text(errno);
            return false;
        }
        if (mkdir(dir.c_str(), 0755) != 0) {
            why = dir + ": mkdir: " + errno_text(errno);
            return false;
        }
        return true;
    }
    if (!S_ISDIR(st.st_mode)) {
        why = dir + ": not a directory";
        return false;
    }
    DIR *d = opendir(dir.c_str());
    if (d == nullptr) {
        why = dir + ": " + errno_text(errno);
        return false;
    }
    bool empty = true;
    while (struct dirent *e = readdir(d))
        if (std::strcmp(e->d_name, ".") != 0 &&
            std::strcmp(e->d_name, "..") != 0)
            empty = false;
    closedir(d);
    if (!empty)
        why = dir +
            ": not empty (the corpus is generated into an empty "
            "directory)";
    return empty;
}

} // namespace

bool
generate_corpus(const std::string &dir, const CorpusOptions &options,
    std::vector<RecordLine> &record)
{
    std::string why;
    if (!prepare_target(dir, why)) {
        record.push_back({RecordLine::Failed, "target", why});
        return false;
    }
    // Modes are exactly the ones asked for.
    mode_t old_umask = umask(0);
    bool ok = true;
    try {
        Generator(dir, options, record).run();
    }
    catch (const GenError &e) {
        record.push_back({RecordLine::Failed, "corpus", e.what()});
        ok = false;
    }
    umask(old_umask);
    return ok;
}

// ---------------------------------------------------------------------------
// The command.

namespace {

const char USAGE[] =
    "usage: tsk-corpus [--seed N] --large-size SIZE [--btree-entries N] DIR\n"
    "Writes the Test corpus into DIR (empty, or created). SIZE is the size of\n"
    "/files/large.bin in bytes, or with a K, M or G suffix (KiB, MiB, GiB).\n"
    "Prints the record: one line per feature, made or not-made, and why.\n";

bool
parse_u64(const std::string &s, uint64_t &v, bool suffix)
{
    if (s.empty() || s[0] == '-' || s[0] == '+')
        return false;
    errno = 0;
    char *end = nullptr;
    unsigned long long x = std::strtoull(s.c_str(), &end, 0);
    if (errno != 0 || end == s.c_str())
        return false;
    std::string rest = end;
    uint64_t mult = 1;
    if (suffix && rest.size() == 1) {
        if (rest == "K")
            mult = 1ULL << 10;
        else if (rest == "M")
            mult = 1ULL << 20;
        else if (rest == "G")
            mult = 1ULL << 30;
        else
            return false;
    }
    else if (!rest.empty()) {
        return false;
    }
    if (x > UINT64_MAX / mult)
        return false;
    v = x * mult;
    return true;
}

} // namespace

int
corpus_cli(
    const std::vector<std::string> &args, std::ostream &out, std::ostream &err)
{
    CorpusOptions opt;
    std::string dir;
    bool have_large = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string &a = args[i];
        if (a == "--help" || a == "-h") {
            out << USAGE;
            return CORPUS_OK;
        }
        if (a == "--seed" || a == "--large-size" || a == "--btree-entries") {
            if (i + 1 >= args.size()) {
                err << "tsk-corpus: " << a << " needs a value\n" << USAGE;
                return CORPUS_USAGE;
            }
            const std::string &v = args[++i];
            uint64_t x = 0;
            if (!parse_u64(v, x, a == "--large-size") ||
                (a == "--btree-entries" && (x == 0 || x > 1000000))) {
                err << "tsk-corpus: bad value for " << a << ": " << v << "\n";
                return CORPUS_USAGE;
            }
            if (a == "--seed")
                opt.seed = x;
            else if (a == "--large-size") {
                opt.large_size = x;
                have_large = true;
            }
            else
                opt.btree_entries = static_cast<unsigned>(x);
            continue;
        }
        if (!a.empty() && a[0] == '-') {
            err << "tsk-corpus: unknown option " << a << "\n" << USAGE;
            return CORPUS_USAGE;
        }
        if (!dir.empty()) {
            err << "tsk-corpus: more than one DIR\n" << USAGE;
            return CORPUS_USAGE;
        }
        dir = a;
    }
    if (dir.empty() || !have_large) {
        err << USAGE;
        return CORPUS_USAGE;
    }

    std::vector<RecordLine> record;
    bool ok = generate_corpus(dir, opt, record);
    out << "# tsk-corpus seed=" << opt.seed << " large-size=" << opt.large_size
        << " btree-entries=" << opt.btree_entries << "\n";
    for (const RecordLine &l : record)
        out << format_record_line(l) << "\n";
    out.flush();
    if (!ok) {
        const RecordLine &last = record.back();
        err << "tsk-corpus: " << last.detail << "\n";
        return last.feature == "target" ? CORPUS_USAGE : CORPUS_FAILED;
    }
    return CORPUS_OK;
}

} // namespace tskval
