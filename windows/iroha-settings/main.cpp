// iroha 設定ウィンドウ。
// タブ: モデル（選択・ダウンロード） / 入力（ライブ変換・予測変換・句読点・候補の数・プライベートモードなど） /
// ユーザ辞書（一覧・追加・削除） / 専門辞書（配布している辞書の追加・削除） / 変換ルール（日付などの候補） /
// 学習・記録（学習・入力履歴・変換記録）。
// 保存先は %LOCALAPPDATA%\iroha\（config.json / user-dictionary.json / dictionaries\ / user-rewrite-rules.json /
// learning.json / input-history\ / logs\conversions\）。
// 保存後は変換サーバへReloadを送って即時反映する（サーバ未起動でも次回起動時に反映）。

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <commctrl.h>
#include <shellapi.h>
#include <urlmon.h>
#include <uxtheme.h>
#include <vssym32.h>

#include <filesystem>
#include <initializer_list>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "ipc_protocol.h"
#include "iroha/config.h"
#include "iroha/conversion_log.h"
#include "iroha/input_history.h"
#include "iroha/learning_store.h"
#include "iroha/term_dictionary.h"
#include "iroha/unicode.h"
#include "iroha/user_dictionary_store.h"
#include "iroha/user_rewrite_rule.h"
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
    kIdLive,
    kIdPredictive,
    kIdSymbols,
    kIdFullWidthSpace,
    kIdPunctuation,
    kIdCandidateCount,
    kIdPrivateMode,
    kIdRuleList,
    kIdRuleTrigger,
    kIdRuleOutput,
    kIdRuleAdd,
    kIdRuleRemove,
    kIdHistoryReset,
    kIdLogEnabled,
    kIdLogScope,
    kIdLogOpen,
    kIdLogReset,
};

// タブの並び
enum TabIndex {
    kTabModel = 0,
    kTabInput,
    kTabUserDictionary,
    kTabTermDictionary,
    kTabRewriteRules,
    kTabLearning,
    kTabCount,
};

// 句読点の組（設定の値と表示）
const wchar_t* const kPunctuations[] = {L"、。", L"，．"};
// 候補の数の範囲（変換サーバの設定と同じ）
constexpr int kMinCandidateCount = 3;
constexpr int kMaxCandidateCount = 16;

std::wstring DataDir() {
    wchar_t buffer[MAX_PATH];
    const DWORD length =
        GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, ARRAYSIZE(buffer));
    const std::wstring base =
        (length > 0 && length < ARRAYSIZE(buffer)) ? buffer : L".";
    return base + L"\\iroha";
}

std::filesystem::path ConfigPath() { return DataDir() + L"\\config.json"; }
std::filesystem::path ConversionLogDirectory() { return DataDir() + L"\\logs\\conversions"; }

// このコンピュータの名前（入力履歴・変換記録のファイル名。変換サーバと同じ）
std::string HostName() {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD length = ARRAYSIZE(name);
    if (!GetComputerNameW(name, &length)) return "windows";
    return iroha::Utf32ToUtf8(iroha::Utf16ToUtf32(std::wstring(name, length)));
}
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
    // タブの地の色（ラベル・チェックボックスの背景をこれで塗る。窓の地の色のままだと灰色の帯に見える）
    COLORREF pageColor = GetSysColor(COLOR_BTNFACE);
    HBRUSH pageBrush = nullptr;
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
    // 入力
    HWND liveCheck = nullptr;
    HWND predictiveCheck = nullptr;
    HWND symbolsCheck = nullptr;
    HWND spaceCheck = nullptr;
    HWND punctuationLabel = nullptr;
    HWND punctuationCombo = nullptr;
    HWND countLabel = nullptr;
    HWND countCombo = nullptr;
    HWND privateCheck = nullptr;
    HWND inputNote = nullptr;
    // 変換ルール
    HWND ruleList = nullptr;
    HWND ruleLabelTrigger = nullptr;
    HWND ruleTrigger = nullptr;
    HWND ruleLabelOutput = nullptr;
    HWND ruleOutput = nullptr;
    HWND ruleAdd = nullptr;
    HWND ruleRemove = nullptr;
    HWND ruleNote = nullptr;
    std::vector<iroha::UserRewriteRule> rules; // 一覧の行と同じ並び
    bool ruleRefreshing = false;               // 一覧を作り直している間はチェックの変化を無視する
    // 学習・記録
    HWND learnStatus = nullptr;
    HWND learnReset = nullptr;
    HWND historyStatus = nullptr;
    HWND historyReset = nullptr;
    HWND logCheck = nullptr;
    HWND logScopeLabel = nullptr;
    HWND logScopeCombo = nullptr;
    HWND logStatus = nullptr;
    HWND logOpen = nullptr;
    HWND logReset = nullptr;
    HWND about = nullptr;
    // タブごとのコントロール（ShowTab で出し分ける）
    std::vector<std::pair<int, HWND>> pages;
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
    std::unique_ptr<iroha::UserRewriteRuleStore> rewriteRules;
    std::unique_ptr<iroha::InputHistoryStore> inputHistory;
    std::unique_ptr<iroha::ConversionLog> conversionLog;

    void AddToPage(int page, std::initializer_list<HWND> controls) {
        for (HWND control : controls) pages.push_back({page, control});
    }

    int Scale(int value) const {
        return MulDiv(value, static_cast<int>(GetDpiForWindow(window)), 96);
    }

    void CreateControls();
    void LayoutControls();
    void ShowTab(int index);
    void RefreshModelPage();
    void RefreshDictionaryList();
    void RefreshLearningPage();
    void RefreshInputPage();
    void SaveInputPage();
    void RefreshRuleList();
    void OnRuleAdd();
    void OnRuleRemove();
    void OnRuleChecked(int row);
    void OnHistoryReset();
    void SaveLogSettings();
    void OnLogOpen();
    void OnLogReset();
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
    // タブの地はテーマの画像で描かれるので、小さな画像に描いて中ほどの色を取る
    if (HTHEME theme = OpenThemeData(tab, L"TAB")) {
        HDC screen = GetDC(nullptr);
        HDC memory = CreateCompatibleDC(screen);
        HBITMAP bitmap = CreateCompatibleBitmap(screen, 64, 64);
        HGDIOBJ previous = SelectObject(memory, bitmap);
        RECT area = {0, 0, 64, 64};
        if (SUCCEEDED(DrawThemeBackground(theme, memory, TABP_PANE, 0, &area, nullptr))) {
            pageColor = GetPixel(memory, 32, 32);
        }
        SelectObject(memory, previous);
        DeleteObject(bitmap);
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        CloseThemeData(theme);
    }
    pageBrush = CreateSolidBrush(pageColor);
    TCITEMW item = {};
    item.mask = TCIF_TEXT;
    const wchar_t* const titles[kTabCount] = {L"モデル",   L"入力",     L"ユーザ辞書",
                                              L"専門辞書", L"変換ルール", L"学習・記録"};
    for (int i = 0; i < kTabCount; ++i) {
        item.pszText = const_cast<wchar_t*>(titles[i]);
        TabCtrl_InsertItem(tab, i, &item);
    }

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
    AddToPage(kTabModel, {modelCombo, modelStatus, modelDownload, modelApply});

    // ---- 入力 ----
    const DWORD check = BS_AUTOCHECKBOX | WS_TABSTOP;
    liveCheck = CreateChild(WC_BUTTONW, L"ライブ変換（入力中に変換して見せる）", check, kIdLive);
    predictiveCheck = CreateChild(WC_BUTTONW, L"予測変換（入力履歴から続きの語を出す。Tab・↓ で選ぶ）", check,
                                  kIdPredictive);
    symbolsCheck = CreateChild(WC_BUTTONW, L"記号・顔文字・絵文字を候補ウィンドウに出す", check, kIdSymbols);
    spaceCheck = CreateChild(WC_BUTTONW, L"入力していないときの Space を全角スペースにする（Shift+Space は半角）",
                             check, kIdFullWidthSpace);
    punctuationLabel = CreateChild(WC_STATICW, L"句読点（Ctrl+. でも切り替え）:", 0, 0);
    punctuationCombo = CreateChild(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, kIdPunctuation);
    for (const wchar_t* punctuation : kPunctuations) {
        SendMessageW(punctuationCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(punctuation));
    }
    countLabel = CreateChild(WC_STATICW, L"候補ウィンドウの候補の数:", 0, 0);
    countCombo = CreateChild(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, kIdCandidateCount);
    for (int count = kMinCandidateCount; count <= kMaxCandidateCount; ++count) {
        const std::wstring text = std::to_wstring(count);
        SendMessageW(countCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    privateCheck = CreateChild(WC_BUTTONW, L"プライベートモード（Ctrl+Alt+P でも切り替え）", check, kIdPrivateMode);
    inputNote = CreateChild(WC_STATICW,
                            L"プライベートモードの間は、学習・入力履歴・予測変換・変換記録・ユーザ辞書を止めます。\n"
                            L"変更はすぐに反映されます。",
                            0, 0);
    AddToPage(kTabInput, {liveCheck, predictiveCheck, symbolsCheck, spaceCheck, punctuationLabel,
                          punctuationCombo, countLabel, countCombo, privateCheck, inputNote});

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
    AddToPage(kTabUserDictionary,
              {dictList, dictLabelReading, dictReading, dictLabelWord, dictWord, dictAdd, dictRemove});

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
    AddToPage(kTabTermDictionary, {termList, termAdd, termRemove, termStatus, termDetail});

    // ---- 変換ルール ----
    ruleList = CreateChild(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | WS_TABSTOP, kIdRuleList,
                           WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(ruleList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_CHECKBOXES);
    const struct {
        const wchar_t* title;
        int width;
    } ruleColumns[] = {{L"読み", 130}, {L"出力", 200}, {L"いま出る候補", 170}};
    for (int i = 0; i < static_cast<int>(ARRAYSIZE(ruleColumns)); ++i) {
        column.cx = Scale(ruleColumns[i].width);
        column.pszText = const_cast<wchar_t*>(ruleColumns[i].title);
        ListView_InsertColumn(ruleList, i, &column);
    }
    ruleLabelTrigger = CreateChild(WC_STATICW, L"読み:", 0, 0);
    ruleTrigger = CreateChild(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, kIdRuleTrigger, WS_EX_CLIENTEDGE);
    ruleLabelOutput = CreateChild(WC_STATICW, L"出力:", 0, 0);
    ruleOutput = CreateChild(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, kIdRuleOutput, WS_EX_CLIENTEDGE);
    ruleAdd = CreateChild(WC_BUTTONW, L"追加", WS_TABSTOP, kIdRuleAdd);
    ruleRemove = CreateChild(WC_BUTTONW, L"選択を削除", WS_TABSTOP, kIdRuleRemove);
    ruleNote = CreateChild(WC_STATICW,
                           L"読みを打って Space を押したときの候補に、出力を加えます（ライブ変換は変えません）。"
                           L"チェックを外したルールは使いません。\n"
                           L"出力に書けるもの: {{date}}（今日の日付）・{{date+1}}（明日）・{{time:HH:mm}}・"
                           L"{{datetime}}・{{wareki}}（和暦）。日付の書式は {{date:yyyy年M月d日（E）}} のように指定します。",
                           0, 0);
    AddToPage(kTabRewriteRules, {ruleList, ruleLabelTrigger, ruleTrigger, ruleLabelOutput, ruleOutput, ruleAdd,
                                 ruleRemove, ruleNote});

    // ---- 学習・記録 ----
    learnStatus = CreateChild(WC_STATICW, L"", 0, kIdLearnStatus);
    learnReset = CreateChild(WC_BUTTONW, L"学習をリセット", WS_TABSTOP, kIdLearnReset);
    historyStatus = CreateChild(WC_STATICW, L"", 0, 0);
    historyReset = CreateChild(WC_BUTTONW, L"入力履歴を消去", WS_TABSTOP, kIdHistoryReset);
    logCheck = CreateChild(WC_BUTTONW, L"変換記録を残す（追加学習に使うデータ。カーソルの手前の文章も残ります）", check,
                           kIdLogEnabled);
    logScopeLabel = CreateChild(WC_STATICW, L"記録する確定:", 0, 0);
    logScopeCombo = CreateChild(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, kIdLogScope);
    SendMessageW(logScopeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"すべての確定"));
    SendMessageW(logScopeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"直した確定だけ"));
    logStatus = CreateChild(WC_STATICW, L"", 0, 0);
    logOpen = CreateChild(WC_BUTTONW, L"フォルダを開く", WS_TABSTOP, kIdLogOpen);
    logReset = CreateChild(WC_BUTTONW, L"変換記録を消去", WS_TABSTOP, kIdLogReset);
    about = CreateChild(WC_STATICW,
                        L"iroha for Windows\nhttps://github.com/TetsuakiBaba/iroha", 0,
                        kIdAbout);
    AddToPage(kTabLearning, {learnStatus, learnReset, historyStatus, historyReset, logCheck, logScopeLabel,
                             logScopeCombo, logStatus, logOpen, logReset, about});
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

    // 入力
    {
        int rowY = y;
        for (HWND checkBox : {liveCheck, predictiveCheck, symbolsCheck, spaceCheck}) {
            MoveWindow(checkBox, x, rowY, width, Scale(24), TRUE);
            rowY += Scale(32);
        }
        MoveWindow(punctuationLabel, x, rowY + Scale(4), Scale(230), Scale(20), TRUE);
        MoveWindow(punctuationCombo, x + Scale(236), rowY, Scale(90), Scale(200), TRUE);
        rowY += Scale(36);
        MoveWindow(countLabel, x, rowY + Scale(4), Scale(230), Scale(20), TRUE);
        MoveWindow(countCombo, x + Scale(236), rowY, Scale(90), Scale(300), TRUE);
        rowY += Scale(40);
        MoveWindow(privateCheck, x, rowY, width, Scale(24), TRUE);
        MoveWindow(inputNote, x, rowY + Scale(36), width, Scale(48), TRUE);
    }

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

    // 変換ルール
    const int noteHeight = Scale(64);
    const int ruleListHeight = page.bottom - y - noteHeight - Scale(84);
    MoveWindow(ruleList, x, y, width, ruleListHeight, TRUE);
    const int ruleRowY = y + ruleListHeight + Scale(10);
    MoveWindow(ruleLabelTrigger, x, ruleRowY + Scale(4), Scale(38), Scale(20), TRUE);
    MoveWindow(ruleTrigger, x + Scale(40), ruleRowY, Scale(120), Scale(24), TRUE);
    MoveWindow(ruleLabelOutput, x + Scale(170), ruleRowY + Scale(4), Scale(38), Scale(20), TRUE);
    MoveWindow(ruleOutput, x + Scale(210), ruleRowY, Scale(200), Scale(24), TRUE);
    MoveWindow(ruleAdd, x + Scale(420), ruleRowY, Scale(60), Scale(26), TRUE);
    MoveWindow(ruleRemove, x, ruleRowY + Scale(34), Scale(110), Scale(26), TRUE);
    MoveWindow(ruleNote, x, ruleRowY + Scale(66), width, noteHeight, TRUE);

    // 学習・記録
    MoveWindow(learnStatus, x, y, width, Scale(20), TRUE);
    MoveWindow(learnReset, x, y + Scale(26), Scale(140), Scale(28), TRUE);
    MoveWindow(historyStatus, x, y + Scale(70), width, Scale(20), TRUE);
    MoveWindow(historyReset, x, y + Scale(96), Scale(140), Scale(28), TRUE);
    MoveWindow(logCheck, x, y + Scale(142), width, Scale(24), TRUE);
    MoveWindow(logScopeLabel, x + Scale(20), y + Scale(176), Scale(110), Scale(20), TRUE);
    MoveWindow(logScopeCombo, x + Scale(134), y + Scale(172), Scale(160), Scale(200), TRUE);
    MoveWindow(logStatus, x + Scale(20), y + Scale(206), width - Scale(20), Scale(20), TRUE);
    MoveWindow(logOpen, x + Scale(20), y + Scale(232), Scale(130), Scale(28), TRUE);
    MoveWindow(logReset, x + Scale(158), y + Scale(232), Scale(140), Scale(28), TRUE);
    MoveWindow(about, x, y + Scale(286), width, Scale(48), TRUE);
}

void App::ShowTab(int index) {
    auto show = [](HWND hwnd, bool visible) {
        ShowWindow(hwnd, visible ? SW_SHOW : SW_HIDE);
    };
    for (const auto& [page, control] : pages) show(control, page == index);
    const bool term = index == kTabTermDictionary;
    // 件数は、ほかのところ（変換サーバ）で増えるので、タブを開くたびに数え直す
    if (index == kTabLearning) RefreshLearningPage();
    if (index == kTabInput) RefreshInputPage();
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
    learning = std::make_unique<iroha::LearningStore>(std::filesystem::path(DataDir() + L"\\learning.json"));
    wchar_t text[256];
    _snwprintf_s(text, _TRUNCATE, L"学習: %zu 件", learning->Count());
    SetWindowTextW(learnStatus, text);
    inputHistory->Reload();
    _snwprintf_s(text, _TRUNCATE, L"入力履歴（予測変換が ON の間だけ記録）: %zu 件", inputHistory->Count());
    SetWindowTextW(historyStatus, text);

    const iroha::Config config = iroha::LoadConfig(ConfigPath());
    SendMessageW(logCheck, BM_SETCHECK, config.conversionLog ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(logScopeCombo, CB_SETCURSEL, config.conversionLogCorrectionsOnly ? 1 : 0, 0);
    EnableWindow(logScopeCombo, config.conversionLog);
    const size_t count = conversionLog->Items().size();
    const uintmax_t size = conversionLog->TotalSize();
    _snwprintf_s(text, _TRUNCATE, L"記録: %zu 件（%.1f KB）%ls", count, static_cast<double>(size) / 1024.0,
                 config.privateMode && config.conversionLog ? L"  プライベートモードの間は記録しません" : L"");
    SetWindowTextW(logStatus, text);
    EnableWindow(logReset, count > 0 || size > 0);
}

// ---- 入力のページ ----

void App::RefreshInputPage() {
    const iroha::Config config = iroha::LoadConfig(ConfigPath());
    const auto setCheck = [](HWND control, bool checked) {
        SendMessageW(control, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    };
    setCheck(liveCheck, config.liveConversion);
    setCheck(predictiveCheck, config.predictive);
    setCheck(symbolsCheck, config.symbolCandidates);
    setCheck(spaceCheck, config.fullWidthSpace);
    setCheck(privateCheck, config.privateMode);
    SendMessageW(punctuationCombo, CB_SETCURSEL, iroha::Utf32ToUtf16(config.punctuation) == kPunctuations[1] ? 1 : 0,
                 0);
    SendMessageW(countCombo, CB_SETCURSEL, config.candidateCount - kMinCandidateCount, 0);
}

// 入力のページの値を設定に書いて、変換サーバに読み直してもらう（ほかの項目は今の設定のまま）
void App::SaveInputPage() {
    iroha::Config config = iroha::LoadConfig(ConfigPath());
    const auto checked = [](HWND control) { return SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED; };
    config.liveConversion = checked(liveCheck);
    config.predictive = checked(predictiveCheck);
    config.symbolCandidates = checked(symbolsCheck);
    config.fullWidthSpace = checked(spaceCheck);
    config.privateMode = checked(privateCheck);
    const LRESULT punctuation = SendMessageW(punctuationCombo, CB_GETCURSEL, 0, 0);
    config.punctuation = iroha::Utf16ToUtf32(kPunctuations[punctuation == 1 ? 1 : 0]);
    const LRESULT count = SendMessageW(countCombo, CB_GETCURSEL, 0, 0);
    if (count >= 0) config.candidateCount = kMinCandidateCount + static_cast<int>(count);
    if (!iroha::SaveConfig(ConfigPath(), config)) {
        MessageBoxW(window, L"設定の保存に失敗しました", L"iroha", MB_ICONERROR);
        return;
    }
    RequestServerReload();
}

// ---- 変換ルールのページ ----

void App::RefreshRuleList() {
    rewriteRules->Reload();
    rules = rewriteRules->Rules();
    ruleRefreshing = true;
    ListView_DeleteAllItems(ruleList);
    const iroha::RewriteContext now = iroha::RewriteContext::Now();
    for (int i = 0; i < static_cast<int>(rules.size()); ++i) {
        const iroha::UserRewriteRule& rule = rules[i];
        std::wstring trigger = iroha::Utf32ToUtf16(rule.trigger);
        std::wstring output = iroha::Utf32ToUtf16(rule.output);
        std::wstring rendered = iroha::Utf32ToUtf16(iroha::RewriteTemplate(rule.output).Render(now));
        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.pszText = trigger.data();
        ListView_InsertItem(ruleList, &item);
        ListView_SetItemText(ruleList, i, 1, output.data());
        ListView_SetItemText(ruleList, i, 2, rendered.data());
        ListView_SetCheckState(ruleList, i, rule.isEnabled);
    }
    ruleRefreshing = false;
}

void App::OnRuleAdd() {
    wchar_t trigger[256];
    wchar_t output[1024];
    GetWindowTextW(ruleTrigger, trigger, ARRAYSIZE(trigger));
    GetWindowTextW(ruleOutput, output, ARRAYSIZE(output));
    const std::u32string normalized = iroha::UserRewriteRule::NormalizedTrigger(iroha::Utf16ToUtf32(trigger));
    const std::u32string templateText = iroha::Utf16ToUtf32(output);
    if (normalized.empty() || templateText.empty()) {
        MessageBoxW(window, L"読みと出力を入力してください", L"iroha", MB_ICONINFORMATION);
        return;
    }
    // 知らない {{…}} はそのまま文字として出るので、追加する前に知らせる
    const auto unknown = iroha::RewriteTemplate(templateText).UnknownPlaceholders();
    if (!unknown.empty()) {
        std::wstring names;
        for (const auto& name : unknown) names += L"{{" + iroha::Utf32ToUtf16(name) + L"}} ";
        const std::wstring message =
            L"次の書き方には対応していないので、そのまま文字として出ます:\n" + names + L"\n\nこのまま追加しますか？";
        if (MessageBoxW(window, message.c_str(), L"iroha", MB_YESNO | MB_ICONWARNING) != IDYES) return;
    }
    rewriteRules->Add(normalized, templateText);
    SetWindowTextW(ruleTrigger, L"");
    SetWindowTextW(ruleOutput, L"");
    RefreshRuleList();
    RequestServerReload();
}

void App::OnRuleRemove() {
    std::set<std::string> ids;
    for (int row = ListView_GetNextItem(ruleList, -1, LVNI_SELECTED); row >= 0;
         row = ListView_GetNextItem(ruleList, row, LVNI_SELECTED)) {
        if (row < static_cast<int>(rules.size())) ids.insert(rules[row].id);
    }
    if (ids.empty()) return;
    rewriteRules->Remove(ids);
    RefreshRuleList();
    RequestServerReload();
}

// 一覧のチェック（使う・使わない）を切り替えた
void App::OnRuleChecked(int row) {
    if (ruleRefreshing || row < 0 || row >= static_cast<int>(rules.size())) return;
    const bool enabled = ListView_GetCheckState(ruleList, row) != FALSE;
    if (enabled == rules[row].isEnabled) return;
    rewriteRules->SetEnabled(enabled, rules[row].id);
    rules[row].isEnabled = enabled;
    RequestServerReload();
}

// ---- 入力履歴・変換記録 ----

void App::OnHistoryReset() {
    if (MessageBoxW(window, L"入力履歴（予測変換の候補）をすべて消去します。よろしいですか？", L"iroha",
                    MB_YESNO | MB_ICONWARNING) != IDYES) {
        return;
    }
    inputHistory->Reset(iroha::InputHistoryStore::NowSeconds());
    inputHistory->Flush();
    RequestServerReload();
    RefreshLearningPage();
}

void App::SaveLogSettings() {
    iroha::Config config = iroha::LoadConfig(ConfigPath());
    config.conversionLog = SendMessageW(logCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    config.conversionLogCorrectionsOnly = SendMessageW(logScopeCombo, CB_GETCURSEL, 0, 0) == 1;
    if (!iroha::SaveConfig(ConfigPath(), config)) {
        MessageBoxW(window, L"設定の保存に失敗しました", L"iroha", MB_ICONERROR);
        return;
    }
    RequestServerReload();
    RefreshLearningPage();
}

void App::OnLogOpen() {
    std::error_code error;
    std::filesystem::create_directories(ConversionLogDirectory(), error);
    ShellExecuteW(window, L"open", ConversionLogDirectory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void App::OnLogReset() {
    if (MessageBoxW(window,
                    L"変換記録をすべて消去します。よろしいですか？\n"
                    L"（データフォルダを共有しているほかのコンピュータの記録も消えます）",
                    L"iroha", MB_YESNO | MB_ICONWARNING) != IDYES) {
        return;
    }
    conversionLog->RemoveAll();
    RefreshLearningPage();
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
            g_app.RefreshInputPage();
            g_app.RefreshRuleList();
            g_app.RefreshLearningPage();
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            // 読み取り専用の入力欄（専門辞書の説明）は既定のまま
            if (reinterpret_cast<HWND>(lParam) != g_app.termDetail && g_app.pageBrush) {
                SetBkColor(reinterpret_cast<HDC>(wParam), g_app.pageColor);
                return reinterpret_cast<LRESULT>(g_app.pageBrush);
            }
            break;
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
            if (header->hwndFrom == g_app.ruleList && header->code == LVN_ITEMCHANGED) {
                const auto* change = reinterpret_cast<const NMLISTVIEW*>(lParam);
                if ((change->uChanged & LVIF_STATE) &&
                    ((change->uNewState ^ change->uOldState) & LVIS_STATEIMAGEMASK)) {
                    g_app.OnRuleChecked(change->iItem);
                }
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
                case kIdLive:
                case kIdPredictive:
                case kIdSymbols:
                case kIdFullWidthSpace:
                case kIdPrivateMode:
                    if (HIWORD(wParam) == BN_CLICKED) g_app.SaveInputPage();
                    return 0;
                case kIdPunctuation:
                case kIdCandidateCount:
                    if (HIWORD(wParam) == CBN_SELCHANGE) g_app.SaveInputPage();
                    return 0;
                case kIdRuleAdd:
                    g_app.OnRuleAdd();
                    return 0;
                case kIdRuleRemove:
                    g_app.OnRuleRemove();
                    return 0;
                case kIdHistoryReset:
                    g_app.OnHistoryReset();
                    return 0;
                case kIdLogEnabled:
                    if (HIWORD(wParam) == BN_CLICKED) g_app.SaveLogSettings();
                    return 0;
                case kIdLogScope:
                    if (HIWORD(wParam) == CBN_SELCHANGE) g_app.SaveLogSettings();
                    return 0;
                case kIdLogOpen:
                    g_app.OnLogOpen();
                    return 0;
                case kIdLogReset:
                    g_app.OnLogReset();
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

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int showCommand) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);

    g_app.dictionary = std::make_unique<iroha::UserDictionaryStore>(
        std::filesystem::path(DataDir() + L"\\user-dictionary.json"));
    g_app.learning = std::make_unique<iroha::LearningStore>(
        std::filesystem::path(DataDir() + L"\\learning.json"));
    // 変換ルールは変換サーバと同じく、初めてなら既定のルール（きょう・あした など）を用意する
    g_app.rewriteRules = std::make_unique<iroha::UserRewriteRuleStore>(
        std::filesystem::path(DataDir() + L"\\user-rewrite-rules.json"), /*seedsDefaults=*/true);
    g_app.inputHistory = std::make_unique<iroha::InputHistoryStore>(
        std::filesystem::path(DataDir() + L"\\input-history"), HostName(), /*saveDelayMs=*/0);
    g_app.conversionLog = std::make_unique<iroha::ConversionLog>(ConversionLogDirectory(), HostName());

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
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, 680, 580,
        nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    // --tab <名前> で開くタブを選ぶ（model / input / dictionary / terms / rules / learning）
    const std::wstring arguments = commandLine ? commandLine : L"";
    const wchar_t* const tabNames[kTabCount] = {L"model", L"input", L"dictionary", L"terms", L"rules", L"learning"};
    if (const size_t option = arguments.find(L"--tab "); option != std::wstring::npos) {
        const std::wstring name = arguments.substr(option + 6, arguments.find(L' ', option + 6) - option - 6);
        for (int i = 0; i < kTabCount; ++i) {
            if (name != tabNames[i]) continue;
            TabCtrl_SetCurSel(g_app.tab, i);
            g_app.ShowTab(i);
        }
    }
    ShowWindow(window, showCommand);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0)) {
        if (IsDialogMessageW(window, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}
