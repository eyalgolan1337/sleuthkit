/*
 * Self-test of the Manifest format and the comparison step, run in gate 1 of
 * the validate command. Hand-made Manifests in, verdicts and reports out.
 */
#include "compare.h"
#include "compare_cli.h"
#include "manifest.h"

#include <cppunit/CompilerOutputter.h>
#include <cppunit/extensions/HelperMacros.h>
#include <cppunit/extensions/TestFactoryRegistry.h>
#include <cppunit/ui/text/TestRunner.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h> // rmdir; mkdtemp is in <cstdlib> on POSIX

using namespace tskval;

namespace {

// The header as documented, independent of MANIFEST_HEADER.
const std::string &
hdr()
{
    static const std::string h = "#tsk-manifest v1\n";
    return h;
}

Manifest
parse(const std::string &text)
{
    std::istringstream in(text);
    Manifest m;
    std::string err;
    if (!read_manifest(in, m, err)) {
        CPPUNIT_FAIL("read_manifest failed: " + err);
    }
    return m;
}

std::string
read_error(const std::string &text)
{
    std::istringstream in(text);
    Manifest m;
    std::string err;
    CPPUNIT_ASSERT_MESSAGE("expected a read error", !read_manifest(in, m, err));
    return err;
}

std::string
to_text(const Manifest &m)
{
    std::ostringstream out;
    write_manifest(out, m);
    return out.str();
}

void
assert_same(const Entry &a, const Entry &b)
{
    CPPUNIT_ASSERT_EQUAL(a.path, b.path);
    CPPUNIT_ASSERT(a.type == b.type);
    CPPUNIT_ASSERT_EQUAL(a.inode, b.inode);
    CPPUNIT_ASSERT_EQUAL(a.mode, b.mode);
    CPPUNIT_ASSERT_EQUAL(a.uid, b.uid);
    CPPUNIT_ASSERT_EQUAL(a.gid, b.gid);
    CPPUNIT_ASSERT_EQUAL(a.size, b.size);
    CPPUNIT_ASSERT_EQUAL(a.nlink, b.nlink);
    CPPUNIT_ASSERT_EQUAL(a.has_target, b.has_target);
    CPPUNIT_ASSERT_EQUAL(a.target, b.target);
    CPPUNIT_ASSERT_EQUAL(a.has_sha256, b.has_sha256);
    CPPUNIT_ASSERT_EQUAL(a.sha256, b.sha256);
    CPPUNIT_ASSERT(a.mtime == b.mtime);
    CPPUNIT_ASSERT(a.ctime == b.ctime);
    CPPUNIT_ASSERT_EQUAL(a.has_crtime, b.has_crtime);
    CPPUNIT_ASSERT(a.crtime == b.crtime);
    CPPUNIT_ASSERT_EQUAL(a.has_atime, b.has_atime);
    CPPUNIT_ASSERT(a.atime == b.atime);
}

} // namespace

class ManifestFormatTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(ManifestFormatTest);
    CPPUNIT_TEST(testReadsHandWrittenLines);
    CPPUNIT_TEST(testAwkwardPathsRoundTrip);
    CPPUNIT_TEST(testEncodedLineHasNoWhitespace);
    CPPUNIT_TEST(testSymlinkTargetsRoundTrip);
    CPPUNIT_TEST(testTimestampsRoundTrip);
    CPPUNIT_TEST(testErrorLineRoundTrips);
    CPPUNIT_TEST(testRejectsMalformedManifests);
    CPPUNIT_TEST_SUITE_END();

  public:
    void testErrorLineRoundTrips()
    {
        Manifest m;
        m.has_error = true;
        m.error_code = "fs-open";
        m.error_detail = "Cannot determine file system type\n(sda4)";
        const std::string text = to_text(m);
        CPPUNIT_ASSERT_EQUAL(hdr() +
                "!error\tfs-open\tCannot%20determine%20file%20system%20type%0A("
                "sda4)\n",
            text);
        const Manifest back = parse(text);
        CPPUNIT_ASSERT(back.has_error);
        CPPUNIT_ASSERT_EQUAL(m.error_code, back.error_code);
        CPPUNIT_ASSERT_EQUAL(m.error_detail, back.error_detail);
        CPPUNIT_ASSERT(back.entries.empty());
    }

    void testRejectsMalformedManifests()
    {
        const std::string good =
            "/a\tfile\t12\t0644\t0\t0\t0\t1\t-\t-\t0\t0\t-\t-";
        parse(hdr() + good + "\n# a comment\n"); // the baseline is valid

        struct Bad {
            std::string text;
            const char *expect; // substring of the error message
        };
        const Bad cases[] = {
            {"", "empty"},
            {good + "\n", "first line"},
            {"#tsk-manifest v2\n" + good + "\n", "first line"},
            {hdr() + "/a\tfile\t12\n", "line 2: expected 14"},
            {hdr() + good + "\textra\n", "expected 14"},
            {hdr() + "\n", "line 2"},
            {hdr() + good + "\n" + good + "\n", "line 3: duplicate path '/a'"},
            {hdr() + "/a b\tfile\t12\t0644\t0\t0\t0\t1\t-\t-\t0\t0\t-\t-\n",
                "bad path"},
            {hdr() + "a\tfile\t12\t0644\t0\t0\t0\t1\t-\t-\t0\t0\t-\t-\n",
                "bad path"},
            {hdr() + "/a/\tfile\t12\t0644\t0\t0\t0\t1\t-\t-\t0\t0\t-\t-\n",
                "bad path"},
            {hdr() + "/a//b\tfile\t12\t0644\t0\t0\t0\t1\t-\t-\t0\t0\t-\t-\n",
                "bad path"},
            {hdr() + "/a/../b\tfile\t12\t0644\t0\t0\t0\t1\t-\t-\t0\t0\t-\t-\n",
                "bad path"},
            {hdr() + "/a%2\tfile\t12\t0644\t0\t0\t0\t1\t-\t-\t0\t0\t-\t-\n",
                "bad path"},
            {hdr() + "/a%zz\tfile\t12\t0644\t0\t0\t0\t1\t-\t-\t0\t0\t-\t-\n",
                "bad path"},
            {hdr() + "/a%00\tfile\t12\t0644\t0\t0\t0\t1\t-\t-\t0\t0\t-\t-\n",
                "bad path"},
            // Not the one canonical spelling: needless escapes, lowercase hex.
            {hdr() + "/%61\tfile\t12\t0644\t0\t0\t0\t1\t-\t-\t0\t0\t-\t-\n",
                "bad path"},
            {hdr() + "/a%2Fb\tfile\t12\t0644\t0\t0\t0\t1\t-\t-\t0\t0\t-\t-\n",
                "bad path"},
            {hdr() + "/a%2Db\tfile\t12\t0644\t0\t0\t0\t1\t-\t-\t0\t0\t-\t-\n",
                "bad path"},
            {hdr() + "/a%c3%a9\tfile\t12\t0644\t0\t0\t0\t1\t-\t-\t0\t0\t-\t-\n",
                "bad path"},
            {hdr() +
                    "/a\tsymlink\t12\t0777\t0\t0\t0\t1\t%2e%2e\t-\t0\t0\t-\t-"
                    "\n",
                "bad symlink target"},
            {hdr() + "/a\tregular\t12\t0644\t0\t0\t0\t1\t-\t-\t0\t0\t-\t-\n",
                "bad type"},
            {hdr() + "/a\tfile\t-1\t0644\t0\t0\t0\t1\t-\t-\t0\t0\t-\t-\n",
                "bad inode"},
            {hdr() + "/a\tfile\t012\t0644\t0\t0\t0\t1\t-\t-\t0\t0\t-\t-\n",
                "bad inode"},
            {hdr() +
                    "/a\tfile\t18446744073709551616\t0644\t0\t0\t0\t1\t-\t-"
                    "\t0\t0\t-\t-\n",
                "bad inode"},
            {hdr() + "/a\tfile\t12\t644\t0\t0\t0\t1\t-\t-\t0\t0\t-\t-\n",
                "bad mode"},
            {hdr() + "/a\tfile\t12\t0844\t0\t0\t0\t1\t-\t-\t0\t0\t-\t-\n",
                "bad mode"},
            {hdr() +
                    "/a\tfile\t12\t0644\t4294967296\t0\t0\t1\t-\t-\t0\t0\t-\t-"
                    "\n",
                "bad uid"},
            {hdr() + "/a\tfile\t12\t0644\t0\t0\t0\t1\t-\tABCD\t0\t0\t-\t-\n",
                "bad SHA-256"},
            {hdr() +
                    "/a\tfile\t12\t0644\t0\t0\t0\t1\t-\t"
                    "A948904F2F0F479B8F8197694B30184B0D2ED1C1CD2A1EC0FB85D299A1"
                    "92A447"
                    "\t0\t0\t-\t-\n",
                "bad SHA-256"},
            {hdr() + "/a\tfile\t12\t0644\t0\t0\t0\t1\t-\t-\t-\t0\t-\t-\n",
                "bad mtime"},
            {hdr() + "/a\tfile\t12\t0644\t0\t0\t0\t1\t-\t-\t0\t1.5\t-\t-\n",
                "bad ctime"},
            {hdr() + good + "\r\n", "bad atime"},
            {hdr() + "!error\tFS\tx\n", "bad !error"},
            {hdr() + "!error\tfs-open\n", "bad !error"},
            {hdr() + "!error\ta\tx\n!error\tb\ty\n", "more than one"},
            {hdr() + "!warning\tx\n", "unknown directive"},
        };
        for (const Bad &b : cases) {
            const std::string err = read_error(b.text);
            CPPUNIT_ASSERT_MESSAGE("'" + err + "' lacks '" + b.expect + "'",
                err.find(b.expect) != std::string::npos);
        }
    }

    void testAwkwardPathsRoundTrip()
    {
        std::string
            every_byte; // 0x01..0xFF except '/' (NUL can't be in a name)
        for (int c = 1; c < 256; ++c) {
            if (c != '/') {
                every_byte += static_cast<char>(c);
            }
        }
        const std::string name255(255, 'n');
        const std::string utf8_255 =
            std::string(253, 'u') + "\xc3\xa9"; // ends in é
        const std::vector<std::string> paths = {"/", "/with space",
            "/ leading and trailing ", "/new\nline", "/tab\there", "/cr\rhere",
            "/percent%41", "/latin1-\xe9\xff", "/-", "/%2D", "/dir/" + name255,
            "/" + name255 + "/" + utf8_255, "/" + every_byte.substr(0, 127),
            "/" + every_byte.substr(127)};

        Manifest m;
        uint64_t ino = 100;
        for (const std::string &p : paths) {
            Entry e;
            e.path = p;
            e.type = EntryType::Dir;
            e.inode = ino++;
            e.mode = 0755;
            m.entries.push_back(e);
        }
        const Manifest back = parse(to_text(m));
        CPPUNIT_ASSERT_EQUAL(m.entries.size(), back.entries.size());
        for (size_t i = 0; i < m.entries.size(); ++i) {
            assert_same(m.entries[i], back.entries[i]);
        }
    }

    // Spaces, tabs and newlines in names never reach the line, so a Manifest
    // stays one entry per line and fields split on tabs.
    void testEncodedLineHasNoWhitespace()
    {
        Entry e;
        e.path = "/a b\tc\nd";
        e.type = EntryType::Symlink;
        e.has_target = true;
        e.target = "x y\n";
        const std::string line = format_entry(e);
        CPPUNIT_ASSERT_EQUAL(std::string::npos, line.find(' '));
        CPPUNIT_ASSERT_EQUAL(std::string::npos, line.find('\n'));
        CPPUNIT_ASSERT_EQUAL(
            std::string("/a%20b%09c%0Ad"), line.substr(0, line.find('\t')));
    }

    void testSymlinkTargetsRoundTrip()
    {
        const std::vector<std::string> targets = {
            "", "-", "%2D", std::string(4095, 't'), "../a b/\x80\n"};
        Manifest m;
        uint64_t ino = 1;
        for (const std::string &t : targets) {
            Entry e;
            e.path = "/l" + std::to_string(ino);
            e.inode = ino++;
            e.type = EntryType::Symlink;
            e.has_target = true;
            e.target = t;
            m.entries.push_back(e);
        }
        Entry none;
        none.path = "/no-target";
        none.type = EntryType::Symlink;
        m.entries.push_back(none);

        const Manifest back = parse(to_text(m));
        CPPUNIT_ASSERT_EQUAL(m.entries.size(), back.entries.size());
        for (size_t i = 0; i < m.entries.size(); ++i) {
            assert_same(m.entries[i], back.entries[i]);
        }
    }

    void testTimestampsRoundTrip()
    {
        struct Case {
            Timestamp t;
            const char *text;
        };
        const Case cases[] = {{{0, 0}, "0"}, {{0, 1}, "1"},
            {{1, 0}, "1000000000"}, {{-1, 999999999}, "-1"},
            {{-1, 0}, "-1000000000"}, {{-2, 500000000}, "-1500000000"},
            // after 2262, beyond int64 nanoseconds (XFS bigtime reaches 2486)
            {{16299260425LL, 999999999}, "16299260425999999999"},
            {{INT64_MAX, 999999999}, "9223372036854775807999999999"},
            {{INT64_MIN, 0}, "-9223372036854775808000000000"},
            {{INT64_MIN, 1}, "-9223372036854775807999999999"}};
        for (const Case &c : cases) {
            CPPUNIT_ASSERT_EQUAL(std::string(c.text), format_timestamp(c.t));
            Timestamp back;
            CPPUNIT_ASSERT_MESSAGE(c.text, parse_timestamp(c.text, back));
            CPPUNIT_ASSERT_MESSAGE(c.text, back == c.t);
        }
        Timestamp t;
        for (const char *bad : {"", "-", "-0", "01", "1.5", "+1", " 1",
                 "9223372036854775808000000000", "1e9"}) {
            CPPUNIT_ASSERT_MESSAGE(bad, !parse_timestamp(bad, t));
        }
    }

    // The example lines from test/vm/README.md, "Manifest format".
    void testReadsHandWrittenLines()
    {
        Manifest m = parse(hdr() +
            "/dir/a%20file\tfile\t131\t0644\t1000\t100\t11\t1\t-\t"
            "a948904f2f0f479b8f8197694b30184b0d2ed1c1cd2a1ec0fb85d299a192a447"
            "\t1700000000123456789\t1700000001000000000\t-"
            "\t1700000002000000000\n"
            "/dir/link\tsymlink\t132\t0777\t0\t0\t9\t1\t../target\t-"
            "\t-1500000000\t0\t1\t-\n");
        CPPUNIT_ASSERT(!m.has_error);
        CPPUNIT_ASSERT_EQUAL(size_t(2), m.entries.size());

        const Entry &f = m.entries[0];
        CPPUNIT_ASSERT_EQUAL(std::string("/dir/a file"), f.path);
        CPPUNIT_ASSERT(f.type == EntryType::File);
        CPPUNIT_ASSERT_EQUAL(uint64_t(131), f.inode);
        CPPUNIT_ASSERT_EQUAL(uint32_t(0644), f.mode);
        CPPUNIT_ASSERT_EQUAL(uint32_t(1000), f.uid);
        CPPUNIT_ASSERT_EQUAL(uint32_t(100), f.gid);
        CPPUNIT_ASSERT_EQUAL(uint64_t(11), f.size);
        CPPUNIT_ASSERT_EQUAL(uint64_t(1), f.nlink);
        CPPUNIT_ASSERT(!f.has_target);
        CPPUNIT_ASSERT(f.has_sha256);
        CPPUNIT_ASSERT_EQUAL(std::string("a948904f2f0f479b8f8197694b30184b0d2ed"
                                         "1c1cd2a1ec0fb85d299a192a447"),
            f.sha256);
        CPPUNIT_ASSERT_EQUAL(int64_t(1700000000), f.mtime.sec);
        CPPUNIT_ASSERT_EQUAL(uint32_t(123456789), f.mtime.nsec);
        CPPUNIT_ASSERT_EQUAL(int64_t(1700000001), f.ctime.sec);
        CPPUNIT_ASSERT_EQUAL(uint32_t(0), f.ctime.nsec);
        CPPUNIT_ASSERT(!f.has_crtime);
        CPPUNIT_ASSERT(f.has_atime);
        CPPUNIT_ASSERT_EQUAL(int64_t(1700000002), f.atime.sec);

        const Entry &l = m.entries[1];
        CPPUNIT_ASSERT(l.type == EntryType::Symlink);
        CPPUNIT_ASSERT_EQUAL(uint32_t(0777), l.mode);
        CPPUNIT_ASSERT(l.has_target);
        CPPUNIT_ASSERT_EQUAL(std::string("../target"), l.target);
        CPPUNIT_ASSERT(!l.has_sha256);
        // -1.5 s: 1969-12-31T23:59:58.5Z
        CPPUNIT_ASSERT_EQUAL(int64_t(-2), l.mtime.sec);
        CPPUNIT_ASSERT_EQUAL(uint32_t(500000000), l.mtime.nsec);
        CPPUNIT_ASSERT_EQUAL(int64_t(0), l.ctime.sec);
        CPPUNIT_ASSERT(l.has_crtime);
        CPPUNIT_ASSERT_EQUAL(int64_t(0), l.crtime.sec);
        CPPUNIT_ASSERT_EQUAL(uint32_t(1), l.crtime.nsec);
        CPPUNIT_ASSERT(!l.has_atime);
    }
};
CPPUNIT_TEST_SUITE_REGISTRATION(ManifestFormatTest);

namespace {

const char *const FIELD_NAMES[] = {"path", "type", "inode", "mode", "uid",
    "gid", "size", "nlink", "target", "sha256", "mtime", "ctime", "crtime",
    "atime"};
const char HASH_A[] =
    "a948904f2f0f479b8f8197694b30184b0d2ed1c1cd2a1ec0fb85d299a192a447";
const char HASH_B[] =
    "b5bb9d8014a0f9b1d61e21e796d78dccdf1352f23cd32812f4850b878ae4944c";
// 2023-11-14T22:13:20Z, and a run time 30 days later.
const char OLD[] = "1700000000000000000";
const Timestamp RUN_TIME = {1700000000 + 30 * 86400, 0};

// A regular-file Manifest line with every timestamp OLD, then each
// (field, value) pair applied.
std::string
entry(const std::string &path,
    std::initializer_list<std::pair<std::string, std::string>> changes = {})
{
    std::vector<std::string> f = {path, "file", "12", "0644", "1000", "100",
        "5", "1", "-", HASH_A, OLD, OLD, OLD, OLD};
    for (const auto &c : changes) {
        size_t i = 0;
        while (i < f.size() && c.first != FIELD_NAMES[i]) {
            ++i;
        }
        CPPUNIT_ASSERT_MESSAGE("unknown field " + c.first, i < f.size());
        f[i] = c.second;
    }
    std::string line;
    for (size_t i = 0; i < f.size(); ++i) {
        line += (i ? "\t" : "") + f[i];
    }
    return line + "\n";
}

// A Manifest from entry() lines.
Manifest
manifest(std::initializer_list<std::string> lines)
{
    std::string text = hdr();
    for (const std::string &l : lines) {
        text += l;
    }
    return parse(text);
}

// A small tree used as Ground truth by most comparison tests.
Manifest
tree(std::initializer_list<std::string> extra = {})
{
    std::string text = hdr() +
        entry("/",
            {{"type", "dir"}, {"inode", "2"}, {"mode", "0755"}, {"sha256", "-"},
                {"nlink", "3"}, {"size", "4096"}}) +
        entry("/etc",
            {{"type", "dir"}, {"inode", "11"}, {"mode", "0755"},
                {"sha256", "-"}, {"nlink", "2"}, {"size", "4096"}}) +
        entry("/etc/hosts", {{"inode", "13"}}) +
        entry("/link",
            {{"type", "symlink"}, {"inode", "14"}, {"mode", "0777"},
                {"sha256", "-"}, {"target", "etc/hosts"}});
    for (const std::string &l : extra) {
        text += l;
    }
    return parse(text);
}

CompareOptions
strict()
{
    return CompareOptions();
}

CompareOptions
best_effort(std::initializer_list<std::string> volatile_paths = {})
{
    CompareOptions o;
    o.mode = Mode::BestEffort;
    o.volatile_paths = volatile_paths;
    o.run_time = RUN_TIME;
    return o;
}

std::string
report(const CompareResult &r)
{
    std::ostringstream out;
    write_report(out, r);
    return out.str();
}

// The finding for (path, difference); fails the test if there isn't exactly one.
Finding
finding(const CompareResult &r, const std::string &path,
    const std::string &difference)
{
    const Finding *found = nullptr;
    int n = 0;
    for (const Finding &f : r.findings) {
        if (f.path == path && f.difference == difference) {
            found = &f;
            ++n;
        }
    }
    CPPUNIT_ASSERT_EQUAL_MESSAGE(
        "findings for " + path + " " + difference + " in:\n" + report(r), 1, n);
    return *found;
}

void
assert_verdict(Verdict expected, const CompareResult &r)
{
    CPPUNIT_ASSERT_EQUAL_MESSAGE(report(r), std::string(verdict_name(expected)),
        std::string(verdict_name(r.verdict())));
}

// Exactly one difference, on (path, difference), with the given severity.
void
assert_only(const CompareResult &r, Severity severity, const std::string &path,
    const std::string &difference)
{
    const Finding f = finding(r, path, difference);
    CPPUNIT_ASSERT_MESSAGE(report(r), f.severity == severity);
    CPPUNIT_ASSERT_EQUAL_MESSAGE(
        report(r), size_t(severity == Severity::Failure ? 1 : 0), r.failures);
    CPPUNIT_ASSERT_EQUAL_MESSAGE(
        report(r), size_t(severity == Severity::Warning ? 1 : 0), r.warnings);
    CPPUNIT_ASSERT_EQUAL_MESSAGE(
        report(r), size_t(severity == Severity::Known ? 1 : 0), r.known);
}

} // namespace

class StrictCompareTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(StrictCompareTest);
    CPPUNIT_TEST(testIdenticalManifestsPass);
    CPPUNIT_TEST(testMissingEntryFails);
    CPPUNIT_TEST(testExtraEntryFails);
    CPPUNIT_TEST(testWrongHashFails);
    CPPUNIT_TEST(testWrongSizeFails);
    CPPUNIT_TEST(testOneNanosecondMtimeDifferenceFails);
    CPPUNIT_TEST(testEveryComparedFieldFails);
    CPPUNIT_TEST(testAtimeDifferenceIsIgnored);
    CPPUNIT_TEST(testEntryOrderDoesNotMatter);
    CPPUNIT_TEST_SUITE_END();

  public:
    void testIdenticalManifestsPass()
    {
        const CompareResult r = compare_manifests(tree(), tree(), strict());
        assert_verdict(Verdict::Pass, r);
        CPPUNIT_ASSERT_EQUAL(size_t(4), r.ground_truth_entries);
        CPPUNIT_ASSERT_EQUAL(size_t(4), r.run_entries);
        CPPUNIT_ASSERT_EQUAL(size_t(0), r.failures + r.warnings + r.known);
        CPPUNIT_ASSERT(report(r).find("PASS") != std::string::npos);
    }

    void testMissingEntryFails()
    {
        const Manifest gt = tree({entry("/etc/passwd")});
        const CompareResult r = compare_manifests(gt, tree(), strict());
        assert_verdict(Verdict::Fail, r);
        assert_only(r, Severity::Failure, "/etc/passwd", "missing");
    }

    void testExtraEntryFails()
    {
        const Manifest run = tree({entry("/etc/passwd")});
        const CompareResult r = compare_manifests(tree(), run, strict());
        assert_verdict(Verdict::Fail, r);
        assert_only(r, Severity::Failure, "/etc/passwd", "extra");
    }

    void testWrongHashFails()
    {
        const Manifest gt = tree({entry("/f")});
        const Manifest run = tree({entry("/f", {{"sha256", HASH_B}})});
        const CompareResult r = compare_manifests(gt, run, strict());
        assert_verdict(Verdict::Fail, r);
        assert_only(r, Severity::Failure, "/f", "sha256");
        // The report shows both values.
        const std::string text = report(r);
        CPPUNIT_ASSERT(text.find(HASH_A) != std::string::npos);
        CPPUNIT_ASSERT(text.find(HASH_B) != std::string::npos);
    }

    void testWrongSizeFails()
    {
        const Manifest gt = tree({entry("/f")});
        const Manifest run = tree({entry("/f", {{"size", "6"}})});
        const CompareResult r = compare_manifests(gt, run, strict());
        assert_verdict(Verdict::Fail, r);
        assert_only(r, Severity::Failure, "/f", "size");
    }

    void testOneNanosecondMtimeDifferenceFails()
    {
        const Manifest gt = tree({entry("/f")});
        const Manifest run =
            tree({entry("/f", {{"mtime", "1700000000000000001"}})});
        const CompareResult r = compare_manifests(gt, run, strict());
        assert_verdict(Verdict::Fail, r);
        assert_only(r, Severity::Failure, "/f", "mtime");
    }

    // Each compared field on its own, including present versus absent.
    void testEveryComparedFieldFails()
    {
        const std::pair<const char *, const char *> changes[] = {
            {"type", "dir"}, {"inode", "99"}, {"mode", "0600"}, {"uid", "0"},
            {"gid", "0"}, {"size", "0"}, {"nlink", "2"}, {"target", "x"},
            {"sha256", "-"}, {"mtime", "0"}, {"ctime", "1700000000000000001"},
            {"crtime", "-"}};
        CPPUNIT_ASSERT_EQUAL(
            NUM_COMPARED_FIELDS, sizeof(changes) / sizeof(changes[0]));
        for (const auto &c : changes) {
            const Manifest gt = tree({entry("/f")});
            const Manifest run = tree({entry("/f", {{c.first, c.second}})});
            const CompareResult r = compare_manifests(gt, run, strict());
            assert_verdict(Verdict::Fail, r);
            assert_only(r, Severity::Failure, "/f", c.first);
        }
    }

    void testAtimeDifferenceIsIgnored()
    {
        const Manifest gt = tree({entry("/f"), entry("/g")});
        const Manifest run =
            tree({entry("/f", {{"atime", "1800000000000000000"}}),
                entry("/g", {{"atime", "-"}})});
        const CompareResult r = compare_manifests(gt, run, strict());
        assert_verdict(Verdict::Pass, r);
        CPPUNIT_ASSERT(r.findings.empty());
    }

    void testEntryOrderDoesNotMatter()
    {
        const Manifest gt = manifest({entry("/a"), entry("/b"), entry("/c")});
        const Manifest run = manifest({entry("/c"), entry("/a"), entry("/b")});
        assert_verdict(Verdict::Pass, compare_manifests(gt, run, strict()));
    }
};
CPPUNIT_TEST_SUITE_REGISTRATION(StrictCompareTest);

namespace {
// Relative to RUN_TIME (1702592000 s).
const char ONE_MINUTE_BEFORE[] = "1702591940000000000";
const char TEN_MINUTES_BEFORE[] = "1702591400000000000";
const char TEN_MINUTES_AND_1NS_BEFORE[] = "1702591399999999999";
const char ONE_MINUTE_AFTER[] = "1702592060000000000";
} // namespace

class BestEffortCompareTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(BestEffortCompareTest);
    CPPUNIT_TEST(testVolatilePathDifferenceWarnsButFailsStrict);
    CPPUNIT_TEST(testVolatilePathCoversMissingAndExtraEntries);
    CPPUNIT_TEST(testVolatilePathMatchesWholeComponents);
    CPPUNIT_TEST(testRecentCtimeDifferenceWarnsButFailsStrict);
    CPPUNIT_TEST(testCtimeChangedAfterGroundTruthWarns);
    CPPUNIT_TEST(testRecentCtimeCoversMissingAndExtraEntries);
    CPPUNIT_TEST(testCtimeAfterTheRunToleratesNothing);
    CPPUNIT_TEST(testCtimeWindowIsTenMinutes);
    CPPUNIT_TEST(testOldCtimeOutsideVolatilePathsFails);
    CPPUNIT_TEST(testIdenticalManifestsPassWithoutWarnings);
    CPPUNIT_TEST(testReadsVolatilePathList);
    CPPUNIT_TEST_SUITE_END();

  public:
    void testVolatilePathDifferenceWarnsButFailsStrict()
    {
        const Manifest gt =
            tree({entry("/var", {{"type", "dir"}, {"sha256", "-"}}),
                entry("/var/log", {{"type", "dir"}, {"sha256", "-"}}),
                entry("/var/log/messages")});
        const Manifest run =
            tree({entry("/var", {{"type", "dir"}, {"sha256", "-"}}),
                entry("/var/log", {{"type", "dir"}, {"sha256", "-"}}),
                entry("/var/log/messages", {{"size", "900"}})});

        const CompareResult be =
            compare_manifests(gt, run, best_effort({"/var/log"}));
        assert_verdict(Verdict::Warnings, be);
        assert_only(be, Severity::Warning, "/var/log/messages", "size");
        // The warning says why it was tolerated.
        CPPUNIT_ASSERT(
            report(be).find("Volatile path /var/log") != std::string::npos);

        CompareOptions s = strict();
        s.volatile_paths = {"/var/log"}; // ignored in Strict mode
        const CompareResult st = compare_manifests(gt, run, s);
        assert_verdict(Verdict::Fail, st);
        assert_only(st, Severity::Failure, "/var/log/messages", "size");
    }

    void testVolatilePathCoversMissingAndExtraEntries()
    {
        const Manifest gt =
            tree({entry("/tmp", {{"type", "dir"}, {"sha256", "-"}}),
                entry("/tmp/old")});
        const Manifest run =
            tree({entry("/tmp", {{"type", "dir"}, {"sha256", "-"}}),
                entry("/tmp/new")});
        const CompareResult r =
            compare_manifests(gt, run, best_effort({"/tmp"}));
        assert_verdict(Verdict::Warnings, r);
        CPPUNIT_ASSERT_EQUAL(size_t(2), r.warnings);
        CPPUNIT_ASSERT(
            finding(r, "/tmp/old", "missing").severity == Severity::Warning);
        CPPUNIT_ASSERT(
            finding(r, "/tmp/new", "extra").severity == Severity::Warning);
    }

    void testVolatilePathMatchesWholeComponents()
    {
        const Manifest gt = tree({entry("/var/log"), entry("/var/logger")});
        const Manifest run = tree({entry("/var/log", {{"size", "1"}}),
            entry("/var/logger", {{"size", "1"}})});
        const CompareResult r =
            compare_manifests(gt, run, best_effort({"/var/log"}));
        assert_verdict(Verdict::Fail, r);
        CPPUNIT_ASSERT(
            finding(r, "/var/log", "size").severity == Severity::Warning);
        CPPUNIT_ASSERT(
            finding(r, "/var/logger", "size").severity == Severity::Failure);
    }

    void testRecentCtimeDifferenceWarnsButFailsStrict()
    {
        const Manifest gt =
            tree({entry("/etc/resolv.conf", {{"ctime", ONE_MINUTE_BEFORE}})});
        const Manifest run = tree({entry("/etc/resolv.conf",
            {{"ctime", ONE_MINUTE_BEFORE}, {"sha256", HASH_B}})});

        const CompareResult be = compare_manifests(gt, run, best_effort());
        assert_verdict(Verdict::Warnings, be);
        assert_only(be, Severity::Warning, "/etc/resolv.conf", "sha256");
        CPPUNIT_ASSERT(report(be).find("ctime") != std::string::npos);

        const CompareResult st = compare_manifests(gt, run, strict());
        assert_verdict(Verdict::Fail, st);
        assert_only(st, Severity::Failure, "/etc/resolv.conf", "sha256");
    }

    // Changed between Ground truth capture and the end of the run: only the
    // run's ctime is recent.
    void testCtimeChangedAfterGroundTruthWarns()
    {
        const Manifest gt = tree({entry("/etc/f")});
        const Manifest run = tree({entry("/etc/f",
            {{"ctime", ONE_MINUTE_BEFORE}, {"mtime", ONE_MINUTE_BEFORE},
                {"size", "7"}})});
        const CompareResult r = compare_manifests(gt, run, best_effort());
        assert_verdict(Verdict::Warnings, r);
        CPPUNIT_ASSERT_EQUAL(size_t(3), r.warnings);
        CPPUNIT_ASSERT(
            finding(r, "/etc/f", "ctime").severity == Severity::Warning);
        CPPUNIT_ASSERT(
            finding(r, "/etc/f", "mtime").severity == Severity::Warning);
        CPPUNIT_ASSERT(
            finding(r, "/etc/f", "size").severity == Severity::Warning);
    }

    void testRecentCtimeCoversMissingAndExtraEntries()
    {
        const Manifest gt =
            tree({entry("/gone", {{"ctime", ONE_MINUTE_BEFORE}}),
                entry("/gone-old")});
        const Manifest run = tree(
            {entry("/new", {{"ctime", ONE_MINUTE_BEFORE}}), entry("/new-old")});
        const CompareResult r = compare_manifests(gt, run, best_effort());
        CPPUNIT_ASSERT(
            finding(r, "/gone", "missing").severity == Severity::Warning);
        CPPUNIT_ASSERT(
            finding(r, "/new", "extra").severity == Severity::Warning);
        CPPUNIT_ASSERT(
            finding(r, "/gone-old", "missing").severity == Severity::Failure);
        CPPUNIT_ASSERT(
            finding(r, "/new-old", "extra").severity == Severity::Failure);
        assert_verdict(Verdict::Fail, r);
    }

    // A ctime later than the run's end can't come from the kernel, only from
    // a misread one (e.g. a bigtime decoding bug), so it tolerates nothing.
    void testCtimeAfterTheRunToleratesNothing()
    {
        const Manifest gt = tree({entry("/etc/f"), entry("/etc/g")});
        const Manifest run =
            tree({entry("/etc/f", {{"ctime", ONE_MINUTE_AFTER}, {"size", "7"}}),
                entry("/etc/g", {{"ctime", "13569465600000000000"}}),
                entry("/etc/new", {{"ctime", ONE_MINUTE_AFTER}})});
        const CompareResult r = compare_manifests(gt, run, best_effort());
        assert_verdict(Verdict::Fail, r);
        CPPUNIT_ASSERT(
            finding(r, "/etc/f", "size").severity == Severity::Failure);
        CPPUNIT_ASSERT(
            finding(r, "/etc/f", "ctime").severity == Severity::Failure);
        CPPUNIT_ASSERT(
            finding(r, "/etc/g", "ctime").severity == Severity::Failure);
        CPPUNIT_ASSERT(
            finding(r, "/etc/new", "extra").severity == Severity::Failure);
        CPPUNIT_ASSERT_EQUAL(size_t(0), r.warnings);
    }

    void testCtimeWindowIsTenMinutes()
    {
        const Manifest gt =
            tree({entry("/edge", {{"ctime", TEN_MINUTES_BEFORE}}),
                entry("/outside", {{"ctime", TEN_MINUTES_AND_1NS_BEFORE}})});
        const Manifest run = tree(
            {entry("/edge", {{"ctime", TEN_MINUTES_BEFORE}, {"size", "1"}}),
                entry("/outside",
                    {{"ctime", TEN_MINUTES_AND_1NS_BEFORE}, {"size", "1"}})});
        const CompareResult r = compare_manifests(gt, run, best_effort());
        CPPUNIT_ASSERT(
            finding(r, "/edge", "size").severity == Severity::Warning);
        CPPUNIT_ASSERT(
            finding(r, "/outside", "size").severity == Severity::Failure);

        // The other end: exactly the run's timestamp, and 1 ns after it.
        const Manifest gt2 = tree({entry("/at-end"), entry("/after-end")});
        const Manifest run2 =
            tree({entry("/at-end", {{"ctime", "1702592000000000000"}}),
                entry("/after-end", {{"ctime", "1702592000000000001"}})});
        const CompareResult r2 = compare_manifests(gt2, run2, best_effort());
        CPPUNIT_ASSERT(
            finding(r2, "/at-end", "ctime").severity == Severity::Warning);
        CPPUNIT_ASSERT(
            finding(r2, "/after-end", "ctime").severity == Severity::Failure);
    }

    void testOldCtimeOutsideVolatilePathsFails()
    {
        const Manifest gt = tree({entry("/etc/f")});
        const Manifest run = tree({entry("/etc/f", {{"sha256", HASH_B}})});
        const CompareResult r =
            compare_manifests(gt, run, best_effort({"/var/log", "/tmp"}));
        assert_verdict(Verdict::Fail, r);
        assert_only(r, Severity::Failure, "/etc/f", "sha256");
    }

    void testIdenticalManifestsPassWithoutWarnings()
    {
        const Manifest gt =
            tree({entry("/tmp/x", {{"ctime", ONE_MINUTE_BEFORE}})});
        const CompareResult r =
            compare_manifests(gt, gt, best_effort({"/tmp"}));
        assert_verdict(Verdict::Pass, r);
        CPPUNIT_ASSERT(r.findings.empty());
    }

    void testReadsVolatilePathList()
    {
        std::istringstream in("# Volatile paths for /\n\n/var/log\n/var/lib\n"
                              "/with%20space\n/\n");
        std::vector<std::string> paths;
        std::string err;
        CPPUNIT_ASSERT_MESSAGE(err, read_volatile_paths(in, paths, err));
        const std::vector<std::string> expected = {
            "/var/log", "/var/lib", "/with space", "/"};
        CPPUNIT_ASSERT(expected == paths);

        for (const char *bad :
            {"var/log\n", "/var/log/\n", "/a b\n", "/x%zz\n"}) {
            std::istringstream b(std::string("# c\n") + bad);
            paths.clear();
            CPPUNIT_ASSERT_MESSAGE(bad, !read_volatile_paths(b, paths, err));
            CPPUNIT_ASSERT_MESSAGE(
                err, err.find("line 2") != std::string::npos);
        }
    }
};
CPPUNIT_TEST_SUITE_REGISTRATION(BestEffortCompareTest);

namespace {

std::vector<KnownFailure>
known_list(const std::string &text)
{
    std::istringstream in(text);
    std::vector<KnownFailure> out;
    std::string err;
    if (!read_known_failures(in, out, err)) {
        CPPUNIT_FAIL("read_known_failures failed: " + err);
    }
    return out;
}

CompareOptions
with_known(CompareOptions o, const std::string &list)
{
    o.check = "strict-guest";
    o.partition = "sda6";
    o.known_failures = known_list(list);
    return o;
}

Manifest
failed_run(const std::string &code)
{
    Manifest m;
    m.has_error = true;
    m.error_code = code;
    m.error_detail = "Cannot determine file system type";
    return m;
}

} // namespace

class KnownFailureTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(KnownFailureTest);
    CPPUNIT_TEST(testReadsKnownFailureList);
    CPPUNIT_TEST(testRejectsMalformedKnownFailureLists);
    CPPUNIT_TEST(testListedKnownFailureIsTolerated);
    CPPUNIT_TEST(testListedKnownFailureThatNowPassesFails);
    CPPUNIT_TEST(testOnlyThisCheckAndPartitionApply);
    CPPUNIT_TEST(testListedMissingEntryIsTolerated);
    CPPUNIT_TEST(testAllEntriesSubjectCoversEveryPath);
    CPPUNIT_TEST(testAllEntriesSubjectThatNowPassesFails);
    CPPUNIT_TEST(testKnownFailureTakesPrecedenceInBestEffort);
    CPPUNIT_TEST(testUnlistedRunErrorFails);
    CPPUNIT_TEST(testListedFileSystemNotRecognisedIsTolerated);
    CPPUNIT_TEST(testListedRunErrorThatNowPassesFails);
    CPPUNIT_TEST(testRunErrorWithAnotherCodeFails);
    CPPUNIT_TEST(testGroundTruthErrorFails);
    CPPUNIT_TEST_SUITE_END();

  public:
    void testReadsKnownFailureList()
    {
        const std::vector<KnownFailure> k = known_list(
            "# check partition subject difference comment\n"
            "\n"
            "strict-guest\tsda4\t*\terror:fs-open\tfile system not recognised\n"
            "strict-host  sda6  /lost+found/a%20b  crtime  ext4 crtime  "
            "(#99)\n"
            "fls sda6 * extra\n");
        CPPUNIT_ASSERT_EQUAL(size_t(3), k.size());
        CPPUNIT_ASSERT_EQUAL(std::string("strict-guest"), k[0].check);
        CPPUNIT_ASSERT_EQUAL(std::string("sda4"), k[0].partition);
        CPPUNIT_ASSERT(k[0].all_entries);
        CPPUNIT_ASSERT_EQUAL(std::string("error:fs-open"), k[0].difference);
        CPPUNIT_ASSERT_EQUAL(
            std::string("file system not recognised"), k[0].comment);
        CPPUNIT_ASSERT_EQUAL(3, k[0].line);
        CPPUNIT_ASSERT(!k[1].all_entries);
        CPPUNIT_ASSERT_EQUAL(std::string("/lost+found/a b"), k[1].path);
        CPPUNIT_ASSERT_EQUAL(std::string("crtime"), k[1].difference);
        CPPUNIT_ASSERT_EQUAL(std::string("ext4 crtime  (#99)"), k[1].comment);
        CPPUNIT_ASSERT_EQUAL(std::string("extra"), k[2].difference);
        CPPUNIT_ASSERT_EQUAL(std::string(""), k[2].comment);
    }

    void testRejectsMalformedKnownFailureLists()
    {
        const char *const bad[] = {
            "strict-guest sda6 /a\n",                 // too few fields
            "strict-guest sda6 /a atime\n",           // atime is never compared
            "strict-guest sda6 /a colour\n",          // unknown difference
            "strict-guest sda6 a size\n",             // not a path
            "strict-guest sda6 /a/ size\n",           // not a canonical path
            "strict-guest sda6 /a error:fs-open\n",   // run errors take "*"
            "strict-guest sda6 * error:\n",           // empty error code
            "strict-guest sda6 * error:FS\n",         // bad error code
            "strict-guest sda6 /lost%2Bfound size\n", // not canonical
            "* sda6 /a size\n",                       // checks are named
            "strict-guest * /a size\n",               // partitions are named
            "strict-guest sda6 /a size\nstrict-guest sda6 /a size again\n",
        };
        for (const char *text : bad) {
            std::istringstream in(text);
            std::vector<KnownFailure> out;
            std::string err;
            CPPUNIT_ASSERT_MESSAGE(text, !read_known_failures(in, out, err));
            CPPUNIT_ASSERT_MESSAGE(err, err.find("line ") == 0);
        }
    }

    void testListedKnownFailureIsTolerated()
    {
        const Manifest gt = tree({entry("/f")});
        const Manifest run = tree({entry("/f", {{"crtime", "-"}})});
        const CompareResult r = compare_manifests(gt, run,
            with_known(
                strict(), "strict-guest sda6 /f crtime TSK lacks crtime\n"));
        assert_verdict(Verdict::Pass, r);
        assert_only(r, Severity::Known, "/f", "crtime");
        CPPUNIT_ASSERT(report(r).find("TSK lacks crtime") != std::string::npos);
    }

    void testListedKnownFailureThatNowPassesFails()
    {
        const CompareResult r =
            compare_manifests(tree({entry("/f")}), tree({entry("/f")}),
                with_known(strict(),
                    "strict-guest sda6 /f crtime TSK lacks crtime\n"));
        assert_verdict(Verdict::Fail, r);
        assert_only(r, Severity::Failure, "/f", "known-failure-passes");
        CPPUNIT_ASSERT(report(r).find("line 1") != std::string::npos);
    }

    // Lines for other checks or partitions neither tolerate nor go stale.
    void testOnlyThisCheckAndPartitionApply()
    {
        const Manifest gt = tree({entry("/f")});
        const Manifest run = tree({entry("/f", {{"crtime", "-"}})});
        const CompareResult r = compare_manifests(gt, run,
            with_known(strict(),
                "strict-host sda6 /f crtime\n"
                "strict-guest sda5 /f crtime\n"
                "strict-guest sda5 /g size\n"));
        assert_verdict(Verdict::Fail, r);
        assert_only(r, Severity::Failure, "/f", "crtime");
    }

    void testListedMissingEntryIsTolerated()
    {
        const Manifest gt =
            tree({entry("/lost+found", {{"type", "dir"}, {"sha256", "-"}})});
        const CompareResult r = compare_manifests(gt, tree(),
            with_known(strict(), "strict-guest sda6 /lost+found missing\n"));
        assert_verdict(Verdict::Pass, r);
        assert_only(r, Severity::Known, "/lost+found", "missing");
    }

    void testAllEntriesSubjectCoversEveryPath()
    {
        const Manifest gt = tree({entry("/f"), entry("/g")});
        const Manifest run = tree({entry("/f", {{"crtime", "-"}}),
            entry("/g", {{"crtime", "-"}, {"size", "9"}})});
        const CompareResult r = compare_manifests(
            gt, run, with_known(strict(), "strict-guest sda6 * crtime\n"));
        assert_verdict(Verdict::Fail, r);
        CPPUNIT_ASSERT_EQUAL(size_t(2), r.known);
        CPPUNIT_ASSERT(finding(r, "/f", "crtime").severity == Severity::Known);
        CPPUNIT_ASSERT(finding(r, "/g", "crtime").severity == Severity::Known);
        CPPUNIT_ASSERT(finding(r, "/g", "size").severity == Severity::Failure);
        CPPUNIT_ASSERT_EQUAL(size_t(1), r.failures);
    }

    void testAllEntriesSubjectThatNowPassesFails()
    {
        const CompareResult r = compare_manifests(tree(), tree(),
            with_known(strict(), "strict-guest sda6 * crtime\n"));
        assert_verdict(Verdict::Fail, r);
        assert_only(r, Severity::Failure, "", "known-failure-passes");
    }

    // A listed difference that Best-effort would also tolerate counts as the
    // Known failure, so the list entry is not reported as stale.
    void testKnownFailureTakesPrecedenceInBestEffort()
    {
        const Manifest gt = tree({entry("/tmp/f")});
        const Manifest run = tree({entry("/tmp/f", {{"size", "9"}})});
        CompareOptions o = with_known(
            best_effort({"/tmp"}), "strict-guest sda6 /tmp/f size\n");
        const CompareResult r = compare_manifests(gt, run, o);
        assert_verdict(Verdict::Pass, r);
        assert_only(r, Severity::Known, "/tmp/f", "size");
    }

    void testUnlistedRunErrorFails()
    {
        const CompareResult r =
            compare_manifests(tree(), failed_run("fs-open"), strict());
        assert_verdict(Verdict::Fail, r);
        assert_only(r, Severity::Failure, "", "error");
        CPPUNIT_ASSERT(report(r).find("Cannot determine file system type") !=
            std::string::npos);
        // A failed run is not compared entry by entry.
        CPPUNIT_ASSERT_EQUAL(size_t(1), r.findings.size());
    }

    void testListedFileSystemNotRecognisedIsTolerated()
    {
        const CompareResult r = compare_manifests(tree(), failed_run("fs-open"),
            with_known(strict(),
                "strict-guest sda6 * error:fs-open file system not recognised\n"
                "strict-guest sda6 /f crtime\n"));
        assert_verdict(Verdict::Pass, r);
        CPPUNIT_ASSERT(finding(r, "", "error").severity == Severity::Known);
        CPPUNIT_ASSERT_EQUAL(size_t(1), r.known);
        // Entry-level lines can't be checked against a failed run: noted, not stale.
        CPPUNIT_ASSERT(
            finding(r, "/f", "not-evaluated").severity == Severity::Note);
        CPPUNIT_ASSERT_EQUAL(size_t(0), r.failures);
    }

    void testListedRunErrorThatNowPassesFails()
    {
        const CompareResult r = compare_manifests(tree(), tree(),
            with_known(strict(),
                "strict-guest sda6 * error:fs-open file system not "
                "recognised\n"));
        assert_verdict(Verdict::Fail, r);
        assert_only(r, Severity::Failure, "", "known-failure-passes");
    }

    void testRunErrorWithAnotherCodeFails()
    {
        const CompareResult r = compare_manifests(tree(), failed_run("walk"),
            with_known(strict(), "strict-guest sda6 * error:fs-open\n"));
        assert_verdict(Verdict::Fail, r);
        CPPUNIT_ASSERT(finding(r, "", "error").severity == Severity::Failure);
        CPPUNIT_ASSERT(finding(r, "", "known-failure-passes").severity ==
            Severity::Failure);
    }

    void testGroundTruthErrorFails()
    {
        const CompareResult r = compare_manifests(failed_run("capture"), tree(),
            with_known(strict(), "strict-guest sda6 * error:capture\n"));
        assert_verdict(Verdict::Fail, r);
        CPPUNIT_ASSERT(
            finding(r, "", "ground-truth-error").severity == Severity::Failure);
    }
};
CPPUNIT_TEST_SUITE_REGISTRATION(KnownFailureTest);

namespace {

// A scratch directory for the files the command line reads; removed with
// everything written through it.
class TempDir {
  public:
    TempDir()
    {
        const char *base = std::getenv("TMPDIR");
        std::string tmpl = std::string(base && *base ? base : "/tmp") +
            "/manifest-selftest.XXXXXX";
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        CPPUNIT_ASSERT_MESSAGE(
            "mkdtemp failed", mkdtemp(buf.data()) != nullptr);
        dir_ = buf.data();
    }
    ~TempDir()
    {
        for (const std::string &f : files_) {
            std::remove(f.c_str());
        }
        rmdir(dir_.c_str());
    }
    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;

    std::string write(const std::string &name, const std::string &content)
    {
        const std::string path = file(name);
        std::ofstream out(path, std::ios::binary);
        out << content;
        return path;
    }
    std::string file(const std::string &name)
    {
        const std::string path = dir_ + "/" + name;
        files_.push_back(path);
        return path;
    }

  private:
    std::string dir_;
    std::vector<std::string> files_;
};

std::string
slurp(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

struct CliRun {
    int status;
    std::string out;
    std::string err;
};

CliRun
cli(const std::vector<std::string> &args)
{
    std::ostringstream out;
    std::ostringstream err;
    const int status = compare_cli(args, out, err);
    return {status, out.str(), err.str()};
}

size_t
count_lines_starting(const std::string &text, const std::string &prefix)
{
    size_t n = 0;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, prefix.size(), prefix) == 0) {
            ++n;
        }
    }
    return n;
}

} // namespace

class CompareCliTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(CompareCliTest);
    CPPUNIT_TEST(testPassExitsZero);
    CPPUNIT_TEST(testFailureExitsOneAndWritesItems);
    CPPUNIT_TEST(testWarningsExitZeroAndWriteWarnItems);
    CPPUNIT_TEST(testKnownFailuresFromFile);
    CPPUNIT_TEST(testItemsAreCapped);
    CPPUNIT_TEST(testBadUsageExitsTwo);
    CPPUNIT_TEST(testBadInputExitsTwo);
    CPPUNIT_TEST(testLintPassesWhenEveryLineIsRead);
    CPPUNIT_TEST(testLintFailsOnALineNoComparisonReads);
    CPPUNIT_TEST(testLintBadUsageAndInputExitTwo);
    CPPUNIT_TEST_SUITE_END();

  public:
    void testPassExitsZero()
    {
        TempDir t;
        const std::string gt = t.write("gt", to_text(tree()));
        const std::string run = t.write("run", to_text(tree()));
        const CliRun r =
            cli({"--ground-truth", gt, "--run", run, "--mode", "strict"});
        CPPUNIT_ASSERT_EQUAL_MESSAGE(r.err, int(COMPARE_PASS), r.status);
        CPPUNIT_ASSERT(r.out.find("Result: PASS") != std::string::npos);
    }

    void testFailureExitsOneAndWritesItems()
    {
        TempDir t;
        const std::string gt = t.write("gt", to_text(tree({entry("/f")})));
        const std::string run =
            t.write("run", to_text(tree({entry("/f", {{"size", "6"}})})));
        const std::string items = t.write("items", "note\tearlier line\n");
        const CliRun r = cli(
            {"--ground-truth", gt, "--run", run, "--mode", "strict", "--check",
                "strict-guest", "--partition", "sda6", "--items", items});
        CPPUNIT_ASSERT_EQUAL_MESSAGE(r.err, int(COMPARE_FAIL), r.status);
        CPPUNIT_ASSERT(
            r.out.find("FAIL  /f: size differs") != std::string::npos);
        CPPUNIT_ASSERT(r.out.find("Result: FAIL") != std::string::npos);
        const std::string it = slurp(items);
        CPPUNIT_ASSERT_MESSAGE(
            it, it.find("note\tearlier line\n") == 0); // appended
        CPPUNIT_ASSERT_EQUAL_MESSAGE(
            it, size_t(1), count_lines_starting(it, "fail\t"));
        CPPUNIT_ASSERT_MESSAGE(it,
            it.find("fail\tstrict-guest sda6: /f: size differs") !=
                std::string::npos);
    }

    void testWarningsExitZeroAndWriteWarnItems()
    {
        TempDir t;
        const std::string gt = t.write("gt", to_text(tree({entry("/tmp/x")})));
        const std::string run =
            t.write("run", to_text(tree({entry("/tmp/x", {{"size", "6"}})})));
        const std::string vol = t.write("vol", "# volatile\n/tmp\n");
        const std::string items = t.file("items");
        const CliRun r = cli({"--ground-truth", gt, "--run", run, "--mode",
            "best-effort", "--volatile", vol, "--run-time",
            format_timestamp(RUN_TIME), "--items", items});
        CPPUNIT_ASSERT_EQUAL_MESSAGE(r.err, int(COMPARE_PASS), r.status);
        CPPUNIT_ASSERT(
            r.out.find("Result: PASS with warnings") != std::string::npos);
        const std::string it = slurp(items);
        CPPUNIT_ASSERT_EQUAL_MESSAGE(
            it, size_t(1), count_lines_starting(it, "warn\t"));
        CPPUNIT_ASSERT_EQUAL_MESSAGE(
            it, size_t(0), count_lines_starting(it, "fail\t"));
    }

    void testKnownFailuresFromFile()
    {
        TempDir t;
        const std::string gt = t.write("gt", to_text(tree({entry("/f")})));
        const std::string run =
            t.write("run", to_text(tree({entry("/f", {{"crtime", "-"}})})));
        const std::string known = t.write("known",
            "strict-guest sda6 /f crtime not read yet\n"
            "strict-guest sda6 /g size fixed already\n");
        const CliRun r = cli({"--ground-truth", gt, "--run", run, "--mode",
            "strict", "--known-failures", known, "--check", "strict-guest",
            "--partition", "sda6"});
        CPPUNIT_ASSERT_EQUAL(int(COMPARE_FAIL), r.status);
        CPPUNIT_ASSERT(
            r.out.find("KNOWN /f: crtime differs") != std::string::npos);
        CPPUNIT_ASSERT(r.out.find("FAIL  Known failure no longer occurs") !=
            std::string::npos);
    }

    void testItemsAreCapped()
    {
        TempDir t;
        std::string gt_text = hdr();
        for (int i = 0; i < 30; ++i) {
            gt_text += entry("/f" + std::to_string(i));
        }
        const std::string gt = t.write("gt", gt_text);
        const std::string run = t.write("run", hdr());
        const std::string items = t.file("items");
        const CliRun r = cli({"--ground-truth", gt, "--run", run, "--mode",
            "strict", "--items", items, "--max-items", "5"});
        CPPUNIT_ASSERT_EQUAL(int(COMPARE_FAIL), r.status);
        // Every failure is in the report; only five are items, plus a count.
        CPPUNIT_ASSERT_EQUAL(size_t(30), count_lines_starting(r.out, "FAIL "));
        const std::string it = slurp(items);
        CPPUNIT_ASSERT_EQUAL_MESSAGE(
            it, size_t(6), count_lines_starting(it, "fail\t"));
        CPPUNIT_ASSERT_MESSAGE(
            it, it.find("25 more failure(s)") != std::string::npos);
    }

    void testBadUsageExitsTwo()
    {
        TempDir t;
        const std::string gt = t.write("gt", to_text(tree()));
        const std::string run = t.write("run", to_text(tree()));
        const std::vector<std::vector<std::string>> bad = {
            {},
            {"--run", run, "--mode", "strict"},
            {"--ground-truth", gt, "--mode", "strict"},
            {"--ground-truth", gt, "--run", run},
            {"--ground-truth", gt, "--run", run, "--mode", "fuzzy"},
            {"--ground-truth", gt, "--run", run, "--mode", "best-effort"},
            {"--ground-truth", gt, "--run", run, "--mode", "strict",
                "--run-time", "soon"},
            {"--ground-truth", gt, "--run", run, "--mode", "strict",
                "--known-failures", gt},
            {"--ground-truth", gt, "--run", run, "--mode", "strict",
                "--max-items", "x"},
            {"--ground-truth", gt, "--run", run, "--mode", "strict", "--bogus"},
            {"--ground-truth", gt, "--run", run, "--mode"},
        };
        for (const auto &args : bad) {
            const CliRun r = cli(args);
            CPPUNIT_ASSERT_EQUAL_MESSAGE(r.out, int(COMPARE_ERROR), r.status);
            CPPUNIT_ASSERT(!r.err.empty());
        }
        const CliRun help = cli({"--help"});
        CPPUNIT_ASSERT_EQUAL(int(COMPARE_PASS), help.status);
        CPPUNIT_ASSERT(help.out.find("--ground-truth") != std::string::npos);
    }

    void testBadInputExitsTwo()
    {
        TempDir t;
        const std::string gt = t.write("gt", to_text(tree()));
        const std::string broken = t.write("broken", hdr() + "/a\tfile\n");
        const std::string known =
            t.write("known", "strict-guest sda6 /a colour\n");
        const std::string vol = t.write("vol", "relative/path\n");

        CliRun r = cli({"--ground-truth", gt, "--run", t.file("absent"),
            "--mode", "strict"});
        CPPUNIT_ASSERT_EQUAL(int(COMPARE_ERROR), r.status);
        CPPUNIT_ASSERT_MESSAGE(
            r.err, r.err.find("absent") != std::string::npos);

        r = cli({"--ground-truth", gt, "--run", broken, "--mode", "strict"});
        CPPUNIT_ASSERT_EQUAL(int(COMPARE_ERROR), r.status);
        CPPUNIT_ASSERT_MESSAGE(
            r.err, r.err.find(broken + ": line 2") != std::string::npos);

        r = cli({"--ground-truth", gt, "--run", gt, "--mode", "strict",
            "--known-failures", known, "--check", "c", "--partition", "p"});
        CPPUNIT_ASSERT_EQUAL(int(COMPARE_ERROR), r.status);
        CPPUNIT_ASSERT_MESSAGE(
            r.err, r.err.find(known + ": line 1") != std::string::npos);

        r = cli({"--ground-truth", gt, "--run", gt, "--mode", "best-effort",
            "--run-time", "0", "--volatile", vol});
        CPPUNIT_ASSERT_EQUAL(int(COMPARE_ERROR), r.status);
        CPPUNIT_ASSERT_MESSAGE(
            r.err, r.err.find(vol + ": line 1") != std::string::npos);
    }

    // A Known failure line can go stale only if a comparison reads it, so
    // the validate command lints the list against the comparisons it runs.
    void testLintPassesWhenEveryLineIsRead()
    {
        TempDir t;
        const std::string known = t.write("known",
            "# comment\n"
            "strict-guest sda4 * error:fs-open no XFS yet\n"
            "fls-host sda6 /a extra phantom name\n");
        const std::string items = t.file("items");
        const CliRun r = cli({"--lint-known-failures", known, "--comparisons",
            "strict-guest:sda4,strict-host:sda4,fls-host:sda6", "--items",
            items});
        CPPUNIT_ASSERT_EQUAL_MESSAGE(r.err, int(COMPARE_PASS), r.status);
        const std::string it = slurp(items);
        CPPUNIT_ASSERT_EQUAL_MESSAGE(
            it, size_t(0), count_lines_starting(it, "fail\t"));
        CPPUNIT_ASSERT_MESSAGE(
            it, it.find("note\t2 Known failure line(s)") != std::string::npos);
    }

    void testLintFailsOnALineNoComparisonReads()
    {
        TempDir t;
        const std::string known = t.write("known",
            "strict-guest sda4 * error:fs-open no XFS yet\n"
            "strict-gest sda4 /a size misspelled check\n"
            "strict-guest sda9 /a size no such partition\n");
        const std::string items = t.file("items");
        const CliRun r = cli({"--lint-known-failures", known, "--comparisons",
            "strict-guest:sda4", "--items", items});
        CPPUNIT_ASSERT_EQUAL_MESSAGE(r.err, int(COMPARE_FAIL), r.status);
        const std::string it = slurp(items);
        CPPUNIT_ASSERT_EQUAL_MESSAGE(
            it, size_t(2), count_lines_starting(it, "fail\t"));
        CPPUNIT_ASSERT_MESSAGE(it,
            it.find("fail\tline 2: strict-gest sda4: no comparison reads") !=
                std::string::npos);
        CPPUNIT_ASSERT_MESSAGE(it,
            it.find("fail\tline 3: strict-guest sda9: no comparison reads") !=
                std::string::npos);
    }

    void testLintBadUsageAndInputExitTwo()
    {
        TempDir t;
        const std::string good = t.write("good", "strict-guest sda4 /a size\n");
        const std::string bad = t.write("bad", "strict-guest sda4 /a colour\n");
        const std::vector<std::vector<std::string>> cases = {
            {"--lint-known-failures", good},
            {"--lint-known-failures", good, "--comparisons", ""},
            {"--lint-known-failures", good, "--comparisons", "strict-guest"},
            {"--lint-known-failures", good, "--comparisons", "a:b,,c:d"},
            {"--lint-known-failures", good, "--comparisons", "a:*"},
            {"--lint-known-failures", good, "--comparisons", "a:b", "--mode",
                "strict"},
            {"--lint-known-failures", t.file("absent"), "--comparisons", "a:b"},
            {"--lint-known-failures", bad, "--comparisons", "a:b"},
        };
        for (const auto &args : cases) {
            const CliRun r = cli(args);
            CPPUNIT_ASSERT_EQUAL_MESSAGE(
                args.back() + ": " + r.out, int(COMPARE_ERROR), r.status);
            CPPUNIT_ASSERT(!r.err.empty());
        }
    }
};
CPPUNIT_TEST_SUITE_REGISTRATION(CompareCliTest);

int
main()
{
    CppUnit::TextUi::TestRunner runner;
    runner.addTest(CppUnit::TestFactoryRegistry::getRegistry().makeTest());
    runner.setOutputter(
        new CppUnit::CompilerOutputter(&runner.result(), std::cerr));
    return runner.run() ? 0 : 1;
}
