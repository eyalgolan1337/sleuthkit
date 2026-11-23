/*
 * Manifest reader and writer. See manifest.h and test/vm/README.md.
 */
#include "manifest.h"

#include <cstdio>
#include <istream>
#include <limits>
#include <ostream>
#include <set>

namespace tskval {

const char *const MANIFEST_HEADER = "#tsk-manifest v1";

namespace {

const uint32_t NSEC_PER_SEC = 1000000000U;
const size_t NUM_FIELDS = 14;
const char ABSENT[] = "-";

struct TypeName {
    EntryType type;
    const char *name;
};
const TypeName TYPE_NAMES[] = {{EntryType::File, "file"},
    {EntryType::Dir, "dir"}, {EntryType::Symlink, "symlink"},
    {EntryType::CharDev, "chr"}, {EntryType::BlockDev, "blk"},
    {EntryType::Fifo, "fifo"}, {EntryType::Socket, "sock"},
    {EntryType::Other, "other"}};

bool
is_digit(char c)
{
    return c >= '0' && c <= '9';
}

int
hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

// Canonical unsigned decimal: digits only, no leading zeros, <= max.
bool
parse_uint(const std::string &s, uint64_t max, uint64_t &out)
{
    if (s.empty() || (s.size() > 1 && s[0] == '0')) {
        return false;
    }
    uint64_t v = 0;
    for (char c : s) {
        if (!is_digit(c)) {
            return false;
        }
        const auto d = static_cast<uint64_t>(c - '0');
        if (v > (max - d) / 10) {
            return false;
        }
        v = v * 10 + d;
    }
    out = v;
    return true;
}

std::vector<std::string>
split_tabs(const std::string &line)
{
    std::vector<std::string> fields;
    size_t start = 0;
    for (;;) {
        const size_t tab = line.find('\t', start);
        if (tab == std::string::npos) {
            fields.push_back(line.substr(start));
            return fields;
        }
        fields.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
}

} // namespace

bool
is_valid_error_code(const std::string &code)
{
    if (code.empty()) {
        return false;
    }
    for (char c : code) {
        if (!(is_digit(c) || (c >= 'a' && c <= 'z') || c == '-')) {
            return false;
        }
    }
    return true;
}

namespace {

bool
parse_type(const std::string &s, EntryType &out)
{
    for (const TypeName &t : TYPE_NAMES) {
        if (s == t.name) {
            out = t.type;
            return true;
        }
    }
    return false;
}

const char *
type_name(EntryType type)
{
    for (const TypeName &t : TYPE_NAMES) {
        if (t.type == type) {
            return t.name;
        }
    }
    return "other";
}

bool
parse_optional_timestamp(const std::string &s, bool &has, Timestamp &out)
{
    if (s == ABSENT) {
        has = false;
        out = Timestamp();
        return true;
    }
    has = true;
    return parse_timestamp(s, out);
}

bool
parse_optional_string(const std::string &s, bool &has, std::string &out)
{
    if (s == ABSENT) {
        has = false;
        out.clear();
        return true;
    }
    has = true;
    return decode_bytes(s, out);
}

bool
valid_sha256(const std::string &s)
{
    if (s.size() != 64) {
        return false;
    }
    for (char c : s) {
        if (!(is_digit(c) || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

} // namespace

bool
operator==(const Timestamp &a, const Timestamp &b)
{
    return a.sec == b.sec && a.nsec == b.nsec;
}

bool
operator!=(const Timestamp &a, const Timestamp &b)
{
    return !(a == b);
}

bool
operator<(const Timestamp &a, const Timestamp &b)
{
    return a.sec < b.sec || (a.sec == b.sec && a.nsec < b.nsec);
}

std::string
format_timestamp(const Timestamp &t)
{
    // Magnitude as whole seconds plus nanoseconds, so that the full int64
    // range of seconds is representable (int64 nanoseconds end in 2262).
    bool negative = false;
    uint64_t mag_sec = 0;
    uint32_t mag_nsec = 0;
    if (t.sec >= 0) {
        mag_sec = static_cast<uint64_t>(t.sec);
        mag_nsec = t.nsec;
    }
    else {
        negative = true;
        // -(sec + 1) cannot overflow, unlike -sec for INT64_MIN.
        const uint64_t below = static_cast<uint64_t>(-(t.sec + 1));
        if (t.nsec == 0) {
            mag_sec = below + 1;
        }
        else {
            mag_sec = below;
            mag_nsec = NSEC_PER_SEC - t.nsec;
        }
    }
    std::string out = negative ? "-" : "";
    if (mag_sec == 0) {
        out += std::to_string(mag_nsec);
    }
    else {
        char frac[16];
        std::snprintf(
            frac, sizeof(frac), "%09u", static_cast<unsigned>(mag_nsec));
        out += std::to_string(mag_sec) + frac;
    }
    return out;
}

bool
parse_timestamp(const std::string &text, Timestamp &out)
{
    const bool negative = !text.empty() && text[0] == '-';
    const std::string digits = negative ? text.substr(1) : text;
    if (digits.empty() || (digits.size() > 1 && digits[0] == '0') ||
        (negative && digits == "0")) {
        return false; // not canonical
    }
    for (char c : digits) {
        if (!is_digit(c)) {
            return false;
        }
    }
    const size_t split = digits.size() > 9 ? digits.size() - 9 : 0;
    uint32_t mag_nsec = 0;
    for (size_t i = split; i < digits.size(); ++i) {
        mag_nsec = mag_nsec * 10 + static_cast<uint32_t>(digits[i] - '0');
    }
    // Seconds must fit int64 after the sign is applied: up to 2^63 - 1
    // seconds forward, and back to -2^63 seconds exactly.
    const uint64_t int64_max =
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    const uint64_t max_sec =
        negative && mag_nsec == 0 ? int64_max + 1 : int64_max;
    uint64_t mag_sec = 0;
    if (split > 0 && !parse_uint(digits.substr(0, split), max_sec, mag_sec)) {
        return false;
    }
    Timestamp t;
    if (!negative) {
        t.sec = static_cast<int64_t>(mag_sec);
        t.nsec = mag_nsec;
    }
    else if (mag_nsec == 0) {
        // -(mag_sec) without overflowing at 2^63.
        t.sec = -static_cast<int64_t>(mag_sec - 1) - 1;
    }
    else {
        t.sec = -static_cast<int64_t>(mag_sec) - 1;
        t.nsec = NSEC_PER_SEC - mag_nsec;
    }
    out = t;
    return true;
}

bool
is_valid_path(const std::string &p)
{
    if (p.empty() || p[0] != '/' || p.find('\0') != std::string::npos) {
        return false;
    }
    if (p == "/") {
        return true;
    }
    size_t start = 1;
    for (;;) {
        size_t end = p.find('/', start);
        if (end == std::string::npos) {
            end = p.size();
        }
        const std::string comp = p.substr(start, end - start);
        if (comp.empty() || comp == "." || comp == "..") {
            return false;
        }
        if (end == p.size()) {
            return true;
        }
        start = end + 1;
    }
}

std::string
encode_bytes(const std::string &raw)
{
    if (raw == ABSENT) {
        return "%2D";
    }
    static const char HEX[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(raw.size());
    for (char ch : raw) {
        const auto c = static_cast<unsigned char>(ch);
        if (c >= 0x21 && c <= 0x7E && c != '%') {
            out += ch;
        }
        else {
            out += '%';
            out += HEX[c >> 4];
            out += HEX[c & 0xF];
        }
    }
    return out;
}

bool
decode_bytes(const std::string &encoded, std::string &raw)
{
    std::string out;
    out.reserve(encoded.size());
    for (size_t i = 0; i < encoded.size(); ++i) {
        const auto c = static_cast<unsigned char>(encoded[i]);
        if (c < 0x21 || c > 0x7E) {
            return false;
        }
        if (c != '%') {
            out += encoded[i];
            continue;
        }
        if (i + 2 >= encoded.size()) {
            return false; // truncated escape
        }
        const int hi = hex_value(encoded[i + 1]);
        const int lo = hex_value(encoded[i + 2]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out += static_cast<char>((hi << 4) | lo);
        i += 2;
    }
    // One spelling per byte string: no lowercase hex, no needless escapes.
    if (encode_bytes(out) != encoded) {
        return false;
    }
    raw = out;
    return true;
}

std::string
format_entry(const Entry &e)
{
    char mode[8];
    std::snprintf(
        mode, sizeof(mode), "%04o", static_cast<unsigned>(e.mode & 07777));
    std::string line = encode_bytes(e.path);
    const auto field = [&line](const std::string &s) {
        line += '\t';
        line += s;
    };
    field(type_name(e.type));
    field(std::to_string(e.inode));
    field(mode);
    field(std::to_string(e.uid));
    field(std::to_string(e.gid));
    field(std::to_string(e.size));
    field(std::to_string(e.nlink));
    field(e.has_target ? encode_bytes(e.target) : ABSENT);
    field(e.has_sha256 ? e.sha256 : ABSENT);
    field(format_timestamp(e.mtime));
    field(format_timestamp(e.ctime));
    field(e.has_crtime ? format_timestamp(e.crtime) : ABSENT);
    field(e.has_atime ? format_timestamp(e.atime) : ABSENT);
    return line;
}

bool
parse_entry_line(const std::string &line, Entry &e, std::string &error)
{
    const std::vector<std::string> f = split_tabs(line);
    if (f.size() != NUM_FIELDS) {
        error = "expected " + std::to_string(NUM_FIELDS) +
            " tab-separated fields, found " + std::to_string(f.size());
        return false;
    }
    Entry out;
    uint64_t v = 0;
    const uint64_t u32_max = std::numeric_limits<uint32_t>::max();
    const uint64_t u64_max = std::numeric_limits<uint64_t>::max();
    if (!decode_bytes(f[0], out.path) || !is_valid_path(out.path)) {
        error = "bad path '" + f[0] + "'";
        return false;
    }
    if (!parse_type(f[1], out.type)) {
        error = "bad type '" + f[1] + "'";
        return false;
    }
    if (!parse_uint(f[2], u64_max, out.inode)) {
        error = "bad inode '" + f[2] + "'";
        return false;
    }
    if (f[3].size() != 4 ||
        f[3].find_first_not_of("01234567") != std::string::npos) {
        error = "bad mode '" + f[3] + "' (expected four octal digits)";
        return false;
    }
    out.mode = static_cast<uint32_t>(std::stoul(f[3], nullptr, 8));
    if (!parse_uint(f[4], u32_max, v)) {
        error = "bad uid '" + f[4] + "'";
        return false;
    }
    out.uid = static_cast<uint32_t>(v);
    if (!parse_uint(f[5], u32_max, v)) {
        error = "bad gid '" + f[5] + "'";
        return false;
    }
    out.gid = static_cast<uint32_t>(v);
    if (!parse_uint(f[6], u64_max, out.size)) {
        error = "bad size '" + f[6] + "'";
        return false;
    }
    if (!parse_uint(f[7], u64_max, out.nlink)) {
        error = "bad link count '" + f[7] + "'";
        return false;
    }
    if (!parse_optional_string(f[8], out.has_target, out.target)) {
        error = "bad symlink target '" + f[8] + "'";
        return false;
    }
    if (f[9] == ABSENT) {
        out.has_sha256 = false;
    }
    else if (valid_sha256(f[9])) {
        out.has_sha256 = true;
        out.sha256 = f[9];
    }
    else {
        error = "bad SHA-256 '" + f[9] +
            "' (expected 64 lowercase hex digits or -)";
        return false;
    }
    if (!parse_timestamp(f[10], out.mtime)) {
        error = "bad mtime '" + f[10] + "'";
        return false;
    }
    if (!parse_timestamp(f[11], out.ctime)) {
        error = "bad ctime '" + f[11] + "'";
        return false;
    }
    if (!parse_optional_timestamp(f[12], out.has_crtime, out.crtime)) {
        error = "bad crtime '" + f[12] + "'";
        return false;
    }
    if (!parse_optional_timestamp(f[13], out.has_atime, out.atime)) {
        error = "bad atime '" + f[13] + "'";
        return false;
    }
    e = out;
    return true;
}

ManifestWriter::ManifestWriter(std::ostream &out) : m_out(out)
{
    m_out << MANIFEST_HEADER << '\n';
}

void
ManifestWriter::write(const Entry &e)
{
    m_out << format_entry(e) << '\n';
}

void
ManifestWriter::write_error(const std::string &code, const std::string &detail)
{
    m_out << "!error\t" << code << '\t' << encode_bytes(detail) << '\n';
}

void
write_manifest(std::ostream &out, const Manifest &m)
{
    ManifestWriter w(out);
    if (m.has_error) {
        w.write_error(m.error_code, m.error_detail);
    }
    for (const Entry &e : m.entries) {
        w.write(e);
    }
}

bool
read_manifest(std::istream &in, Manifest &m, std::string &error)
{
    Manifest out;
    std::set<std::string> paths;
    std::string line;
    size_t lineno = 0;
    const auto fail = [&](const std::string &msg) {
        error = "line " + std::to_string(lineno) + ": " + msg;
        return false;
    };
    while (std::getline(in, line)) {
        ++lineno;
        if (lineno == 1) {
            if (line != MANIFEST_HEADER) {
                return fail(
                    std::string("not a Manifest: the first line must be '") +
                    MANIFEST_HEADER + "'");
            }
            continue;
        }
        if (!line.empty() && line[0] == '#') {
            continue; // comment
        }
        if (!line.empty() && line[0] == '!') {
            const std::vector<std::string> f = split_tabs(line);
            if (f[0] != "!error") {
                return fail("unknown directive '" + f[0] + "'");
            }
            if (out.has_error) {
                return fail("more than one !error line");
            }
            if (f.size() != 3 || !is_valid_error_code(f[1]) ||
                !decode_bytes(f[2], out.error_detail)) {
                return fail(
                    "bad !error line (expected '!error<TAB>code<TAB>detail')");
            }
            out.has_error = true;
            out.error_code = f[1];
            continue;
        }
        Entry e;
        std::string msg;
        if (!parse_entry_line(line, e, msg)) {
            return fail(msg);
        }
        if (!paths.insert(e.path).second) {
            return fail("duplicate path '" + encode_bytes(e.path) + "'");
        }
        out.entries.push_back(e);
    }
    if (lineno == 0) {
        return fail("empty file, not a Manifest");
    }
    m = out;
    return true;
}

} // namespace tskval
