/*
 * The inode walk and block walk checks. See walkcheck.h.
 */
#include "walkcheck.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace tskval {

namespace {

// Thrown by the record's readers, caught by the public functions.
struct RecordError : std::runtime_error {
    explicit RecordError(const std::string &what) : std::runtime_error(what) {}
};

// The parts, joined.
std::string
cat(std::initializer_list<std::string> parts)
{
    std::string s;
    for (const std::string &p : parts) {
        s += p;
    }
    return s;
}

[[noreturn]] void
fail(const std::string &what)
{
    throw RecordError(what);
}

std::vector<std::string>
split_lines(const std::string &text)
{
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
    }
    return lines;
}

std::vector<std::string>
split_words(const std::string &text)
{
    std::istringstream in(text);
    std::vector<std::string> words;
    std::string w;
    while (in >> w) {
        words.push_back(w);
    }
    return words;
}

std::string
trim(const std::string &s)
{
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
        return "";
    }
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

// A whole unsigned number: decimal, or hex with 0x when `base` is 0.
bool
to_u64(const std::string &text, uint64_t &out, int base = 10)
{
    if (text.empty() || text[0] == '-' || text[0] == '+' || text[0] == ' ') {
        return false;
    }
    char *end = nullptr;
    errno = 0;
    const unsigned long long v = std::strtoull(text.c_str(), &end, base);
    if (errno != 0 || end == text.c_str() || *end != '\0') {
        return false;
    }
    out = v;
    return true;
}

uint64_t
number(const std::string &text, const std::string &where, int base = 10)
{
    uint64_t v = 0;
    if (!to_u64(trim(text), v, base)) {
        fail(where + ": not a number: '" + text + "'");
    }
    return v;
}

// One section of the walk record: "== <name> <args...>" and its lines.
struct Section {
    std::vector<std::string> head; // name and arguments
    std::vector<std::string> lines;
};

std::vector<Section>
sections(const std::string &record)
{
    std::vector<Section> out;
    for (const std::string &line : split_lines(record)) {
        if (line.compare(0, 3, "== ") == 0) {
            out.push_back({split_words(line.substr(3)), {}});
        }
        else if (!out.empty()) {
            out.back().lines.push_back(line);
        }
    }
    return out;
}

// The section whose head starts with `head`, or null.
const Section *
find_section(
    const std::vector<Section> &secs, const std::vector<std::string> &head)
{
    for (const Section &s : secs) {
        if (s.head.size() >= head.size() &&
            std::equal(head.begin(), head.end(), s.head.begin())) {
            return &s;
        }
    }
    return nullptr;
}

// Never null.
const Section *
need_section(
    const std::vector<Section> &secs, const std::vector<std::string> &head)
{
    const Section *s = find_section(secs, head);
    if (s == nullptr) {
        std::string name;
        for (const std::string &h : head) {
            name += (name.empty() ? "" : " ") + h;
        }
        fail("the walk record has no section '" + name + "'");
    }
    return s;
}

// "key = value" lines of xfs_db's print.
std::map<std::string, std::string>
xfs_db_fields(const Section &s)
{
    std::map<std::string, std::string> f;
    for (const std::string &line : s.lines) {
        const size_t eq = line.find(" = ");
        if (eq != std::string::npos && line.find('[') == std::string::npos) {
            f[trim(line.substr(0, eq))] = trim(line.substr(eq + 3));
        }
    }
    return f;
}

uint64_t
field(const std::map<std::string, std::string> &f, const std::string &key,
    const std::string &where)
{
    const auto it = f.find(key);
    if (it == f.end()) {
        fail(where + ": no " + key);
    }
    return number(it->second, where + " " + key);
}

// What the file system itself says, apart from the spot-check paths.
struct FsFacts {
    std::string type; // "xfs" or "ext"
    uint64_t block_size = 0;
    uint64_t block_count = 0;
    uint64_t inode_count = 0;
    uint64_t virtual_inode = 0;
    // The free inodes the walk visits as free, and those whose ctime the
    // guest reads (the others were never used).
    std::set<uint64_t> free_inodes;
    std::vector<uint64_t> plan;
    // Allocated inodes no name refers to; ext's only count when their ctime
    // isn't 0 (reserved_inodes).
    std::vector<std::pair<uint64_t, std::string>> internal;
    std::vector<uint64_t> reserved_inodes;
    uint64_t free_block = 0;
    uint64_t header_block = 0;
    std::vector<std::string> notes;
};

// --- XFS ---

const unsigned INODES_PER_CHUNK = 64;
const unsigned INODES_PER_HOLEMASK_BIT = 4;

// The records of the btdump lines of a section: level 0 blocks only, after a
// "recs[...] = [columns]" line. Each record's numbers by column name.
std::vector<std::map<std::string, uint64_t>>
btdump_records(const Section &s, const std::string &where)
{
    static const std::regex HEADER(R"(^recs\[[0-9-]+\] = \[([a-z,]+)\]\s*$)");
    static const std::regex RECORD(R"(^([0-9]+):\[([^\]]*)\]\s*$)");
    std::vector<std::map<std::string, uint64_t>> out;
    std::vector<std::string> columns;
    bool in_records = false;
    for (const std::string &line : s.lines) {
        std::smatch m;
        if (std::regex_match(line, m, HEADER)) {
            columns.clear();
            std::istringstream cols(m[1].str());
            std::string c;
            while (std::getline(cols, c, ',')) {
                columns.push_back(c);
            }
            in_records = true;
            continue;
        }
        if (!in_records) {
            continue;
        }
        if (!std::regex_match(line, m, RECORD)) {
            // keys[...], ptrs[...] or the next block's header.
            if (line.find(" level ") != std::string::npos ||
                line.compare(0, 5, "keys[") == 0 ||
                line.compare(0, 5, "ptrs[") == 0 || trim(line).empty()) {
                in_records = false;
                continue;
            }
            fail(cat({where, ": cannot read the btdump line '", line, "'"}));
        }
        std::vector<std::string> values;
        std::istringstream vals(m[2].str());
        std::string v;
        while (std::getline(vals, v, ',')) {
            values.push_back(v);
        }
        if (values.size() != columns.size()) {
            fail(cat({where, ": cannot read the btdump line '", line, "'"}));
        }
        std::map<std::string, uint64_t> rec;
        for (size_t i = 0; i < columns.size(); ++i) {
            uint64_t n = 0;
            if (!to_u64(values[i], n, 0)) {
                fail(
                    cat({where, ": cannot read the btdump line '", line, "'"}));
            }
            rec[columns[i]] = n;
        }
        out.push_back(rec);
    }
    return out;
}

unsigned
popcount64(uint64_t v)
{
    unsigned n = 0;
    for (; v != 0; v &= v - 1) {
        ++n;
    }
    return n;
}

void
xfs_inodes(const std::vector<Section> &secs, uint64_t agcount,
    unsigned agino_bits, FsFacts &f)
{
    for (uint64_t ag = 0; ag < agcount; ++ag) {
        const std::string where = "xfs_db agi " + std::to_string(ag);
        const Section &s =
            *need_section(secs, {"xfs_db", "agi", std::to_string(ag)});
        const auto fields = xfs_db_fields(s);
        const uint64_t count = field(fields, "count", where);
        const uint64_t freecount = field(fields, "freecount", where);
        const uint64_t level = field(fields, "level", where);
        uint64_t sum = 0;
        uint64_t free_sum = 0;
        const auto recs = btdump_records(s, where);
        for (const auto &r : recs) {
            if (r.count("startino") == 0 || r.count("free") == 0 ||
                r.count("freecount") == 0) {
                fail(where + ": a record without startino, freecount or free");
            }
            const uint64_t holes =
                r.count("holemask") != 0 ? r.at("holemask") : 0;
            const uint64_t chunk_count =
                r.count("count") != 0 ? r.at("count") : INODES_PER_CHUNK;
            uint64_t hole_bits = 0;
            for (unsigned i = 0; i < INODES_PER_CHUNK; ++i) {
                if ((holes >> (i / INODES_PER_HOLEMASK_BIT)) & 1) {
                    hole_bits |= uint64_t{1} << i;
                }
            }
            const uint64_t free_bits = r.at("free") & ~hole_bits;
            const std::string rec =
                where + ", chunk " + std::to_string(r.at("startino"));
            if (INODES_PER_CHUNK - popcount64(hole_bits) != chunk_count) {
                fail(rec + ": its holes don't leave count inodes");
            }
            if (popcount64(free_bits) != r.at("freecount")) {
                fail(rec + ": its free bits don't add up to freecount");
            }
            sum += chunk_count;
            free_sum += r.at("freecount");
            const uint64_t base = (ag << agino_bits) + r.at("startino");
            for (unsigned i = 0; i < INODES_PER_CHUNK; ++i) {
                if ((free_bits >> i) & 1) {
                    f.free_inodes.insert(base + i);
                    f.plan.push_back(base + i);
                }
            }
        }
        if (sum != count || free_sum != freecount) {
            fail("AG " + std::to_string(ag) +
                ": the inode B+tree's records "
                "hold " +
                std::to_string(sum) + " inodes, " + std::to_string(free_sum) +
                " free; agi_count " + std::to_string(count) +
                ", agi_freecount " + std::to_string(freecount));
        }
        f.inode_count += count;
        f.notes.push_back("AG " + std::to_string(ag) + ": inode B+tree level " +
            std::to_string(level) + ", " + std::to_string(recs.size()) +
            " chunk records, " + std::to_string(count) + " inodes, " +
            std::to_string(freecount) + " free");
    }
    std::sort(f.plan.begin(), f.plan.end());
}

FsFacts
xfs_facts(const std::vector<Section> &secs)
{
    FsFacts f;
    f.type = "xfs";
    const std::string where = "xfs_db sb";
    const auto sb = xfs_db_fields(*need_section(secs, {"xfs_db", "sb"}));
    f.block_size = field(sb, "blocksize", where);
    const uint64_t agcount = field(sb, "agcount", where);
    const uint64_t agblocks = field(sb, "agblocks", where);
    f.block_count = field(sb, "dblocks", where);
    const uint64_t agino_bits =
        field(sb, "agblklog", where) + field(sb, "inopblog", where);
    if (f.block_size == 0 || f.block_size % 512 != 0 || agcount < 2 ||
        agino_bits >= 64) {
        fail(where + ": block size, AG count or inode bits out of range");
    }
    for (const char *key :
        {"rbmino", "rsumino", "uquotino", "gquotino", "pquotino"}) {
        const auto it = sb.find(key);
        if (it == sb.end()) {
            fail(where + ": no " + key);
        }
        if (it->second == "null" || it->second == "0") {
            continue;
        }
        f.internal.emplace_back(number(it->second, where + " " + key), key);
    }
    xfs_inodes(secs, agcount, static_cast<unsigned>(agino_bits), f);
    // The first free extent of the first AG that has one.
    bool found = false;
    for (uint64_t ag = 0; ag < agcount && !found; ++ag) {
        const std::string bwhere = "xfs_db bnobt " + std::to_string(ag);
        const auto recs = btdump_records(
            *need_section(secs, {"xfs_db", "bnobt", std::to_string(ag)}),
            bwhere);
        if (!recs.empty()) {
            if (recs[0].count("startblock") == 0) {
                fail(bwhere + ": no startblock");
            }
            f.free_block = ag * agblocks + recs[0].at("startblock");
            found = true;
        }
    }
    if (!found) {
        fail("no AG has a free extent");
    }
    f.header_block = agblocks; // AG 1, block 0: its superblock
    return f;
}

// A run of blocks in file order.
struct Run {
    uint64_t start = 0;
    uint64_t length = 0;
};

// The data blocks of xfs_bmap -v's output, in file order: each extent's
// BLOCK-RANGE in 512-byte units, holes left out.
std::vector<Run>
xfs_bmap_runs(const Section &s, uint64_t block_size, const std::string &where)
{
    static const std::regex EXTENT(
        R"(^\s*[0-9]+: \[[0-9]+\.\.[0-9]+\]:\s+(hole|([0-9]+)\.\.([0-9]+))(\s.*)?$)");
    std::vector<Run> runs;
    const uint64_t per_block = block_size / 512;
    for (const std::string &line : s.lines) {
        if (trim(line).empty() || line.compare(0, 5, " EXT:") == 0 ||
            (!line.empty() && line.back() == ':') ||
            line.find(": no extents") != std::string::npos) {
            continue;
        }
        std::smatch m;
        if (!std::regex_match(line, m, EXTENT)) {
            fail(cat({where, ": cannot read '", line, "'"}));
        }
        if (m[1] == "hole") {
            continue;
        }
        const uint64_t a = number(m[2].str(), where);
        const uint64_t b = number(m[3].str(), where);
        if (b < a || a % per_block != 0 || (b + 1) % per_block != 0) {
            fail(cat({where, ": not whole blocks: '", line, "'"}));
        }
        runs.push_back({a / per_block, (b + 1 - a) / per_block});
    }
    return runs;
}

// --- ext2/3/4 ---

// "a-b, c, ..." of dumpe2fs's Free blocks and Free inodes; empty is none.
std::vector<Run>
ranges(const std::string &text, const std::string &where)
{
    std::vector<Run> out;
    std::istringstream in(text);
    std::string item;
    while (std::getline(in, item, ',')) {
        item = trim(item);
        if (item.empty()) {
            continue;
        }
        const std::string what = cat({where, " '", item, "'"});
        const size_t dash = item.find('-');
        const uint64_t a = number(item.substr(0, dash), what);
        const uint64_t b =
            dash == std::string::npos ? a : number(item.substr(dash + 1), what);
        if (b < a) {
            fail(cat({where, ": bad range '", item, "'"}));
        }
        out.push_back({a, b - a + 1});
    }
    return out;
}

struct Group {
    bool zeroed = false;
    bool has_unused = false;
    uint64_t unused = 0;
    uint64_t free_inodes_count = 0;
    std::vector<Run> free_blocks;
    std::vector<Run> free_inodes;
};

FsFacts
ext_facts(const std::vector<Section> &secs)
{
    static const std::regex GROUP(R"(^Group ([0-9]+): \(Blocks .*$)");
    static const std::regex COUNTS(
        R"(^\s+[0-9]+ free blocks, ([0-9]+) free inodes, [0-9]+ directories(, ([0-9]+) unused inodes)?\s*$)");
    FsFacts f;
    f.type = "ext";
    const Section &s = *need_section(secs, {"dumpe2fs"});
    std::map<std::string, std::string> head;
    std::vector<Group> groups;
    for (const std::string &line : s.lines) {
        std::smatch m;
        if (std::regex_match(line, m, GROUP)) {
            if (number(m[1].str(), "dumpe2fs group") != groups.size()) {
                fail("dumpe2fs: groups out of order at '" + line + "'");
            }
            groups.emplace_back();
            groups.back().zeroed =
                line.find("ITABLE_ZEROED") != std::string::npos;
            continue;
        }
        if (groups.empty()) {
            const size_t colon = line.find(':');
            if (colon != std::string::npos) {
                head[line.substr(0, colon)] = trim(line.substr(colon + 1));
            }
            continue;
        }
        Group &g = groups.back();
        const std::string where =
            "dumpe2fs group " + std::to_string(groups.size() - 1);
        const std::string t = trim(line);
        if (std::regex_match(line, m, COUNTS)) {
            g.free_inodes_count = number(m[1].str(), where);
            g.has_unused = m[3].matched;
            if (g.has_unused) {
                g.unused = number(m[3].str(), where);
            }
        }
        else if (t.compare(0, 12, "Free blocks:") == 0) {
            g.free_blocks = ranges(t.substr(12), where + " free blocks");
        }
        else if (t.compare(0, 12, "Free inodes:") == 0) {
            g.free_inodes = ranges(t.substr(12), where + " free inodes");
        }
    }
    const auto head_number = [&head](const std::string &key) {
        const auto it = head.find(key);
        if (it == head.end()) {
            fail("dumpe2fs: no '" + key + ":'");
        }
        return number(it->second, "dumpe2fs " + key);
    };
    const uint64_t inode_count = head_number("Inode count");
    f.block_count = head_number("Block count");
    f.block_size = head_number("Block size");
    const uint64_t first_block = head_number("First block");
    const uint64_t blocks_per_group = head_number("Blocks per group");
    const uint64_t ipg = head_number("Inodes per group");
    const uint64_t first_ino =
        head.count("First inode") != 0 ? head_number("First inode") : 11;
    if (groups.size() < 2) {
        fail("dumpe2fs: fewer than two groups");
    }
    f.inode_count = ipg * groups.size();
    if (f.inode_count != inode_count) {
        fail("dumpe2fs: " + std::to_string(groups.size()) + " groups of " +
            std::to_string(ipg) + " inodes, but the inode count is " +
            std::to_string(inode_count));
    }
    f.virtual_inode = f.inode_count + 1;
    for (uint64_t i = 1; i < first_ino; ++i) {
        f.reserved_inodes.push_back(i);
        f.plan.push_back(i);
    }
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        const Group &g = groups[gi];
        const uint64_t first = gi * ipg + 1;
        const uint64_t end = first + ipg; // one past the group's last inode
        // Inodes at or past the unused tail were never used.
        const uint64_t tail =
            g.zeroed && g.has_unused && g.unused <= ipg ? end - g.unused : end;
        uint64_t n = 0;
        for (const Run &r : g.free_inodes) {
            if (r.start < first || r.start + r.length > end) {
                fail("dumpe2fs group " + std::to_string(gi) +
                    ": free inodes outside the group");
            }
            for (uint64_t i = r.start; i < r.start + r.length; ++i) {
                f.free_inodes.insert(i);
                if (i < tail && i >= first_ino) {
                    f.plan.push_back(i);
                }
            }
            n += r.length;
        }
        if (n != g.free_inodes_count) {
            fail("dumpe2fs group " + std::to_string(gi) + ": " +
                std::to_string(n) + " free inodes listed, " +
                std::to_string(g.free_inodes_count) + " counted");
        }
    }
    std::sort(f.plan.begin(), f.plan.end());
    f.plan.erase(std::unique(f.plan.begin(), f.plan.end()), f.plan.end());
    bool found = false;
    for (const Group &g : groups) {
        if (!g.free_blocks.empty()) {
            f.free_block = g.free_blocks[0].start;
            found = true;
            break;
        }
    }
    if (!found) {
        fail("dumpe2fs: no group has a free block");
    }
    f.header_block = first_block + blocks_per_group;
    f.notes.push_back(std::to_string(groups.size()) + " groups of " +
        std::to_string(ipg) + " inodes");
    return f;
}

// The data blocks of debugfs stat's EXTENTS: or BLOCKS: list, in file order.
// Extent tree blocks (ETB<n>) and indirect blocks (IND, DIND, TIND) are left
// out.
std::vector<Run>
debugfs_runs(const Section &s, const std::string &where)
{
    static const std::regex DATA(
        R"(^\(([0-9]+)(-([0-9]+))?(\[u\])?\)(\[u\])?:([0-9]+)(-([0-9]+))?$)");
    std::map<uint64_t, Run> by_logical;
    bool in_list = false;
    for (const std::string &line : s.lines) {
        const std::string t = trim(line);
        if (t == "EXTENTS:" || t == "BLOCKS:") {
            in_list = true;
            continue;
        }
        if (!in_list) {
            continue;
        }
        if (t.empty() || t.compare(0, 6, "TOTAL:") == 0) {
            in_list = false;
            continue;
        }
        std::istringstream in(t);
        std::string item;
        while (std::getline(in, item, ',')) {
            item = trim(item);
            if (item.compare(0, 4, "(ETB") == 0 ||
                item.compare(0, 5, "(IND)") == 0 ||
                item.compare(0, 6, "(DIND)") == 0 ||
                item.compare(0, 6, "(TIND)") == 0) {
                continue;
            }
            std::smatch m;
            if (!std::regex_match(item, m, DATA)) {
                fail(cat({where, ": cannot read '", item, "'"}));
            }
            const uint64_t l0 = number(m[1].str(), where);
            const uint64_t l1 = m[3].matched ? number(m[3].str(), where) : l0;
            const uint64_t p0 = number(m[6].str(), where);
            const uint64_t p1 = m[8].matched ? number(m[8].str(), where) : p0;
            if (l1 < l0 || p1 - p0 != l1 - l0) {
                fail(cat({where, ": the ranges of '", item, "' differ"}));
            }
            by_logical[l0] = {p0, l1 - l0 + 1};
        }
    }
    std::vector<Run> runs;
    runs.reserve(by_logical.size());
    for (const auto &r : by_logical) {
        runs.push_back(r.second);
    }
    return runs;
}

// --- Both ---

// The facts of whichever file system the record describes.
FsFacts
fs_facts(const std::vector<Section> &secs)
{
    if (find_section(secs, {"xfs_db", "sb"}) != nullptr) {
        return xfs_facts(secs);
    }
    if (find_section(secs, {"dumpe2fs"}) != nullptr) {
        return ext_facts(secs);
    }
    fail("the walk record has neither an xfs_db sb nor a dumpe2fs section");
}

// The block at index `k` of `runs` in file order.
uint64_t
block_at(const std::vector<Run> &runs, uint64_t k)
{
    for (const Run &r : runs) {
        if (k < r.length) {
            return r.start + k;
        }
        k -= r.length;
    }
    fail("block index past the end");
}

std::vector<IfindExpectation>
ifind_expectations(const std::vector<Section> &secs, const FsFacts &f)
{
    std::vector<IfindExpectation> out;
    const std::string tool = f.type == "xfs" ? "xfs_bmap" : "debugfs";
    for (const Section &s : secs) {
        if (s.head.empty() || s.head[0] != tool) {
            continue;
        }
        if (s.head.size() != 3) {
            fail("bad section '== " + tool + "': needs an inode and a path");
        }
        const std::string where = tool + " " + s.head[2];
        const uint64_t inode = number(s.head[1], where);
        const std::vector<Run> runs = tool == "xfs_bmap"
            ? xfs_bmap_runs(s, f.block_size, where)
            : debugfs_runs(s, where);
        uint64_t n = 0;
        for (const Run &r : runs) {
            n += r.length;
        }
        if (n == 0) {
            continue;
        }
        const std::string &path = s.head[2];
        out.push_back({path + "@first", block_at(runs, 0), inode});
        out.push_back({path + "@middle", block_at(runs, n / 2), inode});
        out.push_back({path + "@last", block_at(runs, n - 1), inode});
    }
    out.push_back({"/free-block", f.free_block, 0});
    out.push_back({"/header-block", f.header_block, 0});
    return out;
}

// The inode-ctime section: raw ctime by inode.
std::map<uint64_t, uint64_t>
ctimes(const std::vector<Section> &secs)
{
    std::map<uint64_t, uint64_t> out;
    const Section &s = *need_section(secs, {"inode-ctime"});
    for (const std::string &line : s.lines) {
        const std::vector<std::string> w = split_words(line);
        if (w.empty()) {
            continue;
        }
        if (w.size() != 2) {
            fail("inode-ctime: cannot read '" + line + "'");
        }
        out[number(w[0], "inode-ctime")] = number(w[1], "inode-ctime", 16);
    }
    return out;
}

} // namespace

bool
walk_plan_inodes(
    const std::string &record, std::vector<uint64_t> &out, std::string &error)
{
    try {
        out = fs_facts(sections(record)).plan;
        return true;
    }
    catch (const RecordError &e) {
        error = e.what();
        return false;
    }
}

bool
walk_plan_blocks(
    const std::string &record, std::vector<uint64_t> &out, std::string &error)
{
    try {
        const std::vector<Section> secs = sections(record);
        std::set<uint64_t> blocks;
        for (const IfindExpectation &e :
            ifind_expectations(secs, fs_facts(secs))) {
            blocks.insert(e.block);
        }
        out.assign(blocks.begin(), blocks.end());
        return true;
    }
    catch (const RecordError &e) {
        error = e.what();
        return false;
    }
}

bool
parse_walk_truth(const std::string &record, WalkTruth &out, std::string &error)
{
    try {
        const std::vector<Section> secs = sections(record);
        const FsFacts f = fs_facts(secs);
        const std::map<uint64_t, uint64_t> ct = ctimes(secs);
        const auto ctime_of = [&ct](uint64_t inode) {
            const auto it = ct.find(inode);
            if (it == ct.end()) {
                fail("inode-ctime: no ctime of inode " + std::to_string(inode));
            }
            return it->second;
        };
        WalkTruth t;
        t.fs_type = f.type;
        t.block_count = f.block_count;
        t.inode_count = f.inode_count;
        t.virtual_inode = f.virtual_inode;
        t.internal_inodes = f.internal;
        for (uint64_t r : f.reserved_inodes) {
            if (f.free_inodes.count(r) == 0 && ctime_of(r) != 0) {
                t.internal_inodes.emplace_back(r, "reserved");
            }
        }
        for (uint64_t i : f.plan) {
            if (f.free_inodes.count(i) != 0 && ctime_of(i) != 0) {
                t.used_free.insert(i);
            }
        }
        t.ifind = ifind_expectations(secs, f);
        t.notes = f.notes;
        out = t;
        return true;
    }
    catch (const RecordError &e) {
        error = e.what();
        return false;
    }
}

} // namespace tskval

namespace tskval {

namespace {

Manifest
error_manifest(const std::string &code, const std::string &detail)
{
    Manifest m;
    m.has_error = true;
    m.error_code = code;
    m.error_detail = detail;
    return m;
}

Summary
error_summary(const Manifest &m)
{
    Summary s;
    s.has_error = true;
    s.error_code = m.error_code;
    s.error_detail = m.error_detail;
    return s;
}

// A failed tool's !error Manifest (set_tool_error's codes).
Manifest
tool_failure(const ToolOutput &t)
{
    Manifest m;
    set_tool_error(m, t);
    return m;
}

Entry
path_entry(const std::string &path, uint64_t size = 0)
{
    Entry e;
    e.path = path;
    e.size = size;
    return e;
}

bool
starts_with(const std::string &s, const char *prefix)
{
    return s.compare(0, std::string(prefix).size(), prefix) == 0;
}

} // namespace

bool
parse_ils(
    const std::string &text, std::vector<IlsLine> &out, std::string &error)
{
    const std::vector<std::string> lines = split_lines(text);
    if (lines.size() < 3 || lines[0] != "class|host|device|start_time" ||
        !starts_with(lines[1], "ils|") || !starts_with(lines[2], "st_ino|")) {
        error = "not ils output: no header";
        return false;
    }
    std::vector<IlsLine> v;
    for (size_t i = 3; i < lines.size(); ++i) {
        std::vector<std::string> f;
        std::istringstream in(lines[i]);
        std::string x;
        while (std::getline(in, x, '|')) {
            f.push_back(x);
        }
        IlsLine l;
        if (f.size() != 11 || !to_u64(f[0], l.inode) ||
            (f[1] != "a" && f[1] != "f")) {
            error = "cannot read the ils line '" + lines[i] + "'";
            return false;
        }
        l.alloc = f[1][0];
        v.push_back(l);
    }
    out = v;
    return true;
}

Manifest
ils_expected(const Manifest &ground_truth, const std::string &record,
    const std::string &deleted)
{
    if (ground_truth.has_error) {
        return error_manifest(
            ground_truth.error_code, ground_truth.error_detail);
    }
    WalkTruth t;
    std::string error;
    if (!parse_walk_truth(record, t, error)) {
        return error_manifest("walk-record", error);
    }
    for (const std::string &line : split_lines(deleted)) {
        const std::string d = trim(line);
        uint64_t inode = 0;
        if (d.empty()) {
            continue;
        }
        if (!to_u64(d, inode)) {
            return error_manifest(
                "corpus-deleted", "cannot read the deleted inode '" + d + "'");
        }
        if (t.used_free.count(inode) == 0) {
            return error_manifest("corpus-deleted",
                cat({"inode ", d,
                    ", which the Test corpus deleted, is not a free inode "
                    "that was once in use"}));
        }
    }
    std::set<uint64_t> allocated;
    for (const Entry &e : ground_truth.entries) {
        allocated.insert(e.inode);
    }
    for (const auto &i : t.internal_inodes) {
        allocated.insert(i.first);
    }
    if (t.virtual_inode != 0) {
        allocated.insert(t.virtual_inode);
    }
    Manifest m;
    for (uint64_t i : allocated) {
        m.entries.push_back(path_entry("/allocated/" + std::to_string(i)));
    }
    for (uint64_t i : t.used_free) {
        m.entries.push_back(path_entry("/used-free/" + std::to_string(i)));
    }
    m.entries.push_back(path_entry(
        "/inode-count", t.inode_count + (t.virtual_inode != 0 ? 1 : 0)));
    return m;
}

Manifest
ils_run(const ToolOutput &allocated, const ToolOutput &all,
    const ToolOutput &used_free)
{
    const struct {
        const char *name;
        const ToolOutput &t;
    } runs[] = {{"ils -a", allocated}, {"ils -e", all}, {"ils", used_free}};
    std::vector<IlsLine> lines[3];
    for (size_t i = 0; i < 3; ++i) {
        if (runs[i].t.status != 0) {
            return tool_failure(runs[i].t);
        }
        std::string error;
        if (!parse_ils(runs[i].t.out, lines[i], error)) {
            return error_manifest(
                "tool-output", std::string(runs[i].name) + ": " + error);
        }
    }
    Manifest m;
    for (const IlsLine &l : lines[0]) {
        m.entries.push_back(
            path_entry("/allocated/" + std::to_string(l.inode)));
    }
    for (const IlsLine &l : lines[2]) {
        m.entries.push_back(
            path_entry("/used-free/" + std::to_string(l.inode)));
    }
    m.entries.push_back(path_entry("/inode-count", lines[1].size()));
    return m;
}

Manifest
ifind_expected(const std::string &record)
{
    WalkTruth t;
    std::string error;
    if (!parse_walk_truth(record, t, error)) {
        return error_manifest("walk-record", error);
    }
    Manifest m;
    for (const IfindExpectation &x : t.ifind) {
        Entry e = path_entry(x.subject, x.block);
        e.inode = x.inode;
        m.entries.push_back(e);
    }
    return m;
}

Manifest
ifind_run(
    const Manifest &expected, const std::map<uint64_t, ToolOutput> &outputs)
{
    Manifest m;
    for (const Entry &x : expected.entries) {
        const auto it = outputs.find(x.size);
        if (it == outputs.end()) {
            continue; // never asked: missing
        }
        const ToolOutput &t = it->second;
        if (t.status != 0) {
            Manifest failed = tool_failure(t);
            if (failed.error_code == "fs-open") {
                return failed;
            }
            continue; // this block's question failed: missing
        }
        const std::string answer = trim(t.out);
        Entry e = x;
        if (answer == "Inode not found" || answer == "Meta Data") {
            e.inode = 0;
        }
        else if (!to_u64(answer, e.inode) || e.inode == 0) {
            return error_manifest("tool-output",
                "ifind -d " + std::to_string(x.size) + ": cannot read '" +
                    answer + "'");
        }
        m.entries.push_back(e);
    }
    return m;
}

Summary
blkls_expected(const std::string &record)
{
    WalkTruth t;
    std::string error;
    if (!parse_walk_truth(record, t, error)) {
        return error_summary(error_manifest("walk-record", error));
    }
    Summary s;
    s.fields.emplace_back("block-count", std::to_string(t.block_count));
    return s;
}

Summary
blkls_run(const ToolOutput &t, const std::string &lines)
{
    if (t.status != 0) {
        return error_summary(tool_failure(t));
    }
    const std::vector<std::string> kept = split_lines(t.out);
    uint64_t n = 0;
    static const std::regex LAST(R"(^[0-9]+\|[af]$)");
    if (kept.size() < 3 || kept[0] != "class|host|image|first_time|unit" ||
        !starts_with(kept[1], "blkls|") || kept[2] != "addr|alloc") {
        return error_summary(
            error_manifest("tool-output", "blkls -e -l: no header"));
    }
    if (!to_u64(trim(lines), n) || n < 3 ||
        (n > 3 && (kept.size() != 4 || !std::regex_match(kept[3], LAST)))) {
        return error_summary(error_manifest("tool-output",
            "blkls -e -l: cannot read its line count '" + trim(lines) +
                "' or last line"));
    }
    Summary s;
    s.fields.emplace_back("block-count", std::to_string(n - 3));
    return s;
}

} // namespace tskval
