/*
 * The comparison step. See compare.h and test/vm/README.md.
 */
#include "compare.h"

#include <algorithm>
#include <istream>
#include <map>
#include <ostream>
#include <set>

namespace tskval {

const char *const COMPARED_FIELDS[] = {"type", "inode", "mode", "uid", "gid",
    "size", "nlink", "target", "sha256", "mtime", "ctime", "crtime"};
const size_t NUM_COMPARED_FIELDS =
    sizeof(COMPARED_FIELDS) / sizeof(COMPARED_FIELDS[0]);
const int64_t RECENT_CTIME_SECONDS = 10 * 60;
const char *const SUMMARY_FIELDS[] = {"fs-type", "block-size", "inode-size",
    "group-count", "uuid", "label", "version"};
const size_t NUM_SUMMARY_FIELDS =
    sizeof(SUMMARY_FIELDS) / sizeof(SUMMARY_FIELDS[0]);

Verdict
CompareResult::verdict() const
{
    if (failures > 0) {
        return Verdict::Fail;
    }
    return warnings > 0 ? Verdict::Warnings : Verdict::Pass;
}

const char *
verdict_name(Verdict v)
{
    switch (v) {
    case Verdict::Pass:
        return "PASS";
    case Verdict::Warnings:
        return "PASS with warnings";
    case Verdict::Fail:
        return "FAIL";
    }
    return "?";
}

namespace {

const char *const WHITESPACE = " \t";

// Check and partition names: [A-Za-z0-9._-]+.
bool
valid_name(const std::string &s)
{
    if (s.empty()) {
        return false;
    }
    for (char c : s) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

bool
is_compared_field(const std::string &s)
{
    for (size_t i = 0; i < NUM_COMPARED_FIELDS; ++i) {
        if (s == COMPARED_FIELDS[i]) {
            return true;
        }
    }
    return false;
}

bool
is_summary_field(const std::string &s)
{
    for (size_t i = 0; i < NUM_SUMMARY_FIELDS; ++i) {
        if (s == SUMMARY_FIELDS[i]) {
            return true;
        }
    }
    return false;
}

const char ERROR_PREFIX[] = "error:";
const size_t ERROR_PREFIX_LEN = sizeof(ERROR_PREFIX) - 1;

bool
is_error_difference(const std::string &difference)
{
    return difference.compare(0, ERROR_PREFIX_LEN, ERROR_PREFIX) == 0;
}

// Splits off the next whitespace-separated token from `rest`.
std::string
next_token(std::string &rest)
{
    const size_t start = rest.find_first_not_of(WHITESPACE);
    if (start == std::string::npos) {
        rest.clear();
        return "";
    }
    const size_t end = rest.find_first_of(WHITESPACE, start);
    const std::string token = rest.substr(start, end - start);
    rest = end == std::string::npos ? "" : rest.substr(end);
    return token;
}

std::string
trim(const std::string &s)
{
    const size_t start = s.find_first_not_of(WHITESPACE);
    if (start == std::string::npos) {
        return "";
    }
    return s.substr(start, s.find_last_not_of(WHITESPACE) - start + 1);
}

// Free text for a one-line report: bytes outside ' '..'~' become %XX.
std::string
display_text(const std::string &raw)
{
    static const char HEX[] = "0123456789ABCDEF";
    std::string out;
    for (char ch : raw) {
        const auto c = static_cast<unsigned char>(ch);
        if (c >= 0x20 && c <= 0x7E) {
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

std::string
subject_text(const KnownFailure &k)
{
    return k.all_entries ? "*" : encode_bytes(k.path);
}

} // namespace

bool
read_known_failures(
    std::istream &in, std::vector<KnownFailure> &out, std::string &error)
{
    std::vector<KnownFailure> list;
    std::set<std::string> seen;
    std::string line;
    int lineno = 0;
    const auto fail = [&](const std::string &msg) {
        error = "line " + std::to_string(lineno) + ": " + msg;
        return false;
    };
    while (std::getline(in, line)) {
        ++lineno;
        if (trim(line).empty() || trim(line)[0] == '#') {
            continue;
        }
        std::string rest = line;
        KnownFailure k;
        k.line = lineno;
        k.check = next_token(rest);
        k.partition = next_token(rest);
        const std::string subject = next_token(rest);
        k.difference = next_token(rest);
        k.comment = trim(rest);
        if (k.difference.empty()) {
            return fail("expected '<check> <partition> <subject> <difference> "
                        "[comment]'");
        }
        if (!valid_name(k.check)) {
            return fail(
                "bad check name '" + k.check + "' (expected [A-Za-z0-9._-]+)");
        }
        if (!valid_name(k.partition)) {
            return fail("bad partition name '" + k.partition +
                "' (expected [A-Za-z0-9._-]+)");
        }
        if (subject == "*") {
            k.all_entries = true;
        }
        else if (!decode_bytes(subject, k.path) || !is_valid_path(k.path)) {
            return fail("bad subject '" + subject +
                "' (expected an encoded path or *)");
        }
        if (is_error_difference(k.difference)) {
            if (!k.all_entries) {
                return fail("'" + k.difference + "' needs the subject *");
            }
            if (!is_valid_error_code(k.difference.substr(ERROR_PREFIX_LEN))) {
                return fail("bad error code in '" + k.difference +
                    "' (expected [a-z0-9-]+)");
            }
        }
        else if (is_summary_field(k.difference)) {
            if (!k.all_entries) {
                return fail(
                    "summary field '" + k.difference + "' needs the subject *");
            }
        }
        else if (k.difference != "missing" && k.difference != "extra" &&
            !is_compared_field(k.difference)) {
            return fail("bad difference '" + k.difference +
                "' (expected missing, extra, a compared field, a summary "
                "field or error:<code>)");
        }
        const std::string key =
            k.check + ' ' + k.partition + ' ' + subject + ' ' + k.difference;
        if (!seen.insert(key).second) {
            return fail("duplicate Known failure '" + key + "'");
        }
        list.push_back(k);
    }
    out = list;
    return true;
}

bool
read_volatile_paths(
    std::istream &in, std::vector<std::string> &out, std::string &error)
{
    std::vector<std::string> paths;
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::string path;
        if (!decode_bytes(line, path) || !is_valid_path(path)) {
            error = "line " + std::to_string(lineno) + ": bad Volatile path '" +
                line + "' (expected one encoded path such as /var/log)";
            return false;
        }
        paths.push_back(path);
    }
    out = paths;
    return true;
}

namespace {

// One difference between Ground truth and the run, before classification.
struct Difference {
    std::string path;
    std::string difference; // "missing", "extra" or a field name
    std::string gt_value;   // as written in a Manifest; empty if no entry
    std::string run_value;
    const Entry *gt;  // null for "extra"
    const Entry *run; // null for "missing"
};

std::vector<std::string>
columns(const Entry &e)
{
    // The Manifest's text form is canonical, so two fields are equal exactly
    // when their columns are.
    std::vector<std::string> out;
    const std::string line = format_entry(e);
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

std::vector<Difference>
differences(const Manifest &gt, const Manifest &run)
{
    std::map<std::string, const Entry *> run_by_path;
    for (const Entry &e : run.entries) {
        run_by_path[e.path] = &e;
    }
    std::map<std::string, const Entry *> gt_by_path;
    for (const Entry &e : gt.entries) {
        gt_by_path[e.path] = &e;
    }

    std::vector<Difference> out;
    for (const auto &g : gt_by_path) {
        const auto r = run_by_path.find(g.first);
        if (r == run_by_path.end()) {
            out.push_back({g.first, "missing", format_entry(*g.second), "",
                g.second, nullptr});
            continue;
        }
        const std::vector<std::string> gc = columns(*g.second);
        const std::vector<std::string> rc = columns(*r->second);
        // Column 0 is the path; columns 1..NUM_COMPARED_FIELDS are compared;
        // the last one, atime, is not.
        for (size_t i = 0; i < NUM_COMPARED_FIELDS; ++i) {
            if (gc[i + 1] != rc[i + 1]) {
                out.push_back({g.first, COMPARED_FIELDS[i], gc[i + 1],
                    rc[i + 1], g.second, r->second});
            }
        }
    }
    for (const auto &r : run_by_path) {
        if (gt_by_path.find(r.first) == gt_by_path.end()) {
            out.push_back({r.first, "extra", "", format_entry(*r.second),
                nullptr, r.second});
        }
    }
    std::stable_sort(
        out.begin(), out.end(), [](const Difference &a, const Difference &b) {
            return a.path < b.path;
        });
    return out;
}

std::string
describe(const Difference &d)
{
    if (d.path.empty()) { // a summary field
        return d.difference + " differs: Ground truth " + d.gt_value +
            ", run " + d.run_value;
    }
    const std::string path = encode_bytes(d.path);
    if (d.difference == "missing") {
        return path + ": in Ground truth, missing from the run";
    }
    if (d.difference == "extra") {
        return path + ": in the run, not in Ground truth";
    }
    return path + ": " + d.difference + " differs: Ground truth " + d.gt_value +
        ", run " + d.run_value;
}

void
add(CompareResult &r, Severity severity, const std::string &path,
    const std::string &difference, const std::string &message)
{
    r.findings.push_back({severity, path, difference, message});
    switch (severity) {
    case Severity::Failure:
        ++r.failures;
        break;
    case Severity::Warning:
        ++r.warnings;
        break;
    case Severity::Known:
        ++r.known;
        break;
    case Severity::Note:
        break;
    }
}

// The Volatile path that covers `path`, or null.
const std::string *
volatile_cover(const std::string &path, const std::vector<std::string> &vols)
{
    for (const std::string &v : vols) {
        if (v == "/" || path == v ||
            (path.size() > v.size() && path.compare(0, v.size(), v) == 0 &&
                path[v.size()] == '/')) {
            return &v;
        }
    }
    return nullptr;
}

// Best-effort only: why a difference is tolerated, or "" if it isn't.
std::string
tolerance(const Difference &d, const CompareOptions &o)
{
    if (const std::string *v = volatile_cover(d.path, o.volatile_paths)) {
        return "under Volatile path " + encode_bytes(*v);
    }
    // Changed in the 10 minutes before the end of the run: either side's
    // ctime may show it (a change before or after Ground truth capture). No
    // real ctime is later than the run's end, so a later one (a misread
    // timestamp) tolerates nothing.
    Timestamp window_start = o.run_time;
    window_start.sec -= RECENT_CTIME_SECONDS;
    for (const Entry *e : {d.gt, d.run}) {
        if (e && !(e->ctime < window_start) && !(o.run_time < e->ctime)) {
            return std::string("ctime ") + format_timestamp(e->ctime) +
                " is within the " + std::to_string(RECENT_CTIME_SECONDS / 60) +
                " minutes before the run's end (" +
                format_timestamp(o.run_time) + ")";
        }
    }
    return "";
}

// "Known failure, line N: <check> <partition> <subject> <difference> [comment]"
std::string
known_text(const KnownFailure &k)
{
    std::string text = "Known failure, line " + std::to_string(k.line) + ": " +
        k.check + " " + k.partition + " " + subject_text(k) + " " +
        k.difference;
    if (!k.comment.empty()) {
        text += " (" + k.comment + ")";
    }
    return text;
}

// The Known failure that covers a difference, or -1. A line naming the path
// wins over a "*" line.
int
match_known(const Difference &d, const std::vector<const KnownFailure *> &known)
{
    int any = -1;
    for (size_t i = 0; i < known.size(); ++i) {
        const KnownFailure &k = *known[i];
        if (k.difference != d.difference) {
            continue;
        }
        if (!k.all_entries && k.path == d.path) {
            return static_cast<int>(i);
        }
        if (k.all_entries && any < 0) {
            any = static_cast<int>(i);
        }
    }
    return any;
}

const char *
severity_tag(Severity s)
{
    switch (s) {
    case Severity::Failure:
        return "FAIL ";
    case Severity::Warning:
        return "WARN ";
    case Severity::Known:
        return "KNOWN";
    case Severity::Note:
        return "NOTE ";
    }
    return "?    ";
}

// Whether a Manifest or summary records a failure as a whole.
struct WholeRun {
    bool has_error;
    const std::string &code;
    const std::string &detail;
};

// The rules shared by compare_manifests and compare_summaries: whole-run
// errors, then each difference as a Known failure, a Best-effort warning or
// a failure, then stale Known failures.
void
judge(CompareResult &r, const WholeRun &ground_truth, const WholeRun &run,
    const std::vector<Difference> &diffs, const CompareOptions &options)
{
    // The Known failures of this check and partition; used[i] once one of
    // them has tolerated a difference.
    std::vector<const KnownFailure *> known;
    for (const KnownFailure &k : options.known_failures) {
        if (k.check == options.check && k.partition == options.partition) {
            known.push_back(&k);
        }
    }
    std::vector<bool> used(known.size(), false);

    if (ground_truth.has_error) {
        add(r, Severity::Failure, "", "ground-truth-error",
            "Ground truth capture failed (" + ground_truth.code + ": " +
                display_text(ground_truth.detail) + "); nothing was compared");
        return;
    }

    if (run.has_error) {
        const std::string difference = ERROR_PREFIX + run.code;
        const std::string what = "the run failed as a whole (" + run.code +
            ": " + display_text(run.detail) +
            "), so its entries were not compared";
        bool tolerated = false;
        for (size_t i = 0; i < known.size(); ++i) {
            if (known[i]->all_entries && known[i]->difference == difference) {
                used[i] = true;
                tolerated = true;
                add(r, Severity::Known, "", "error",
                    what + " (" + known_text(*known[i]) + ")");
            }
        }
        if (!tolerated) {
            add(r, Severity::Failure, "", "error", what);
        }
        // Entry-level Known failures can't be checked against a failed run.
        for (size_t i = 0; i < known.size(); ++i) {
            if (!used[i] && !is_error_difference(known[i]->difference)) {
                used[i] = true;
                add(r, Severity::Note, known[i]->path, "not-evaluated",
                    "not evaluated, the run failed as a whole: " +
                        known_text(*known[i]));
            }
        }
    }
    else {
        for (const Difference &d : diffs) {
            const int known_index = match_known(d, known);
            if (known_index >= 0) {
                const auto i = static_cast<size_t>(known_index);
                used[i] = true;
                add(r, Severity::Known, d.path, d.difference,
                    describe(d) + " (" + known_text(*known[i]) + ")");
                continue;
            }
            const std::string why =
                options.mode == Mode::BestEffort ? tolerance(d, options) : "";
            if (!why.empty()) {
                add(r, Severity::Warning, d.path, d.difference,
                    describe(d) + " (tolerated: " + why + ")");
            }
            else {
                add(r, Severity::Failure, d.path, d.difference, describe(d));
            }
        }
    }

    // The list only shrinks: a listed failure that no longer occurs fails.
    for (size_t i = 0; i < known.size(); ++i) {
        if (!used[i]) {
            add(r, Severity::Failure, known[i]->path, "known-failure-passes",
                "Known failure no longer occurs; remove it from the list: " +
                    known_text(*known[i]));
        }
    }
}

// The differences between two summaries, field by field.
std::vector<Difference>
summary_differences(const Summary &expected, const Summary &run)
{
    static const std::string NONE = "(none)";
    std::map<std::string, std::string> run_fields(
        run.fields.begin(), run.fields.end());
    std::map<std::string, std::string> expected_fields(
        expected.fields.begin(), expected.fields.end());
    std::vector<Difference> out;
    for (const auto &f : expected.fields) {
        const auto r = run_fields.find(f.first);
        const std::string &value = r == run_fields.end() ? NONE : r->second;
        if (value != f.second) {
            out.push_back({"", f.first, f.second, value, nullptr, nullptr});
        }
    }
    for (const auto &f : run.fields) {
        if (expected_fields.find(f.first) == expected_fields.end()) {
            out.push_back({"", f.first, NONE, f.second, nullptr, nullptr});
        }
    }
    return out;
}

} // namespace

CompareResult
compare_manifests(const Manifest &ground_truth, const Manifest &run,
    const CompareOptions &options)
{
    CompareResult r;
    r.ground_truth_entries = ground_truth.entries.size();
    r.run_entries = run.entries.size();
    judge(r,
        {ground_truth.has_error, ground_truth.error_code,
            ground_truth.error_detail},
        {run.has_error, run.error_code, run.error_detail},
        ground_truth.has_error || run.has_error
            ? std::vector<Difference>()
            : differences(ground_truth, run),
        options);
    return r;
}

CompareResult
compare_summaries(
    const Summary &expected, const Summary &run, const CompareOptions &options)
{
    CompareResult r;
    r.ground_truth_entries = expected.fields.size();
    r.run_entries = run.fields.size();
    CompareOptions strict = options;
    strict.mode = Mode::Strict;
    judge(r, {expected.has_error, expected.error_code, expected.error_detail},
        {run.has_error, run.error_code, run.error_detail},
        expected.has_error || run.has_error
            ? std::vector<Difference>()
            : summary_differences(expected, run),
        strict);
    return r;
}

void
write_report(std::ostream &out, const CompareResult &r)
{
    for (const Finding &f : r.findings) {
        out << severity_tag(f.severity) << ' ' << f.message << '\n';
    }
    out << "Result: " << verdict_name(r.verdict()) << ": " << r.failures
        << " failure(s), " << r.warnings << " warning(s), " << r.known
        << " Known failure(s); Ground truth " << r.ground_truth_entries
        << " entries, run " << r.run_entries << " entries\n";
}

} // namespace tskval
