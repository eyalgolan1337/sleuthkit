/*
 * The Manifest-comparison helper. See manifest_check.h.
 */
#include "manifest_check.h"

#include "harness.h"
#include "sha256.h"

#include <tsk/libtsk.h>

#include <fstream>
#include <memory>
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

// The comparison of `run` with `expected`, and its report followed by `log`.
WalkCheck
compare(const tskval::Manifest &expected, const tskval::Manifest &run,
    const tskval::CompareOptions &options, const std::string &log)
{
    WalkCheck check;
    check.result = tskval::compare_manifests(expected, run, options);
    check.passed = check.result.verdict() != tskval::Verdict::Fail;
    std::ostringstream report;
    tskval::write_report(report, check.result);
    report << log;
    check.report = report.str();
    return check;
}

// The CompareOptions of a Strict comparison with `known_failures`.
bool
strict_options(const std::string &known_failures,
    tskval::CompareOptions &options, std::string &error)
{
    options.mode = tskval::Mode::Strict;
    options.check = KNOWN_CHECK;
    options.partition = KNOWN_PARTITION;
    return parse_known_failures(known_failures, options.known_failures, error);
}

std::string
tsk_error_line()
{
    const char *msg = tsk_error_get();
    return msg != nullptr ? msg : "(no TSK error)";
}

// The SHA-256 of a file's first `size` bytes, read with tsk_fs_file_read in
// 1 MiB pieces; false if a read fails or returns nothing.
bool
content_sha256(TSK_FS_FILE *file, TSK_OFF_T size, std::string &hex)
{
    tskval::Sha256 h;
    std::vector<char> buf(1 << 20);
    TSK_OFF_T off = 0;
    while (off < size) {
        size_t want = buf.size();
        if (static_cast<TSK_OFF_T>(want) > size - off) {
            want = static_cast<size_t>(size - off);
        }
        const ssize_t n = tsk_fs_file_read(
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

struct FileCloser {
    void operator()(TSK_FS_FILE *f) const
    {
        tsk_fs_file_close(f);
    }
};
struct FsCloser {
    void operator()(TSK_FS_INFO *fs) const
    {
        tsk_fs_close(fs);
    }
};
struct ImgCloser {
    void operator()(TSK_IMG_INFO *img) const
    {
        tsk_img_close(img);
    }
};

} // namespace

WalkCheck
check_walk(const std::string &image, const tskval::Manifest &expected,
    const std::string &known_failures)
{
    tskval::CompareOptions compare_opts;
    std::string error;
    if (!strict_options(known_failures, compare_opts, error)) {
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

    return compare(expected, walked, compare_opts, walk_err.str());
}

WalkCheck
check_by_inode(const std::string &image, const tskval::Manifest &expected,
    const std::string &known_failures)
{
    tskval::CompareOptions compare_opts;
    std::string error;
    if (!strict_options(known_failures, compare_opts, error)) {
        return failed("malformed Known failure list: " + error);
    }
    const char *paths[] = {image.c_str()};
    tsk_error_reset();
    std::unique_ptr<TSK_IMG_INFO, ImgCloser> img(
        tsk_img_open_utf8(1, paths, TSK_IMG_TYPE_DETECT, 0));
    if (!img) {
        return failed(
            "can't open the image " + image + ": " + tsk_error_line());
    }
    std::unique_ptr<TSK_FS_INFO, FsCloser> fs(
        tsk_fs_open_img(img.get(), 0, TSK_FS_TYPE_DETECT));
    if (!fs) {
        return failed(
            "can't open the file system in " + image + ": " + tsk_error_line());
    }

    tskval::Manifest run;
    std::ostringstream log;
    for (const tskval::Entry &x : expected.entries) {
        const std::string shown = tskval::encode_bytes(x.path);
        tsk_error_reset();
        std::unique_ptr<TSK_FS_FILE, FileCloser> file(
            tsk_fs_file_open_meta(fs.get(), nullptr, x.inode));
        if (!file || file->meta == nullptr) {
            log << shown << ": inode " << x.inode
                << " doesn't open: " << tsk_error_line() << "\n";
            continue;
        }
        tskval::Entry e = tskval::entry_from_meta(x.path, *file->meta);
        if (e.type == tskval::EntryType::File) {
            e.has_sha256 =
                content_sha256(file.get(), file->meta->size, e.sha256);
            if (!e.has_sha256) {
                log << shown << ": inode " << x.inode
                    << " content can't be read: " << tsk_error_line() << "\n";
            }
        }
        run.entries.push_back(e);
    }
    return compare(expected, run, compare_opts, log.str());
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
