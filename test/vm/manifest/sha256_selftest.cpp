/*
 * Self-test of the SHA-256 implementation that the harness and Ground truth
 * capture share (sha256.h). Expected digests are the FIPS 180-4 examples
 * (NIST CSRC "Examples with Intermediate Values") and well-known values
 * from `sha256sum`.
 */
#include "sha256.h"

#include <cppunit/extensions/HelperMacros.h>

#include <cstddef>
#include <string>

using tskval::Sha256;

namespace {

std::string
digest_of(const std::string &data)
{
    Sha256 h;
    h.update(data.data(), data.size());
    return h.hex_digest();
}

// The same data fed in pieces of `step` bytes.
std::string
digest_in_steps(const std::string &data, size_t step)
{
    Sha256 h;
    for (size_t off = 0; off < data.size(); off += step) {
        size_t n = data.size() - off < step ? data.size() - off : step;
        h.update(data.data() + off, n);
    }
    return h.hex_digest();
}

} // namespace

class Sha256Test : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(Sha256Test);
    CPPUNIT_TEST(empty_message);
    CPPUNIT_TEST(abc);
    CPPUNIT_TEST(two_block_message);
    CPPUNIT_TEST(million_a);
    CPPUNIT_TEST(padding_boundaries);
    CPPUNIT_TEST(pieces_give_the_same_digest);
    CPPUNIT_TEST(all_byte_values);
    CPPUNIT_TEST_SUITE_END();

  public:
    void empty_message()
    {
        CPPUNIT_ASSERT_EQUAL(std::string("e3b0c44298fc1c149afbf4c8996fb924"
                                         "27ae41e4649b934ca495991b7852b855"),
            digest_of(""));
    }

    void abc()
    {
        CPPUNIT_ASSERT_EQUAL(std::string("ba7816bf8f01cfea414140de5dae2223"
                                         "b00361a396177a9cb410ff61f20015ad"),
            digest_of("abc"));
    }

    void two_block_message()
    {
        CPPUNIT_ASSERT_EQUAL(std::string("248d6a61d20638b8e5c026930c3e6039"
                                         "a33ce45964ff2167f6ecedd419db06c1"),
            digest_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnop"
                      "nopq"));
    }

    void million_a()
    {
        CPPUNIT_ASSERT_EQUAL(std::string("cdc76e5c9914fb9281a1c7e284d73e67"
                                         "f1809a48a497200e046d39ccc7112cd0"),
            digest_of(std::string(1000000, 'a')));
    }

    // Lengths around the 55/56/64-byte padding boundaries, checked against
    // `printf 'a%.0s' $(seq N) | sha256sum`.
    void padding_boundaries()
    {
        CPPUNIT_ASSERT_EQUAL(std::string("9f4390f8d30c2dd92ec9f095b65e2b9a"
                                         "e9b0a925a5258e241c9f1e910f734318"),
            digest_of(std::string(55, 'a')));
        CPPUNIT_ASSERT_EQUAL(std::string("b35439a4ac6f0948b6d6f9e3c6af0f5f"
                                         "590ce20f1bde7090ef7970686ec6738a"),
            digest_of(std::string(56, 'a')));
        CPPUNIT_ASSERT_EQUAL(std::string("ffe054fe7ae0cb6dc65c3af9b61d5209"
                                         "f439851db43d0ba5997337df154668eb"),
            digest_of(std::string(64, 'a')));
    }

    void pieces_give_the_same_digest()
    {
        std::string data;
        for (int i = 0; i < 1000; ++i) {
            data += static_cast<char>(i * 7 + 3);
        }
        const std::string whole = digest_of(data);
        const size_t steps[] = {1, 3, 55, 63, 64, 65, 127, 999};
        for (size_t step : steps) {
            CPPUNIT_ASSERT_EQUAL(whole, digest_in_steps(data, step));
        }
    }

    // Bytes 0x00..0xFF once each, checked against
    // `printf "$(printf '\\%03o' $(seq 0 255))" | sha256sum`.
    void all_byte_values()
    {
        std::string data;
        for (int i = 0; i < 256; ++i) {
            data += static_cast<char>(i);
        }
        CPPUNIT_ASSERT_EQUAL(std::string("40aff2e9d2d8922e47afd4648e6967497"
                                         "158785fbd1da870e7110266bf944880"),
            digest_of(data));
    }
};
CPPUNIT_TEST_SUITE_REGISTRATION(Sha256Test);
