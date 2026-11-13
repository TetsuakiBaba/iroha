#pragma once
#include <string>
#include <vector>

namespace iroha {

// かな漢字変換エンジンの抽象。実装を差し替えることで別モデルを利用できる。
// 移植元: macos/Sources/IrohaCore/ConversionEngine.swift
//   - reading: ひらがなの読み（例: "きょうはいいてんき"）
//   - context: 直前に確定した文字列（文脈条件付け用、空でも可）
//   - candidateCount: 返す候補数の上限
//   - 成功時true、resultsに尤度順の候補。失敗時falseでerrorに理由
class ConversionEngine {
public:
    virtual ~ConversionEngine() = default;
    virtual bool Convert(const std::u32string& reading, const std::u32string& context,
                         int candidateCount, std::vector<std::u32string>* results,
                         std::string* error) = 0;
};

} // namespace iroha
