#pragma once
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace iroha::json {

// 依存を持ち込まないための最小限のJSON表現。
// 学習・ユーザ辞書のファイル（macOS版のJSONEncoder出力と互換）の読み書きに使う。
struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string string; // UTF-8
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    // オブジェクトからキーを引く（無ければnullptr）
    const Value* Find(const std::string& key) const;
    // 文字列ならその値、それ以外は空
    std::string AsString() const { return type == Type::String ? string : ""; }
};

// 失敗時false。UTF-8入力を想定
bool Parse(std::string_view text, Value* out);

// JSON文字列リテラルとしてエスケープする（前後の引用符込み。
// macOS版に合わせて '/' はエスケープしない）
std::string QuoteString(std::string_view s);

} // namespace iroha::json
