#pragma once
// IBus のエンジン（IBusEngine のサブクラス）。入力コンテキストごとに 1 つ作られ、それぞれが
// SessionManager のセッションを 1 つ持つ。キーを送って返ってきた表示（SessionOutput）を IBus に反映するだけで、
// 入力の判断と状態はセッション（iroha-core の InputSession）にある。
// Windows版 TIP の TextService にあたる（文書への書き込みは IBus がアプリに伝える）
#include <ibus.h>

#include <functional>
#include <string>

namespace iroha {
class EngineHost;
class SessionManager;
} // namespace iroha

G_BEGIN_DECLS
#define IROHA_TYPE_ENGINE (iroha_engine_get_type())
GType iroha_engine_get_type(void);
G_END_DECLS

namespace iroha::ibus {

// プロセスで 1 つのエンジン一式とセッションの表（main が作り、ibus_main の前に渡す）
void SetShared(EngineHost* host, SessionManager* sessions, std::function<void(const std::string&)> log);
// 設定を読み直した（入力メニューの状態を合わせる）
void SettingsReloaded();

} // namespace iroha::ibus
