/*
 * The Manifest-comparison helper. See manifest_check.h.
 */
#include "manifest_check.h"

#include "harness.h"

#include <fstream>
#include <sstream>
#include <vector>

namespace fstest {

namespace {

// The check and partition columns that the helper adds to each Known failure
// line; only lines with these apply to the comparison.
const char KNOWN_CHECK[] = "unit";
const char KNOWN_PARTITION[] = "fs";

// Reads known_failures (see manifest_check.h) with the comparison step's
// reader, after adding the check and partition columns to each line that
// isn't blank or a comment. Line numbers stay those of known_failures.
bool
parse_known_failures(const std::string &text,
    std::vector<tskval::KnownFailure> &out, std::string &error)
{
    std::istringstream in(text);
    std::ostringstream full;
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        std::istringstream tokens(line);
        std::string subject;
        std::string difference;
        tokens >> subject >> difference;
        if (!subject.empty() && subject[0] != '#') {
            if (difference.empty()) {
                error = "line " + std::to_string(lineno) +
                    ": expected '<subject> <difference> [comment]'";
                return false;
            }
            full << KNOWN_CHECK << ' ' << KNOWN_PARTITION << ' ';
        }
        full << line << '\n';
    }
    std::istringstream list(full.str());
    return tskval::read_known_failures(list, out, error);
}

// A check that failed before comparing anything.
WalkCheck
failed(const std::string &why)
{
    WalkCheck check;
    check.report = why + "\n";
    return check;
}

} // namespace

WalkCheck
check_walk(const std::string &image, const tskval::Manifest &expected,
    const std::string &known_failures)
{
    tskval::CompareOptions compare_opts;
    compare_opts.mode = tskval::Mode::Strict;
    compare_opts.check = KNOWN_CHECK;
    compare_opts.partition = KNOWN_PARTITION;
    std::string error;
    if (!parse_known_failures(
            known_failures, compare_opts.known_failures, error)) {
        return failed("malformed Known failure list: " + error);
    }

    tskval::HarnessOptions harness_opts;
    harness_opts.images.push_back(image);
    std::stringstream walk_out;
    std::ostringstream walk_err;
    tskval::run_harness(harness_opts, walk_out, walk_err);

    tskval::Manifest walked;
    if (!tskval::read_manifest(walk_out, walked, error)) {
        return failed("the walk wrote a malformed Manifest: " + error + "\n" +
            walk_err.str());
    }

    WalkCheck check;
    check.result = tskval::compare_manifests(expected, walked, compare_opts);
    check.passed = check.result.verdict() != tskval::Verdict::Fail;
    std::ostringstream report;
    tskval::write_report(report, check.result);
    report << walk_err.str();
    check.report = report.str();
    return check;
}

WalkCheck
check_walk(const std::string &image, const std::string &expected_manifest,
    const std::string &known_failures)
{
    std::ifstream in(expected_manifest, std::ios::binary);
    if (!in) {
        return failed("can't open the expected Manifest " + expected_manifest);
    }
    tskval::Manifest expected;
    std::string error;
    if (!tskval::read_manifest(in, expected, error)) {
        return failed("expected Manifest " + expected_manifest + ": " + error);
    }
    return check_walk(image, expected, known_failures);
}

} // namespace fstest
