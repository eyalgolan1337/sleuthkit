/*
 * The comparison step: compares a run's Manifest with Ground truth under the
 * rules of the Strict check or the Best-effort check, tolerating Known
 * failures. See test/vm/README.md ("The comparison step") and GLOSSARY.md.
 */
#ifndef TSK_TEST_VM_COMPARE_H
#define TSK_TEST_VM_COMPARE_H

#include "manifest.h"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

namespace tskval {

enum class Mode { Strict, BestEffort };

// One line of the Known failure list:
//   <check> <partition> <subject> <difference> [comment]
// subject is an encoded path, or "*" for every entry (or the whole run).
// difference is "missing", "extra", a compared field name, or, with subject
// "*", a summary field (SUMMARY_FIELDS) or "error:<code>" for a run that
// failed as a whole with that error code.
struct KnownFailure {
    std::string check;
    std::string partition;
    bool all_entries = false; // subject "*"
    std::string path;         // raw bytes, when !all_entries
    std::string difference;
    std::string comment;
    int line = 0;
};

bool read_known_failures(
    std::istream &in, std::vector<KnownFailure> &out, std::string &error);

// A comparison the validate command runs: one check on one partition. Only
// the Known failure lines with the same check and partition apply to it.
struct Comparison {
    std::string check;
    std::string partition;
};

// Parses "CHECK:PARTITION[,CHECK:PARTITION...]".
bool parse_comparisons(
    const std::string &text, std::vector<Comparison> &out, std::string &error);

// The lines of a Known failure list that none of the comparisons reads, for
// example a misspelled check name or a partition no gate checks. Such a line
// can never be found stale, so the validate command fails on it.
std::vector<KnownFailure> unread_known_failures(
    const std::vector<KnownFailure> &list,
    const std::vector<Comparison> &comparisons);

// One encoded path per line; blank lines and lines starting with '#' are
// skipped. A Volatile path covers itself and everything below it.
bool read_volatile_paths(
    std::istream &in, std::vector<std::string> &out, std::string &error);

struct CompareOptions {
    Mode mode = Mode::Strict;
    // Best-effort only.
    std::vector<std::string> volatile_paths; // raw bytes
    Timestamp run_time;                      // when the run finished
    // The Known failure list; only lines for this check and partition apply.
    std::string check;
    std::string partition;
    std::vector<KnownFailure> known_failures;
};

// The fields the comparison step compares, in Manifest order (no atime).
extern const char *const COMPARED_FIELDS[];
extern const size_t NUM_COMPARED_FIELDS;

// Best-effort: an entry whose ctime is at most this long before the run's
// timestamp, and not after it, is "recently changed".
extern const int64_t RECENT_CTIME_SECONDS;

enum class Severity { Failure, Warning, Known, Note };

struct Finding {
    Severity severity = Severity::Note;
    std::string path;       // raw bytes; empty for findings about the whole run
    std::string difference; // "missing", "extra", a field name, "error", ...
    std::string message;    // one line, paths encoded
};

enum class Verdict { Pass, Warnings, Fail };

struct CompareResult {
    std::vector<Finding> findings;
    size_t ground_truth_entries = 0;
    size_t run_entries = 0;
    size_t failures = 0;
    size_t warnings = 0;
    size_t known = 0;
    Verdict verdict() const;
};

CompareResult compare_manifests(const Manifest &ground_truth,
    const Manifest &run, const CompareOptions &options);

// A file-system summary: the superblock facts that fsstat prints and the
// guest reports (the CLI tool checks, test/vm/toolcheck/), as named fields
// with canonical values. A producer that couldn't make one sets has_error.
struct Summary {
    bool has_error = false;
    std::string error_code; // [a-z0-9-]+, like a Manifest's
    std::string error_detail;
    std::vector<std::pair<std::string, std::string>> fields; // name, value
};

// The names of a summary's fields. A Known failure's difference may be one of
// them too.
extern const char *const SUMMARY_FIELDS[];
extern const size_t NUM_SUMMARY_FIELDS;

// Compares a run's summary with the expected one field by field, under the
// Strict check's rules: every difference (a different value, or a field on
// one side only) is a failure, named after the field, unless a Known failure
// with the subject * covers it. Whole-run errors and stale Known failures
// work as in compare_manifests; options.mode is ignored.
CompareResult compare_summaries(
    const Summary &expected, const Summary &run, const CompareOptions &options);

// The per-entry report followed by a one-line summary.
void write_report(std::ostream &out, const CompareResult &result);

const char *verdict_name(Verdict v);

} // namespace tskval

#endif
