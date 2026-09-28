#include "security.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <array>
#include <vector>
#include <stdexcept>

namespace {
std::string hex(const unsigned char *data, size_t size) {
    static const char alphabet[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) { out += alphabet[data[i] >> 4]; out += alphabet[data[i] & 15]; }
    return out;
}
}
std::string Security::randomHex(int bytes) {
    std::vector<unsigned char> data(bytes);
    if (RAND_bytes(data.data(), bytes) != 1) throw std::runtime_error("Secure randomness unavailable");
    return hex(data.data(), data.size());
}
std::string Security::digest(const std::string &text) {
    std::array<unsigned char, 32> data{};
    unsigned int size = 0;
    if (EVP_Digest(text.data(), text.size(), data.data(), &size, EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("Digest failed");
    return hex(data.data(), size);
}
std::string Security::passwordHash(const std::string &password, const std::string &salt) {
    std::array<unsigned char, 32> data{};
    // Version 1 credential format: scrypt N=32768, r=8, p=3, 64 MiB limit.
    if (EVP_PBE_scrypt(password.data(), password.size(), reinterpret_cast<const unsigned char *>(salt.data()),
                       salt.size(), 32768, 8, 3, 64 * 1024 * 1024, data.data(), data.size()) != 1)
        throw std::runtime_error("Password derivation failed");
    return hex(data.data(), data.size());
}
bool Security::equal(const std::string &a, const std::string &b) {
    return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}
