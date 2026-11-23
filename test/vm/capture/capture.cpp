/*
 * Ground truth capture. See capture.h.
 */
#include "capture.h"

#include "sha256.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <ostream>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tskval {

namespace {

const char USAGE[] =
    "Usage: capture-ground-truth [-o FILE] DIR\n"
    "\n"
    "Writes a Manifest of the directory tree under DIR, read through the\n"
    "kernel. Does not follow symlinks or cross into other file systems.\n"
    "Exit status: 0 complete, 1 written but some entries incomplete (see\n"
    "standard error), 2 bad usage, DIR unreadable or output failed.\n"
    "\n"
    "Options:\n"
    "  -o FILE     write the Manifest to FILE instead of standard output\n"
    "  -h, --help  show this help\n";

const unsigned STATX_WANTED = STATX_BASIC_STATS | STATX_BTIME;

std::string
errno_text(int err)
{
    return std::strerror(err);
}

// Display form of a path in problem messages.
std::string
shown(const std::string &path)
{
    return encode_bytes(path);
}

Timestamp
to_timestamp(const struct statx_timestamp &t)
{
    Timestamp out;
    out.sec = t.tv_sec;
    out.nsec = t.tv_nsec;
    return out;
}

EntryType
type_of(uint16_t mode)
{
    switch (mode & S_IFMT) {
    case S_IFREG:
        return EntryType::File;
    case S_IFDIR:
        return EntryType::Dir;
    case S_IFLNK:
        return EntryType::Symlink;
    case S_IFCHR:
        return EntryType::CharDev;
    case S_IFBLK:
        return EntryType::BlockDev;
    case S_IFIFO:
        return EntryType::Fifo;
    case S_IFSOCK:
        return EntryType::Socket;
    default:
        return EntryType::Other;
    }
}

Entry
entry_from_statx(const std::string &path, const struct statx &stx)
{
    Entry e;
    e.path = path;
    e.type = type_of(stx.stx_mode);
    e.inode = stx.stx_ino;
    e.mode = stx.stx_mode & 07777;
    e.uid = stx.stx_uid;
    e.gid = stx.stx_gid;
    e.size = stx.stx_size;
    e.nlink = stx.stx_nlink;
    e.mtime = to_timestamp(stx.stx_mtime);
    e.ctime = to_timestamp(stx.stx_ctime);
    e.atime = to_timestamp(stx.stx_atime);
    e.has_atime = true;
    if (stx.stx_mask & STATX_BTIME) {
        e.crtime = to_timestamp(stx.stx_btime);
        e.has_crtime = true;
    }
    return e;
}

// Opens `name` in `dirfd` without following a symlink and without updating
// its atime where the kernel allows that (O_NOATIME needs ownership).
int
open_nofollow(int dirfd, const std::string &name, int flags)
{
    flags |= O_RDONLY | O_NOFOLLOW | O_CLOEXEC;
    int fd = openat(dirfd, name.c_str(), flags | O_NOATIME);
    if (fd < 0 && errno == EPERM) {
        fd = openat(dirfd, name.c_str(), flags);
    }
    return fd;
}

// O_NONBLOCK and the fstat check: on a live file system the file may have
// been replaced by a FIFO since statx, and opening or reading that must not
// block the capture.
bool
hash_file(int dirfd, const std::string &name, std::string &hex, int &err)
{
    int fd = open_nofollow(dirfd, name, O_NONBLOCK);
    if (fd < 0) {
        err = errno;
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        err = errno;
        close(fd);
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        err = EINVAL; // no longer a regular file
        close(fd);
        return false;
    }
    Sha256 h;
    char buf[1 << 16];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            err = errno;
            close(fd);
            return false;
        }
        h.update(buf, static_cast<size_t>(n));
    }
    close(fd);
    hex = h.hex_digest();
    return true;
}

bool
read_link(int dirfd, const std::string &name, uint64_t size,
    std::string &target, int &err)
{
    // st_size is the target length on Linux file systems, but procfs-like
    // ones report 0: grow the buffer until the target fits.
    size_t cap = size + 1 > 256 ? size + 1 : 256;
    for (;;) {
        std::string buf(cap, '\0');
        ssize_t n = readlinkat(dirfd, name.c_str(), &buf[0], cap);
        if (n < 0) {
            err = errno;
            return false;
        }
        if (static_cast<size_t>(n) < cap) {
            target.assign(buf, 0, static_cast<size_t>(n));
            return true;
        }
        cap *= 2;
    }
}

class Walker {
  public:
    Walker(std::vector<Entry> &entries, std::vector<std::string> &problems)
        : m_entries(entries), m_problems(problems)
    {}

    bool run(const std::string &root)
    {
        int fd = open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        struct statx stx;
        if (fd < 0 || statx(fd, "", AT_EMPTY_PATH, STATX_WANTED, &stx) != 0) {
            m_problems.push_back("cannot read root directory " + root + ": " +
                errno_text(errno));
            if (fd >= 0) {
                close(fd);
            }
            return false;
        }
        m_dev_major = stx.stx_dev_major;
        m_dev_minor = stx.stx_dev_minor;
        m_entries.push_back(entry_from_statx("/", stx));
        walk_dir(fd, "");
        return true;
    }

  private:
    void problem(const std::string &path, const std::string &what, int err)
    {
        m_problems.push_back(
            shown(path) + ": " + what + ": " + errno_text(err));
    }

    // Records the contents of the open directory `fd` (closed on return),
    // whose Manifest path is `dirPath` ("" for the root).
    void walk_dir(int fd, const std::string &dirPath)
    {
        DIR *dir = fdopendir(fd);
        if (dir == nullptr) {
            problem(dirPath.empty() ? "/" : dirPath, "cannot list", errno);
            close(fd);
            return;
        }
        std::vector<std::string> names;
        errno = 0;
        while (struct dirent *de = readdir(dir)) {
            if (std::strcmp(de->d_name, ".") != 0 &&
                std::strcmp(de->d_name, "..") != 0) {
                names.emplace_back(de->d_name);
            }
            errno = 0;
        }
        if (errno != 0) {
            problem(dirPath.empty() ? "/" : dirPath, "cannot list", errno);
        }
        std::sort(names.begin(), names.end());
        for (const std::string &name : names) {
            record(dirfd(dir), dirPath, name);
        }
        closedir(dir);
    }

    void record(int dirfd, const std::string &dirPath, const std::string &name)
    {
        const std::string path = dirPath + "/" + name;
        struct statx stx;
        if (statx(dirfd, name.c_str(), AT_SYMLINK_NOFOLLOW, STATX_WANTED,
                &stx) != 0) {
            problem(path, "cannot stat (not recorded)", errno);
            return;
        }
        m_entries.push_back(entry_from_statx(path, stx));
        // m_entries may reallocate during the recursion below: fill the
        // entry in now and don't keep a reference to it.
        Entry &e = m_entries.back();
        int err = 0;
        if (e.type == EntryType::File) {
            e.has_sha256 = hash_file(dirfd, name, e.sha256, err);
            if (!e.has_sha256) {
                problem(path, "cannot read content (no SHA-256)", err);
            }
        }
        else if (e.type == EntryType::Symlink) {
            e.has_target = read_link(dirfd, name, e.size, e.target, err);
            if (!e.has_target) {
                problem(path, "cannot read symlink target", err);
            }
        }
        else if (e.type == EntryType::Dir) {
            if (stx.stx_dev_major != m_dev_major ||
                stx.stx_dev_minor != m_dev_minor) {
                return; // a mount point: another file system
            }
            int fd = open_nofollow(dirfd, name, O_DIRECTORY);
            if (fd < 0) {
                problem(path, "cannot open directory", errno);
                return;
            }
            walk_dir(fd, path);
        }
    }

    std::vector<Entry> &m_entries;
    std::vector<std::string> &m_problems;
    uint32_t m_dev_major = 0;
    uint32_t m_dev_minor = 0;
};

} // namespace

bool
capture_tree(const std::string &root, std::vector<Entry> &entries,
    std::vector<std::string> &problems)
{
    return Walker(entries, problems).run(root);
}

int
capture_cli(
    const std::vector<std::string> &args, std::ostream &out, std::ostream &err)
{
    std::string outFile;
    std::string root;
    bool haveOut = false;
    bool haveRoot = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string &a = args[i];
        if (a == "-h" || a == "--help") {
            out << USAGE;
            return CAPTURE_OK;
        }
        if (a == "-o") {
            if (i + 1 >= args.size()) {
                err << "capture-ground-truth: -o needs a file name\n" << USAGE;
                return CAPTURE_ERROR;
            }
            outFile = args[++i];
            haveOut = true;
        }
        else if (!a.empty() && a[0] == '-') {
            err << "capture-ground-truth: unknown option '" << a << "'\n"
                << USAGE;
            return CAPTURE_ERROR;
        }
        else if (haveRoot) {
            err << "capture-ground-truth: only one DIR\n" << USAGE;
            return CAPTURE_ERROR;
        }
        else {
            root = a;
            haveRoot = true;
        }
    }
    if (!haveRoot) {
        err << "capture-ground-truth: missing DIR\n" << USAGE;
        return CAPTURE_ERROR;
    }

    std::vector<Entry> entries;
    std::vector<std::string> problems;
    const bool rootOk = capture_tree(root, entries, problems);
    for (const std::string &p : problems) {
        err << "capture-ground-truth: " << p << "\n";
    }
    if (!rootOk) {
        return CAPTURE_ERROR;
    }

    std::ofstream file;
    if (haveOut) {
        file.open(outFile, std::ios::binary | std::ios::trunc);
        if (!file) {
            err << "capture-ground-truth: cannot write '" << outFile << "'\n";
            return CAPTURE_ERROR;
        }
    }
    std::ostream &dest = haveOut ? file : out;
    ManifestWriter writer(dest);
    for (const Entry &e : entries) {
        writer.write(e);
    }
    dest.flush();
    if (!dest) {
        err << "capture-ground-truth: write error\n";
        return CAPTURE_ERROR;
    }
    return problems.empty() ? CAPTURE_OK : CAPTURE_INCOMPLETE;
}

} // namespace tskval
