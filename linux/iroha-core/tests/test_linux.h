#pragma once
// Linux版で足した部品のテスト（SHA-256・パスの変換）
#include <filesystem>
#include <fstream>
#include <string>

#include "iroha/sha256.h"
#include "iroha/unicode.h"

// FIPS 180-4 の例と、ブロックの境目（55・56・64 バイト）をまたぐ長さ
void TestSha256() {
    EXPECT_TRUE(iroha::Sha256::Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_TRUE(iroha::Sha256::Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_TRUE(iroha::Sha256::Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
                "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    EXPECT_TRUE(iroha::Sha256::Hex(std::string(1000000, 'a')) ==
                "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    EXPECT_TRUE(iroha::Sha256::Hex(std::string(55, 'a')) ==
                "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    EXPECT_TRUE(iroha::Sha256::Hex(std::string(56, 'a')) ==
                "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    EXPECT_TRUE(iroha::Sha256::Hex(std::string(64, 'a')) ==
                "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
    // ファイルから読んでも同じ（日本語の名前のファイル）
    const auto path = std::filesystem::temp_directory_path() / iroha::PathFromUtf8("iroha-テスト-sha256.txt");
    {
        std::ofstream file(path, std::ios::binary);
        file << "abc";
    }
    EXPECT_TRUE(iroha::Sha256::FileHex(path) == iroha::Sha256::Hex("abc"));
    std::filesystem::remove(path);
    EXPECT_TRUE(iroha::Sha256::FileHex(path).empty());
}

void TestPathUtf8() {
    const std::string name = "変換記録-ホスト.jsonl";
    EXPECT_TRUE(iroha::PathToUtf8(iroha::PathFromUtf8(name)) == name);
    EXPECT_TRUE(iroha::PathToUtf8(iroha::TemporaryPath(iroha::PathFromUtf8("/a/学習.json"))) == "/a/学習.json.tmp");
}

void RunLinuxTests() {
    TestSha256();
    TestPathUtf8();
}
