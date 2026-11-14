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
    // オブジェクトにキーを足す（あれば置き換える）
    void Set(const std::string& key, Value member);
};

// 値を作る（書き出し用）
Value MakeString(std::string_view text);
Value MakeNumber(double number);
Value MakeBool(bool boolean);
Value MakeArray();
Value MakeObject();

// JSON にする。オブジェクトのキーは名前の順（macOS版の JSONEncoder の sortedKeys と同じ）。pretty なら 2 文字の字下げと
// 「"キー" : 値」（JSONEncoder の prettyPrinted と同じ見た目）。数は元に戻せる最短の表記（整数なら小数点なし）
std::string Serialize(const Value& value, bool pretty = false);
std::string FormatNumber(double value);

// 失敗時false。UTF-8入力を想定
bool Parse(std::string_view text, Value* out);

// JSON文字列リテラルとしてエスケープする（前後の引用符込み。
// macOS版に合わせて '/' はエスケープしない）
std::string QuoteString(std::string_view s);

} // namespace iroha::json
