#pragma once
// 設定 > モデル > 変換のベンチマーク（開発者モード）。
// 移植元: macos/Sources/iroha/BenchmarkView.swift・BenchmarkCoordinator.swift（Windows版 windows/iroha-settings/benchmark_section）
//
// 評価セットを取得し、今動いているモデル・アダプタで 1 件ずつ変換して精度（acc@1・MinCER）と 1 回の変換の時間を測る。
// 計測は別プロセス iroha-bench（入力を止めないため・異常終了を設定画面から切り離すため）で行い、標準出力の
// JSON Lines（iroha::benchmark_run::Event）を受け取って画面に出す。結果は <データフォルダ>/logs/bench/ に残る
#include <gtk/gtk.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "iroha/benchmark_catalog.h"
#include "iroha/benchmark_run.h"

namespace benchmark_section {

class Section {
public:
    // showDialog: 知らせの窓を出す（設定画面のもの）
    Section(std::filesystem::path dataDirectory, std::string appVersion,
            std::function<void(const std::string&)> showDialog);
    ~Section();
    Section(const Section&) = delete;
    Section& operator=(const Section&) = delete;

    // モデルのページに足す部品（開発者モードのときだけ見せる）
    GtkWidget* Widget() const { return root_; }
    // 開発者モードか（見せるか）
    void SetVisible(bool visible);
    // 測るモデル・アダプタ（IBus のエンジンと同じ決め方。アダプタが無ければ空）
    void SetTarget(const std::filesystem::path& modelPath, const std::filesystem::path& adapterPath);
    // ページを開いた（入れた評価セットと記録を読み直し、開発者モードなら一覧を 1 回取りにいく）
    void Show();
    // 計測中なら止める（窓を閉じるとき）
    void Shutdown();

    struct Impl;

private:
    GtkWidget* root_ = nullptr;
    std::unique_ptr<Impl> impl_;
};

} // namespace benchmark_section
