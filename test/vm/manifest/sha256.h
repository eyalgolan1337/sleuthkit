/*
 * SHA-256 (FIPS 180-4), streaming. The harness and Ground truth capture hash
 * file content with it, so both depend on nothing but the C++ standard
 * library: libtsk gets no hashing API, and the guest build links no extra
 * library.
 *
 * C++14; no dependency on libtsk.
 */
#ifndef TSK_TEST_VM_SHA256_H
#define TSK_TEST_VM_SHA256_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace tskval {

class Sha256 {
  public:
    Sha256();
    void update(const void *data, size_t len);
    // The digest as 64 lowercase hex digits. Ends the computation: call it
    // once, after the last update.
    std::string hex_digest();

  private:
    void compress(const uint8_t *block);

    uint64_t m_total = 0; // bytes passed to update
    uint32_t m_state[8];
    uint8_t m_block[64];
    size_t m_used = 0; // bytes of m_block in use
};

} // namespace tskval

#endif
