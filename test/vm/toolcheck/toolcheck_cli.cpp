/*
 * Command line of the CLI tool checks. See toolcheck_cli.h.
 */
#include "toolcheck_cli.h"

#include "compare.h"
#include "compare_cli.h"
#include "toolcheck.h"

#include <fstream>
#include <map>
#include <ostream>
#include <sstream>

namespace tskval {

namespace {

const char USAGE[] =
    "Usage: check-tool fls|icat|fsstat|istat --ground-truth-dir DIR\n"
    "                  --run-dir DIR [options]\n"
    "\n"
    "Compares the output of one of TSK's command-line tools with Ground\n"
    "truth under the Strict check's rules and prints a report. Exit status:\n"
    "0 pass, 1 failures, 2 bad usage or unreadable input.\n"
    "\n"
    "Ground truth directory: ground-truth.manifest (fls, icat),\n"
    "fs-summary.txt (fsstat), stat.txt (istat). Run directory: the files\n"
    "test/vm/toolcheck/run-tools.sh writes: fls.{out,err,status},\n"
    "icat-root.{out,err,status} and icat.tsv, fsstat.{out,err,status},\n"
    "istat/<inode>.{out,err,status}.\n"
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
    "  --override FIELD=VALUE  fsstat, test only: replace the expected value\n"
    "                          of a summary field\n"
    "  -h, --help              show this help\n";

const char *const VALUE_OPTIONS[] = {"--ground-truth-dir", "--run-dir",
    "--known-failures", "--check", "--partition", "--items", "--max-items",
    "--override"};

const char *const TOOLS[] = {"fls", "icat", "fsstat", "istat"};

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
    Manifest gt;
    if (!read_ground_truth(gt_dir, gt, err)) {
        return false;
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
    if (has("--override") && tool != "fsstat") {
        return usage_error("--override is for fsstat only");
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
    if (tool == "fsstat") {
        std::string raw;
        ToolOutput t;
        if (!read_file(gt_dir + "/fs-summary.txt", raw, err) ||
            !read_tool(run_dir + "/fsstat", t, err)) {
            return COMPARE_ERROR;
        }
        Summary expected = guest_summary(raw);
        // The run is read against the guest's real summary, so an override
        // can't change how fsstat's output is read.
        const Summary run = fsstat_run(t, expected);
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
        write_items(items, r, label, max_items);
    }
    return r.verdict() == Verdict::Fail ? COMPARE_FAIL : COMPARE_PASS;
}

} // namespace tskval
