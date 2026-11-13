#pragma once
// SHA-256（FIPS 180-4）。ダウンロードしたモデル・辞書の照合に使う（外部のライブラリに頼らない）
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace iroha {

class Sha256 {
public:
    Sha256();
    void Update(const void* data, size_t length);
    std::array<uint8_t, 32> Finish();

    // 16進小文字
    static std::string Hex(const std::string& data);
    // ファイルの SHA-256（16進小文字）。読めなければ空
    static std::string FileHex(const std::filesystem::path& path);

private:
    void Block(const uint8_t* block);
    uint32_t state_[8];
    uint8_t buffer_[64];
    size_t bufferLength_ = 0;
    uint64_t totalLength_ = 0;
};

} // namespace iroha
