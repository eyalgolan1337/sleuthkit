/*
 * Command line of the comparison step. See compare_cli.h.
 */
#include "compare_cli.h"

#include "compare.h"

#include <fstream>
#include <map>
#include <ostream>

namespace tskval {

namespace {

const char USAGE[] =
    "Usage: compare-manifests --ground-truth FILE --run FILE\n"
    "                         --mode strict|best-effort [options]\n"
    "\n"
    "Compares a run's Manifest with Ground truth and prints a per-entry\n"
    "report. Exit status: 0 pass (warnings allowed), 1 failures, 2 bad usage\n"
    "or unreadable input.\n"
    "\n"
    "Options:\n"
    "  --ground-truth FILE    Ground truth Manifest\n"
    "  --run FILE             the run's Manifest\n"
    "  --mode MODE            strict, or best-effort\n"
    "  --run-time NS          the run's timestamp: when it finished, in\n"
    "                         nanoseconds since the epoch\n"
    "                         (required for best-effort)\n"
    "  --volatile FILE        Volatile path list (best-effort)\n"
    "  --known-failures FILE  Known failure list; needs --check and "
    "--partition\n"
    "  --check NAME           the check this comparison belongs to\n"
    "  --partition NAME       the partition it compared\n"
    "  --items FILE           append fail/warn/note lines for the validate\n"
    "                         command's report to FILE\n"
    "  --max-items N          at most N fail and N warn items (default 20)\n"
    "  -h, --help             show this help\n";

const char *const VALUE_OPTIONS[] = {"--ground-truth", "--run", "--mode",
    "--run-time", "--volatile", "--known-failures", "--check", "--partition",
    "--items", "--max-items"};

bool
open_input(const std::string &path, const char *what, std::ifstream &in,
    std::ostream &err)
{
    in.open(path, std::ios::binary);
    if (!in) {
        err << "compare-manifests: cannot open " << what << " '" << path
            << "'\n";
        return false;
    }
    return true;
}

bool
load_manifest(
    const std::string &path, const char *what, Manifest &m, std::ostream &err)
{
    std::ifstream in;
    if (!open_input(path, what, in, err)) {
        return false;
    }
    std::string error;
    if (!read_manifest(in, m, error)) {
        err << "compare-manifests: " << path << ": " << error << '\n';
        return false;
    }
    return true;
}

// Appends the validate command's report items: failures and warnings (at
// most max_items of each, then a count), and a summary note.
void
write_items(std::ostream &out, const CompareResult &r, const std::string &label,
    size_t max_items)
{
    const std::string prefix = label.empty() ? "" : label + ": ";
    size_t fails = 0;
    size_t warns = 0;
    for (const Finding &f : r.findings) {
        switch (f.severity) {
        case Severity::Failure:
            if (fails++ < max_items) {
                out << "fail\t" << prefix << f.message << '\n';
            }
            break;
        case Severity::Warning:
            if (warns++ < max_items) {
                out << "warn\t" << prefix << f.message << '\n';
            }
            break;
        case Severity::Note:
            out << "note\t" << prefix << f.message << '\n';
            break;
        case Severity::Known:
            break;
        }
    }
    if (fails > max_items) {
        out << "fail\t" << prefix << (fails - max_items)
            << " more failure(s); see the log for the full report\n";
    }
    if (warns > max_items) {
        out << "warn\t" << prefix << (warns - max_items)
            << " more warning(s); see the log for the full report\n";
    }
    out << "note\t" << prefix << verdict_name(r.verdict()) << ": " << r.failures
        << " failure(s), " << r.warnings << " warning(s), " << r.known
        << " Known failure(s) tolerated; Ground truth "
        << r.ground_truth_entries << " entries, run " << r.run_entries
        << " entries\n";
}

} // namespace

int
compare_cli(
    const std::vector<std::string> &args, std::ostream &out, std::ostream &err)
{
    std::map<std::string, std::string> opt;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string &a = args[i];
        if (a == "-h" || a == "--help") {
            out << USAGE;
            return COMPARE_PASS;
        }
        bool known = false;
        for (const char *o : VALUE_OPTIONS) {
            known = known || a == o;
        }
        if (!known) {
            err << "compare-manifests: unknown argument '" << a << "'\n"
                << USAGE;
            return COMPARE_ERROR;
        }
        if (i + 1 >= args.size()) {
            err << "compare-manifests: " << a << " needs a value\n";
            return COMPARE_ERROR;
        }
        opt[a] = args[++i];
    }
    const auto has = [&opt](const char *o) { return opt.count(o) > 0; };
    const auto usage_error = [&err](const std::string &msg) {
        err << "compare-manifests: " << msg << '\n' << USAGE;
        return COMPARE_ERROR;
    };

    if (!has("--ground-truth") || !has("--run") || !has("--mode")) {
        return usage_error("--ground-truth, --run and --mode are required");
    }
    CompareOptions o;
    if (opt["--mode"] == "strict") {
        o.mode = Mode::Strict;
    }
    else if (opt["--mode"] == "best-effort") {
        o.mode = Mode::BestEffort;
    }
    else {
        return usage_error("--mode must be strict or best-effort");
    }
    if (has("--run-time") && !parse_timestamp(opt["--run-time"], o.run_time)) {
        return usage_error("--run-time must be nanoseconds since the epoch");
    }
    if (o.mode == Mode::BestEffort && !has("--run-time")) {
        return usage_error("--mode best-effort needs --run-time");
    }
    if (has("--known-failures") && (!has("--check") || !has("--partition"))) {
        return usage_error("--known-failures needs --check and --partition");
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
    o.check = opt["--check"];
    o.partition = opt["--partition"];

    Manifest gt;
    Manifest run;
    if (!load_manifest(opt["--ground-truth"], "Ground truth", gt, err) ||
        !load_manifest(opt["--run"], "run Manifest", run, err)) {
        return COMPARE_ERROR;
    }
    if (has("--volatile")) {
        std::ifstream in;
        std::string error;
        if (!open_input(opt["--volatile"], "Volatile path list", in, err)) {
            return COMPARE_ERROR;
        }
        if (!read_volatile_paths(in, o.volatile_paths, error)) {
            err << "compare-manifests: " << opt["--volatile"] << ": " << error
                << '\n';
            return COMPARE_ERROR;
        }
    }
    if (has("--known-failures")) {
        std::ifstream in;
        std::string error;
        if (!open_input(
                opt["--known-failures"], "Known failure list", in, err)) {
            return COMPARE_ERROR;
        }
        if (!read_known_failures(in, o.known_failures, error)) {
            err << "compare-manifests: " << opt["--known-failures"] << ": "
                << error << '\n';
            return COMPARE_ERROR;
        }
    }

    const CompareResult r = compare_manifests(gt, run, o);
    out << "Comparison step: " << opt["--mode"] << " mode";
    if (!o.check.empty() || !o.partition.empty()) {
        out << ", check " << o.check << ", partition " << o.partition;
    }
    out << "\nGround truth: " << opt["--ground-truth"]
        << "\nRun: " << opt["--run"] << '\n';
    if (o.mode == Mode::BestEffort) {
        out << "Run time: " << format_timestamp(o.run_time) << "; "
            << o.volatile_paths.size() << " Volatile path(s)\n";
    }
    write_report(out, r);

    if (has("--items")) {
        std::ofstream items(opt["--items"], std::ios::app | std::ios::binary);
        if (!items) {
            err << "compare-manifests: cannot append to '" << opt["--items"]
                << "'\n";
            return COMPARE_ERROR;
        }
        std::string label = o.check;
        if (!o.partition.empty()) {
            label += (label.empty() ? "" : " ") + o.partition;
        }
        write_items(items, r, label, max_items);
    }
    return r.verdict() == Verdict::Fail ? COMPARE_FAIL : COMPARE_PASS;
}

} // namespace tskval
