#pragma once
// API キーなどの秘密の文字列を、秘密の保管庫（libsecret。GNOME キーリングなど）に置く。
// macOS版の SecretStore（キーチェーン）・Windows版の SecretStore（資格情報マネージャー）にあたる。
// config.json には書かない（平文のファイルを避ける）。
// **IBus のエンジンの main thread では呼ばない**（保管庫が鍵を掛けていると、答えるまで止まることがある。
// macOS版でキーチェーンを main thread で読んで入力が止まったのと同じ。AI に依頼する裏のスレッドで読む）

#include <string>

namespace iroha {
namespace SecretStore {

// 読めなければ（無ければ）偽
bool Get(const std::string& account, std::string* value);
// 空の値を渡すと消す
bool Set(const std::string& account, const std::string& value);

} // namespace SecretStore
} // namespace iroha
