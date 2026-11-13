#include "iroha/sha256.h"

#include <cstring>
#include <fstream>
#include <vector>

namespace iroha {

namespace {

constexpr uint32_t kRoundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

uint32_t RotateRight(uint32_t value, int amount) { return (value >> amount) | (value << (32 - amount)); }

std::string ToHex(const std::array<uint8_t, 32>& digest) {
    static const char kDigits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(64);
    for (uint8_t byte : digest) {
        hex += kDigits[byte >> 4];
        hex += kDigits[byte & 0xF];
    }
    return hex;
}

} // namespace

Sha256::Sha256()
    : state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19},
      buffer_{} {}

void Sha256::Block(const uint8_t* block) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
               (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = RotateRight(w[i - 15], 7) ^ RotateRight(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = RotateRight(w[i - 2], 17) ^ RotateRight(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t s1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
        const uint32_t choose = (e & f) ^ (~e & g);
        const uint32_t t1 = h + s1 + choose + kRoundConstants[i] + w[i];
        const uint32_t s0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::Update(const void* data, size_t length) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    totalLength_ += length;
    while (length > 0) {
        const size_t take = std::min(length, sizeof(buffer_) - bufferLength_);
        std::memcpy(buffer_ + bufferLength_, bytes, take);
        bufferLength_ += take;
        bytes += take;
        length -= take;
        if (bufferLength_ == sizeof(buffer_)) {
            Block(buffer_);
            bufferLength_ = 0;
        }
    }
}

std::array<uint8_t, 32> Sha256::Finish() {
    const uint64_t bitLength = totalLength_ * 8;
    const uint8_t padding = 0x80;
    Update(&padding, 1);
    const uint8_t zero = 0;
    while (bufferLength_ != 56) Update(&zero, 1);
    uint8_t lengthBytes[8];
    for (int i = 0; i < 8; ++i) lengthBytes[i] = static_cast<uint8_t>(bitLength >> (56 - 8 * i));
    Update(lengthBytes, 8);
    std::array<uint8_t, 32> digest{};
    for (int i = 0; i < 8; ++i) {
        digest[i * 4] = static_cast<uint8_t>(state_[i] >> 24);
        digest[i * 4 + 1] = static_cast<uint8_t>(state_[i] >> 16);
        digest[i * 4 + 2] = static_cast<uint8_t>(state_[i] >> 8);
        digest[i * 4 + 3] = static_cast<uint8_t>(state_[i]);
    }
    return digest;
}

std::string Sha256::Hex(const std::string& data) {
    Sha256 hash;
    hash.Update(data.data(), data.size());
    return ToHex(hash.Finish());
}

std::string Sha256::FileHex(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    Sha256 hash;
    std::vector<char> buffer(1 << 16);
    while (file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = file.gcount();
        if (got > 0) hash.Update(buffer.data(), static_cast<size_t>(got));
    }
    if (file.bad()) return {};
    return ToHex(hash.Finish());
}

} // namespace iroha
