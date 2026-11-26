/*
 * Command line of the CLI tool checks. See toolcheck_cli.h.
 */
#include "toolcheck_cli.h"

#include "compare.h"
#include "compare_cli.h"
#include "toolcheck.h"
#include "walkcheck.h"

#include <fstream>
#include <map>
#include <ostream>
#include <sstream>

namespace tskval {

namespace {

const char USAGE[] =
    "Usage: check-tool fls|icat|fsstat|istat|ils|ifind|blkls\n"
    "                  --ground-truth-dir DIR --run-dir DIR [options]\n"
    "       check-tool plan-inodes|plan-blocks WALK-RECORD\n"
    "       check-tool read-bytes DEVICE SKIP LENGTH LIST\n"
    "\n"
    "Compares the output of one of TSK's command-line tools with Ground\n"
    "truth under the Strict check's rules and prints a report. Exit status:\n"
    "0 pass, 1 failures, 2 bad usage or unreadable input.\n"
    "\n"
    "Ground truth directory: ground-truth.manifest (fls, icat, ils),\n"
    "fs-summary.txt (fsstat), stat.txt (istat), walk-record.txt (ils,\n"
    "ifind, blkls), corpus-deleted.txt (ils). Run directory: the files\n"
    "test/vm/toolcheck/run-tools.sh writes: fls.{out,err,status},\n"
    "icat-root.{out,err,status} and icat.tsv, fsstat.{out,err,status},\n"
    "istat/<inode>.{out,err,status}, ils-a, ils-e and ils.{out,err,status},\n"
    "ifind/<block>.{out,err,status}, blkls.{out,err,status} and\n"
    "blkls.lines.\n"
    "\n"
    "plan-inodes and plan-blocks print, one per line, the inodes whose ctime\n"
    "the guest must read and the blocks ifind -d must be asked about, from\n"
    "a walk record (test/vm/toolcheck/walkcheck.h). Exit status 0, or 2 if\n"
    "the record can't be read.\n"
    "\n"
    "read-bytes reads LIST, lines \"<label> <byte offset>\", and prints\n"
    "\"<label> <hex>\" with the LENGTH bytes (1 to 64) at offset + SKIP of\n"
    "DEVICE: the guest's raw read of each free XFS inode's ctime. Exit\n"
    "status 0, or 2 on a bad line or a short read.\n"
    "\n"
    "Options:\n"
    "  --ground-truth-dir DIR  Ground truth of the partition\n"
    "  --run-dir DIR           the tools' output\n"
    "  --known-failures FILE   Known failure list; needs --check and "
    "--partition\n"
    "  --check NAME            the check, e.g. fls-guest\n"
    "  --partition NAME        the partition, e.g. sda6\n"
    "  --items FILE            append fail/warn/note lines for the validate\n"
    "                          command's report to FILE\n"
    "  --max-items N           at most N fail items (default 20)\n"
    "  --override FIELD=VALUE  fsstat, blkls, test only: replace the\n"
    "                          expected value of a summary field\n"
    "  --override SUBJECT=drop ils, test only: drop an expected entry\n"
    "  --override SUBJECT=INODE\n"
    "                          ifind, test only: replace an expected answer\n"
    "  -h, --help              show this help\n";

const char *const VALUE_OPTIONS[] = {"--ground-truth-dir", "--run-dir",
    "--known-failures", "--check", "--partition", "--items", "--max-items",
    "--override"};

const char *const TOOLS[] = {
    "fls", "icat", "fsstat", "istat", "ils", "ifind", "blkls"};

bool
read_file(const std::string &path, std::string &out, std::ostream &err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err << "check-tool: cannot open '" << path << "'\n";
        return false;
    }
    std::ostringstream s;
    s << in.rdbuf();
    out = s.str();
    return true;
}

// <base>.status (a number), <base>.out and <base>.err.
bool
read_tool(const std::string &base, ToolOutput &t, std::ostream &err)
{
    std::string status;
    if (!read_file(base + ".status", status, err) ||
        !read_file(base + ".out", t.out, err) ||
        !read_file(base + ".err", t.err, err)) {
        return false;
    }
    std::istringstream in(status);
    if (!(in >> t.status)) {
        err << "check-tool: " << base << ".status: not an exit status\n";
        return false;
    }
    return true;
}

bool
file_exists(const std::string &path)
{
    return std::ifstream(path).good();
}

bool
read_ground_truth(const std::string &dir, Manifest &m, std::ostream &err)
{
    const std::string path = dir + "/ground-truth.manifest";
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err << "check-tool: cannot open '" << path << "'\n";
        return false;
    }
    std::string error;
    if (!read_manifest(in, m, error)) {
        err << "check-tool: " << path << ": " << error << '\n';
        return false;
    }
    return true;
}

// The expected and run Manifests of fls, icat or istat.
bool
manifests(const std::string &tool, const std::string &gt_dir,
    const std::string &run_dir, Manifest &expected, Manifest &run,
    std::ostream &err)
{
    if (tool == "istat") {
        std::string records;
        if (!read_file(gt_dir + "/stat.txt", records, err)) {
            return false;
        }
        expected = istat_expected(records);
        std::map<uint64_t, ToolOutput> outputs;
        for (const Entry &e : expected.entries) {
            const std::string base =
                run_dir + "/istat/" + std::to_string(e.inode);
            if (!file_exists(base + ".status")) {
                continue; // no run for this inode: the entry is missing
            }
            if (!read_tool(base, outputs[e.inode], err)) {
                return false;
            }
        }
        run = istat_run(expected, outputs);
        return true;
    }
    if (tool == "ifind") {
        std::string record;
        if (!read_file(gt_dir + "/walk-record.txt", record, err)) {
            return false;
        }
        expected = ifind_expected(record);
        std::map<uint64_t, ToolOutput> outputs;
        for (const Entry &e : expected.entries) {
            const std::string base =
                run_dir + "/ifind/" + std::to_string(e.size);
            if (outputs.count(e.size) != 0 || !file_exists(base + ".status")) {
                continue; // asked already, or never: its entries are missing
            }
            if (!read_tool(base, outputs[e.size], err)) {
                return false;
            }
        }
        run = ifind_run(expected, outputs);
        return true;
    }
    Manifest gt;
    if (!read_ground_truth(gt_dir, gt, err)) {
        return false;
    }
    if (tool == "ils") {
        std::string record;
        std::string deleted;
        ToolOutput allocated;
        ToolOutput all;
        ToolOutput used_free;
        if (!read_file(gt_dir + "/walk-record.txt", record, err) ||
            !read_file(gt_dir + "/corpus-deleted.txt", deleted, err) ||
            !read_tool(run_dir + "/ils-a", allocated, err) ||
            !read_tool(run_dir + "/ils-e", all, err) ||
            !read_tool(run_dir + "/ils", used_free, err)) {
            return false;
        }
        expected = ils_expected(gt, record, deleted);
        run = ils_run(allocated, all, used_free);
        return true;
    }
    if (tool == "fls") {
        ToolOutput t;
        if (!read_tool(run_dir + "/fls", t, err)) {
            return false;
        }
        expected = fls_expected(gt);
        run = fls_run(t);
        return true;
    }
    ToolOutput probe;
    std::string results;
    if (!read_tool(run_dir + "/icat-root", probe, err) ||
        !read_file(run_dir + "/icat.tsv", results, err)) {
        return false;
    }
    expected = icat_expected(gt);
    run = icat_run(gt, probe, results);
    return true;
}

// A whole decimal number of at most 19 digits.
bool
decimal(const std::string &s, uint64_t &v)
{
    if (s.empty() || s.size() > 19 ||
        s.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }
    v = std::stoull(s);
    return true;
}

// The test hook of ils and ifind: SUBJECT=drop removes an expected entry
// (ils), SUBJECT=INODE replaces its expected inode (ifind). `hook` gets the
// report's note; false with `why` on a bad value or an unknown subject.
bool
override_entry(const std::string &tool, const std::string &ov,
    Manifest &expected, std::string &hook, std::string &why)
{
    const size_t eq = ov.find('=');
    const std::string subject = ov.substr(0, eq);
    const std::string value = eq == std::string::npos ? "" : ov.substr(eq + 1);
    uint64_t inode = 0;
    const bool is_inode = decimal(value, inode);
    if (eq == std::string::npos || (tool == "ils" && value != "drop") ||
        (tool == "ifind" && !is_inode)) {
        why = "--override needs " +
            std::string(tool == "ils" ? "SUBJECT=drop" : "SUBJECT=INODE") +
            " for " + tool + ", not '" + ov + "'";
        return false;
    }
    if (expected.has_error) {
        return true; // nothing to change; the comparison reports the error
    }
    for (auto e = expected.entries.begin(); e != expected.entries.end(); ++e) {
        if (e->path != subject) {
            continue;
        }
        if (tool == "ils") {
            expected.entries.erase(e);
            hook = "test hook: " + subject + " was dropped from Ground truth";
        }
        else {
            e->inode = inode;
            hook = "test hook: the expected answer for ";
            hook += subject;
            hook += " was replaced with inode ";
            hook += value;
        }
        return true;
    }
    why = "--override: no expected entry " + subject;
    return false;
}

// The walk record's facts for the report: its notes, and for ils how many
// free inodes were once in use and how many of them the Test corpus deleted.
// An unreadable record is left to the comparison, which reports it.
bool
walk_notes(const std::string &gt_dir, const std::string &tool,
    std::vector<std::string> &notes, std::ostream &err)
{
    std::string record;
    if (!read_file(gt_dir + "/walk-record.txt", record, err)) {
        return false;
    }
    WalkTruth t;
    std::string error;
    if (!parse_walk_truth(record, t, error)) {
        return true;
    }
    notes = t.notes;
    if (tool == "ils") {
        std::string deleted;
        if (!read_file(gt_dir + "/corpus-deleted.txt", deleted, err)) {
            return false;
        }
        size_t n = 0;
        std::istringstream in(deleted);
        std::string line;
        while (in >> line) {
            ++n;
        }
        std::string note = std::to_string(t.used_free.size());
        note += " free inodes once in use, ";
        note += std::to_string(n);
        note += " of them deleted by the Test corpus";
        notes.push_back(note);
    }
    return true;
}

// check-tool read-bytes DEVICE SKIP LENGTH LIST.
int
read_bytes_cli(
    const std::vector<std::string> &args, std::ostream &out, std::ostream &err)
{
    uint64_t skip = 0;
    uint64_t length = 0;
    if (args.size() != 5 || !decimal(args[2], skip) ||
        !decimal(args[3], length) || length == 0 || length > 64) {
        err << "check-tool: read-bytes needs DEVICE SKIP LENGTH (1-64) LIST\n"
            << USAGE;
        return COMPARE_ERROR;
    }
    std::ifstream dev(args[1], std::ios::binary);
    std::string list;
    if (!dev) {
        err << "check-tool: cannot open '" << args[1] << "'\n";
        return COMPARE_ERROR;
    }
    if (!read_file(args[4], list, err)) {
        return COMPARE_ERROR;
    }
    static const char HEX[] = "0123456789abcdef";
    std::istringstream lines(list);
    std::string line;
    std::vector<char> buf(length);
    while (std::getline(lines, line)) {
        std::istringstream words(line);
        std::string label;
        std::string offset;
        std::string extra;
        uint64_t at = 0;
        if (!(words >> label)) {
            continue; // a blank line
        }
        if (!(words >> offset) || (words >> extra) || !decimal(offset, at)) {
            err << "check-tool: read-bytes: cannot read '" << line << "'\n";
            return COMPARE_ERROR;
        }
        dev.clear();
        dev.seekg(static_cast<std::streamoff>(at + skip));
        if (!dev.read(buf.data(), static_cast<std::streamsize>(length))) {
            err << "check-tool: read-bytes: cannot read " << length
                << " bytes at " << at + skip << " of " << args[1] << '\n';
            return COMPARE_ERROR;
        }
        out << label << ' ';
        for (char c : buf) {
            const auto b = static_cast<unsigned char>(c);
            out << HEX[b >> 4] << HEX[b & 0xF];
        }
        out << '\n';
    }
    return COMPARE_PASS;
}

// check-tool plan-inodes|plan-blocks RECORD.
int
plan_cli(
    const std::vector<std::string> &args, std::ostream &out, std::ostream &err)
{
    if (args.size() != 2) {
        err << "check-tool: " << args[0] << " needs one walk record\n" << USAGE;
        return COMPARE_ERROR;
    }
    std::string record;
    if (!read_file(args[1], record, err)) {
        return COMPARE_ERROR;
    }
    std::vector<uint64_t> v;
    std::string error;
    const bool ok = args[0] == "plan-inodes"
        ? walk_plan_inodes(record, v, error)
        : walk_plan_blocks(record, v, error);
    if (!ok) {
        err << "check-tool: " << args[1] << ": " << error << '\n';
        return COMPARE_ERROR;
    }
    for (uint64_t n : v) {
        out << n << '\n';
    }
    return COMPARE_PASS;
}

} // namespace

int
check_tool_cli(
    const std::vector<std::string> &args, std::ostream &out, std::ostream &err)
{
    const auto usage_error = [&err](const std::string &msg) {
        err << "check-tool: " << msg << '\n' << USAGE;
        return COMPARE_ERROR;
    };
    if (args.empty()) {
        return usage_error("no tool");
    }
    if (args[0] == "-h" || args[0] == "--help") {
        out << USAGE;
        return COMPARE_PASS;
    }
    if (args[0] == "plan-inodes" || args[0] == "plan-blocks") {
        return plan_cli(args, out, err);
    }
    if (args[0] == "read-bytes") {
        return read_bytes_cli(args, out, err);
    }
    const std::string &tool = args[0];
    bool known_tool = false;
    for (const char *t : TOOLS) {
        known_tool = known_tool || tool == t;
    }
    if (!known_tool) {
        return usage_error("unknown tool '" + tool + "'");
    }
    std::map<std::string, std::string> opt;
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string &a = args[i];
        bool known = false;
        for (const char *o : VALUE_OPTIONS) {
            known = known || a == o;
        }
        if (!known) {
            return usage_error("unknown argument '" + a + "'");
        }
        if (i + 1 >= args.size()) {
            return usage_error(a + " needs a value");
        }
        opt[a] = args[++i];
    }
    const auto has = [&opt](const char *o) { return opt.count(o) > 0; };
    if (!has("--ground-truth-dir") || !has("--run-dir")) {
        return usage_error("--ground-truth-dir and --run-dir are required");
    }
    if (has("--known-failures") && (!has("--check") || !has("--partition"))) {
        return usage_error("--known-failures needs --check and --partition");
    }
    if (has("--override") && tool != "fsstat" && tool != "blkls" &&
        tool != "ils" && tool != "ifind") {
        return usage_error("--override is for fsstat, blkls, ils and ifind");
    }
    size_t max_items = 20;
    if (has("--max-items")) {
        const std::string &m = opt["--max-items"];
        if (m.empty() || m.size() > 9 ||
            m.find_first_not_of("0123456789") != std::string::npos) {
            return usage_error("--max-items must be a number");
        }
        max_items = static_cast<size_t>(std::stoul(m));
    }

    CompareOptions o;
    o.check = opt["--check"];
    o.partition = opt["--partition"];
    if (has("--known-failures")) {
        std::ifstream in(opt["--known-failures"], std::ios::binary);
        std::string error;
        if (!in) {
            err << "check-tool: cannot open Known failure list '"
                << opt["--known-failures"] << "'\n";
            return COMPARE_ERROR;
        }
        if (!read_known_failures(in, o.known_failures, error)) {
            err << "check-tool: " << opt["--known-failures"] << ": " << error
                << '\n';
            return COMPARE_ERROR;
        }
    }

    const std::string &gt_dir = opt["--ground-truth-dir"];
    const std::string &run_dir = opt["--run-dir"];
    std::string hook; // the test hook's note, if any
    CompareResult r;
    std::vector<std::string> notes; // the walk record's facts, for the report
    if (tool == "ils" || tool == "ifind" || tool == "blkls") {
        if (!walk_notes(gt_dir, tool, notes, err)) {
            return COMPARE_ERROR;
        }
    }
    if (tool == "fsstat" || tool == "blkls") {
        Summary expected;
        Summary run;
        if (tool == "fsstat") {
            std::string raw;
            ToolOutput t;
            if (!read_file(gt_dir + "/fs-summary.txt", raw, err) ||
                !read_tool(run_dir + "/fsstat", t, err)) {
                return COMPARE_ERROR;
            }
            expected = guest_summary(raw);
            // The run is read against the guest's real summary, so an
            // override can't change how fsstat's output is read.
            run = fsstat_run(t, expected);
        }
        else {
            std::string record;
            std::string lines;
            ToolOutput t;
            if (!read_file(gt_dir + "/walk-record.txt", record, err) ||
                !read_tool(run_dir + "/blkls", t, err) ||
                !read_file(run_dir + "/blkls.lines", lines, err)) {
                return COMPARE_ERROR;
            }
            expected = blkls_expected(record);
            run = blkls_run(t, lines);
        }
        if (has("--override")) {
            const std::string &ov = opt["--override"];
            const size_t eq = ov.find('=');
            bool found = false;
            for (auto &f : expected.fields) {
                if (eq != std::string::npos && f.first == ov.substr(0, eq)) {
                    f.second = ov.substr(eq + 1);
                    found = true;
                }
            }
            if (!found && !expected.has_error) {
                return usage_error("--override needs FIELD=VALUE with a field "
                                   "of the summary, not '" +
                    ov + "'");
            }
            hook = "test hook: the expected " + ov.substr(0, eq) +
                " was replaced with '" + ov.substr(eq + 1) + "'";
        }
        r = compare_summaries(expected, run, o);
    }
    else {
        Manifest expected;
        Manifest run;
        if (!manifests(tool, gt_dir, run_dir, expected, run, err)) {
            return COMPARE_ERROR;
        }
        if (has("--override")) {
            std::string why;
            if (!override_entry(tool, opt["--override"], expected, hook, why)) {
                return usage_error(why);
            }
        }
        r = compare_manifests(expected, run, o);
    }

    out << "CLI tool check: " << tool;
    if (!o.check.empty() || !o.partition.empty()) {
        out << ", check " << o.check << ", partition " << o.partition;
    }
    out << "\nGround truth: " << gt_dir << "\nRun: " << run_dir << '\n';
    if (!hook.empty()) {
        out << hook << '\n';
    }
    for (const std::string &n : notes) {
        out << "Walk record: " << n << '\n';
    }
    write_report(out, r);

    if (has("--items")) {
        std::ofstream items(opt["--items"], std::ios::app | std::ios::binary);
        if (!items) {
            err << "check-tool: cannot append to '" << opt["--items"] << "'\n";
            return COMPARE_ERROR;
        }
        std::string label = o.check;
        if (!o.partition.empty()) {
            label += (label.empty() ? "" : " ") + o.partition;
        }
        if (!hook.empty()) {
            items << "warn\t" << (label.empty() ? "" : label + ": ") << hook
                  << '\n';
        }
        if (tool == "ils") {
            for (const std::string &n : notes) {
                items << "note\t" << (label.empty() ? "" : label + ": ") << n
                      << '\n';
            }
        }
        write_items(items, r, label, max_items);
    }
    return r.verdict() == Verdict::Fail ? COMPARE_FAIL : COMPARE_PASS;
}

} // namespace tskval
