/*
 * Manifest: the one-line-per-entry record of a file system's contents that
 * Ground truth capture, the harness and the comparison step share.
 * The format is documented in test/vm/README.md ("Manifest format").
 *
 * C++14; no dependency on libtsk, so the harness, the guest-side Ground truth
 * capture and the comparison step can all link it.
 */
#ifndef TSK_TEST_VM_MANIFEST_H
#define TSK_TEST_VM_MANIFEST_H

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace tskval {

// First line of every Manifest.
extern const char *const MANIFEST_HEADER;

enum class EntryType {
    File,
    Dir,
    Symlink,
    CharDev,
    BlockDev,
    Fifo,
    Socket,
    Other
};

// A point in time with nanosecond resolution: sec seconds plus nsec
// nanoseconds after the epoch, nsec in [0, 1e9) (like struct timespec).
// 1969-12-31T23:59:59.5Z is {-1, 500000000}.
struct Timestamp {
    int64_t sec = 0;
    uint32_t nsec = 0;
};
bool operator==(const Timestamp &a, const Timestamp &b);
bool operator!=(const Timestamp &a, const Timestamp &b);
bool operator<(const Timestamp &a, const Timestamp &b);
// Decimal nanoseconds since the epoch, e.g. "-500000000" for the example above.
std::string format_timestamp(const Timestamp &t);
bool parse_timestamp(const std::string &text, Timestamp &out);

// One Manifest entry. The fields are grouped by size (clang-tidy's padding
// check); the Manifest's column order is in format_entry.
struct Entry {
    uint64_t inode = 0;
    uint64_t size = 0;
    uint64_t nlink = 0;
    Timestamp mtime;
    Timestamp ctime;
    Timestamp crtime; // when has_crtime
    Timestamp atime;  // when has_atime; recorded but never compared
    std::string
        path; // raw bytes, relative to the file system root, starts with '/'
    std::string target; // raw bytes; symlinks, when has_target
    std::string
        sha256; // 64 lowercase hex digits; regular files, when has_sha256
    EntryType type = EntryType::Other;
    uint32_t mode = 0; // permission bits only (07777); the type is in `type`
    uint32_t uid = 0;
    uint32_t gid = 0;
    bool has_target = false;
    bool has_sha256 = false;
    bool has_crtime = false;
    bool has_atime = false;
};

// A Manifest. A producer that could not record the whole file system (for
// example, the file system was not recognised) sets has_error; the entries
// are then incomplete.
struct Manifest {
    bool has_error = false;
    std::string error_code;   // short token: [a-z0-9-]+, e.g. "fs-open"
    std::string error_detail; // free text, any bytes
    std::vector<Entry> entries;
};

// Path encoding: every byte outside '!'..'~' (0x21-0x7E), and '%' itself,
// becomes %XX with two uppercase hex digits. A string that is exactly "-"
// is written "%2D", because "-" alone marks an absent field.
std::string encode_bytes(const std::string &raw);
// Inverse of encode_bytes. Accepts only what encode_bytes writes, so every
// byte string has exactly one spelling: rejects bytes outside '!'..'~',
// malformed escapes, lowercase hex and needless escapes such as %61.
bool decode_bytes(const std::string &encoded, std::string &raw);

// An error code of an !error line: [a-z0-9-]+.
bool is_valid_error_code(const std::string &code);

// A Manifest path: "/" or "/a/b", relative to the file system root, with no
// empty, "." or ".." component and no NUL byte.
bool is_valid_path(const std::string &raw);

// One Manifest line (no newline) for an entry, and its inverse. On failure
// parse_entry_line sets `error` to a message without the line number.
std::string format_entry(const Entry &e);
bool parse_entry_line(const std::string &line, Entry &e, std::string &error);

// Streaming writer: the header on construction, then one line per call.
class ManifestWriter {
  public:
    explicit ManifestWriter(std::ostream &out);
    void write(const Entry &e);
    void write_error(const std::string &code, const std::string &detail);

  private:
    std::ostream &m_out;
};

void write_manifest(std::ostream &out, const Manifest &m);

// Reads a whole Manifest. Fails, with `error` naming the line, on a missing
// header, a malformed line or a duplicate path.
bool read_manifest(std::istream &in, Manifest &m, std::string &error);

} // namespace tskval

#endif
