/*
 * The harness. See harness.h. Uses only the public libtsk API
 * (<tsk/libtsk.h>, built against the installed headers).
 */
#include "harness.h"

#include "sha256.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <ostream>
#include <utility>

namespace tskval {

namespace {

const char USAGE[] =
    "Usage: tsk-harness [--partition N | --offset BYTES] [-o FILE] IMAGE...\n"
    "\n"
    "Writes a Manifest of the file system in a disk image (raw, .vmdk or any\n"
    "other format libtsk opens; several IMAGE arguments are the segments of\n"
    "one split image), read with the public libtsk API.\n"
    "Exit status: 0 complete, 1 written but incomplete (an !error line, or\n"
    "entries left out or without content; see standard error), 2 bad usage\n"
    "or output failed.\n"
    "\n"
    "Options:\n"
    "  --partition N   the Nth allocated partition of the volume system\n"
    "                  (1-based, in volume-system order; for a GPT disk\n"
    "                  without gaps, partition N is /dev/sdXN)\n"
    "  --offset BYTES  the file system starts BYTES bytes into the image\n"
    "                  (default: 0, the image is the file system)\n"
    "  -o FILE         write the Manifest to FILE instead of standard output\n"
    "  -h, --help      show this help\n";

// Canonical unsigned decimal: digits only, no overflow.
bool
parse_u64(const std::string &text, uint64_t &out)
{
    if (text.empty() || text.size() > 20) {
        return false;
    }
    uint64_t v = 0;
    for (char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        const uint64_t d = static_cast<uint64_t>(c - '0');
        if (v > (UINT64_MAX - d) / 10) {
            return false;
        }
        v = v * 10 + d;
    }
    out = v;
    return true;
}

std::string
tsk_error_text()
{
    const char *msg = tsk_error_get();
    std::string text = msg != nullptr ? msg : "unknown libtsk error";
    tsk_error_reset();
    // tsk_error_get() may end with a newline.
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

Timestamp
make_time(time_t sec, uint32_t nsec)
{
    Timestamp t;
    t.sec = static_cast<int64_t>(sec);
    t.nsec = nsec;
    return t;
}

EntryType
type_of(TSK_FS_META_TYPE_ENUM type)
{
    switch (type) {
    case TSK_FS_META_TYPE_REG:
        return EntryType::File;
    case TSK_FS_META_TYPE_DIR:
        return EntryType::Dir;
    case TSK_FS_META_TYPE_LNK:
        return EntryType::Symlink;
    case TSK_FS_META_TYPE_CHR:
        return EntryType::CharDev;
    case TSK_FS_META_TYPE_BLK:
        return EntryType::BlockDev;
    case TSK_FS_META_TYPE_FIFO:
        return EntryType::Fifo;
    case TSK_FS_META_TYPE_SOCK:
        return EntryType::Socket;
    default:
        return EntryType::Other;
    }
}

struct ImgCloser {
    void operator()(TSK_IMG_INFO *p) const
    {
        tsk_img_close(p);
    }
};
struct VsCloser {
    void operator()(TSK_VS_INFO *p) const
    {
        tsk_vs_close(p);
    }
};
struct FsCloser {
    void operator()(TSK_FS_INFO *p) const
    {
        tsk_fs_close(p);
    }
};
struct DirCloser {
    void operator()(TSK_FS_DIR *p) const
    {
        tsk_fs_dir_close(p);
    }
};
struct FileCloser {
    void operator()(TSK_FS_FILE *p) const
    {
        tsk_fs_file_close(p);
    }
};
using FilePtr = std::unique_ptr<TSK_FS_FILE, FileCloser>;

// Walks a file system from its root and writes one entry per allocated name.
class Walker {
  public:
    Walker(TSK_FS_INFO *fs, ManifestWriter &writer, std::ostream &err)
        : m_fs(fs), m_writer(writer), m_err(err)
    {}

    // Writes the root's entry and everything below it.
    void run(const TSK_FS_META &rootMeta)
    {
        m_writer.write(entry_from_meta("/", rootMeta));
        std::vector<TSK_INUM_T> ancestors;
        walk_dir(rootMeta.addr, "", ancestors);
    }

    bool complete() const
    {
        return m_problems == 0;
    }

  private:
    void problem(const std::string &path, const std::string &what)
    {
        ++m_problems;
        m_err << "tsk-harness: " << encode_bytes(path) << ": " << what << "\n";
    }

    // TSK's own virtual entries ($OrphanFiles and the like) are not part of
    // the file system's tree.
    bool is_virtual(const TSK_FS_NAME &name) const
    {
        return name.meta_addr == TSK_FS_ORPHANDIR_INUM(m_fs) ||
            name.type == TSK_FS_NAME_TYPE_VIRT ||
            name.type == TSK_FS_NAME_TYPE_VIRT_DIR;
    }

    void walk_dir(TSK_INUM_T addr, const std::string &dirPath,
        std::vector<TSK_INUM_T> &ancestors)
    {
        const std::string shownPath = dirPath.empty() ? "/" : dirPath;
        std::unique_ptr<TSK_FS_DIR, DirCloser> dir(
            tsk_fs_dir_open_meta(m_fs, addr));
        if (!dir) {
            problem(shownPath,
                "cannot open directory (contents left out): " +
                    tsk_error_text());
            return;
        }
        // Names first, sorted bytewise like Ground truth capture; the
        // metadata is loaded one entry at a time below.
        std::vector<std::pair<std::string, size_t>> names;
        const size_t count = tsk_fs_dir_getsize(dir.get());
        for (size_t i = 0; i < count; ++i) {
            const TSK_FS_NAME *name = tsk_fs_dir_get_name(dir.get(), i);
            if (name == nullptr || name->name == nullptr ||
                (name->flags & TSK_FS_NAME_FLAG_ALLOC) == 0 ||
                TSK_FS_ISDOT(name->name) || is_virtual(*name)) {
                continue;
            }
            names.emplace_back(name->name, i);
        }
        std::sort(names.begin(), names.end());
        ancestors.push_back(addr);
        for (const auto &n : names) {
            record(dir.get(), n.second, dirPath + "/" + n.first, ancestors);
        }
        ancestors.pop_back();
    }

    void record(TSK_FS_DIR *dir, size_t idx, const std::string &path,
        std::vector<TSK_INUM_T> &ancestors)
    {
        FilePtr file(tsk_fs_dir_get(dir, idx));
        if (!file || file->meta == nullptr) {
            problem(path, "no metadata (left out): " + tsk_error_text());
            return;
        }
        const TSK_FS_META &meta = *file->meta;
        Entry e = entry_from_meta(path, meta);
        if (e.type == EntryType::File) {
            e.has_sha256 = hash_content(file.get(), meta.size, e.sha256);
            if (!e.has_sha256) {
                problem(path,
                    "cannot read content (no SHA-256): " + tsk_error_text());
            }
        }
        m_writer.write(e);
        if (e.type == EntryType::Dir) {
            if (std::find(ancestors.begin(), ancestors.end(), meta.addr) !=
                ancestors.end()) {
                problem(path, "directory loop (contents left out)");
                return;
            }
            walk_dir(meta.addr, path, ancestors);
        }
    }

    static bool hash_content(
        TSK_FS_FILE *file, TSK_OFF_T size, std::string &hex)
    {
        Sha256 h;
        std::vector<char> buf(1 << 20);
        TSK_OFF_T off = 0;
        while (off < size) {
            size_t want = buf.size();
            if (static_cast<TSK_OFF_T>(want) > size - off) {
                want = static_cast<size_t>(size - off);
            }
            ssize_t n = tsk_fs_file_read(
                file, off, buf.data(), want, TSK_FS_FILE_READ_FLAG_NONE);
            if (n <= 0) {
                return false;
            }
            h.update(buf.data(), static_cast<size_t>(n));
            off += n;
        }
        hex = h.hex_digest();
        return true;
    }

    TSK_FS_INFO *m_fs;
    ManifestWriter &m_writer;
    std::ostream &m_err;
    unsigned m_problems = 0;
};

// Writes an !error Manifest and reports it.
int
whole_run_error(std::ostream &out, std::ostream &err, const std::string &code,
    const std::string &detail)
{
    ManifestWriter writer(out);
    writer.write_error(code, detail);
    err << "tsk-harness: " << code << ": " << detail << "\n";
    return HARNESS_INCOMPLETE;
}

} // namespace

bool
parse_harness_args(const std::vector<std::string> &args, HarnessOptions &opts,
    bool &help, std::string &error)
{
    opts = HarnessOptions();
    help = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string &a = args[i];
        if (a == "-h" || a == "--help") {
            help = true;
            return true;
        }
        if (a == "-o" || a == "--offset" || a == "--partition") {
            if (i + 1 >= args.size()) {
                error = a + " needs a value";
                return false;
            }
            const std::string &v = args[++i];
            uint64_t n = 0;
            if (a == "-o") {
                opts.output = v;
            }
            else if (a == "--offset") {
                if (!parse_u64(v, n)) {
                    error = "--offset needs a number of bytes, not '" + v + "'";
                    return false;
                }
                opts.offset = n;
                opts.has_offset = true;
            }
            else {
                if (!parse_u64(v, n) || n == 0 || n > 0xffffffffu) {
                    error =
                        "--partition needs a number from 1, not '" + v + "'";
                    return false;
                }
                opts.partition = static_cast<unsigned>(n);
            }
        }
        else if (!a.empty() && a[0] == '-') {
            error = "unknown option '" + a + "'";
            return false;
        }
        else {
            opts.images.push_back(a);
        }
    }
    if (opts.has_offset && opts.partition != 0) {
        error = "--partition and --offset exclude each other";
        return false;
    }
    if (opts.images.empty()) {
        error = "missing IMAGE";
        return false;
    }
    return true;
}

Entry
entry_from_meta(const std::string &path, const TSK_FS_META &meta)
{
    Entry e;
    e.path = path;
    e.type = type_of(meta.type);
    e.inode = meta.addr;
    e.mode = static_cast<uint32_t>(meta.mode) & 07777;
    e.uid = static_cast<uint32_t>(meta.uid);
    e.gid = static_cast<uint32_t>(meta.gid);
    e.size = meta.size > 0 ? static_cast<uint64_t>(meta.size) : 0;
    e.nlink = meta.nlink > 0 ? static_cast<uint64_t>(meta.nlink) : 0;
    e.mtime = make_time(meta.mtime, meta.mtime_nano);
    e.ctime = make_time(meta.ctime, meta.ctime_nano);
    e.atime = make_time(meta.atime, meta.atime_nano);
    e.has_atime = true;
    if (meta.crtime != 0 || meta.crtime_nano != 0) {
        e.crtime = make_time(meta.crtime, meta.crtime_nano);
        e.has_crtime = true;
    }
    if (e.type == EntryType::Symlink && meta.link != nullptr) {
        e.target = meta.link;
        e.has_target = true;
    }
    return e;
}

int
run_harness(const HarnessOptions &opts, std::ostream &out, std::ostream &err)
{
    std::vector<const char *> paths;
    paths.reserve(opts.images.size());
    for (const std::string &p : opts.images) {
        paths.push_back(p.c_str());
    }
    tsk_error_reset();
    std::unique_ptr<TSK_IMG_INFO, ImgCloser> img(tsk_img_open_utf8(
        static_cast<int>(paths.size()), paths.data(), TSK_IMG_TYPE_DETECT, 0));
    if (!img) {
        return whole_run_error(out, err, "img-open", tsk_error_text());
    }

    std::unique_ptr<TSK_VS_INFO, VsCloser> vs;
    std::unique_ptr<TSK_FS_INFO, FsCloser> fs;
    if (opts.partition != 0) {
        vs.reset(tsk_vs_open(img.get(), 0, TSK_VS_TYPE_DETECT));
        if (!vs) {
            return whole_run_error(out, err, "vs-open", tsk_error_text());
        }
        const TSK_VS_PART_INFO *found = nullptr;
        unsigned allocated = 0;
        for (TSK_PNUM_T i = 0; i < vs->part_count && found == nullptr; ++i) {
            const TSK_VS_PART_INFO *part = tsk_vs_part_get(vs.get(), i);
            if (part != nullptr && (part->flags & TSK_VS_PART_FLAG_ALLOC) &&
                ++allocated == opts.partition) {
                found = part;
            }
        }
        if (found == nullptr) {
            return whole_run_error(out, err, "partition",
                "no allocated partition " + std::to_string(opts.partition) +
                    " (the volume system has " + std::to_string(allocated) +
                    ")");
        }
        err << "tsk-harness: partition " << opts.partition << " starts at byte "
            << found->start * vs->block_size << "\n";
        fs.reset(tsk_fs_open_vol(found, TSK_FS_TYPE_DETECT));
    }
    else {
        fs.reset(tsk_fs_open_img(img.get(), static_cast<TSK_OFF_T>(opts.offset),
            TSK_FS_TYPE_DETECT));
    }
    if (!fs) {
        return whole_run_error(out, err, "fs-open", tsk_error_text());
    }

    // The root is read before the writer starts the Manifest: if it can't
    // be read, the Manifest is an !error one.
    FilePtr root(tsk_fs_file_open_meta(fs.get(), nullptr, fs->root_inum));
    if (!root || root->meta == nullptr) {
        return whole_run_error(out, err, "root", tsk_error_text());
    }
    ManifestWriter writer(out);
    Walker walker(fs.get(), writer, err);
    walker.run(*root->meta);
    return walker.complete() ? HARNESS_OK : HARNESS_INCOMPLETE;
}

int
harness_cli(
    const std::vector<std::string> &args, std::ostream &out, std::ostream &err)
{
    HarnessOptions opts;
    bool help = false;
    std::string error;
    if (!parse_harness_args(args, opts, help, error)) {
        err << "tsk-harness: " << error << "\n" << USAGE;
        return HARNESS_ERROR;
    }
    if (help) {
        out << USAGE;
        return HARNESS_OK;
    }
    std::ofstream file;
    if (!opts.output.empty()) {
        file.open(opts.output, std::ios::binary | std::ios::trunc);
        if (!file) {
            err << "tsk-harness: cannot write '" << opts.output << "'\n";
            return HARNESS_ERROR;
        }
    }
    std::ostream &dest = opts.output.empty() ? out : file;
    const int rc = run_harness(opts, dest, err);
    dest.flush();
    if (!dest) {
        err << "tsk-harness: write error\n";
        return HARNESS_ERROR;
    }
    return rc;
}

} // namespace tskval
