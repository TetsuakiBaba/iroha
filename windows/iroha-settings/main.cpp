// iroha 設定ウィンドウ。
// タブ: モデル（選択・ダウンロード） / ユーザ辞書（一覧・追加・削除） / 専門辞書（配布している辞書の追加・削除） / 学習。
// 保存先は %LOCALAPPDATA%\iroha\（config.json / user-dictionary.json / dictionaries\ / learning.json）。
// 保存後は変換サーバへReloadを送って即時反映する（サーバ未起動でも次回起動時に反映）。

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <commctrl.h>
#include <urlmon.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "ipc_protocol.h"
#include "iroha/config.h"
#include "iroha/learning_store.h"
#include "iroha/term_dictionary.h"
#include "iroha/unicode.h"
#include "iroha/user_dictionary_store.h"
#include "term_dictionary_fetcher.h"

#pragma comment(linker, \
    "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
    "version='6.0.0.0' processorArchitecture='*' " \
    "publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr UINT WM_APP_DOWNLOAD_DONE = WM_APP + 1;
// 専門辞書の一覧の取得・追加・削除が終わった（lParam は TermTaskResult*。受け取った側が消す）
constexpr UINT WM_APP_TERM_DONE = WM_APP + 2;

struct KnownModel {
    const wchar_t* label;
    const wchar_t* fileName;
    const wchar_t* url;
};
const KnownModel kKnownModels[] = {
    {L"iroha-kkc-v1（既定・文字単位T5 / 約82MB）",
     L"iroha-kkc-v1-Q5_K_M.gguf",
     L"https://github.com/TetsuakiBaba/iroha/releases/download/iroha-kkc-v1/iroha-kkc-v1-Q5_K_M.gguf"},
    {L"zenz-v3.1-small（以前の既定 95M / 約70MB）",
     L"zenz-v3.1-small-Q5_K_M.gguf",
     L"https://huggingface.co/Miwa-Keita/zenz-v3.1-small-gguf/resolve/main/ggml-model-Q5_K_M.gguf"},
    {L"zenz-v3.1-xsmall（高速・軽量 26M / 約21MB）",
     L"zenz-v3.1-xsmall-Q5_K_M.gguf",
     L"https://huggingface.co/Miwa-Keita/zenz-v3.1-xsmall-gguf/resolve/main/ggml-model-Q5_K_M.gguf"},
};

// コントロールID
enum {
    kIdTab = 100,
    kIdModelCombo,
    kIdModelStatus,
    kIdModelDownload,
    kIdModelApply,
    kIdDictList,
    kIdDictReading,
    kIdDictWord,
    kIdDictAdd,
    kIdDictRemove,
    kIdLearnStatus,
    kIdLearnReset,
    kIdAbout,
    kIdTermList,
    kIdTermAdd,
    kIdTermRemove,
    kIdTermStatus,
    kIdTermDetail,
};

// タブの並び
enum TabIndex { kTabModel = 0, kTabUserDictionary, kTabTermDictionary, kTabLearning };

std::wstring DataDir() {
    wchar_t buffer[MAX_PATH];
    const DWORD length =
        GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, ARRAYSIZE(buffer));
    const std::wstring base =
        (length > 0 && length < ARRAYSIZE(buffer)) ? buffer : L".";
    return base + L"\\iroha";
}

std::filesystem::path ConfigPath() { return DataDir() + L"\\config.json"; }
std::filesystem::path ModelPath(const wchar_t* fileName) {
    return DataDir() + L"\\models\\" + fileName;
}

// サーバに設定・辞書の再読み込みを頼む（モデル切替はロード込みで数秒かかりうる）
bool RequestServerReload() {
    std::vector<char> response;
    return iroha::ipc::Call(iroha::ipc::BuildReloadRequest(), &response, 20000);
}

// 専門辞書の一覧の 1 行（カタログの辞書か、カタログに無いが入れてある辞書）
struct TermRow {
    std::string id;
    std::wstring name;
    std::wstring group;
    std::wstring summary;
    uint64_t entries = 0;
    std::wstring license;
    std::vector<iroha::TermDictionaryCatalog::Source> sources;
    std::vector<iroha::TermDictionaryCatalog::Sample> samples;
    const iroha::TermDictionaryCatalog::Dictionary* catalogEntry = nullptr;
    bool installed = false;
    bool needsUpdate = false;
};

// 裏のスレッドでした専門辞書の作業の結果
struct TermTaskResult {
    bool catalogFetched = false;
    std::unique_ptr<iroha::TermDictionaryCatalog> catalog;
    std::wstring message; // 画面の下に出す（空なら出さない）
    bool failed = false;
};

struct App {
    HWND window = nullptr;
    HWND tab = nullptr;
    HFONT font = nullptr;
    // モデル
    HWND modelCombo = nullptr;
    HWND modelStatus = nullptr;
    HWND modelDownload = nullptr;
    HWND modelApply = nullptr;
    bool downloading = false;
    // 辞書
    HWND dictList = nullptr;
    HWND dictReading = nullptr;
    HWND dictWord = nullptr;
    HWND dictAdd = nullptr;
    HWND dictRemove = nullptr;
    HWND dictLabelReading = nullptr;
    HWND dictLabelWord = nullptr;
    // 学習
    HWND learnStatus = nullptr;
    HWND learnReset = nullptr;
    HWND about = nullptr;
    // 専門辞書
    HWND termList = nullptr;
    HWND termAdd = nullptr;
    HWND termRemove = nullptr;
    HWND termStatus = nullptr;
    HWND termDetail = nullptr;
    std::unique_ptr<iroha::TermDictionaryCatalog> termCatalog;
    std::vector<TermRow> termRows;
    bool termBusy = false;
    bool termCatalogRequested = false;

    std::unique_ptr<iroha::UserDictionaryStore> dictionary;
    std::unique_ptr<iroha::LearningStore> learning;

    int Scale(int value) const {
        return MulDiv(value, static_cast<int>(GetDpiForWindow(window)), 96);
    }

    void CreateControls();
    void LayoutControls();
    void ShowTab(int index);
    void RefreshModelPage();
    void RefreshDictionaryList();
    void RefreshLearningPage();
    void OnModelDownload();
    void OnModelApply();
    void OnDictAdd();
    void OnDictRemove();
    void OnLearnReset();
    void RefreshTermList();
    void ShowTermDetail();
    void StartTermTask(std::wstring busyMessage, bool fetchCatalog, std::string installId,
                       std::string removeId);
    void OnTermTaskDone(TermTaskResult* result);
};

App g_app;

HWND CreateChild(const wchar_t* className, const wchar_t* text, DWORD style, int id,
                 DWORD exStyle = 0) {
    HWND child = CreateWindowExW(exStyle, className, text,
                                 WS_CHILD | style, 0, 0, 10, 10, g_app.window,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 GetModuleHandleW(nullptr), nullptr);
    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.font), TRUE);
    return child;
}

void App::CreateControls() {
    const UINT dpi = GetDpiForWindow(window);
    font = CreateFontW(-MulDiv(12, static_cast<int>(dpi), 96), 0, 0, 0, FW_NORMAL,
                       FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                       L"Meiryo UI");

    tab = CreateChild(WC_TABCONTROLW, L"", WS_VISIBLE | WS_CLIPSIBLINGS, kIdTab);
    TCITEMW item = {};
    item.mask = TCIF_TEXT;
    item.pszText = const_cast<wchar_t*>(L"モデル");
    TabCtrl_InsertItem(tab, 0, &item);
    item.pszText = const_cast<wchar_t*>(L"ユーザ辞書");
    TabCtrl_InsertItem(tab, kTabUserDictionary, &item);
    item.pszText = const_cast<wchar_t*>(L"専門辞書");
    TabCtrl_InsertItem(tab, kTabTermDictionary, &item);
    item.pszText = const_cast<wchar_t*>(L"学習");
    TabCtrl_InsertItem(tab, kTabLearning, &item);

    // ---- モデル ----
    modelCombo = CreateChild(WC_COMBOBOXW, L"",
                             CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, kIdModelCombo);
    for (const KnownModel& model : kKnownModels) {
        SendMessageW(modelCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(model.label));
    }
    modelStatus = CreateChild(WC_STATICW, L"", 0, kIdModelStatus);
    modelDownload = CreateChild(WC_BUTTONW, L"ダウンロード", WS_TABSTOP, kIdModelDownload);
    modelApply = CreateChild(WC_BUTTONW, L"このモデルを使う", WS_TABSTOP, kIdModelApply);

    // ---- 辞書 ----
    dictList = CreateChild(WC_LISTVIEWW, L"",
                           LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP,
                           kIdDictList, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(dictList,
                                      LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    LVCOLUMNW column = {};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.cx = Scale(150);
    column.pszText = const_cast<wchar_t*>(L"読み");
    ListView_InsertColumn(dictList, 0, &column);
    column.cx = Scale(200);
    column.pszText = const_cast<wchar_t*>(L"単語");
    ListView_InsertColumn(dictList, 1, &column);
    column.cx = Scale(80);
    column.pszText = const_cast<wchar_t*>(L"出所");
    ListView_InsertColumn(dictList, 2, &column);

    dictLabelReading = CreateChild(WC_STATICW, L"読み:", 0, 0);
    dictReading = CreateChild(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, kIdDictReading,
                              WS_EX_CLIENTEDGE);
    dictLabelWord = CreateChild(WC_STATICW, L"単語:", 0, 0);
    dictWord = CreateChild(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, kIdDictWord,
                           WS_EX_CLIENTEDGE);
    dictAdd = CreateChild(WC_BUTTONW, L"追加", WS_TABSTOP, kIdDictAdd);
    dictRemove = CreateChild(WC_BUTTONW, L"選択を削除", WS_TABSTOP, kIdDictRemove);

    // ---- 専門辞書 ----
    termList = CreateChild(WC_LISTVIEWW, L"",
                           LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, kIdTermList,
                           WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(termList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    const struct {
        const wchar_t* title;
        int width;
    } termColumns[] = {{L"辞書", 150}, {L"まとまり", 150}, {L"語数", 70}, {L"状態", 110}};
    for (int i = 0; i < static_cast<int>(ARRAYSIZE(termColumns)); ++i) {
        column.cx = Scale(termColumns[i].width);
        column.pszText = const_cast<wchar_t*>(termColumns[i].title);
        ListView_InsertColumn(termList, i, &column);
    }
    termAdd = CreateChild(WC_BUTTONW, L"追加", WS_TABSTOP, kIdTermAdd);
    termRemove = CreateChild(WC_BUTTONW, L"削除", WS_TABSTOP, kIdTermRemove);
    termStatus = CreateChild(WC_STATICW, L"", 0, kIdTermStatus);
    // 出典・ライセンスの URL を写せるよう、読み取り専用の入力欄に出す
    termDetail = CreateChild(WC_EDITW, L"",
                             ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, kIdTermDetail,
                             WS_EX_CLIENTEDGE);

    // ---- 学習 ----
    learnStatus = CreateChild(WC_STATICW, L"", 0, kIdLearnStatus);
    learnReset = CreateChild(WC_BUTTONW, L"学習をリセット", WS_TABSTOP, kIdLearnReset);
    about = CreateChild(WC_STATICW,
                        L"iroha for Windows\nhttps://github.com/TetsuakiBaba/iroha", 0,
                        kIdAbout);
}

void App::LayoutControls() {
    RECT client;
    GetClientRect(window, &client);
    MoveWindow(tab, 0, 0, client.right, client.bottom, TRUE);

    RECT page = client;
    TabCtrl_AdjustRect(tab, FALSE, &page);
    const int x = page.left + Scale(12);
    const int width = page.right - page.left - Scale(24);
    int y = page.top + Scale(12);

    // モデル
    MoveWindow(modelCombo, x, y, width, Scale(200), TRUE);
    MoveWindow(modelStatus, x, y + Scale(36), width, Scale(40), TRUE);
    MoveWindow(modelDownload, x, y + Scale(84), Scale(130), Scale(28), TRUE);
    MoveWindow(modelApply, x + Scale(140), y + Scale(84), Scale(150), Scale(28), TRUE);

    // 辞書
    const int listHeight = page.bottom - y - Scale(88);
    MoveWindow(dictList, x, y, width, listHeight, TRUE);
    const int rowY = y + listHeight + Scale(10);
    MoveWindow(dictLabelReading, x, rowY + Scale(4), Scale(38), Scale(20), TRUE);
    MoveWindow(dictReading, x + Scale(40), rowY, Scale(140), Scale(24), TRUE);
    MoveWindow(dictLabelWord, x + Scale(190), rowY + Scale(4), Scale(38), Scale(20), TRUE);
    MoveWindow(dictWord, x + Scale(230), rowY, Scale(150), Scale(24), TRUE);
    MoveWindow(dictAdd, x + Scale(390), rowY, Scale(60), Scale(26), TRUE);
    MoveWindow(dictRemove, x, rowY + Scale(34), Scale(110), Scale(26), TRUE);

    // 専門辞書
    const int detailHeight = Scale(120);
    const int termListHeight = page.bottom - y - detailHeight - Scale(74);
    MoveWindow(termList, x, y, width, termListHeight, TRUE);
    const int termRowY = y + termListHeight + Scale(8);
    MoveWindow(termAdd, x, termRowY, Scale(80), Scale(26), TRUE);
    MoveWindow(termRemove, x + Scale(88), termRowY, Scale(80), Scale(26), TRUE);
    MoveWindow(termStatus, x + Scale(178), termRowY + Scale(4), width - Scale(178), Scale(20), TRUE);
    MoveWindow(termDetail, x, termRowY + Scale(34), width, detailHeight, TRUE);

    // 学習
    MoveWindow(learnStatus, x, y, width, Scale(40), TRUE);
    MoveWindow(learnReset, x, y + Scale(48), Scale(140), Scale(28), TRUE);
    MoveWindow(about, x, y + Scale(96), width, Scale(48), TRUE);
}

void App::ShowTab(int index) {
    auto show = [](HWND hwnd, bool visible) {
        ShowWindow(hwnd, visible ? SW_SHOW : SW_HIDE);
    };
    const bool model = index == kTabModel;
    const bool dict = index == kTabUserDictionary;
    const bool term = index == kTabTermDictionary;
    const bool learn = index == kTabLearning;
    show(modelCombo, model);
    show(modelStatus, model);
    show(modelDownload, model);
    show(modelApply, model);
    show(dictList, dict);
    show(dictLabelReading, dict);
    show(dictReading, dict);
    show(dictLabelWord, dict);
    show(dictWord, dict);
    show(dictAdd, dict);
    show(dictRemove, dict);
    show(termList, term);
    show(termAdd, term);
    show(termRemove, term);
    show(termStatus, term);
    show(termDetail, term);
    show(learnStatus, learn);
    show(learnReset, learn);
    show(about, learn);
    // 専門辞書の一覧は、タブを初めて開いたときに取りにいく（入れてある辞書が作り直されていれば入れ直す）
    if (term && !termCatalogRequested) {
        termCatalogRequested = true;
        StartTermTask(L"辞書の一覧を取得しています…", true, {}, {});
    }
}

void App::RefreshModelPage() {
    const int index =
        static_cast<int>(SendMessageW(modelCombo, CB_GETCURSEL, 0, 0));
    if (index < 0 || index >= static_cast<int>(ARRAYSIZE(kKnownModels))) return;
    const KnownModel& model = kKnownModels[index];
    const bool downloaded = std::filesystem::exists(ModelPath(model.fileName));

    const iroha::Config config = iroha::LoadConfig(ConfigPath());
    // 未設定なら既定のモデル（変換サーバと同じ決め方）
    const std::wstring current =
        config.model.empty()
            ? iroha::DefaultModelPath(ModelPath(L"").parent_path()).filename().wstring()
            : iroha::Utf32ToUtf16(config.model);
    std::wstring status = downloaded ? L"状態: ダウンロード済み" : L"状態: 未ダウンロード";
    if (current == model.fileName) status += L"（現在使用中）";
    SetWindowTextW(modelStatus, status.c_str());
    EnableWindow(modelDownload, !downloaded && !downloading);
    EnableWindow(modelApply, downloaded && !downloading);
}

void App::RefreshDictionaryList() {
    ListView_DeleteAllItems(dictList);
    int row = 0;
    for (const iroha::UserDictionaryEntry& entry : dictionary->Entries()) {
        const std::wstring reading = iroha::Utf32ToUtf16(entry.reading);
        const std::wstring word = iroha::Utf32ToUtf16(entry.word);
        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        item.pszText = const_cast<wchar_t*>(reading.c_str());
        ListView_InsertItem(dictList, &item);
        ListView_SetItemText(dictList, row, 1, const_cast<wchar_t*>(word.c_str()));
        ListView_SetItemText(
            dictList, row, 2,
            const_cast<wchar_t*>(
                entry.source == iroha::UserDictionaryEntry::Source::System ? L"取込"
                                                                           : L"手動"));
        ++row;
    }
}

void App::RefreshLearningPage() {
    wchar_t text[128];
    _snwprintf_s(text, _TRUNCATE, L"学習済み: %zu 件", learning->Count());
    SetWindowTextW(learnStatus, text);
}

struct DownloadContext {
    HWND window;
    std::wstring url;
    std::wstring destination;
};

DWORD WINAPI DownloadThread(LPVOID param) {
    auto* context = static_cast<DownloadContext*>(param);
    const std::wstring temp = context->destination + L".tmp";
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(context->destination).parent_path(), ec);
    const HRESULT hr =
        URLDownloadToFileW(nullptr, context->url.c_str(), temp.c_str(), 0, nullptr);
    bool ok = SUCCEEDED(hr);
    if (ok) {
        std::filesystem::rename(temp, context->destination, ec);
        ok = !ec;
    } else {
        std::filesystem::remove(temp, ec);
    }
    PostMessageW(context->window, WM_APP_DOWNLOAD_DONE, ok ? 1 : 0, 0);
    delete context;
    return 0;
}

void App::OnModelDownload() {
    const int index =
        static_cast<int>(SendMessageW(modelCombo, CB_GETCURSEL, 0, 0));
    if (index < 0 || index >= static_cast<int>(ARRAYSIZE(kKnownModels))) return;
    const KnownModel& model = kKnownModels[index];
    downloading = true;
    SetWindowTextW(modelStatus, L"状態: ダウンロード中...");
    EnableWindow(modelDownload, FALSE);
    EnableWindow(modelApply, FALSE);
    auto* context = new DownloadContext{window, model.url,
                                        ModelPath(model.fileName).wstring()};
    HANDLE thread = CreateThread(nullptr, 0, DownloadThread, context, 0, nullptr);
    if (thread) CloseHandle(thread);
}

void App::OnModelApply() {
    const int index =
        static_cast<int>(SendMessageW(modelCombo, CB_GETCURSEL, 0, 0));
    if (index < 0 || index >= static_cast<int>(ARRAYSIZE(kKnownModels))) return;
    const KnownModel& model = kKnownModels[index];

    iroha::Config config = iroha::LoadConfig(ConfigPath());
    config.model = iroha::Utf16ToUtf32(model.fileName);
    if (!iroha::SaveConfig(ConfigPath(), config)) {
        MessageBoxW(window, L"設定の保存に失敗しました", L"iroha", MB_ICONERROR);
        return;
    }
    SetWindowTextW(modelStatus, L"状態: サーバに反映中...（モデルロードに数秒かかります）");
    const bool reloaded = RequestServerReload();
    RefreshModelPage();
    MessageBoxW(window,
                reloaded ? L"モデルを切り替えました。"
                         : L"設定を保存しました。変換サーバが起動していないため、"
                           L"次回の変換時から反映されます。",
                L"iroha", MB_OK);
}

void App::OnDictAdd() {
    wchar_t reading[256];
    wchar_t word[256];
    GetWindowTextW(dictReading, reading, ARRAYSIZE(reading));
    GetWindowTextW(dictWord, word, ARRAYSIZE(word));
    if (reading[0] == 0 || word[0] == 0) {
        MessageBoxW(window, L"読みと単語を入力してください", L"iroha", MB_ICONINFORMATION);
        return;
    }
    dictionary->Add(iroha::Utf16ToUtf32(reading), iroha::Utf16ToUtf32(word));
    SetWindowTextW(dictReading, L"");
    SetWindowTextW(dictWord, L"");
    RefreshDictionaryList();
    RequestServerReload();
}

void App::OnDictRemove() {
    const int selected = ListView_GetNextItem(dictList, -1, LVNI_SELECTED);
    if (selected < 0) return;
    std::vector<iroha::UserDictionaryEntry> entries = dictionary->Entries();
    if (selected >= static_cast<int>(entries.size())) return;
    entries.erase(entries.begin() + selected);
    dictionary->ReplaceAll(std::move(entries));
    RefreshDictionaryList();
    RequestServerReload();
}

void App::OnLearnReset() {
    if (MessageBoxW(window,
                    L"学習した変換をすべて消去します。よろしいですか？\n"
                    L"（ユーザ辞書は消えません）",
                    L"iroha", MB_YESNO | MB_ICONWARNING) != IDYES) {
        return;
    }
    learning->Reset();
    RefreshLearningPage();
    RequestServerReload();
}

// ---- 専門辞書 ----

std::wstring Wide(const std::u32string& text) { return iroha::Utf32ToUtf16(text); }
std::wstring Wide(const std::string& utf8) { return iroha::Utf32ToUtf16(iroha::Utf8ToUtf32(utf8)); }

// カタログの辞書と入れてある辞書を、一覧の行にまとめる（カタログが無くても、入れてある辞書は出す）
void App::RefreshTermList() {
    const std::filesystem::path data(DataDir());
    const auto records = iroha::TermDictionaryInstall::InstalledRecords(data);
    termRows.clear();
    if (termCatalog) {
        for (const auto& entry : termCatalog->dictionaries) {
            TermRow row;
            row.id = entry.id;
            row.name = Wide(entry.name);
            row.group = Wide(entry.group);
            row.summary = Wide(entry.summary);
            row.entries = entry.entries;
            row.license = Wide(entry.license);
            row.sources = entry.sources;
            row.samples = entry.samples;
            row.catalogEntry = &entry;
            for (const auto& record : records) {
                if (record.id != entry.id) continue;
                row.installed = true;
                row.needsUpdate = iroha::TermDictionaryInstall::NeedsUpdate(record, entry);
            }
            termRows.push_back(std::move(row));
        }
    }
    for (const auto& record : records) {
        if (termCatalog && termCatalog->Find(record.id)) continue;
        TermRow row;
        row.id = record.id;
        row.name = Wide(record.name);
        row.group = Wide(record.group);
        row.entries = record.entries;
        row.license = Wide(record.license);
        row.sources = record.sources;
        row.installed = true;
        termRows.push_back(std::move(row));
    }

    const int selected = ListView_GetNextItem(termList, -1, LVNI_SELECTED);
    ListView_DeleteAllItems(termList);
    for (int i = 0; i < static_cast<int>(termRows.size()); ++i) {
        const TermRow& row = termRows[i];
        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.pszText = const_cast<wchar_t*>(row.name.c_str());
        ListView_InsertItem(termList, &item);
        ListView_SetItemText(termList, i, 1, const_cast<wchar_t*>(row.group.c_str()));
        std::wstring entries = std::to_wstring(row.entries);
        ListView_SetItemText(termList, i, 2, entries.data());
        std::wstring state = !row.installed ? L"" : row.needsUpdate ? L"入れてある（更新あり）"
                                             : row.catalogEntry   ? L"入れてある"
                                                                  : L"入れてある（一覧に無い）";
        ListView_SetItemText(termList, i, 3, state.data());
    }
    if (selected >= 0 && selected < static_cast<int>(termRows.size())) {
        ListView_SetItemState(termList, selected, LVIS_SELECTED, LVIS_SELECTED);
    }
    ShowTermDetail();
}

// 選んでいる辞書の説明・語の例・出典とライセンス
void App::ShowTermDetail() {
    const int selected = ListView_GetNextItem(termList, -1, LVNI_SELECTED);
    const bool valid = selected >= 0 && selected < static_cast<int>(termRows.size());
    EnableWindow(termAdd, valid && !termBusy && termRows[selected].catalogEntry &&
                              (!termRows[selected].installed || termRows[selected].needsUpdate));
    EnableWindow(termRemove, valid && !termBusy && termRows[selected].installed);
    if (!valid) {
        SetWindowTextW(termDetail,
                       L"候補ウィンドウに、選んだ分野の用語・人名・地名などを加えます（ライブ変換は変えません）。\r\n"
                       L"一覧から辞書を選ぶと、説明と出典・ライセンスが出ます。");
        return;
    }
    const TermRow& row = termRows[selected];
    std::wstring text = row.name + L"（" + row.group + L"）  " + std::to_wstring(row.entries) + L" 語\r\n";
    if (!row.summary.empty()) text += row.summary + L"\r\n";
    if (!row.samples.empty()) {
        text += L"例: ";
        for (size_t i = 0; i < row.samples.size(); ++i) {
            if (i > 0) text += L"、";
            text += Wide(row.samples[i].word) + L"（" + Wide(row.samples[i].reading) + L"）";
        }
        text += L"\r\n";
    }
    text += L"ライセンス: " + row.license + L"\r\n";
    for (const auto& source : row.sources) {
        text += L"出典: " + Wide(source.name) + L"  © " + Wide(source.holder) + L"  " +
                Wide(source.license) + L"\r\n  " + Wide(source.url) + L"\r\n";
        if (!source.licenseUrl.empty()) text += L"  ライセンス文書: " + Wide(source.licenseUrl) + L"\r\n";
    }
    SetWindowTextW(termDetail, text.c_str());
}

// 一覧の取得・入れる・外すを裏のスレッドで行い、終わったら WM_APP_TERM_DONE で知らせる。
// 一覧を取ったときは、入れてある辞書のうち作り直されたものを入れ直す（macOS版と同じ）
void App::StartTermTask(std::wstring busyMessage, bool fetchCatalog, std::string installId,
                        std::string removeId) {
    if (termBusy) return;
    termBusy = true;
    SetWindowTextW(termStatus, busyMessage.c_str());
    ShowTermDetail();
    struct Task {
        HWND window;
        bool fetchCatalog;
        std::string installId;
        std::string removeId;
        std::unique_ptr<iroha::TermDictionaryCatalog> catalog;
    };
    auto* task = new Task{window, fetchCatalog, std::move(installId), std::move(removeId),
                          termCatalog ? std::make_unique<iroha::TermDictionaryCatalog>(*termCatalog)
                                      : nullptr};
    const auto run = [](LPVOID param) -> DWORD {
        std::unique_ptr<Task> task(static_cast<Task*>(param));
        auto* result = new TermTaskResult();
        const std::filesystem::path data(DataDir());
        std::string error;
        bool changed = false;
        if (task->fetchCatalog) {
            auto catalog = std::make_unique<iroha::TermDictionaryCatalog>();
            if (iroha::TermDictionaryFetcher::FetchCatalog(iroha::TermDictionaryCatalog::DefaultUrl(),
                                                           catalog.get(), &error)) {
                result->catalogFetched = true;
                int updated = 0;
                for (const auto& record : iroha::TermDictionaryInstall::InstalledRecords(data)) {
                    const auto* dictionary = catalog->Find(record.id);
                    if (!dictionary || !iroha::TermDictionaryInstall::NeedsUpdate(record, *dictionary)) {
                        continue;
                    }
                    if (iroha::TermDictionaryFetcher::Install(*dictionary, data, nullptr, &error)) ++updated;
                }
                changed = updated > 0;
                if (updated > 0) result->message = L"新しい版の辞書を " + std::to_wstring(updated) + L" つ入れ直しました";
                result->catalog = std::move(catalog);
            } else {
                result->failed = true;
                result->message = L"辞書の一覧を取得できませんでした: " + Wide(error);
            }
        }
        if (!task->installId.empty() && task->catalog) {
            if (const auto* dictionary = task->catalog->Find(task->installId)) {
                if (iroha::TermDictionaryFetcher::Install(*dictionary, data, nullptr, &error)) {
                    changed = true;
                    result->message = Wide(dictionary->name) + L" を入れました";
                } else {
                    result->failed = true;
                    result->message = L"入れられませんでした: " + Wide(error);
                }
            }
        }
        if (!task->removeId.empty()) {
            if (iroha::TermDictionaryInstall::Remove(data, task->removeId)) {
                changed = true;
                result->message = L"外しました";
            } else {
                result->failed = true;
                result->message = L"外せませんでした";
            }
        }
        // 変換サーバに読み直してもらう（数十万語で 1 秒前後）
        if (changed && !RequestServerReload() && !result->failed) {
            result->message += L"（変換サーバが動いていないため、次に起動したときから使います）";
        }
        PostMessageW(task->window, WM_APP_TERM_DONE, 0, reinterpret_cast<LPARAM>(result));
        return 0;
    };
    HANDLE thread = CreateThread(nullptr, 0, run, task, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        delete task;
        termBusy = false;
        SetWindowTextW(termStatus, L"作業を始められませんでした");
    }
}

void App::OnTermTaskDone(TermTaskResult* result) {
    termBusy = false;
    if (result->catalog) termCatalog = std::move(result->catalog);
    SetWindowTextW(termStatus, result->message.c_str());
    RefreshTermList();
    delete result;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            g_app.window = hwnd;
            g_app.CreateControls();
            g_app.LayoutControls();
            g_app.ShowTab(0);
            SendMessageW(g_app.modelCombo, CB_SETCURSEL, 0, 0);
            {
                // 現在のモデルをコンボボックスに反映
                const iroha::Config config = iroha::LoadConfig(ConfigPath());
                const std::wstring current = iroha::Utf32ToUtf16(config.model);
                for (int i = 0; i < static_cast<int>(ARRAYSIZE(kKnownModels)); ++i) {
                    if (current == kKnownModels[i].fileName) {
                        SendMessageW(g_app.modelCombo, CB_SETCURSEL, i, 0);
                    }
                }
            }
            g_app.RefreshModelPage();
            g_app.RefreshDictionaryList();
            g_app.RefreshTermList();
            g_app.RefreshLearningPage();
            return 0;
        case WM_SIZE:
            g_app.LayoutControls();
            return 0;
        case WM_NOTIFY: {
            const NMHDR* header = reinterpret_cast<NMHDR*>(lParam);
            if (header->hwndFrom == g_app.tab && header->code == TCN_SELCHANGE) {
                g_app.ShowTab(TabCtrl_GetCurSel(g_app.tab));
            }
            if (header->hwndFrom == g_app.termList && header->code == LVN_ITEMCHANGED) {
                g_app.ShowTermDetail();
            }
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case kIdModelCombo:
                    if (HIWORD(wParam) == CBN_SELCHANGE) g_app.RefreshModelPage();
                    return 0;
                case kIdModelDownload:
                    g_app.OnModelDownload();
                    return 0;
                case kIdModelApply:
                    g_app.OnModelApply();
                    return 0;
                case kIdDictAdd:
                    g_app.OnDictAdd();
                    return 0;
                case kIdDictRemove:
                    g_app.OnDictRemove();
                    return 0;
                case kIdLearnReset:
                    g_app.OnLearnReset();
                    return 0;
                case kIdTermAdd:
                case kIdTermRemove: {
                    const int selected = ListView_GetNextItem(g_app.termList, -1, LVNI_SELECTED);
                    if (selected < 0 || selected >= static_cast<int>(g_app.termRows.size())) return 0;
                    const TermRow& row = g_app.termRows[selected];
                    if (LOWORD(wParam) == kIdTermAdd) {
                        g_app.StartTermTask(row.name + L" を取得しています…", false, row.id, {});
                    } else {
                        g_app.StartTermTask(row.name + L" を外しています…", false, {}, row.id);
                    }
                    return 0;
                }
            }
            break;
        case WM_APP_TERM_DONE:
            g_app.OnTermTaskDone(reinterpret_cast<TermTaskResult*>(lParam));
            return 0;
        case WM_APP_DOWNLOAD_DONE:
            g_app.downloading = false;
            g_app.RefreshModelPage();
            MessageBoxW(hwnd,
                        wParam ? L"ダウンロードが完了しました。「このモデルを使う」で切り替えられます。"
                               : L"ダウンロードに失敗しました。ネットワークを確認してください。",
                        L"iroha", wParam ? MB_OK : MB_ICONERROR);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);

    g_app.dictionary = std::make_unique<iroha::UserDictionaryStore>(
        std::filesystem::path(DataDir() + L"\\user-dictionary.json"));
    g_app.learning = std::make_unique<iroha::LearningStore>(
        std::filesystem::path(DataDir() + L"\\learning.json"));

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"IrohaSettings";
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    HWND window = CreateWindowExW(
        0, L"IrohaSettings", L"iroha の設定",
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, 620, 560,
        nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    ShowWindow(window, showCommand);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0)) {
        if (IsDialogMessageW(window, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}
