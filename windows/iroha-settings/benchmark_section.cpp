#include "benchmark_section.h"

#include <shellapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <cstdio>

#include "benchmark_fetcher.h"
#include "iroha/unicode.h"

namespace benchmark_section {

namespace {

using iroha::benchmark_run::Lattice;
using Row = ui::Page::Row;

enum Id {
    kIdSetList = kIdFirst,
    kIdSetFetch,
    kIdSetRemove,
    kIdSetCombo,
    kIdLatticeCombo,
    kIdStart,
    kIdCancel,
    kIdResult,
    kIdCopy,
    kIdFolder,
    kIdMisses,
    kIdHistory,
    kIdDeleteRecord,
};

std::wstring Wide(const std::string& utf8) { return iroha::Utf32ToUtf16(iroha::Utf8ToUtf32(utf8)); }
std::string Narrow(const std::wstring& text) { return iroha::Utf32ToUtf8(iroha::Utf16ToUtf32(text)); }

std::wstring Format(const wchar_t* format, double value) {
    wchar_t buffer[64];
    _snwprintf_s(buffer, _TRUNCATE, format, value);
    return buffer;
}

std::wstring Percent(double ratio, int digits) {
    return Format(digits == 1 ? L"%.1f%%" : digits == 2 ? L"%.2f%%" : L"%.0f%%", ratio * 100);
}

// 「173 KB」「1.6 MB」
std::wstring ByteCount(uint64_t bytes) {
    if (bytes < 1000 * 1000) return Format(L"%.0f KB", static_cast<double>(bytes) / 1000);
    return Format(L"%.1f MB", static_cast<double>(bytes) / 1000 / 1000);
}

HWND MakeList(HWND parent, int id, std::initializer_list<std::pair<const wchar_t*, int>> columns) {
    HWND list = ui::CreateControl(parent, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, id);
    SetWindowTheme(list, L"Explorer", nullptr);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    ListView_SetBkColor(list, ui::g_theme.card);
    ListView_SetTextBkColor(list, ui::g_theme.card);
    LVCOLUMNW column = {};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    int index = 0;
    for (const auto& [title, width] : columns) {
        column.cx = ui::g_theme.S(width);
        column.pszText = const_cast<wchar_t*>(title);
        ListView_InsertColumn(list, index++, &column);
    }
    return list;
}

void SetListRow(HWND list, int row, const std::vector<std::wstring>& cells) {
    LVITEMW item = {};
    item.mask = LVIF_TEXT;
    item.iItem = row;
    item.pszText = const_cast<wchar_t*>(cells[0].c_str());
    ListView_InsertItem(list, &item);
    for (size_t column = 1; column < cells.size(); ++column) {
        ListView_SetItemText(list, row, static_cast<int>(column), const_cast<wchar_t*>(cells[column].c_str()));
    }
}

int SelectedRow(HWND list) { return ListView_GetNextItem(list, -1, LVNI_SELECTED); }

// 操作できるかを flag から読む部品（ページを並べ直しても EnableWindow の状態を保つ）
ui::Page::Control Gated(HWND hwnd, int width, const bool* flag, int height = 32, int dropHeight = 0) {
    ui::Page::Control control;
    control.hwnd = hwnd;
    control.width = width;
    control.height = height;
    control.dropHeight = dropHeight;
    control.enabled = [flag] { return *flag; };
    return control;
}

// コマンドラインの 1 つの引数（CommandLineToArgvW が元に戻せる形）
std::wstring Quote(const std::wstring& argument) {
    std::wstring quoted = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
        } else {
            quoted.append(backslashes, L'\\');
        }
        backslashes = 0;
        quoted += c;
    }
    quoted.append(backslashes * 2, L'\\');
    quoted += L'"';
    return quoted;
}

std::filesystem::path ExecutableDirectory() {
    wchar_t modulePath[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    return std::filesystem::path(modulePath).parent_path();
}

std::string HostName() {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD length = ARRAYSIZE(name);
    if (!GetComputerNameW(name, &length)) return "windows";
    return Narrow(std::wstring(name, length));
}

std::wstring RecordTitle(const iroha::benchmark_run::Record& record) {
    return Wide(iroha::benchmark_run::FormatLocalDateTime(record.startedAt)) + L"  " + Wide(record.benchmarkName) +
           L"・" + Wide(iroha::benchmark_run::LatticeDisplayName(record.condition.lattice)) + L"・" +
           std::filesystem::path(Wide(record.condition.model)).stem().wstring();
}

// 1 回の計測の結果（精度の表・速度の表・読みの長さ別の中央値）。表の列はタブで区切る
std::wstring RecordText(const iroha::benchmark_run::Record& record) {
    const auto& s = record.summary;
    const auto& e = record.environment;
    std::wstring text = RecordTitle(record) + L"\r\n";
    text += Wide(record.condition.SummaryText()) + L"\r\n";
    text += Wide(e.cpu) + (e.machineModel ? L"（" + Wide(*e.machineModel) + L"）" : L"") + L"・" + Wide(e.osVersion) +
            L"・iroha " + Wide(e.appVersion) + L"\r\n";
    if (record.inputDuringRun && *record.inputDuringRun) {
        text += L"※ 計測中にキーボードかマウスの入力がありました。速度の数字は入力の変換と取り合った値です。\r\n";
    }
    if (const auto note = e.ThrottlingNote()) text += L"※ " + Wide(*note) + L"。速度の数字はこの影響を受けています。\r\n";
    text += L"\r\n区分\t件数\tacc@1\tMinCER\r\n";
    const auto row = [&](const std::wstring& label, const iroha::benchmark::Tally& tally) {
        if (tally.count == 0) return;
        text += label + L"\t" + std::to_wstring(tally.count) + L"\t" + Percent(tally.Accuracy(), 1) + L"\t" +
                Percent(tally.MeanMinCER(), 2) + L"\r\n";
    };
    row(L"全体", s.total);
    row(L"文脈あり", s.withContext);
    row(L"文脈なし", s.withoutContext);
    for (const auto& source : s.perSource) row(Wide(source.source), source.tally);
    const auto& l = s.latency;
    text += L"\r\n平均\t中央値\t90%\t99%\t最大\tNN の割合\r\n";
    text += Format(L"%.1f", l.mean) + L"\t" + Format(L"%.1f", l.p50) + L"\t" + Format(L"%.1f", l.p90) + L"\t" +
            Format(L"%.1f", l.p99) + L"\t" + Format(L"%.1f", l.max) + L"\t" +
            (s.neuralNetworkShare ? Percent(*s.neuralNetworkShare, 0) : L"—") + L"\r\n";
    text += L"\r\n読みの文字数";
    for (const auto& bin : s.lengthBins) text += L"\t" + Wide(bin.Label());
    text += L"\r\n中央値";
    for (const auto& bin : s.lengthBins) text += L"\t" + Format(L"%.1f", bin.p50);
    text += L"\r\n件数";
    for (const auto& bin : s.lengthBins) text += L"\t" + std::to_wstring(bin.count);
    text += L"\r\n\r\n単位はミリ秒（1 件 = 読み 1 つの変換 1 回）。モデルの読み込みを含む最初の変換 " +
            Format(L"%.0f", record.warmUpMilliseconds) + L" ms は除いています。";
    return text;
}

std::wstring MissesText(const iroha::benchmark_run::Record& record) {
    std::wstring text = L"間違えた項目（" + std::to_wstring(record.misses.size()) + L" 件）\r\n";
    for (const auto& miss : record.misses) {
        std::wstring expected;
        for (size_t i = 0; i < miss.expected.size(); ++i) {
            if (i > 0) expected += L" / ";
            expected += Wide(miss.expected[i]);
        }
        text += L"\r\n" + Wide(miss.output) + L"\r\n  正解: " + expected + L"\r\n  読み: " + Wide(miss.input) +
                (miss.context.empty() ? L"" : L"　文脈: " + Wide(miss.context)) + L"\r\n  " + Wide(miss.index) + L"\r\n";
    }
    return text;
}

bool CopyToClipboard(HWND window, const std::wstring& text) {
    if (!OpenClipboard(window)) return false;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (memory) {
        if (void* target = GlobalLock(memory)) {
            memcpy(target, text.c_str(), bytes);
            GlobalUnlock(memory);
            ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
        }
        if (!ok) GlobalFree(memory);
    }
    CloseClipboard();
    return ok;
}

// Markdown の改行を CRLF にする（貼り付け先のメモ帳などで行が分かれるように）
std::wstring WithCrLf(const std::wstring& text) {
    std::wstring out;
    for (wchar_t c : text) {
        if (c == L'\n') out += L'\r';
        out += c;
    }
    return out;
}

} // namespace

struct Section::TaskResult {
    std::unique_ptr<iroha::BenchmarkCatalog> catalog;
    std::wstring message;
    bool failed = false;
};

Section::~Section() { Shutdown(); }

void Section::Build(HWND window, ui::Page* page, std::function<bool()> visible, std::filesystem::path dataDirectory,
                    std::string appVersion) {
    window_ = window;
    page_ = page;
    visible_ = std::move(visible);
    dataDirectory_ = std::move(dataDirectory);
    appVersion_ = std::move(appVersion);
    HWND parent = page->hwnd();

    Row* header = page->AddHeader(
        L"変換のベンチマーク",
        L"評価セットを 1 件ずつ、今使っているモデル・アダプタで変換し、正解との一致（acc@1 = 正解のどれかと完全に一致した割合、"
        L"MinCER = 正解との文字の違いの割合）と、1 回の変換にかかった時間を測ります。"
        L"ユーザ辞書・学習・専門辞書は使いません（ほかの機械の結果と比べられるようにするため）。"
        L"条件の「モデルだけ」はライブ変換の第一候補と同じ経路、「辞書ラティス + 再採点」は辞書の候補をモデルで採点し直して選ぶ"
        L"経路です。計測は別のプロセス（iroha-bench.exe）で行うので計測中も入力できますが、そのあいだの速度の数字は入力の変換と"
        L"取り合った値になります（結果に印が付きます）。ノート PC は電源につないで測ってください。"
        L"結果はデータフォルダの logs\\bench\\ に残ります。.tsv は testdata\\speed\\summarize.py でそのまま集計できます。"
        L"評価セットは「取得」を押したときにダウンロードします（iroha-bench-v1 は CC BY-SA 4.0）。",
        /*developer=*/true);
    header->visible = visible_;

    setList_ = MakeList(parent, kIdSetList, {{L"評価セット", 230}, {L"件数", 70}, {L"大きさ", 80}, {L"状態", 170}});
    setFetch_ = ui::CreateControl(parent, WC_BUTTONW, L"取得", WS_TABSTOP, kIdSetFetch);
    setRemove_ = ui::CreateControl(parent, WC_BUTTONW, L"削除", WS_TABSTOP, kIdSetRemove);
    setRow_ = page->AddItem(L"評価セット", L"", L"", /*developer=*/true);
    setRow_->below = {{{setList_, -1, 104}}, {Gated(setFetch_, 110, &canFetch_), Gated(setRemove_, 90, &canRemove_)}};
    setRow_->visible = visible_;

    setCombo_ = ui::CreateControl(parent, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, kIdSetCombo);
    Row* setChoice = page->AddItem(L"測る評価セット", L"", L"", /*developer=*/true);
    setChoice->right = {Gated(setCombo_, 260, &canChooseSet_, 32, 160)};
    setChoice->visible = visible_;

    latticeCombo_ = ui::CreateControl(parent, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, kIdLatticeCombo);
    for (Lattice lattice : {Lattice::Off, Lattice::Always}) {
        SendMessageW(latticeCombo_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(Wide(iroha::benchmark_run::LatticeDisplayName(lattice)).c_str()));
    }
    SendMessageW(latticeCombo_, CB_SETCURSEL, 0, 0);
    Row* latticeRow = page->AddItem(L"条件", L"",
                                    L"「モデルだけ」はライブ変換の第一候補と同じ経路（iroha-cli の IROHA_LATTICE=off）、"
                                    L"「辞書ラティス + 再採点」は辞書ラティスの候補をモデルで採点し直して第一候補を選ぶ経路"
                                    L"（IROHA_LATTICE=always）です。",
                                    /*developer=*/true);
    latticeRow->right = {Gated(latticeCombo_, 260, &canChooseLattice_, 32, 120)};
    latticeRow->visible = visible_;

    targetRow_ = page->AddItem(L"測るモデル", L"", L"", /*developer=*/true);
    targetRow_->visible = visible_;

    start_ = ui::CreateControl(parent, WC_BUTTONW, L"計測を開始", WS_TABSTOP, kIdStart);
    cancel_ = ui::CreateControl(parent, WC_BUTTONW, L"中止", WS_TABSTOP, kIdCancel);
    runRow_ = page->AddItem(L"計測", L"", L"", /*developer=*/true);
    runRow_->right = {Gated(start_, 130, &canStart_), Gated(cancel_, 80, &canCancel_)};
    runRow_->visible = visible_;

    // 結果は写せるよう、読み取り専用の入力欄に出す（表の列はタブで区切る）
    result_ = ui::CreateControl(parent, WC_EDITW, L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, kIdResult);
    const int tabs[] = {64, 108, 152, 196, 240, 284, 328};
    SendMessageW(result_, EM_SETTABSTOPS, ARRAYSIZE(tabs), reinterpret_cast<LPARAM>(tabs));
    copy_ = ui::CreateControl(parent, WC_BUTTONW, L"Markdown でコピー", WS_TABSTOP, kIdCopy);
    folder_ = ui::CreateControl(parent, WC_BUTTONW, L"記録のフォルダを開く", WS_TABSTOP, kIdFolder);
    misses_ = ui::CreateControl(parent, WC_BUTTONW, L"間違えた項目", WS_TABSTOP, kIdMisses);
    resultRow_ = page->AddItem(L"結果", L"", L"", /*developer=*/true);
    resultRow_->below = {{{result_, -1, 330}}, {{copy_, 170}, {folder_, 180}, Gated(misses_, 190, &canMisses_)}};
    resultRow_->visible = [this] { return visible_() && !records_.empty(); };

    history_ = MakeList(parent, kIdHistory,
                        {{L"日時", 120}, {L"評価セット", 150}, {L"条件", 150}, {L"モデル", 160}, {L"acc@1", 64},
                         {L"中央値", 70}, {L"機械", 200}});
    deleteRecord_ = ui::CreateControl(parent, WC_BUTTONW, L"選んだ結果を削除", WS_TABSTOP, kIdDeleteRecord);
    historyRow_ = page->AddItem(L"過去の結果", L"選ぶと上の結果に出します", L"", /*developer=*/true);
    historyRow_->below = {{{history_, -1, 150}}, {Gated(deleteRecord_, 170, &canDeleteRecord_)}};
    historyRow_->visible = [this] { return visible_() && records_.size() > 1; };

    UpdateControls();
}

void Section::SetTarget(const std::filesystem::path& modelPath, const std::filesystem::path& adapterPath) {
    modelPath_ = modelPath;
    adapterPath_ = adapterPath;
    if (page_) UpdateControls();
}

void Section::Show() {
    if (!page_ || !visible_()) return;
    RefreshInstalled();
    RefreshRecords();
    if (!catalogRequested_) {
        catalogRequested_ = true;
        StartTask(L"評価セットの一覧を取得しています…", true, {});
    }
}

void Section::Shutdown() {
    if (process_) {
        TerminateProcess(process_, 15);
        CloseHandle(process_);
        process_ = nullptr;
    }
}

std::vector<iroha::BenchmarkInstall::Record> Section::InstalledRecords() const {
    return iroha::BenchmarkInstall::InstalledRecords(dataDirectory_);
}

std::optional<std::filesystem::path> Section::HelperPath() const {
    const std::filesystem::path directory = ExecutableDirectory();
    // インストールした版は同じフォルダ、開発中のビルドは windows\build\iroha-bench\ にある
    for (const std::filesystem::path& candidate :
         {directory / L"iroha-bench.exe", directory.parent_path() / L"iroha-bench" / L"iroha-bench.exe"}) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error)) return candidate;
    }
    return std::nullopt;
}

std::filesystem::path Section::LogPath() const {
    return dataDirectory_ / L"logs" / (L"bench-" + Wide(HostName()) + L".log");
}

// ---- 評価セット ----

void Section::RefreshInstalled() {
    installed_ = iroha::BenchmarkInstall::InstalledRecords(dataDirectory_);
    setRows_.clear();
    if (catalog_) {
        for (const auto& entry : catalog_->benchmarks) setRows_.push_back(entry.id);
    }
    // カタログから消えた・オフラインでも、入れてあるものは消せるようにする
    for (const auto& record : installed_) {
        if (std::find(setRows_.begin(), setRows_.end(), record.id) == setRows_.end()) setRows_.push_back(record.id);
    }
    const int selected = SelectedRow(setList_);
    ListView_DeleteAllItems(setList_);
    for (int i = 0; i < static_cast<int>(setRows_.size()); ++i) {
        const std::string& id = setRows_[i];
        const auto* entry = catalog_ ? catalog_->Find(id) : nullptr;
        const auto record = std::find_if(installed_.begin(), installed_.end(), [&](const auto& r) { return r.id == id; });
        const bool isInstalled = record != installed_.end();
        std::wstring state = !isInstalled ? L"" : !entry ? L"入れてある（一覧に無い）"
                                       : iroha::BenchmarkInstall::IsOutdated(*record, *entry) ? L"入れてある（更新あり）"
                                                                                              : L"入れてある";
        SetListRow(setList_, i,
                   {Wide(entry ? entry->name : record->name), std::to_wstring(entry ? entry->items : record->items),
                    entry ? ByteCount(entry->file.bytes) : L"", state});
    }
    if (selected >= 0 && selected < static_cast<int>(setRows_.size())) {
        ListView_SetItemState(setList_, selected, LVIS_SELECTED, LVIS_SELECTED);
    } else if (!setRows_.empty()) {
        ListView_SetItemState(setList_, 0, LVIS_SELECTED, LVIS_SELECTED);
    }
    // 測る評価セットの選択肢（入れてあるもの）。選んだものが無ければ先頭
    SendMessageW(setCombo_, CB_RESETCONTENT, 0, 0);
    int chosen = 0;
    for (int i = 0; i < static_cast<int>(installed_.size()); ++i) {
        SendMessageW(setCombo_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>((Wide(installed_[i].name) + L"（" + std::to_wstring(installed_[i].items) +
                                               L" 件）")
                                                  .c_str()));
        if (installed_[i].id == selectedSet_) chosen = i;
    }
    if (!installed_.empty()) {
        SendMessageW(setCombo_, CB_SETCURSEL, chosen, 0);
        selectedSet_ = installed_[chosen].id;
    }
    UpdateControls();
}

void Section::RefreshRecords() {
    records_ = iroha::benchmark_run::Records(dataDirectory_);
    if (!shownRecordId_.empty() &&
        std::none_of(records_.begin(), records_.end(), [&](const auto& r) { return r.id == shownRecordId_; })) {
        shownRecordId_.clear();
    }
    ListView_DeleteAllItems(history_);
    for (int i = 0; i < static_cast<int>(records_.size()); ++i) {
        const auto& record = records_[i];
        SetListRow(history_, i,
                   {Wide(iroha::benchmark_run::FormatLocalDateTime(record.startedAt)), Wide(record.benchmarkName),
                    Wide(iroha::benchmark_run::LatticeDisplayName(record.condition.lattice)),
                    std::filesystem::path(Wide(record.condition.model)).stem().wstring(),
                    Percent(record.summary.total.Accuracy(), 1), Format(L"%.1f ms", record.summary.latency.p50),
                    Wide(record.environment.cpu) + L"（" + Wide(record.environment.hostName) + L"）"});
    }
    showingMisses_ = false;
    ShowRecord();
    page_->Relayout();
}

void Section::ShowRecord() {
    const iroha::benchmark_run::Record* record = nullptr;
    for (const auto& candidate : records_) {
        if (shownRecordId_.empty() || candidate.id == shownRecordId_) {
            record = &candidate;
            break;
        }
    }
    if (!record) {
        SetWindowTextW(result_, L"");
        canMisses_ = false;
        EnableWindow(misses_, FALSE);
        return;
    }
    SetWindowTextW(result_, (showingMisses_ ? MissesText(*record) : RecordText(*record)).c_str());
    SetWindowTextW(misses_, showingMisses_ ? L"結果に戻る"
                                           : (L"間違えた項目（" + std::to_wstring(record->misses.size()) + L" 件）").c_str());
    canMisses_ = !record->misses.empty() || showingMisses_;
    EnableWindow(misses_, canMisses_);
    page_->SetDescription(resultRow_, record == &records_.front() ? L"直近の結果" : L"過去の結果");
}

void Section::UpdateControls() {
    if (!page_) return;
    const bool running = process_ != nullptr;
    // 評価セットの表
    const int row = SelectedRow(setList_);
    const bool valid = row >= 0 && row < static_cast<int>(setRows_.size());
    const auto* entry = valid && catalog_ ? catalog_->Find(setRows_[row]) : nullptr;
    const auto record = valid ? std::find_if(installed_.begin(), installed_.end(),
                                             [&](const auto& r) { return r.id == setRows_[row]; })
                              : installed_.end();
    const bool isInstalled = record != installed_.end();
    const bool outdated = isInstalled && entry && iroha::BenchmarkInstall::IsOutdated(*record, *entry);
    SetWindowTextW(setFetch_, outdated ? L"更新" : L"取得");
    canFetch_ = entry && !taskBusy_ && (!isInstalled || outdated);
    canRemove_ = isInstalled && !taskBusy_ && !running;
    std::wstring setDescription = catalogMessage_;
    if (setDescription.empty()) {
        setDescription = entry && !entry->summary.empty() ? Wide(entry->summary)
                         : entry                          ? L"ライセンス: " + Wide(entry->license)
                                                          : L"";
    }
    page_->SetDescription(setRow_, setDescription, catalogFailed_);
    canChooseSet_ = !running && !installed_.empty();
    canChooseLattice_ = !running;
    // 測るモデル
    std::wstring target = modelPath_.empty() ? L"（未設定）" : modelPath_.stem().wstring() + L"（llama.cpp）";
    std::error_code error;
    const bool modelMissing = !modelPath_.empty() && !std::filesystem::exists(modelPath_, error);
    if (modelMissing) target += L"  モデルのファイルがありません";
    if (!adapterPath_.empty()) target += L"＋アダプタ " + adapterPath_.filename().wstring();
    page_->SetDescription(targetRow_, target, modelMissing);
    // 計測
    const bool helper = HelperPath().has_value();
    canStart_ = !running && helper && !installed_.empty() && !modelPath_.empty() && !modelMissing;
    canCancel_ = running && state_ != State::Cancelled;
    canDeleteRecord_ = SelectedRow(history_) >= 0;
    for (const auto& [control, enabled] : {std::pair<HWND, bool>{setFetch_, canFetch_}, {setRemove_, canRemove_},
                                           {setCombo_, canChooseSet_}, {latticeCombo_, canChooseLattice_},
                                           {start_, canStart_}, {cancel_, canCancel_}, {deleteRecord_, canDeleteRecord_}}) {
        EnableWindow(control, enabled);
    }
    std::wstring status;
    bool warning = false;
    switch (state_) {
    case State::Idle:
        status = !helper              ? L"計測ヘルパー（iroha-bench.exe）が見つかりません"
                 : installed_.empty() ? L"先に評価セットを取得してください"
                                      : L"";
        warning = !helper;
        break;
    case State::Loading:
        status = L"モデルを読み込み中…";
        break;
    case State::Running:
        status = std::to_wstring(done_) + L"/" + std::to_wstring(total_) + L" 件（正解 " + std::to_wstring(hits_) +
                 L" 件）。計測中に入力すると、速度の数字が乱れます。";
        break;
    case State::Failed:
        status = L"計測に失敗しました: " + failure_;
        warning = true;
        break;
    case State::Cancelled:
        status = L"計測を中止しました。";
        break;
    }
    page_->SetDescription(runRow_, status, warning);
}

void Section::StartTask(std::wstring busyMessage, bool fetchCatalog, std::string installId) {
    if (taskBusy_) return;
    taskBusy_ = true;
    catalogMessage_ = std::move(busyMessage);
    catalogFailed_ = false;
    UpdateControls();
    struct Task {
        HWND window;
        std::filesystem::path data;
        bool fetchCatalog;
        std::string installId;
        std::unique_ptr<iroha::BenchmarkCatalog> catalog;
    };
    auto* task = new Task{window_, dataDirectory_, fetchCatalog, std::move(installId),
                          catalog_ ? std::make_unique<iroha::BenchmarkCatalog>(*catalog_) : nullptr};
    const auto run = [](LPVOID parameter) -> DWORD {
        std::unique_ptr<Task> task(static_cast<Task*>(parameter));
        auto* result = new TaskResult();
        std::string error;
        if (task->fetchCatalog) {
            auto catalog = std::make_unique<iroha::BenchmarkCatalog>();
            if (iroha::BenchmarkFetcher::FetchCatalog(iroha::BenchmarkCatalog::DefaultUrl(), catalog.get(), &error)) {
                result->catalog = std::move(catalog);
            } else {
                result->failed = true;
                result->message = L"評価セットの一覧を取得できませんでした: " + Wide(error);
            }
        }
        if (!task->installId.empty() && task->catalog) {
            if (const auto* entry = task->catalog->Find(task->installId)) {
                if (iroha::BenchmarkFetcher::Install(*entry, task->data, nullptr, &error)) {
                    result->message = Wide(entry->name) + L" を取得しました";
                } else {
                    result->failed = true;
                    result->message = L"取得できませんでした: " + Wide(entry->name) + L": " + Wide(error);
                }
            }
        }
        PostMessageW(task->window, WM_APP_BENCH_TASK, 0, reinterpret_cast<LPARAM>(result));
        return 0;
    };
    HANDLE thread = CreateThread(nullptr, 0, run, task, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        delete task;
        taskBusy_ = false;
        catalogMessage_ = L"作業を始められませんでした";
        catalogFailed_ = true;
        UpdateControls();
    }
}

void Section::OnTaskDone(TaskResult* result) {
    taskBusy_ = false;
    if (result->catalog) catalog_ = std::move(result->catalog);
    catalogMessage_ = result->message;
    catalogFailed_ = result->failed;
    delete result;
    RefreshInstalled();
}

// ---- 計測 ----

void Section::Start() {
    const auto helper = HelperPath();
    if (!helper || process_ || installed_.empty()) return;
    const auto record =
        std::find_if(installed_.begin(), installed_.end(), [&](const auto& r) { return r.id == selectedSet_; });
    if (record == installed_.end()) return;

    std::wstring command = Quote(helper->wstring()) + L" run";
    const auto add = [&](const wchar_t* key, const std::wstring& value) {
        command += L" ";
        command += key;
        command += L" " + Quote(value);
    };
    add(L"--set", iroha::BenchmarkInstall::FilePath(dataDirectory_, record->id).wstring());
    add(L"--id", Wide(record->id));
    add(L"--name", Wide(record->name));
    add(L"--model", modelPath_.wstring());
    add(L"--backend", L"llama");
    add(L"--lattice", Wide(iroha::benchmark_run::LatticeName(lattice_)));
    add(L"--app-version", Wide(appVersion_));
    if (!adapterPath_.empty()) add(L"--adapter", adapterPath_.wstring());

    SECURITY_ATTRIBUTES inherit = {sizeof(inherit), nullptr, TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &inherit, 0)) {
        state_ = State::Failed;
        failure_ = L"iroha-bench を起動できません（パイプを作れません）";
        UpdateControls();
        return;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    // iroha-bench の標準エラー（llama.cpp のログ）を残す
    std::error_code ignored;
    std::filesystem::create_directories(LogPath().parent_path(), ignored);
    HANDLE log = CreateFileW(LogPath().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0,
                             nullptr);
    STARTUPINFOW startup = {sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nul;
    startup.hStdOutput = writePipe;
    startup.hStdError = log != INVALID_HANDLE_VALUE ? log : nul;
    PROCESS_INFORMATION info = {};
    // 計測中も入力の変換を優先させる（数字は少し遅くなるが、入力が止まらない。macOS版と同じ考え方）
    const BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &startup, &info);
    const DWORD createError = GetLastError();
    CloseHandle(writePipe);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!created) {
        CloseHandle(readPipe);
        state_ = State::Failed;
        failure_ = L"iroha-bench を起動できません（" + std::to_wstring(createError) + L"）";
        UpdateControls();
        return;
    }
    CloseHandle(info.hThread);
    process_ = info.hProcess;
    ++generation_;
    state_ = State::Loading;
    done_ = 0;
    total_ = record->items;
    hits_ = 0;
    failure_.clear();

    // 標準出力を 1 行ずつ窓へ送り、終わったら終了コードを送る
    struct Reader {
        HWND window;
        HANDLE pipe;
        HANDLE process;
        WPARAM generation;
    };
    HANDLE processCopy = nullptr;
    DuplicateHandle(GetCurrentProcess(), process_, GetCurrentProcess(), &processCopy, 0, FALSE, DUPLICATE_SAME_ACCESS);
    auto* reader = new Reader{window_, readPipe, processCopy, generation_};
    const auto read = [](LPVOID parameter) -> DWORD {
        std::unique_ptr<Reader> reader(static_cast<Reader*>(parameter));
        std::string buffer;
        char chunk[4096];
        DWORD got = 0;
        while (ReadFile(reader->pipe, chunk, sizeof(chunk), &got, nullptr) && got > 0) {
            buffer.append(chunk, got);
            size_t newline;
            while ((newline = buffer.find('\n')) != std::string::npos) {
                auto* line = new std::string(buffer.substr(0, newline));
                buffer.erase(0, newline + 1);
                if (!PostMessageW(reader->window, WM_APP_BENCH_LINE, reader->generation, reinterpret_cast<LPARAM>(line))) {
                    delete line;
                }
            }
        }
        CloseHandle(reader->pipe);
        DWORD exitCode = 1;
        if (reader->process) {
            WaitForSingleObject(reader->process, INFINITE);
            GetExitCodeProcess(reader->process, &exitCode);
            CloseHandle(reader->process);
        }
        PostMessageW(reader->window, WM_APP_BENCH_EXIT, reader->generation, static_cast<LPARAM>(exitCode));
        return 0;
    };
    HANDLE thread = CreateThread(nullptr, 0, read, reader, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        CloseHandle(readPipe);
        if (processCopy) CloseHandle(processCopy);
        delete reader;
    }
    UpdateControls();
}

void Section::Cancel() {
    if (!process_) return;
    state_ = State::Cancelled;
    TerminateProcess(process_, 15);
    UpdateControls();
}

void Section::OnLine(const std::string& line) {
    if (state_ == State::Cancelled) return;
    const auto event = iroha::benchmark_run::Event::Parse(line);
    if (!event) return;
    using Kind = iroha::benchmark_run::Event::Kind;
    switch (event->kind) {
    case Kind::Loading:
        state_ = State::Loading;
        total_ = event->total.value_or(total_);
        break;
    case Kind::Progress:
        state_ = State::Running;
        done_ = event->done.value_or(0);
        total_ = event->total.value_or(0);
        hits_ = event->hits.value_or(0);
        break;
    case Kind::Finished:
        // 新しい記録が一覧の先頭に来るので、それを開く
        state_ = State::Idle;
        shownRecordId_.clear();
        RefreshRecords();
        break;
    case Kind::Failed:
        state_ = State::Failed;
        failure_ = Wide(event->message.value_or("計測に失敗しました"));
        break;
    }
    UpdateControls();
}

void Section::OnExit(DWORD exitCode) {
    if (process_) {
        CloseHandle(process_);
        process_ = nullptr;
    }
    if (state_ == State::Loading || state_ == State::Running) {
        if (exitCode == 15) {
            state_ = State::Cancelled;
        } else {
            state_ = State::Failed;
            failure_ = L"計測が途中で終了しました（終了コード " + std::to_wstring(exitCode) + L"）。詳細は logs\\" +
                       LogPath().filename().wstring() + L" を参照";
        }
    }
    UpdateControls();
}

// ---- 窓の知らせ ----

bool Section::OnCommand(int id, int code) {
    if (id < kIdFirst || id > kIdLast) return false;
    const bool clicked = code == BN_CLICKED;
    switch (id) {
    case kIdSetFetch: {
        const int row = SelectedRow(setList_);
        if (clicked && row >= 0 && row < static_cast<int>(setRows_.size()) && catalog_) {
            if (const auto* entry = catalog_->Find(setRows_[row])) {
                StartTask(Wide(entry->name) + L" を取得しています…", false, entry->id);
            }
        }
        return true;
    }
    case kIdSetRemove: {
        const int row = SelectedRow(setList_);
        if (clicked && row >= 0 && row < static_cast<int>(setRows_.size()) && !process_) {
            iroha::BenchmarkInstall::Remove(dataDirectory_, setRows_[row]);
            catalogMessage_.clear();
            RefreshInstalled();
        }
        return true;
    }
    case kIdSetCombo:
        if (code == CBN_SELCHANGE) {
            const int index = static_cast<int>(SendMessageW(setCombo_, CB_GETCURSEL, 0, 0));
            if (index >= 0 && index < static_cast<int>(installed_.size())) selectedSet_ = installed_[index].id;
        }
        return true;
    case kIdLatticeCombo:
        if (code == CBN_SELCHANGE) {
            lattice_ = SendMessageW(latticeCombo_, CB_GETCURSEL, 0, 0) == 1 ? Lattice::Always : Lattice::Off;
        }
        return true;
    case kIdStart:
        if (clicked) {
            if (state_ == State::Failed || state_ == State::Cancelled) state_ = State::Idle;
            Start();
        }
        return true;
    case kIdCancel:
        if (clicked) Cancel();
        return true;
    case kIdCopy:
        if (clicked) {
            for (const auto& record : records_) {
                if (shownRecordId_.empty() || record.id == shownRecordId_) {
                    CopyToClipboard(window_, WithCrLf(Wide(iroha::benchmark_run::Markdown(record))));
                    break;
                }
            }
        }
        return true;
    case kIdFolder:
        if (clicked) {
            for (const auto& record : records_) {
                if (shownRecordId_.empty() || record.id == shownRecordId_) {
                    const std::wstring parameters =
                        L"/select," + Quote(iroha::benchmark_run::RecordPath(dataDirectory_, record.id).wstring());
                    ShellExecuteW(window_, L"open", L"explorer.exe", parameters.c_str(), nullptr, SW_SHOWNORMAL);
                    break;
                }
            }
        }
        return true;
    case kIdMisses:
        if (clicked) {
            showingMisses_ = !showingMisses_;
            ShowRecord();
        }
        return true;
    case kIdDeleteRecord: {
        const int row = SelectedRow(history_);
        if (clicked && row >= 0 && row < static_cast<int>(records_.size())) {
            iroha::benchmark_run::Delete(dataDirectory_, records_[row].id);
            RefreshRecords();
            UpdateControls();
        }
        return true;
    }
    default:
        return true;
    }
}

bool Section::OnNotify(const NMHDR* header) {
    if (header->hwndFrom == setList_ && header->code == LVN_ITEMCHANGED) {
        if (!taskBusy_ && !catalogFailed_) catalogMessage_.clear();
        UpdateControls();
        return true;
    }
    if (header->hwndFrom == history_ && header->code == LVN_ITEMCHANGED) {
        const auto* change = reinterpret_cast<const NMLISTVIEW*>(header);
        if ((change->uChanged & LVIF_STATE) && (change->uNewState & LVIS_SELECTED) && change->iItem >= 0 &&
            change->iItem < static_cast<int>(records_.size())) {
            shownRecordId_ = records_[change->iItem].id;
            showingMisses_ = false;
            ShowRecord();
        }
        UpdateControls();
        return true;
    }
    return false;
}

bool Section::OnMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_APP_BENCH_TASK:
        OnTaskDone(reinterpret_cast<TaskResult*>(lParam));
        return true;
    case WM_APP_BENCH_LINE: {
        std::unique_ptr<std::string> line(reinterpret_cast<std::string*>(lParam));
        if (wParam == generation_) OnLine(*line);
        return true;
    }
    case WM_APP_BENCH_EXIT:
        if (wParam == generation_) OnExit(static_cast<DWORD>(lParam));
        return true;
    default:
        return false;
    }
}

} // namespace benchmark_section
