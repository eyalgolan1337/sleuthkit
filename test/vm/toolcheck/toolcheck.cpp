/*
 * The CLI tool checks' parsers. See toolcheck.h and test/vm/README.md.
 */
#include "toolcheck.h"

#include <map>
#include <sstream>
#include <vector>

namespace tskval {

namespace {

// fls's one-letter type (tsk_fs_name_type_str, tsk_fs_meta_type_str) for
// each Manifest type that has one.
bool
fls_letter_type(char letter, EntryType &type)
{
    switch (letter) {
    case 'r':
        type = EntryType::File;
        return true;
    case 'd':
        type = EntryType::Dir;
        return true;
    case 'l':
        type = EntryType::Symlink;
        return true;
    case 'c':
        type = EntryType::CharDev;
        return true;
    case 'b':
        type = EntryType::BlockDev;
        return true;
    case 'p':
        type = EntryType::Fifo;
        return true;
    case 's':
        type = EntryType::Socket;
        return true;
    default:
        return false;
    }
}

bool
under(const std::string &path, const std::string &dir)
{
    return path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0 &&
        path[dir.size()] == '/';
}

std::string
first_line(const std::string &text)
{
    return text.substr(0, text.find('\n'));
}

void
set_error(Manifest &m, const std::string &code, const std::string &detail)
{
    m.entries.clear();
    m.has_error = true;
    m.error_code = code;
    m.error_detail = detail;
}

// Splits a line at tabs.
std::vector<std::string>
split_tabs(const std::string &line)
{
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const size_t tab = line.find('\t', start);
        out.push_back(line.substr(start, tab - start));
        if (tab == std::string::npos) {
            return out;
        }
        start = tab + 1;
    }
}

// A decimal number without sign: digits only, at most 19 of them.
bool
parse_u64(const std::string &text, uint64_t &out)
{
    if (text.empty() || text.size() > 19 ||
        text.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }
    out = std::stoull(text);
    return true;
}

bool
is_sha256(const std::string &s)
{
    return s.size() == 64 &&
        s.find_first_not_of("0123456789abcdef") == std::string::npos;
}

// Days from 1970-01-01 to the proleptic Gregorian date y-m-d (Howard
// Hinnant's days_from_civil).
int64_t
days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2 ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

bool
digits(const std::string &s, size_t pos, size_t n, unsigned &out)
{
    if (pos + n > s.size()) {
        return false;
    }
    out = 0;
    for (size_t i = pos; i < pos + n; ++i) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        out = out * 10 + static_cast<unsigned>(s[i] - '0');
    }
    return true;
}

// "YYYY-MM-DD HH:MM:SS[.NNNNNNNNN]<suffix>" in UTC. With `need_fraction`,
// the nine fraction digits are required. All zeros (istat's way of printing
// a time at or before the epoch) is 0.
bool
parse_utc_time(const std::string &text, const std::string &suffix,
    bool need_fraction, Timestamp &out)
{
    unsigned year = 0;
    unsigned mon = 0;
    unsigned day = 0;
    unsigned hour = 0;
    unsigned min = 0;
    unsigned sec = 0;
    unsigned nsec = 0;
    if (!digits(text, 0, 4, year) || text[4] != '-' ||
        !digits(text, 5, 2, mon) || text[7] != '-' ||
        !digits(text, 8, 2, day) || text[10] != ' ' ||
        !digits(text, 11, 2, hour) || text[13] != ':' ||
        !digits(text, 14, 2, min) || text[16] != ':' ||
        !digits(text, 17, 2, sec)) {
        return false;
    }
    size_t pos = 19;
    if (pos < text.size() && text[pos] == '.') {
        if (!digits(text, pos + 1, 9, nsec)) {
            return false;
        }
        pos += 10;
    }
    else if (need_fraction) {
        return false;
    }
    if (text.compare(pos, std::string::npos, suffix) != 0) {
        return false;
    }
    if (year == 0 && mon == 0 && day == 0) {
        out = Timestamp();
        return hour == 0 && min == 0 && sec == 0 && nsec == 0;
    }
    if (mon < 1 || mon > 12 || day < 1 || day > 31 || hour > 23 || min > 59 ||
        sec > 60) {
        return false;
    }
    out.sec =
        days_from_civil(year, mon, day) * 86400 + hour * 3600 + min * 60 + sec;
    out.nsec = nsec;
    return true;
}

bool
parse_u32(const std::string &text, uint32_t &out)
{
    uint64_t v = 0;
    if (!parse_u64(text, v) || v > 0xFFFFFFFFu) {
        return false;
    }
    out = static_cast<uint32_t>(v);
    return true;
}

// The type bits of a stat mode (S_IFMT).
bool
type_from_mode(uint32_t mode, EntryType &type)
{
    switch (mode & 0170000) {
    case 0100000:
        type = EntryType::File;
        return true;
    case 0040000:
        type = EntryType::Dir;
        return true;
    case 0120000:
        type = EntryType::Symlink;
        return true;
    case 0020000:
        type = EntryType::CharDev;
        return true;
    case 0060000:
        type = EntryType::BlockDev;
        return true;
    case 0010000:
        type = EntryType::Fifo;
        return true;
    case 0140000:
        type = EntryType::Socket;
        return true;
    default:
        return false;
    }
}

// One stat record line (see istat_expected).
bool
parse_stat_record(const std::string &line, Entry &e, std::string &error)
{
    std::vector<std::string> f = split_tabs(line);
    if (f.size() > 11) { // a target with tabs
        for (size_t i = 11; i < f.size(); ++i) {
            f[10] += "\t" + f[i];
        }
        f.resize(11);
    }
    if (f.size() != 11) {
        error = "expected 11 tab-separated fields";
        return false;
    }
    if (!decode_bytes(f[0], e.path) || !is_valid_path(e.path)) {
        error = "bad path '" + f[0] + "'";
        return false;
    }
    uint32_t mode = 0;
    if (f[2].empty() || f[2].size() > 8 ||
        f[2].find_first_not_of("0123456789abcdef") != std::string::npos ||
        !type_from_mode(
            mode = static_cast<uint32_t>(std::stoul(f[2], nullptr, 16)),
            e.type)) {
        error = "bad mode '" + f[2] + "' (expected stat's %f)";
        return false;
    }
    e.mode = mode & 07777;
    if (!parse_u64(f[1], e.inode) || !parse_u32(f[3], e.uid) ||
        !parse_u32(f[4], e.gid) || !parse_u64(f[5], e.size) ||
        !parse_u64(f[6], e.nlink)) {
        error = "bad inode, uid, gid, size or link count";
        return false;
    }
    if (!parse_utc_time(f[7], " +0000", true, e.mtime) ||
        !parse_utc_time(f[8], " +0000", true, e.ctime)) {
        error = "bad time (expected stat's %y in UTC, "
                "YYYY-MM-DD HH:MM:SS.NNNNNNNNN +0000)";
        return false;
    }
    if (f[9] != "-") {
        if (!parse_utc_time(f[9], " +0000", true, e.crtime)) {
            error = "bad birth time '" + f[9] + "'";
            return false;
        }
        e.has_crtime = true;
    }
    if (e.type == EntryType::Symlink) {
        e.has_target = true;
        e.target = f[10];
    }
    return true;
}

// istat's ls-style mode string (tsk_fs_meta_make_ls): a type letter, then
// rwx triplets with s/S and t/T for setuid, setgid and sticky.
bool
parse_ls_mode(const std::string &s, EntryType &type, uint32_t &mode)
{
    if (s.size() != 10 || !fls_letter_type(s[0], type)) {
        return false;
    }
    mode = 0;
    // Positions 1-9: r w x, r w x, r w x; x positions carry the special bits.
    static const char READ_WRITE[] = "rw";
    for (int who = 0; who < 3; ++who) {
        const size_t base = 1 + static_cast<size_t>(who) * 3;
        const uint32_t shift = static_cast<uint32_t>(2 - who) * 3;
        for (size_t i = 0; i < 2; ++i) {
            if (s[base + i] == READ_WRITE[i]) {
                mode |= (4u >> i) << shift;
            }
            else if (s[base + i] != '-') {
                return false;
            }
        }
        const char x = s[base + 2];
        const char special = who == 2 ? 't' : 's';
        const uint32_t special_bit = who == 0 ? 04000u
            : who == 1                        ? 02000u
                                              : 01000u;
        if (x == 'x' || x == special) {
            mode |= 1u << shift;
        }
        if (x == special || x == special - ('a' - 'A')) {
            mode |= special_bit;
        }
        else if (x != 'x' && x != '-') {
            return false;
        }
    }
    return true;
}

// "key: value" -> value, if line starts with key.
bool
value_of(const std::string &line, const std::string &key, std::string &value)
{
    if (line.compare(0, key.size(), key) != 0) {
        return false;
    }
    value = line.substr(key.size());
    return true;
}

void
set_summary_error(
    Summary &s, const std::string &code, const std::string &detail)
{
    s.fields.clear();
    s.has_error = true;
    s.error_code = code;
    s.error_detail = detail;
}

std::string
trim_spaces(const std::string &s)
{
    const size_t start = s.find_first_not_of(" \t");
    if (start == std::string::npos) {
        return "";
    }
    return s.substr(start, s.find_last_not_of(" \t") - start + 1);
}

// The sections of the guest's summary: "== <name>" lines start them.
std::map<std::string, std::vector<std::string>>
sections(const std::string &raw)
{
    std::map<std::string, std::vector<std::string>> out;
    std::istringstream in(raw);
    std::string line;
    std::vector<std::string> *current = nullptr;
    while (std::getline(in, line)) {
        if (line.compare(0, 3, "== ") == 0) {
            current = &out[line.substr(3)];
        }
        else if (current != nullptr) {
            current->push_back(line);
        }
    }
    return out;
}

// "Key:   value" lines (dumpe2fs -h): the trimmed value of the first line
// whose text before the colon is `key`.
bool
colon_value(const std::vector<std::string> &lines, const std::string &key,
    std::string &value)
{
    for (const std::string &l : lines) {
        const size_t colon = l.find(':');
        if (colon != std::string::npos && l.substr(0, colon) == key) {
            value = trim_spaces(l.substr(colon + 1));
            return true;
        }
    }
    return false;
}

// "name=value" tokens of xfs_info: the first token named `name` on a line
// that starts with `section` ("" for any line).
bool
xfs_value(const std::vector<std::string> &lines, const std::string &section,
    const std::string &name, std::string &value)
{
    for (const std::string &l : lines) {
        if (l.compare(0, section.size(), section) != 0) {
            continue;
        }
        size_t at = 0;
        while ((at = l.find(name + "=", at)) != std::string::npos) {
            if (at == 0 || l[at - 1] == ' ' || l[at - 1] == '\t') {
                const size_t start = at + name.size() + 1;
                const size_t end = l.find_first_of(" ,\t", start);
                value = l.substr(start, end - start);
                return !value.empty();
            }
            at += name.size();
        }
    }
    return false;
}

bool
is_number(const std::string &s)
{
    uint64_t v = 0;
    return parse_u64(s, v);
}

// The 16 bytes of a canonical UUID, or false.
bool
uuid_bytes(const std::string &uuid, std::vector<unsigned> &bytes)
{
    std::string hex;
    for (size_t i = 0; i < uuid.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (uuid[i] != '-') {
                return false;
            }
            continue;
        }
        hex += uuid[i];
    }
    if (uuid.size() != 36 || hex.size() != 32 ||
        hex.find_first_not_of("0123456789abcdef") != std::string::npos) {
        return false;
    }
    bytes.clear();
    for (size_t i = 0; i < 32; i += 2) {
        bytes.push_back(
            static_cast<unsigned>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    return true;
}

const char HEX_DIGITS[] = "0123456789abcdef";

std::string
uuid_text(const std::vector<unsigned> &bytes)
{
    std::string out;
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out += '-';
        }
        out += HEX_DIGITS[bytes[i] >> 4];
        out += HEX_DIGITS[bytes[i] & 0xF];
    }
    return out;
}

// How ext fsstat prints a UUID ("Volume ID"): bytes 8-15, then 0-7, each as a
// little-endian 64-bit number in hex without leading zeros (PRIx64).
std::string
ext_volume_id(const std::vector<unsigned> &bytes)
{
    std::string out;
    for (size_t half : {size_t(8), size_t(0)}) {
        std::string digits;
        for (size_t i = 0; i < 8; ++i) {
            const unsigned b = bytes[half + 7 - i];
            digits += HEX_DIGITS[b >> 4];
            digits += HEX_DIGITS[b & 0xF];
        }
        const size_t nz = digits.find_first_not_of('0');
        out += nz == std::string::npos ? "0" : digits.substr(nz);
    }
    return out;
}

// The error code and detail of a tool that failed as a whole (see
// set_tool_error in toolcheck.h).
void
tool_error(const ToolOutput &t, std::string &code, std::string &detail)
{
    // libtsk's messages when file system autodetection fails
    // (tsk/fs/fs_open.c, tsk_err_fs_str): the file system wasn't recognised.
    static const char *const NOT_RECOGNISED[] = {
        "Cannot determine file system type", "Possible encryption detected",
        "Encryption detected", "Multiple file system types detected"};
    const std::string msg = first_line(t.err);
    for (const char *prefix : NOT_RECOGNISED) {
        if (msg.compare(0, std::string(prefix).size(), prefix) == 0) {
            code = "fs-open";
            detail = msg;
            return;
        }
    }
    code = "tool-failed";
    detail = "exit status " + std::to_string(t.status) + ": " + msg;
}

} // namespace

void
set_tool_error(Manifest &m, const ToolOutput &t)
{
    std::string code;
    std::string detail;
    tool_error(t, code, detail);
    set_error(m, code, detail);
}

Manifest
fls_run(const ToolOutput &t)
{
    Manifest m;
    std::string error;
    if (t.status != 0) {
        set_tool_error(m, t);
    }
    else if (!parse_fls(t.out, m, error)) {
        set_error(m, "tool-output", error);
    }
    return m;
}

std::string
fls_sanitize(const std::string &raw)
{
    std::string out = raw;
    for (char &c : out) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) {
            c = '^';
        }
    }
    return out;
}

Manifest
fls_expected(const Manifest &ground_truth)
{
    Manifest m;
    m.has_error = ground_truth.has_error;
    m.error_code = ground_truth.error_code;
    m.error_detail = ground_truth.error_detail;
    for (const Entry &g : ground_truth.entries) {
        if (g.path == "/") {
            continue;
        }
        Entry e;
        e.path = fls_sanitize(g.path);
        e.type = g.type;
        m.entries.push_back(e);
    }
    return m;
}

bool
parse_fls(const std::string &text, Manifest &out, std::string &error)
{
    Manifest m;
    std::vector<std::string> virtual_dirs;
    std::istringstream in(text);
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        if (line.empty()) {
            continue;
        }
        // "<name type>/<meta type> [* ]<inode>[...]:\t<path>"
        const size_t sep = line.find(":\t");
        if (sep == std::string::npos || line.size() < 4 || line[1] != '/' ||
            line[3] != ' ') {
            error = "fls output, line " + std::to_string(lineno) +
                ": expected '<type>/<type> <inode>:<TAB><path>'";
            return false;
        }
        const std::string path = "/" + line.substr(sep + 2);
        if (path == "/" || !is_valid_path(path)) {
            error = "fls output, line " + std::to_string(lineno) +
                ": not a path: " + encode_bytes(path);
            return false;
        }
        const char name_type = line[0];
        const char meta_type = line[2];
        if (name_type == 'V' || meta_type == 'V') {
            virtual_dirs.push_back(path);
            continue;
        }
        bool in_virtual = false;
        for (const std::string &v : virtual_dirs) {
            in_virtual = in_virtual || under(path, v);
        }
        if (in_virtual) {
            continue;
        }
        Entry e;
        e.path = path;
        EntryType type = EntryType::Other;
        if (name_type == meta_type && fls_letter_type(name_type, type)) {
            e.type = type;
        }
        m.entries.push_back(e);
    }
    out = m;
    return true;
}

Manifest
icat_expected(const Manifest &ground_truth)
{
    Manifest m;
    m.has_error = ground_truth.has_error;
    m.error_code = ground_truth.error_code;
    m.error_detail = ground_truth.error_detail;
    for (const Entry &g : ground_truth.entries) {
        if (g.type != EntryType::File) {
            continue;
        }
        Entry e;
        e.path = g.path;
        e.type = g.type;
        e.has_sha256 = g.has_sha256;
        e.sha256 = g.sha256;
        m.entries.push_back(e);
    }
    return m;
}

Manifest
icat_run(const Manifest &ground_truth, const ToolOutput &probe,
    const std::string &results)
{
    Manifest m;
    if (probe.status != 0) {
        set_tool_error(m, probe);
        return m;
    }
    // inode -> SHA-256 of a successful run
    std::map<uint64_t, std::string> hashes;
    std::istringstream in(results);
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        if (line.empty()) {
            continue;
        }
        const std::vector<std::string> f = split_tabs(line);
        uint64_t inode = 0;
        uint64_t status = 0;
        if (f.size() != 4 || !parse_u64(f[0], inode) ||
            !parse_u64(f[1], status) || (f[2] != "-" && !is_sha256(f[2]))) {
            set_error(m, "tool-output",
                "icat results, line " + std::to_string(lineno) +
                    ": expected '<inode> TAB <status> TAB <sha256 or -> TAB "
                    "<error>'");
            return m;
        }
        if (status == 0 && f[2] != "-") {
            hashes[inode] = f[2];
        }
    }
    for (const Entry &g : ground_truth.entries) {
        if (g.type != EntryType::File) {
            continue;
        }
        Entry e;
        e.path = g.path;
        e.type = g.type;
        const auto h = hashes.find(g.inode);
        if (h != hashes.end()) {
            e.has_sha256 = true;
            e.sha256 = h->second;
        }
        m.entries.push_back(e);
    }
    return m;
}

Manifest
istat_expected(const std::string &stat_records)
{
    Manifest m;
    std::istringstream in(stat_records);
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        if (line.empty()) {
            continue;
        }
        const std::vector<std::string> f = split_tabs(line);
        if (f.size() >= 2 && f[1] == "!missing") {
            set_error(m, "spot-check",
                "spot-check path " + f[0] +
                    " not found in the guest: " + (f.size() > 2 ? f[2] : ""));
            return m;
        }
        Entry e;
        std::string error;
        if (!parse_stat_record(line, e, error)) {
            set_error(m, "stat-output",
                "stat records, line " + std::to_string(lineno) + ": " + error);
            return m;
        }
        m.entries.push_back(e);
    }
    return m;
}

bool
parse_istat(const std::string &text, Entry &out, std::string &error)
{
    Entry e;
    bool have_inode = false;
    bool have_mode = false;
    bool have_ids = false;
    bool have_size = false;
    bool have_links = false;
    bool have_mtime = false;
    bool have_ctime = false;
    bool in_times = false; // in the first "Inode Times:" section
    bool done_times = false;
    std::istringstream in(text);
    std::string line;
    std::string v;
    const auto time = [&error](const std::string &value, Timestamp &t) {
        if (parse_utc_time(value, " (UTC)", false, t)) {
            return true;
        }
        error = "istat output: bad time '" + value +
            "' (expected UTC; run with TZ=UTC)";
        return false;
    };
    while (std::getline(in, line)) {
        if (line.empty()) {
            if (in_times) {
                in_times = false;
                done_times = true;
            }
            continue;
        }
        if (line == "Inode Times:" || line == "Original Inode Times:") {
            in_times = !done_times;
            continue;
        }
        if (in_times) {
            Timestamp t;
            if (value_of(line, "File Modified:\t", v)) {
                if (!time(v, e.mtime)) {
                    return false;
                }
                have_mtime = true;
            }
            else if (value_of(line, "Inode Modified:\t", v)) {
                if (!time(v, e.ctime)) {
                    return false;
                }
                have_ctime = true;
            }
            else if (value_of(line, "File Created:\t", v)) {
                if (!time(v, t)) {
                    return false;
                }
                e.has_crtime = t != Timestamp();
                e.crtime = e.has_crtime ? t : Timestamp();
            }
            continue;
        }
        if (value_of(line, "inode: ", v)) {
            have_inode = parse_u64(v, e.inode);
        }
        else if (value_of(line, "symbolic link to: ", v)) {
            e.has_target = true;
            e.target = v;
        }
        else if (value_of(line, "uid / gid: ", v)) {
            const size_t slash = v.find(" / ");
            have_ids = slash != std::string::npos &&
                parse_u32(v.substr(0, slash), e.uid) &&
                parse_u32(v.substr(slash + 3), e.gid);
        }
        else if (value_of(line, "mode: ", v)) {
            have_mode = parse_ls_mode(v, e.type, e.mode);
        }
        else if (value_of(line, "size: ", v)) {
            have_size = parse_u64(v, e.size);
        }
        else if (value_of(line, "num of links: ", v)) {
            have_links = parse_u64(v, e.nlink);
        }
    }
    if (!have_inode || !have_mode || !have_ids || !have_size || !have_links ||
        !have_mtime || !have_ctime) {
        error = "istat output: missing or malformed inode, mode, uid / gid, "
                "size, num of links, File Modified or Inode Modified line";
        return false;
    }
    out = e;
    return true;
}

Manifest
istat_run(
    const Manifest &expected, const std::map<uint64_t, ToolOutput> &outputs)
{
    Manifest m;
    for (const Entry &x : expected.entries) {
        const auto o = outputs.find(x.inode);
        if (o == outputs.end()) {
            continue;
        }
        const ToolOutput &t = o->second;
        if (t.status != 0) {
            Manifest failed;
            set_tool_error(failed, t);
            if (failed.error_code == "fs-open") {
                return failed;
            }
            continue;
        }
        Entry e;
        std::string error;
        if (!parse_istat(t.out, e, error)) {
            set_error(m, "tool-output",
                "inode " + std::to_string(x.inode) + ": " + error);
            return m;
        }
        e.path = x.path;
        m.entries.push_back(e);
    }
    return m;
}

Summary
guest_summary(const std::string &raw)
{
    Summary s;
    const auto sec = sections(raw);
    const auto fail = [&s](const std::string &why) {
        set_summary_error(
            s, "summary", "the guest's file-system summary: " + why);
        return s;
    };
    const auto blkid_it = sec.find("blkid");
    if (blkid_it == sec.end()) {
        return fail("no blkid section");
    }
    std::map<std::string, std::string> blkid;
    for (const std::string &l : blkid_it->second) {
        const size_t eq = l.find('=');
        if (eq != std::string::npos) {
            blkid[l.substr(0, eq)] = l.substr(eq + 1);
        }
    }
    const std::string type = blkid["TYPE"];
    std::string block_size;
    std::string inode_size;
    std::string groups;
    std::string version;
    if (type == "ext2" || type == "ext3" || type == "ext4") {
        const auto d = sec.find("dumpe2fs");
        std::string count;
        std::string first;
        std::string per_group;
        std::string revision;
        if (d == sec.end() ||
            !colon_value(d->second, "Block size", block_size) ||
            !colon_value(d->second, "Inode size", inode_size) ||
            !colon_value(d->second, "Block count", count) ||
            !colon_value(d->second, "First block", first) ||
            !colon_value(d->second, "Blocks per group", per_group) ||
            !colon_value(d->second, "Filesystem revision #", revision)) {
            return fail("dumpe2fs output lacks block size, inode size, block "
                        "count, first block, blocks per group or revision");
        }
        version = revision.substr(0, revision.find(' '));
        uint64_t c = 0;
        uint64_t f = 0;
        uint64_t p = 0;
        if (!parse_u64(count, c) || !parse_u64(first, f) ||
            !parse_u64(per_group, p) || p == 0 || f > c ||
            !is_number(block_size) || !is_number(inode_size) ||
            !is_number(version)) {
            return fail("malformed number in the dumpe2fs output");
        }
        groups = std::to_string((c - f + p - 1) / p);
    }
    else if (type == "xfs") {
        const auto x = sec.find("xfs_info");
        std::string crc;
        if (x == sec.end() ||
            !xfs_value(x->second, "data", "bsize", block_size) ||
            !xfs_value(x->second, "meta-data", "isize", inode_size) ||
            !xfs_value(x->second, "meta-data", "agcount", groups) ||
            !xfs_value(x->second, "", "crc", crc) || !is_number(block_size) ||
            !is_number(inode_size) || !is_number(groups) ||
            (crc != "0" && crc != "1")) {
            return fail("xfs_info output lacks data bsize, isize, agcount or "
                        "crc");
        }
        version = crc == "1" ? "5" : "4";
    }
    else {
        return fail("file system type '" + type +
            "' is not one the check knows (ext2/3/4, xfs)");
    }
    std::vector<unsigned> bytes;
    if (!uuid_bytes(blkid["UUID"], bytes)) {
        return fail("bad UUID '" + blkid["UUID"] + "'");
    }
    s.fields = {{"fs-type", type}, {"block-size", block_size},
        {"inode-size", inode_size}, {"group-count", groups},
        {"uuid", blkid["UUID"]}, {"label", blkid["LABEL"]},
        {"version", version}};
    return s;
}

Summary
fsstat_run(const ToolOutput &t, const Summary &expected)
{
    Summary s;
    if (t.status != 0) {
        std::string code;
        std::string detail;
        tool_error(t, code, detail);
        set_summary_error(s, code, detail);
        return s;
    }
    std::vector<std::string> lines;
    std::istringstream in(t.out);
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
    }
    std::string type;
    if (!colon_value(lines, "File System Type", type)) {
        set_summary_error(
            s, "tool-output", "fsstat output: no File System Type");
        return s;
    }
    if (type != "Ext2" && type != "Ext3" && type != "Ext4") {
        set_summary_error(s, "tool-output",
            "fsstat output of File System Type " + type +
                ": this check doesn't read it yet");
        return s;
    }
    std::string label;
    std::string volume_id;
    std::string inode_size;
    std::string block_size;
    std::string groups;
    bool dynamic = false;
    bool is_static = false;
    for (const std::string &l : lines) {
        dynamic = dynamic || l == "Dynamic Structure";
        is_static = is_static || l == "Static Structure";
        // "Volume Name: " keeps an empty name; colon_value would trim it.
        if (l.compare(0, 13, "Volume Name: ") == 0 && label.empty()) {
            label = l.substr(13);
        }
    }
    if (!colon_value(lines, "Volume ID", volume_id) ||
        !colon_value(lines, "Inode Size", inode_size) ||
        !colon_value(lines, "Block Size", block_size) ||
        !colon_value(lines, "Number of Block Groups", groups) ||
        dynamic == is_static) {
        set_summary_error(s, "tool-output",
            "fsstat output: no Volume ID, Inode Size, Block Size, Number of "
            "Block Groups or Static/Dynamic Structure line");
        return s;
    }
    // The UUID, as described in toolcheck.h.
    std::string uuid = "Volume ID " + volume_id;
    std::vector<unsigned> bytes;
    for (const auto &f : expected.fields) {
        if (f.first == "uuid" && uuid_bytes(f.second, bytes) &&
            ext_volume_id(bytes) == volume_id) {
            uuid = f.second;
        }
    }
    if (uuid != volume_id && uuid.compare(0, 10, "Volume ID ") == 0 &&
        volume_id.size() == 32 &&
        volume_id.find_first_not_of("0123456789abcdef") == std::string::npos) {
        // Printed: bytes 15..8, then 7..0, so byte 15 - i is digit pair i.
        bytes.assign(16, 0);
        for (size_t i = 0; i < 16; ++i) {
            bytes[15 - i] = static_cast<unsigned>(
                std::stoul(volume_id.substr(i * 2, 2), nullptr, 16));
        }
        uuid = uuid_text(bytes);
    }
    std::string fs_type = type;
    fs_type[0] = 'e';
    s.fields = {{"fs-type", fs_type}, {"block-size", block_size},
        {"inode-size", inode_size}, {"group-count", groups}, {"uuid", uuid},
        {"label", label}, {"version", dynamic ? "1" : "0"}};
    return s;
}

} // namespace tskval
