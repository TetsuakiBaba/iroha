#pragma once
// 設定 > モデル > 変換のベンチマーク（開発者モード）。
// 移植元: macos/Sources/iroha/BenchmarkView.swift・BenchmarkCoordinator.swift
//
// 評価セットを取得し、今動いているモデル・アダプタで 1 件ずつ変換して精度（acc@1・MinCER）と 1 回の変換の時間を測る。
// 計測は別プロセス iroha-bench.exe（入力を止めないため・異常終了を設定画面から切り離すため）で行い、標準出力の
// JSON Lines（iroha::benchmark_run::Event）を受け取って画面に出す。結果は <データフォルダ>\logs\bench\ に残る
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <commctrl.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "iroha/benchmark_catalog.h"
#include "iroha/benchmark_run.h"
#include "ui.h"

namespace benchmark_section {

// 評価セットの一覧の取得・評価セットの取得が終わった（lParam は TaskResult*。受け取った側が消す）
constexpr UINT WM_APP_BENCH_TASK = WM_APP + 20;
// iroha-bench の標準出力の 1 行（wParam は計測の番号、lParam は std::string*。受け取った側が消す）
constexpr UINT WM_APP_BENCH_LINE = WM_APP + 21;
// iroha-bench が終わった（wParam は計測の番号、lParam は終了コード）
constexpr UINT WM_APP_BENCH_EXIT = WM_APP + 22;

// 部品の番号（設定画面のほかの部品と重ならない範囲）
constexpr int kIdFirst = 900;
constexpr int kIdLast = 919;

class Section {
public:
    Section() = default;
    Section(const Section&) = delete;
    Section& operator=(const Section&) = delete;
    ~Section();

    // モデルのページの末尾に節を足す。visible は開発者モードか
    void Build(HWND window, ui::Page* page, std::function<bool()> visible, std::filesystem::path dataDirectory,
               std::string appVersion);
    // 測るモデル・アダプタ（変換サーバと同じ決め方。アダプタが無ければ空）
    void SetTarget(const std::filesystem::path& modelPath, const std::filesystem::path& adapterPath);
    // ページを開いた（入れた評価セットと記録を読み直し、開発者モードなら一覧を 1 回取りにいく）
    void Show();
    // 計測中なら止める（窓を閉じるとき）
    void Shutdown();

    bool OnCommand(int id, int code);
    bool OnNotify(const NMHDR* header);
    // WM_APP_BENCH_* を受けたら真
    bool OnMessage(UINT message, WPARAM wParam, LPARAM lParam);

    // 入れてある評価セット（設定 > 情報 のライセンス一覧に出典を出す）
    std::vector<iroha::BenchmarkInstall::Record> InstalledRecords() const;

    struct TaskResult;

private:
    enum class State { Idle, Loading, Running, Failed, Cancelled };

    void RefreshInstalled();
    void RefreshRecords();
    void UpdateControls();
    void ShowRecord();
    void StartTask(std::wstring busyMessage, bool fetchCatalog, std::string installId);
    void OnTaskDone(TaskResult* result);
    void Start();
    void Cancel();
    void OnLine(const std::string& line);
    void OnExit(DWORD exitCode);
    std::optional<std::filesystem::path> HelperPath() const;
    std::filesystem::path LogPath() const;

    HWND window_ = nullptr;
    ui::Page* page_ = nullptr;
    std::function<bool()> visible_;
    std::filesystem::path dataDirectory_;
    std::string appVersion_;
    std::filesystem::path modelPath_;
    std::filesystem::path adapterPath_;

    HWND setList_ = nullptr, setFetch_ = nullptr, setRemove_ = nullptr;
    HWND setCombo_ = nullptr, latticeCombo_ = nullptr;
    HWND start_ = nullptr, cancel_ = nullptr;
    HWND result_ = nullptr, copy_ = nullptr, folder_ = nullptr, misses_ = nullptr;
    HWND history_ = nullptr, deleteRecord_ = nullptr;
    ui::Page::Row* setRow_ = nullptr;
    ui::Page::Row* targetRow_ = nullptr;
    ui::Page::Row* runRow_ = nullptr;
    ui::Page::Row* resultRow_ = nullptr;
    ui::Page::Row* historyRow_ = nullptr;

    std::unique_ptr<iroha::BenchmarkCatalog> catalog_;
    std::wstring catalogMessage_;
    bool catalogFailed_ = false;
    bool catalogRequested_ = false;
    bool taskBusy_ = false;
    std::vector<iroha::BenchmarkInstall::Record> installed_;
    std::vector<iroha::benchmark_run::Record> records_;
    // 表の行（カタログの評価セット + カタログに無いが入れてあるもの）の ID
    std::vector<std::string> setRows_;
    // 開いている結果（空なら直近のもの）と、間違えた項目を出しているか
    std::string shownRecordId_;
    bool showingMisses_ = false;
    // 測る評価セット・条件（設定画面を開いている間だけ覚える）
    std::string selectedSet_;
    iroha::benchmark_run::Lattice lattice_ = iroha::benchmark_run::Lattice::Off;

    // 部品ごとに操作できるか（ページを並べ直しても保つため、部品の enabled から読む）
    bool canFetch_ = false;
    bool canRemove_ = false;
    bool canChooseSet_ = false;
    bool canChooseLattice_ = true;
    bool canStart_ = false;
    bool canCancel_ = false;
    bool canMisses_ = false;
    bool canDeleteRecord_ = false;

    State state_ = State::Idle;
    int done_ = 0;
    int total_ = 0;
    int hits_ = 0;
    std::wstring failure_;
    HANDLE process_ = nullptr;
    WPARAM generation_ = 0;
};

} // namespace benchmark_section
