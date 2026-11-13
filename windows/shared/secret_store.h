#pragma once
// API キーなどの秘密の文字列を、Windows の資格情報マネージャー（汎用資格情報 "iroha/<名前>"）に置く。
// macOS版の SecretStore（キーチェーン）にあたる。config.json には書かない（平文のファイルを避ける）

#include <string>

namespace iroha {
namespace SecretStore {

// 読めなければ（無ければ）偽
bool Get(const std::string& account, std::string* value);
// 空の値を渡すと消す
bool Set(const std::string& account, const std::string& value);

} // namespace SecretStore
} // namespace iroha
