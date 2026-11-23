/*
 * The Test corpus generator: writes a fixed tree, derived from a seed, that
 * exercises each on-disk format variant (every XFS directory format, an
 * extent B+tree, holes, reflinks, links, odd names, extreme timestamps, a
 * large file). See test/vm/README.md ("The Test corpus").
 *
 * The same seed and options on an empty target give the same tree and the
 * same content: every name, size, byte, mode, owner and mtime/atime. Only
 * what the kernel chooses differs (inode numbers, ctime, crtime, physical
 * layout). The generator adapts to the target: what it can't make there (for
 * example reflinks on ext4) is recorded with the reason. Where it can, it
 * makes the nearest thing instead (a plain copy for a reflink, the clamped
 * value for a timestamp out of range), so the paths stay the same.
 *
 * C++14, Linux. Needs only the C++ standard library and glibc, so it links
 * statically into the guest build.
 */
#ifndef TSK_TEST_VM_CORPUS_H
#define TSK_TEST_VM_CORPUS_H

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace tskval {

// A seeded pseudo-random stream (xoshiro256**, seeded with splitmix64). The
// same seed always gives the same sequence, on every platform.
class Prng {
  public:
    explicit Prng(uint64_t seed);
    uint64_t next();
    // A value in [lo, hi], both included.
    uint64_t range(uint64_t lo, uint64_t hi);
    void fill(unsigned char *buf, size_t len);

  private:
    uint64_t m_s[4];
};

// A seed derived from the corpus seed and a string, such as a path, so each
// file's content depends only on the seed and its own path.
uint64_t derive_seed(uint64_t seed, const std::string &key);

struct CorpusOptions {
    uint64_t seed = 0x75b0c0de;
    // Size of /files/large.bin.
    uint64_t large_size = 0;
    // Entries of the B+tree directory, /dirs/btree.
    unsigned btree_entries = 50000;
};

// One line of the generator's record: a feature it made, one it could not
// make on this target and why, or the error that stopped it.
struct RecordLine {
    enum Kind { Made, NotMade, Failed };
    Kind kind = Made;
    std::string feature;
    std::string detail; // what was made, why not, or what failed
};

// Writes the corpus into `dir`, which must be an empty directory or not
// exist (its parent must). Returns false if it could not write it; the
// record's last line (Failed) says why. Features the target lacks are not
// failures: they are recorded as NotMade.
bool generate_corpus(const std::string &dir, const CorpusOptions &options,
    std::vector<RecordLine> &record);

// Record lines as the command prints them: "made", "not-made" or "failed",
// a tab, the feature, a tab, the detail.
std::string format_record_line(const RecordLine &line);

// Exit status of tsk-corpus.
enum CorpusExit {
    CORPUS_OK = 0,     // the corpus is complete
    CORPUS_FAILED = 1, // writing it failed; see the record and stderr
    CORPUS_USAGE = 2   // bad usage, or the target is not empty
};

// tsk-corpus [--seed N] --large-size BYTES[K|M|G] [--btree-entries N] DIR.
// args excludes the program name. The record goes to `out`, errors to `err`.
int corpus_cli(
    const std::vector<std::string> &args, std::ostream &out, std::ostream &err);

} // namespace tskval

#endif
