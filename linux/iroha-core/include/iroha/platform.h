#pragma once
// Linux の置き場所と環境（Windows版の %LOCALAPPDATA%\iroha・GetModuleFileName・GetComputerName にあたる）
#include <filesystem>
#include <optional>
#include <string>

namespace iroha::platform {

// 環境変数（無い・空なら nullopt）
std::optional<std::string> GetEnv(const char* name);
// 環境変数が value と一致するか
bool EnvEquals(const char* name, const char* value);

// データフォルダ: IROHA_DATA_DIR > $XDG_DATA_HOME/iroha > ~/.local/share/iroha
// （config.json・models/・learning.json・user-dictionary.json などの置き場所。形式は macOS版・Windows版と同じ）
std::filesystem::path DataDirectory();
// ファイルの場所: 環境変数 envName があればそれ、無ければ <データフォルダ>/<relative>
std::filesystem::path StorePath(const char* envName, const std::filesystem::path& relative);
// 動いている実行ファイルのあるフォルダ（/proc/self/exe。辞書・記号のデータを探すのに使う）
std::filesystem::path ExecutableDirectory();
// このコンピュータの名前（入力履歴・変換記録のファイル名に使う。macOS版のホスト名にあたる）
std::string HostName();
// 使うモデルのパス: IROHA_MODEL > config.json の "model"（models/ からの相対か絶対パス）> 既定のモデル
std::string ResolveModelPath();

} // namespace iroha::platform
