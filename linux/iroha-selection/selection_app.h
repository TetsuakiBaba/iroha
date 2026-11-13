#pragma once
// 選択テキストのAI編集の常駐プログラム（GtkApplication。X11 だけ）。
// 移植元: windows/iroha-server/selection_assistant.cpp（コミット c12af85）・macos/Sources/iroha/Selection/

#include <functional>
#include <string>

namespace iroha::selection {

using Logger = std::function<void(const std::string&)>;

// GtkApplication を動かす（ID jp.iroha.Selection。すでに動いていれば、そちらに知らせて終わる）。終了コードを返す
int RunApplication(const Logger& log);

} // namespace iroha::selection
