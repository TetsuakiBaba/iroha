#pragma once
// 上のバーの iroha のアイコン（macOS版と同じ「い」「A」の絵）と入力メニュー。
//
// GNOME は入力ソースが 1 つだけのとき上のバーの入力ソースの表示（と IBus のプロパティのメニュー）を出さないので、
// iroha だけを入力ソースにすると、入力モードが見えずメニューも開けなくなる。そのときだけ、Ubuntu の
// トレイの表示領域（AppIndicator の拡張機能。StatusNotifierItem を受ける）に iroha のアイコンを出す。
// 入力ソースが 2 つ以上なら GNOME が出すので出さない（同じものが 2 つ並ばないように）。
//
// GTK を使わず、GDBus で StatusNotifierItem（org.kde.StatusNotifierItem）と、そのメニュー
// （com.canonical.dbusmenu）を直接出す。IBus のエンジンのプロセスの GLib の main loop で動く。
// 出すかどうか: 環境変数 IROHA_TRAY_ICON が "1" なら常に、"0" なら出さない。無ければ GNOME の設定
// org.gnome.desktop.input-sources の sources が iroha だけのとき（変わったら追う）
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "input_menu.h"

namespace iroha::ibus {

class TrayIcon {
public:
    using Activate = std::function<void(const std::string& key)>;
    using Logger = std::function<void(const std::string&)>;

    // activate: メニューの項目が押された（項目の名前）
    TrayIcon(Activate activate, Logger log);
    ~TrayIcon();
    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    // セッションの D-Bus につなぎ、出すかどうかの判定を始める（つなげなければ何もしない）
    void Start();
    // メニューの項目と入力モードの記号（menu::ModeSymbol。どちらのモードの絵を出すかの判定に使う）を今のものにする（変わったところだけ知らせる）
    void Update(const std::vector<menu::Item>& items, const std::string& symbol);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

// GNOME の入力ソースの一覧（(種類, 名前) の組）が iroha だけか（開発用の iroha-dev も含む。テストする）
inline bool OnlyIrohaInputSource(const std::vector<std::pair<std::string, std::string>>& sources) {
    return sources.size() == 1 && sources[0].first == "ibus" && sources[0].second.rfind("iroha", 0) == 0;
}

} // namespace iroha::ibus
