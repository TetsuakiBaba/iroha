// iroha 設定ウィンドウ。
// タブ: モデル（選択・ダウンロード） / ユーザ辞書（一覧・追加・削除） / 学習。
// 保存先は %LOCALAPPDATA%\iroha\（config.json / user-dictionary.json / learning.json）。
// 保存後は変換サーバへReloadを送って即時反映する（サーバ未起動でも次回起動時に反映）。

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <commctrl.h>
#include <urlmon.h>

#include <filesystem>
#include <string>
#include <vector>

#include "ipc_protocol.h"
#include "iroha/config.h"
#include "iroha/learning_store.h"
#include "iroha/unicode.h"
#include "iroha/user_dictionary_store.h"

#pragma comment(linker, \
    "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
    "version='6.0.0.0' processorArchitecture='*' " \
    "publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr UINT WM_APP_DOWNLOAD_DONE = WM_APP + 1;

struct KnownModel {
    const wchar_t* label;
    const wchar_t* fileName;
    const wchar_t* url;
};
const KnownModel kKnownModels[] = {
    {L"zenz-v3.1-small（標準・高精度 95M / 約70MB）",
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
};

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
    TabCtrl_InsertItem(tab, 1, &item);
    item.pszText = const_cast<wchar_t*>(L"学習");
    TabCtrl_InsertItem(tab, 2, &item);

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

    // 学習
    MoveWindow(learnStatus, x, y, width, Scale(40), TRUE);
    MoveWindow(learnReset, x, y + Scale(48), Scale(140), Scale(28), TRUE);
    MoveWindow(about, x, y + Scale(96), width, Scale(48), TRUE);
}

void App::ShowTab(int index) {
    auto show = [](HWND hwnd, bool visible) {
        ShowWindow(hwnd, visible ? SW_SHOW : SW_HIDE);
    };
    const bool model = index == 0;
    const bool dict = index == 1;
    const bool learn = index == 2;
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
    show(learnStatus, learn);
    show(learnReset, learn);
    show(about, learn);
}

void App::RefreshModelPage() {
    const int index =
        static_cast<int>(SendMessageW(modelCombo, CB_GETCURSEL, 0, 0));
    if (index < 0 || index >= static_cast<int>(ARRAYSIZE(kKnownModels))) return;
    const KnownModel& model = kKnownModels[index];
    const bool downloaded = std::filesystem::exists(ModelPath(model.fileName));

    const iroha::Config config = iroha::LoadConfig(ConfigPath());
    const std::wstring current = config.model.empty()
                                     ? L"zenz-v3.1-small-Q5_K_M.gguf"
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
    size_t sentences = 0;
    size_t segments = 0;
    for (const iroha::LearningEntry& entry : learning->Current().Entries()) {
        if (entry.kind == iroha::LearningEntry::Kind::Sentence) ++sentences;
        else ++segments;
    }
    wchar_t text[128];
    _snwprintf_s(text, _TRUNCATE, L"学習済み: 文 %zu 件 / 文節 %zu 件", sentences,
                 segments);
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
            }
            break;
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
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, 560, 480,
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
