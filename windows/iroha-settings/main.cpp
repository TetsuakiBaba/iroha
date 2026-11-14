// iroha 設定ウィンドウ（Windows 11 の設定アプリの見た目に合わせる。2026-11-14 に作り直した）。
//
// 左に項目の一覧（入力 / 辞書・学習 / ユーザ辞書 / 専門辞書 / 変換ルール / 選択テキスト / AI / モデル / 情報）、
// 右に節の見出しと、項目のカード（題・灰色の短い説明・右側の操作部品）を並べる。細かな説明は画面に並べず「?」に
// 入れる（macOS版の SettingsHelp と同じ考え方）。開発者モード（情報 > 開発者向け、既定 OFF）の項目には紫の
// 「開発者」の印を付け、OFF の間は表示しない（値はそのまま効く）。
//
// 保存先は %LOCALAPPDATA%\iroha\（config.json / user-dictionary.json / dictionaries\ / user-rewrite-rules.json /
// learning.json / input-history\ / logs\conversions\）。変えたらすぐ保存し、変換サーバへ Reload を送って反映する
// （サーバ未起動でも次回起動時に反映）。
// 開くページは --tab <名前>（input / learning / dictionary / terms / rules / selection / ai / model / about）で選べる

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <urlmon.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <objidl.h>
namespace Gdiplus {
using std::max;
using std::min;
} // namespace Gdiplus
#include <gdiplus.h>

#include "ai_client.h"
#include "ipc_protocol.h"
#include "iroha/ai_conversion.h"
#include "iroha/config.h"
#include "iroha/conversion_log.h"
#include "iroha/input_history.h"
#include "iroha/learning_store.h"
#include "iroha/term_dictionary.h"
#include "iroha/typo_normalizer.h"
#include "iroha/typo_normalizer_catalog.h"
#include "iroha/unicode.h"
#include "iroha/user_dictionary_store.h"
#include "iroha/user_rewrite_rule.h"
#include "key_shortcut.h"
#include "secret_store.h"
#include "term_dictionary_fetcher.h"
#include "typo_normalizer_fetcher.h"
#include "ui.h"
#include "updater.h"
#include "voice_recognizer.h"

#pragma comment(linker, \
    "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
    "version='6.0.0.0' processorArchitecture='*' " \
    "publicKeyToken='6595b64144ccf1df' language='*'\"")

#ifndef IROHA_VERSION
#define IROHA_VERSION "開発版"
#endif

namespace {

constexpr UINT WM_APP_DOWNLOAD_DONE = WM_APP + 1;
// 専門辞書の一覧の取得・追加・削除が終わった（lParam は TermTaskResult*。受け取った側が消す）
constexpr UINT WM_APP_TERM_DONE = WM_APP + 2;
// 打ち間違い訂正のモデルの取得が終わった（lParam は TypoTaskResult*。受け取った側が消す）
constexpr UINT WM_APP_TYPO_DONE = WM_APP + 3;
// AI のモデルの一覧の取得が終わった（lParam は AIModelsResult*。受け取った側が消す）
constexpr UINT WM_APP_AI_MODELS = WM_APP + 4;

using ui::g_theme;
using Row = ui::Page::Row;
using Control = ui::Page::Control;

struct KnownModel {
    const wchar_t* label;
    const wchar_t* fileName;
    const wchar_t* url;
    // 取得したファイルの照合（途中で切れた・差し替わったファイルを置かない）。0・空なら照合しない
    uintmax_t bytes;
    const char* sha256;
};
const KnownModel kKnownModels[] = {
    // 大きさ・SHA-256 は macOS版の ModelDownloader.swift・fetch-model.ps1・インストーラー（installer/iroha.iss）と合わせる
    {L"iroha-kkc-v1（既定・文字単位 T5・約 82MB）",
     L"iroha-kkc-v1-Q5_K_M.gguf",
     L"https://github.com/TetsuakiBaba/iroha/releases/download/iroha-kkc-v1/iroha-kkc-v1-Q5_K_M.gguf", 81535360,
     "f060e259dbed28754f71e1634bc01a5d9d735d519f8ff72c333cac77c859efda"},
    {L"zenz-v3.1-small（以前の既定・約 70MB）",
     L"zenz-v3.1-small-Q5_K_M.gguf",
     L"https://huggingface.co/Miwa-Keita/zenz-v3.1-small-gguf/resolve/main/ggml-model-Q5_K_M.gguf", 0, ""},
    {L"zenz-v3.1-xsmall（高速・軽量・約 21MB）",
     L"zenz-v3.1-xsmall-Q5_K_M.gguf",
     L"https://huggingface.co/Miwa-Keita/zenz-v3.1-xsmall-gguf/resolve/main/ggml-model-Q5_K_M.gguf", 0, ""},
};

// 部品の番号
enum ControlId {
    kIdNone = 0,
    // 入力
    kIdLive = 100,
    kIdCandidateCount,
    kIdDocumentContext,
    kIdTypo,
    kIdTypoLicense,
    kIdTypoRemove,
    kIdTypoDelay,
    kIdTypoMinLength,
    kIdTypoThreshold,
    kIdTypoThresholdDefault,
    kIdPredictive,
    kIdHistoryReset,
    kIdPunctuation,
    kIdFullWidthSpace,
    kIdVoice,
    kIdVoiceKey,
    kIdDelimited,
    kIdDelimitedStart,
    kIdDelimitedEnd,
    kIdAIFix,
    kIdAIFixThreshold,
    kIdAIFixShortcut,
    // 辞書・学習
    kIdPrivateMode,
    kIdPrivateHotkey,
    kIdLearning,
    kIdLearnReset,
    kIdLearnOpen,
    kIdLogEnabled,
    kIdLogScope,
    kIdLogOpen,
    kIdLogReset,
    kIdSymbols,
    // ユーザ辞書
    kIdDictList,
    kIdDictReading,
    kIdDictWord,
    kIdDictAdd,
    kIdDictRemove,
    // 専門辞書
    kIdTermList,
    kIdTermAdd,
    kIdTermRemove,
    kIdTermDetail,
    // 変換ルール
    kIdRuleList,
    kIdRuleTrigger,
    kIdRuleOutput,
    kIdRuleAdd,
    kIdRuleRemove,
    // 選択テキスト
    kIdSelEnabled,
    kIdSelTrigger,
    kIdSelCount,
    kIdSelOnDemand,
    kIdSelExcluded,
    kIdSelPresetEnabled0,
    kIdSelPresetName0 = kIdSelPresetEnabled0 + 5,
    kIdSelPresetHotkey0 = kIdSelPresetName0 + 5,
    kIdSelPresetPrompt0 = kIdSelPresetHotkey0 + 5,
    kIdSelEnd = kIdSelPresetPrompt0 + 5,
    // AI
    kIdAIService,
    kIdAIEndpoint,
    kIdAIModel,
    kIdAIModels,
    kIdAIKey,
    kIdAIKeySave,
    kIdAIKeyClear,
    kIdAIPresetName0,
    kIdAIPresetHotkey0 = kIdAIPresetName0 + 3,
    kIdAIPresetPrompt0 = kIdAIPresetHotkey0 + 3,
    kIdAIPresetReset0 = kIdAIPresetPrompt0 + 3,
    kIdAIEnd = kIdAIPresetReset0 + 3,
    // モデル
    kIdModelCombo,
    kIdModelDownload,
    kIdModelApply,
    kIdModelFile,
    kIdModelFileDefault,
    kIdModelFolder,
    kIdAdapterFile,
    kIdAdapterClear,
    // 情報
    kIdGitHub,
    kIdDataFolder,
    kIdAutoUpdate,
    kIdCheckUpdate,
    kIdDeveloperMode,
    kIdDeveloperOverlay,
};

// 句読点の組（設定の値と表示）
const wchar_t* const kPunctuations[] = {L"、。", L"，．"};
// 候補の数の範囲（変換サーバの設定と同じ）
constexpr int kMinCandidateCount = 3;
constexpr int kMaxCandidateCount = 16;
// 選択テキストのマウスのきっかけ（設定の値と表示。macOS版と同じ値）
const char* const kTriggerModes[] = {"off", "bubble", "immediateMenu", "optionMenu"};
const wchar_t* const kTriggerLabels[] = {L"オフ", L"アイコンを表示（クリックでメニュー）", L"すぐにメニューを表示",
                                         L"Alt を押しながら選択でメニュー"};

std::wstring DataDir() {
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, ARRAYSIZE(buffer));
    const std::wstring base = (length > 0 && length < ARRAYSIZE(buffer)) ? buffer : L".";
    return base + L"\\iroha";
}

std::filesystem::path ConfigPath() { return DataDir() + L"\\config.json"; }
std::filesystem::path ConversionLogDirectory() { return DataDir() + L"\\logs\\conversions"; }
std::filesystem::path ModelPath(const wchar_t* fileName) { return DataDir() + L"\\models\\" + fileName; }

// このコンピュータの名前（入力履歴・変換記録のファイル名。変換サーバと同じ）
std::string HostName() {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD length = ARRAYSIZE(name);
    if (!GetComputerNameW(name, &length)) return "windows";
    return iroha::Utf32ToUtf8(iroha::Utf16ToUtf32(std::wstring(name, length)));
}

// サーバに設定・辞書の再読み込みを頼む（モデル切替はロード込みで数秒かかりうる）
bool RequestServerReload() {
    std::vector<char> response;
    return iroha::ipc::Call(iroha::ipc::BuildReloadRequest(), &response, 20000);
}

std::wstring Wide(const std::u32string& text) { return iroha::Utf32ToUtf16(text); }
std::wstring Wide(const std::string& utf8) { return iroha::Utf32ToUtf16(iroha::Utf8ToUtf32(utf8)); }
std::string Narrow(const std::wstring& text) { return iroha::Utf32ToUtf8(iroha::Utf16ToUtf32(text)); }

std::wstring WindowText(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}

// 前後の空白を落とした入力欄の文字
std::u32string TrimmedText(HWND control) { return iroha::TrimWhitespace(iroha::Utf16ToUtf32(WindowText(control))); }

// 複数行の入力欄は改行を CRLF で受ける
std::wstring ToEditText(const std::u32string& text) {
    std::wstring wide;
    for (char32_t c : text) {
        if (c == U'\n') wide += L'\r';
        wide += iroha::Utf32ToUtf16(std::u32string(1, c));
    }
    return wide;
}

std::u32string FromEditText(HWND control) {
    std::u32string text = iroha::Utf16ToUtf32(WindowText(control));
    text.erase(std::remove(text.begin(), text.end(), U'\r'), text.end());
    return iroha::TrimWhitespace(text);
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

// AI の接続先の並び（コンボボックスの順）
const iroha::AIService kAIServices[] = {iroha::AIService::None, iroha::AIService::Ollama, iroha::AIService::LMStudio,
                                        iroha::AIService::OpenAI, iroha::AIService::Claude};

struct AIModelsResult {
    std::vector<std::string> models;
    std::wstring error;
};
struct TypoTaskResult {
    std::wstring message;
    bool failed = false;
};
struct TermTaskResult {
    bool catalogFetched = false;
    std::unique_ptr<iroha::TermDictionaryCatalog> catalog;
    std::wstring message;
    bool failed = false;
};

// 左の一覧の 1 項目
struct NavEntry {
    const wchar_t* key;   // --tab の名前
    const wchar_t* glyph; // Segoe Fluent Icons の字
    const wchar_t* title;
    std::unique_ptr<ui::Page> page;
    RECT rect = {};
};

int AIServiceIndex(iroha::AIService service) {
    for (int i = 0; i < static_cast<int>(ARRAYSIZE(kAIServices)); ++i) {
        if (kAIServices[i] == service) return i;
    }
    return 0;
}

// 確信の強さの目安（macOS版 TypoThresholdRow と同じ区切り）
std::wstring TypoThresholdLabel(double threshold) {
    if (threshold < 1.0) return L"よく拾う";
    if (threshold < 3.0) return std::abs(threshold - iroha::TypoNormalizer::kDefaultThreshold) < 1e-9 ? L"標準・既定" : L"標準";
    if (threshold < 5.0) return L"慎重";
    return L"ほとんど出さない";
}

std::wstring FormatNumber(double value, int digits) {
    wchar_t buffer[32];
    _snwprintf_s(buffer, _TRUNCATE, L"%.*f", digits, value);
    return buffer;
}

struct App {
    HWND window = nullptr;
    std::vector<NavEntry> nav;
    int current = 0;
    int hoverNav = -1;
    bool developerMode = false;
    // 設定の控え（表示の出し分けに使う。保存するたびに読み直す）
    iroha::Config config;

    // 入力
    HWND liveToggle = nullptr, countCombo = nullptr, contextToggle = nullptr;
    HWND typoToggle = nullptr, typoLicense = nullptr, typoRemove = nullptr;
    HWND typoDelay = nullptr, typoMinLength = nullptr, typoThreshold = nullptr, typoThresholdDefault = nullptr;
    HWND predictiveToggle = nullptr, historyReset = nullptr;
    HWND punctuationCombo = nullptr, spaceToggle = nullptr;
    HWND voiceToggle = nullptr, voiceKeyCombo = nullptr;
    HWND delimitedToggle = nullptr, delimitedStart = nullptr, delimitedEnd = nullptr;
    HWND aiFixToggle = nullptr, aiFixThreshold = nullptr, aiFixShortcut = nullptr;
    Row* typoRow = nullptr;
    Row* typoModelRow = nullptr;
    Row* typoDelayRow = nullptr;
    Row* typoThresholdRow = nullptr;
    Row* historyRow = nullptr;
    Row* voiceRow = nullptr;
    Row* delimitedStartRow = nullptr;
    Row* delimitedEndRow = nullptr;
    Row* aiFixThresholdRow = nullptr;
    Row* aiFixShortcutRow = nullptr;
    bool typoBusy = false;
    bool typoChecked = false;
    bool voiceEngineChecked = false;
    // 辞書・学習
    HWND privateToggle = nullptr, privateHotkey = nullptr;
    HWND learningToggle = nullptr, learnReset = nullptr, learnOpen = nullptr;
    HWND logToggle = nullptr, logScopeCombo = nullptr, logOpen = nullptr, logReset = nullptr;
    HWND symbolsToggle = nullptr;
    Row* privateHotkeyRow = nullptr;
    Row* learnRow = nullptr;
    Row* logRow = nullptr;
    // ユーザ辞書
    HWND dictList = nullptr, dictReading = nullptr, dictWord = nullptr, dictAdd = nullptr, dictRemove = nullptr;
    // 専門辞書
    HWND termList = nullptr, termAdd = nullptr, termRemove = nullptr, termDetail = nullptr;
    Row* termRowStatus = nullptr;
    std::unique_ptr<iroha::TermDictionaryCatalog> termCatalog;
    std::vector<TermRow> termRows;
    bool termBusy = false;
    bool termCatalogRequested = false;
    // 変換ルール
    HWND ruleList = nullptr, ruleTrigger = nullptr, ruleOutput = nullptr, ruleAdd = nullptr, ruleRemove = nullptr;
    std::vector<iroha::UserRewriteRule> rules;
    bool ruleRefreshing = false;
    // 選択テキスト
    HWND selToggle = nullptr, selTrigger = nullptr, selCount = nullptr, selOnDemand = nullptr, selExcluded = nullptr;
    HWND selPresetEnabled[5] = {}, selPresetName[5] = {}, selPresetHotkey[5] = {}, selPresetPrompt[5] = {};
    Row* selStatusRow = nullptr;
    // AI
    HWND aiService = nullptr, aiEndpoint = nullptr, aiModel = nullptr, aiModels = nullptr;
    HWND aiKey = nullptr, aiKeySave = nullptr, aiKeyClear = nullptr;
    HWND aiPresetName[3] = {}, aiPresetHotkey[3] = {}, aiPresetPrompt[3] = {}, aiPresetReset[3] = {};
    Row* aiServiceRow = nullptr;
    Row* aiKeyRow = nullptr;
    Row* aiPresetStatusRow = nullptr;
    int aiShownService = -1;
    // モデル
    HWND modelCombo = nullptr, modelDownload = nullptr, modelApply = nullptr;
    HWND modelFile = nullptr, modelFileDefault = nullptr, modelFolder = nullptr;
    HWND adapterFile = nullptr, adapterClear = nullptr;
    Row* modelRow = nullptr;
    Row* modelPathRow = nullptr;
    Row* adapterRow = nullptr;
    bool downloading = false;
    // 情報
    HWND gitHub = nullptr, dataFolder = nullptr, developerToggle = nullptr;
    HWND autoUpdateToggle = nullptr, checkUpdate = nullptr, developerOverlayToggle = nullptr;
    Row* licenseTypoRows[16] = {};
    std::vector<Row*> licenseRows;

    std::unique_ptr<iroha::UserDictionaryStore> dictionary;
    std::unique_ptr<iroha::LearningStore> learning;
    std::unique_ptr<iroha::UserRewriteRuleStore> rewriteRules;
    std::unique_ptr<iroha::InputHistoryStore> inputHistory;
    std::unique_ptr<iroha::ConversionLog> conversionLog;

    int S(int value) const { return g_theme.S(value); }
    ui::Page* PageFor(const wchar_t* key) {
        for (auto& entry : nav) {
            if (wcscmp(entry.key, key) == 0) return entry.page.get();
        }
        return nullptr;
    }

    void Build();
    void BuildInputPage();
    void BuildLearningPage();
    void BuildDictionaryPage();
    void BuildTermPage();
    void BuildRulePage();
    void BuildSelectionPage();
    void BuildAIPage();
    void BuildModelPage();
    void BuildAboutPage();
    void Layout();
    void PaintNav(HDC dc);
    void ShowPage(int index);
    void RelayoutAll();
    void OnDpiChanged();

    // 設定を変えて保存し、変換サーバに読み直してもらう
    void Update(const std::function<void(iroha::Config&)>& change, bool reload = true);
    void Refresh();
    void OnCommand(int id, int code, HWND control);
    void OnSlider(HWND control, bool finished);

    // 打ち間違いの訂正
    void RefreshTypoStatus();
    void StartTypoTask(bool announce);
    void OnTypoTaskDone(TypoTaskResult* result);
    void OnTypoLicense();
    void OnTypoRemove();
    // 学習・記録
    void RefreshLearningCounts();
    void OnHistoryReset();
    void OnLearnReset();
    void OnLogReset();
    // ショートカットの欄を確かめて保存する（読めなければ説明を注意の色にして保存しない）
    bool SaveShortcut(HWND edit, ui::Page* page, Row* row, const std::wstring& normal,
                      const std::function<void(iroha::Config&, const std::string&)>& store);
    // ユーザ辞書・専門辞書・変換ルール
    void RefreshDictionaryList();
    void OnDictAdd();
    void OnDictRemove();
    void RefreshTermList();
    void ShowTermDetail();
    void StartTermTask(std::wstring busyMessage, bool fetchCatalog, std::string installId, std::string removeId);
    void OnTermTaskDone(TermTaskResult* result);
    void RefreshRuleList();
    void OnRuleAdd();
    void OnRuleRemove();
    void OnRuleChecked(int row);
    // 選択テキスト
    void SaveSelectionTexts();
    // AI
    void ShowAIServiceFields(int index);
    void StoreAIServiceFields(int index, iroha::Config* config);
    void RefreshAIKeyStatus(iroha::AIService service);
    void OnAIServiceChanged();
    void SaveAIPresets();
    void OnAIKeySave();
    void OnAIKeyClear();
    void OnAIListModels();
    void OnAIModelsDone(AIModelsResult* result);
    // モデル
    void RefreshModelPage();
    void OnModelDownload();
    void OnModelApply();
    void OnChooseFile(bool adapter);
    // 情報
    void RefreshLicenses();
};

App g_app;

// ---- 部品を作る ----

HWND MakeToggle(ui::Page* page, int id) { return ui::CreateToggle(page->hwnd(), id); }

HWND MakeCombo(ui::Page* page, int id, std::initializer_list<const wchar_t*> items) {
    HWND combo = ui::CreateControl(page->hwnd(), WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, id);
    for (const wchar_t* item : items) SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    return combo;
}

HWND MakeEdit(ui::Page* page, int id, const wchar_t* cue = L"", DWORD style = ES_AUTOHSCROLL) {
    HWND edit = ui::CreateControl(page->hwnd(), WC_EDITW, L"", WS_TABSTOP | style, id, WS_EX_CLIENTEDGE);
    if (cue && *cue) SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue));
    return edit;
}

HWND MakeButton(ui::Page* page, int id, const wchar_t* text) {
    return ui::CreateControl(page->hwnd(), WC_BUTTONW, text, WS_TABSTOP, id);
}

HWND MakeSlider(ui::Page* page, int id, int minimum, int maximum) {
    HWND slider = ui::CreateControl(page->hwnd(), TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS, id);
    SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(minimum, maximum));
    return slider;
}

HWND MakeList(ui::Page* page, int id, std::initializer_list<std::pair<const wchar_t*, int>> columns, DWORD extra = 0) {
    HWND list = ui::CreateControl(page->hwnd(), WC_LISTVIEWW, L"",
                                  LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, id);
    SetWindowTheme(list, L"Explorer", nullptr);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | extra);
    ListView_SetBkColor(list, g_theme.card);
    ListView_SetTextBkColor(list, g_theme.card);
    LVCOLUMNW column = {};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    int index = 0;
    for (const auto& [title, width] : columns) {
        column.cx = g_theme.S(width);
        column.pszText = const_cast<wchar_t*>(title);
        ListView_InsertColumn(list, index++, &column);
    }
    return list;
}

void SetCheck(HWND control, bool checked) { SendMessageW(control, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0); }
bool Checked(HWND control) { return SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED; }

// ---- 全体 ----

void App::Build() {
    nav.push_back({L"input", L"\xE765", L"入力", nullptr});
    nav.push_back({L"learning", L"\xE8F1", L"辞書・学習", nullptr});
    nav.push_back({L"dictionary", L"\xE82D", L"ユーザ辞書", nullptr});
    nav.push_back({L"terms", L"\xE736", L"専門辞書", nullptr});
    nav.push_back({L"rules", L"\xE8FD", L"変換ルール", nullptr});
    nav.push_back({L"selection", L"\xE8B3", L"選択テキスト", nullptr});
    nav.push_back({L"ai", L"\xE945", L"AI", nullptr});
    nav.push_back({L"model", L"\xE950", L"モデル", nullptr});
    nav.push_back({L"about", L"\xE946", L"情報", nullptr});
    for (auto& entry : nav) entry.page = std::make_unique<ui::Page>(window, entry.title);
    config = iroha::LoadConfig(ConfigPath());
    developerMode = config.developerMode;
    BuildInputPage();
    BuildLearningPage();
    BuildDictionaryPage();
    BuildTermPage();
    BuildRulePage();
    BuildSelectionPage();
    BuildAIPage();
    BuildModelPage();
    BuildAboutPage();
}

void App::BuildInputPage() {
    ui::Page* page = PageFor(L"input");
    const auto dev = [this] { return developerMode; };
    const auto typoOn = [this] { return config.typoCorrection && !typoBusy; };

    page->AddHeader(L"変換");
    liveToggle = MakeToggle(page, kIdLive);
    page->AddItem(L"ライブ変換", L"入力中の読みをその場で変換して見せます。OFF なら Space を押すまでかなのままです")
        ->right = {{liveToggle, 110}};
    countCombo = ui::CreateControl(page->hwnd(), WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
                                   kIdCandidateCount);
    for (int count = kMinCandidateCount; count <= kMaxCandidateCount; ++count) {
        SendMessageW(countCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(std::to_wstring(count).c_str()));
    }
    page->AddItem(L"候補ウィンドウでモデルが並べる候補数", L"",
                  L"この数の下に、読みが一致する辞書の残りの候補（単漢字・異体字・人名など）が続きます。")
        ->right = {{countCombo, 90, 32, 240}};
    contextToggle = MakeToggle(page, kIdDocumentContext);
    Row* context = page->AddItem(
        L"アプリの文章を文脈に使う", L"",
        L"入力を始めた位置の手前にある文章（同じ行の最大 40 文字）をアプリから読み取り、変換の文脈にします。"
        L"文章の途中に書き足すときや、別のアプリに移った直後でも前後に合った変換になります。"
        L"文章を返さないアプリ・OFF のときは、iroha で直前に確定した文字列を文脈にします。",
        /*developer=*/true);
    context->right = {{contextToggle, 110}};
    context->visible = dev;

    page->AddHeader(L"打ち間違いの訂正",
                    L"入力の手が止まったとき、読みの打ち間違い（隣のキー・抜け・重複・入れ替え・「っ」の過不足）を直してから"
                    L"変換します。直したときは何をどう直したかをカーソルの下に表示し、そのまま Backspace を押すと打ったとおりの"
                    L"読みに戻せます。休止を待たずに Space を押したときは、読みは変えずに候補ウィンドウに訂正を足します。"
                    L"訂正に使うモデルはアプリに含まれていないので、ON にしたときに 1 回だけダウンロードします。");
    typoToggle = MakeToggle(page, kIdTypo);
    typoRow = page->AddItem(L"打ち間違いを自動で直す", L"");
    typoRow->right = {{typoToggle, 110}};
    typoLicense = MakeButton(page, kIdTypoLicense, L"学習元とライセンス");
    typoRemove = MakeButton(page, kIdTypoRemove, L"モデルを削除");
    typoModelRow = page->AddItem(L"訂正モデル", L"", L"", /*developer=*/true);
    typoModelRow->right = {{typoLicense, 150}, {typoRemove, 120}};
    typoModelRow->visible = dev;
    typoDelay = MakeSlider(page, kIdTypoDelay, 2, 40); // 100〜2000ms を 50ms 刻み
    typoDelayRow = page->AddItem(L"訂正するまでの休止時間", L"",
                                 L"キーを離してからこの時間だけ何も押さなければ訂正します。"
                                 L"短いほど早く直りますが、語の途中で考えているだけのときにも動きます。",
                                 /*developer=*/true);
    typoDelayRow->right = {{typoDelay, 220}};
    typoDelayRow->visible = dev;
    typoDelayRow->enabled = typoOn;
    typoMinLength = ui::CreateControl(page->hwnd(), WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
                                      kIdTypoMinLength);
    for (int length = 1; length <= 20; ++length) {
        SendMessageW(typoMinLength, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>((std::to_wstring(length) + L" 文字").c_str()));
    }
    Row* minLength = page->AddItem(L"訂正する読みの最低文字数", L"",
                                   L"読みがこの文字数に満たないときは直しません。短い読みは正しく打っていても別の語の打ち間違いに"
                                   L"見えやすいためです（例:「さど」が「さいど」に直る）。",
                                   /*developer=*/true);
    minLength->right = {{typoMinLength, 100, 32, 300}};
    minLength->visible = dev;
    minLength->enabled = typoOn;
    typoThreshold = MakeSlider(page, kIdTypoThreshold, 0, 16); // 0〜8 を 0.5 刻み
    typoThresholdDefault = MakeButton(page, kIdTypoThresholdDefault, L"既定");
    typoThresholdRow = page->AddItem(L"訂正を出す確信の強さ", L"",
                                     L"大きくするほど、モデルがよほど確信したときしか直しません。"
                                     L"小さくすると打ち間違いをよく拾いますが、正しく打った読みも直してしまいます。",
                                     /*developer=*/true);
    typoThresholdRow->right = {{typoThreshold, 220}, {typoThresholdDefault, 64}};
    typoThresholdRow->visible = dev;
    typoThresholdRow->enabled = typoOn;

    page->AddHeader(L"予測変換");
    predictiveToggle = MakeToggle(page, kIdPredictive);
    page->AddItem(L"入力履歴から予測する",
                  L"読みの先頭が一致する語を、これまでに確定した語から出します",
                  L"入力中、読みの先頭が一致する語を、これまでに確定した語（入力履歴）からカーソルの下に表示します。"
                  L"読みを 2 文字打つと出ます。Tab で選び（押すたびに次の候補、Shift+Tab で前の候補）、選んだまま続きを打つと"
                  L"その候補が入ります。Enter で入れて確定、Esc で選ぶのをやめます。入れた部分は Backspace で打った読みに"
                  L"戻せます。入力履歴はこの設定が ON の間だけ、ローマ字で入力して確定した語を記録します。"
                  L"記録はデータフォルダの input-history\\ にコンピュータごとのファイルとして残ります。")
        ->right = {{predictiveToggle, 110}};
    historyReset = MakeButton(page, kIdHistoryReset, L"消去");
    historyRow = page->AddItem(L"入力履歴", L"");
    historyRow->right = {{historyReset, 90}};

    page->AddHeader(L"句読点・スペース");
    punctuationCombo = MakeCombo(page, kIdPunctuation, {kPunctuations[0], kPunctuations[1]});
    page->AddItem(L"句読点", L"Ctrl+. でも切り替えられます", L"変更は次の入力から反映されます。")
        ->right = {{punctuationCombo, 90, 32, 120}};
    spaceToggle = MakeToggle(page, kIdFullWidthSpace);
    page->AddItem(L"入力していないときのスペースを全角にする", L"Shift+Space は半角です",
                  L"OFF にすると、かなモードで入力していないときのスペースも半角になります。"
                  L"入力中のスペースは変換に使うので、この設定には関係しません。英数モードでは常に半角です。")
        ->right = {{spaceToggle, 110}};

    page->AddHeader(L"「こえ」モード",
                    L"試作中の機能です。音声で話した内容を、キーボードで打った読みと同じようにかな漢字変換へ通します。"
                    L"音声認識の出した表記から読みを求め直すので、読みを取り違えることがあります（人名、「何時」をいつと読む、"
                    L"など）。そのときは Space の候補やキーボードで直します。");
    voiceToggle = MakeToggle(page, kIdVoice);
    voiceRow = page->AddItem(
        L"「こえ」モードを使う", L"",
        L"ON の間、下で決めたキーを押している間だけマイクで録音し、キーを離すと、話した内容を入力中の文字（未確定文字列）に"
        L"入れます。確定はしません。話した内容は読み（ひらがな）に戻してから iroha のかな漢字変換に通すので、キーボードで"
        L"打ったときと同じように、Space で候補を選ぶ・続きをキーボードで打つ・Enter で確定する、ができます。"
        L"英字・数字は読みに戻さず、音声認識が出した表記のまま入ります。話している間は、途中結果が入力中の文字として"
        L"見えます。キーを押してすぐ離したとき、押している間にほかのキーを使ったとき（ショートカット）は録音を捨てます。"
        L"音声認識は Windows の音声認識（SAPI）がこのコンピュータの中で行い、音声を外部に送りません。"
        L"声で入れた文は、変換の学習・変換記録・入力履歴に残しません。");
    voiceRow->right = {{voiceToggle, 110}};
    voiceKeyCombo = MakeCombo(page, kIdVoiceKey, {L"右 Ctrl", L"右 Shift"});
    Row* voiceKey = page->AddItem(L"押している間だけ録音するキー", L"",
                                  L"このキーだけを押し続けている間、録音します。ほかのキーと一緒に押したとき（右 Ctrl+C など）は"
                                  L"録音しません。キーはアプリにもそのまま届きます。");
    voiceKey->right = {{voiceKeyCombo, 120, 32, 120}};
    voiceKey->enabled = [this] { return config.voiceInput; };

    page->AddHeader(L"区切り記号で英字を入力",
                    L"かな入力のまま、開始の記号を打つと英字入力になり、終わりの記号を打つまで打ったとおりの英字が入ります"
                    L"（例: kyouha_iPhone_wotukau →「今日はiPhoneを使う」）。大文字・小文字は Shift で打ち分けます。"
                    L"英字の中にスペースを入れるときは Shift+Space を押します。英字は確定されずに入力の途中に残るので、"
                    L"英数モードに切り替えずに 1 回の Enter で確定できます。記号そのものを入れるときは、開始の記号のあとすぐに"
                    L"終わりの記号を打ちます（既定なら「__」）。",
                    /*developer=*/true)
        ->visible = dev;
    delimitedToggle = MakeToggle(page, kIdDelimited);
    Row* delimited = page->AddItem(L"記号で囲んだ部分を英字で入力する", L"例: kyouha_iPhone_wo → 今日はiPhoneを");
    delimited->right = {{delimitedToggle, 110}};
    delimited->visible = dev;
    delimitedStart = MakeEdit(page, kIdDelimitedStart);
    delimitedStartRow = page->AddItem(L"開始の記号", L"英字・数字以外の ASCII の記号 1 文字");
    delimitedStartRow->right = {{delimitedStart, 60, 30}};
    delimitedStartRow->visible = dev;
    delimitedStartRow->enabled = [this] { return config.delimitedAlphabet; };
    delimitedEnd = MakeEdit(page, kIdDelimitedEnd);
    delimitedEndRow = page->AddItem(L"終わりの記号", L"英字・数字以外の ASCII の記号 1 文字");
    delimitedEndRow->right = {{delimitedEnd, 60, 30}};
    delimitedEndRow->visible = dev;
    delimitedEndRow->enabled = [this] { return config.delimitedAlphabet; };

    page->AddHeader(L"AIで直す", L"", /*developer=*/true)->visible = dev;
    aiFixToggle = MakeToggle(page, kIdAIFix);
    Row* aiFix = page->AddItem(
        L"誤変換の疑いがあるとき「AIで直す」を出す", L"",
        L"ライブ変換の結果に誤変換の疑いがあると、入力が止まったときにカーソルの下に「AIで直す」を出します。"
        L"ショートカットを押すと、前の文脈・読み・変換結果を AI変換と同じ AI サービス（「AI」のページで選ぶ）に送り、"
        L"返ってきた案に未確定文字列を差し替えます。直後に Backspace を押すと元の変換に戻ります。"
        L"AI に送るのは押したときだけです。AI の案で確定した文は、学習にも変換記録にも残しません。"
        L"疑いの判定には、モデルが変換したときの同音異義語どうしの確率の差を使います。学習やユーザ辞書で差し替わった"
        L"変換、固定した部分や英字を含む入力、80 文字を超える読みでは出しません。プライベートモードの間は働きません。");
    aiFix->right = {{aiFixToggle, 110}};
    aiFix->visible = dev;
    aiFixThreshold = MakeSlider(page, kIdAIFixThreshold, 1, 12); // 0.25〜3 を 0.25 刻み
    aiFixThresholdRow = page->AddItem(L"閾値", L"",
                                      L"同音異義語どうしの確率の差（自然対数）の最小値がこの値より小さい変換を疑います。"
                                      L"大きくするほど多くの変換に「AIで直す」が出ます。測定では、0.5 で文の 15〜18%、"
                                      L"1.0 で 28〜30%、2.0 で 50〜54% に出ました。");
    aiFixThresholdRow->right = {{aiFixThreshold, 220}};
    aiFixThresholdRow->visible = dev;
    aiFixThresholdRow->enabled = [this] { return config.aiFix; };
    aiFixShortcut = MakeEdit(page, kIdAIFixShortcut, L"例: Ctrl+Alt+A");
    aiFixShortcutRow = page->AddItem(L"ショートカット", L"",
                                     L"「AIで直す」が出ていなくても、入力中ならいつでも今の変換を AI に送ります。"
                                     L"「Ctrl+Alt+A」のように、Ctrl・Alt・Shift とキーを + でつないで書きます。空にすると使いません。");
    aiFixShortcutRow->right = {{aiFixShortcut, 180, 30}};
    aiFixShortcutRow->visible = dev;
    aiFixShortcutRow->enabled = [this] { return config.aiFix; };
}

void App::BuildLearningPage() {
    ui::Page* page = PageFor(L"learning");
    const auto dev = [this] { return developerMode; };
    page->AddHeader(L"プライベートモード");
    privateToggle = MakeToggle(page, kIdPrivateMode);
    page->AddItem(L"プライベートモードを使う", L"学習・ユーザ辞書・変換記録・入力履歴と予測変換を止めます",
                  L"ON の間は、入力した内容を残さず、これまでの入力に基づく候補も出しません。止めるのは、変換の学習"
                  L"（覚えることも、覚えた変換に差し替えることもしません）・ユーザ辞書・変換記録・入力履歴の記録と予測変換です。"
                  L"変換は iroha 本来の結果になります。それぞれの設定は変えないので、OFF に戻すと元の動作になります。"
                  L"変換ルールはそのまま使います。AI変換・選択テキストの AI 編集は、操作したときにだけ文章を AI サービスに"
                  L"送るもので、止めません。ON の間は、言語バーの「あ」に下線を引きます。")
        ->right = {{privateToggle, 110}};
    privateHotkey = MakeEdit(page, kIdPrivateHotkey, L"例: Ctrl+Alt+P");
    privateHotkeyRow = page->AddItem(L"ショートカット", L"",
                                     L"「Ctrl+Alt+P」のように、Ctrl・Alt・Shift とキーを + でつないで書きます。iroha で入力して"
                                     L"いるときに効きます（入力中かどうかによりません）。切り替えると、ON/OFF のどちらになったかを"
                                     L"カーソルの下に表示します。空にすると割り当てなしになります。");
    privateHotkeyRow->right = {{privateHotkey, 180, 30}};

    page->AddHeader(L"変換の学習");
    learningToggle = MakeToggle(page, kIdLearning);
    page->AddItem(L"変換の修正を学習する", L"選び直して確定した変換を覚えて、次から使います",
                  L"文節変換（Space）で候補を選び直して確定すると、入力した読みの全体・確定した文字列・直す前の変換結果を"
                  L"覚えます。次に同じ読みを入力して、変換結果が直す前と同じになったときに、覚えた文字列に差し替えます"
                  L"（「きしゃ」を「貴社」に直すと、次から「きしゃ」はそう変換されます）。前の文章によって変換結果が"
                  L"変わったときは差し替えません。読みの一部には当てはめません。")
        ->right = {{learningToggle, 110}};
    learnOpen = MakeButton(page, kIdLearnOpen, L"フォルダを開く");
    learnReset = MakeButton(page, kIdLearnReset, L"リセット");
    learnRow = page->AddItem(L"学習した変換", L"");
    learnRow->right = {{learnOpen, 130}, {learnReset, 90}};

    page->AddHeader(L"変換記録",
                    L"確定した変換を、そのときモデルに渡した文脈（カーソル手前の文章の末尾 40 文字）・読み・モデルの出力・"
                    L"確定した文字列とともに 1 件ずつ記録します。左文脈のない確定は記録しません。記録はデータフォルダの "
                    L"logs\\conversions\\ にこのコンピュータのファイルとして残るだけで、どこにも送信されません。"
                    L"あとでこの記録を使って、自分の入力に合わせた変換モデルの追加学習ができます。");
    logToggle = MakeToggle(page, kIdLogEnabled);
    page->AddItem(L"確定した変換を記録する", L"追加学習に使うデータです。カーソルの手前の文章も残ります")
        ->right = {{logToggle, 110}};
    logScopeCombo = MakeCombo(page, kIdLogScope, {L"すべての確定", L"直した確定だけ"});
    Row* scope = page->AddItem(L"記録する範囲", L"");
    scope->right = {{logScopeCombo, 160, 32, 120}};
    scope->enabled = [this] { return config.conversionLog; };
    logOpen = MakeButton(page, kIdLogOpen, L"フォルダを開く");
    logReset = MakeButton(page, kIdLogReset, L"削除");
    logRow = page->AddItem(L"記録したデータ", L"");
    logRow->right = {{logOpen, 130}, {logReset, 90}};

    page->AddHeader(L"記号・顔文字・絵文字", L"", /*developer=*/true)->visible = dev;
    symbolsToggle = MakeToggle(page, kIdSymbols);
    Row* symbols = page->AddItem(L"記号・顔文字・絵文字を候補に出す", L"",
                                 L"文節の読みに合う記号・顔文字・絵文字を候補ウィンドウに加えます（「やじるし」→ →、"
                                 L"「にこにこ」→ (^^)、「わらい」→ 😂）。絵文字は上から 7 番目に、顔文字と記号は辞書の候補の"
                                 L"後ろに並べます。「えもじ」ですべての絵文字、「かおもじ」ですべての顔文字を出します。データは"
                                 L"Google 日本語入力のオープンソース版 Mozc のものです。Windows で描けない絵文字は出しません。"
                                 L"ライブ変換には使わず、選んで確定しても学習しません。");
    symbols->right = {{symbolsToggle, 110}};
    symbols->visible = dev;
}

void App::BuildDictionaryPage() {
    ui::Page* page = PageFor(L"dictionary");
    page->AddNote(L"登録した語は、読みが一致したときに変換に使います。読みの一部に一致したときも、語の区切りに合えば当てはめます。");
    dictReading = MakeEdit(page, kIdDictReading, L"読み（ひらがな）");
    dictWord = MakeEdit(page, kIdDictWord, L"単語");
    dictAdd = MakeButton(page, kIdDictAdd, L"追加");
    dictRemove = MakeButton(page, kIdDictRemove, L"選んだ語を削除");
    Row* add = page->AddItem(L"語を追加", L"");
    add->below = {{{dictReading, 180, 30}, {dictWord, 220, 30}, {dictAdd, 80}, {dictRemove, 140}}};
    dictList = MakeList(page, kIdDictList, {{L"読み", 180}, {L"単語", 240}, {L"出所", 90}});
    page->AddFill(dictList, 200);
}

void App::BuildTermPage() {
    ui::Page* page = PageFor(L"terms");
    page->AddNote(L"分野の用語や地名・人名の辞書を追加すると、その語を文節の候補ウィンドウに出します（ライブ変換は変えません）。"
                  L"辞書はダウンロードしてデータフォルダの dictionaries\\ に置きます。出典とライセンスは一覧で選ぶと出ます。");
    termList = MakeList(page, kIdTermList, {{L"辞書", 180}, {L"まとまり", 170}, {L"語数", 80}, {L"状態", 150}});
    page->AddFill(termList, 180);
    termAdd = MakeButton(page, kIdTermAdd, L"追加");
    termRemove = MakeButton(page, kIdTermRemove, L"削除");
    termRowStatus = page->AddItem(L"選んだ辞書", L"");
    termRowStatus->right = {{termAdd, 90}, {termRemove, 90}};
    // 出典・ライセンスの URL を写せるよう、読み取り専用の入力欄に出す
    termDetail = ui::CreateControl(page->hwnd(), WC_EDITW, L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
                                   kIdTermDetail);
    Row* detail = page->AddItem(L"", L"");
    detail->below = {{{termDetail, -1, 130}}};
}

void App::BuildRulePage() {
    ui::Page* page = PageFor(L"rules");
    page->AddNote(L"読みを打って Space を押したときの候補に、出力を加えます（ライブ変換は変えません）。チェックを外したルールは使いません。"
                  L"出力に書けるもの: {{date}}（今日の日付）・{{date+1}}（明日）・{{time:HH:mm}}・{{datetime}}・{{wareki}}（和暦）。"
                  L"日付の書式は {{date:yyyy年M月d日（E）}} のように指定します。");
    ruleTrigger = MakeEdit(page, kIdRuleTrigger, L"読み");
    ruleOutput = MakeEdit(page, kIdRuleOutput, L"出力（例: {{date:yyyy/MM/dd}}）");
    ruleAdd = MakeButton(page, kIdRuleAdd, L"追加");
    ruleRemove = MakeButton(page, kIdRuleRemove, L"選んだルールを削除");
    Row* add = page->AddItem(L"ルールを追加", L"");
    add->below = {{{ruleTrigger, 150, 30}, {ruleOutput, 260, 30}, {ruleAdd, 80}, {ruleRemove, 160}}};
    ruleList = MakeList(page, kIdRuleList, {{L"読み", 150}, {L"出力", 240}, {L"いま出る候補", 200}}, LVS_EX_CHECKBOXES);
    page->AddFill(ruleList, 200);
}

void App::BuildSelectionPage() {
    ui::Page* page = PageFor(L"selection");
    const auto dev = [this] { return developerMode; };
    const auto enabled = [this] { return config.selectionEnabled; };
    page->AddHeader(L"選択テキストのAI編集", L"処理に使う AI サービス（Ollama・Claude など）は「AI」のページで設定します。");
    selToggle = MakeToggle(page, kIdSelEnabled);
    page->AddItem(L"選択テキストのAI編集を有効にする", L"どのアプリでも、選んだ文章を AI で処理して置き換えます",
                  L"ほかのアプリで選んだ文章を、ショートカットかマウス操作から AI で処理して置き換えます。ショートカットで"
                  L"使うときは、選んでいる文章をクリップボード経由（Ctrl+C）で受け取り、結果は Ctrl+V で貼って、"
                  L"クリップボードの中身は元に戻します。入力中（未確定文字列があるとき）は動きません。"
                  L"iroha の変換サーバが動いている間だけ効きます。")
        ->right = {{selToggle, 110}};

    page->AddHeader(L"マウスで選択したとき");
    selTrigger = MakeCombo(page, kIdSelTrigger, {kTriggerLabels[0], kTriggerLabels[1], kTriggerLabels[2], kTriggerLabels[3]});
    Row* trigger = page->AddItem(L"選んだときの動き", L"",
                                 L"文章をドラッグかダブルクリックで選んだときに、マウスの近くにアイコンかメニューを出します。"
                                 L"選んだ文章は Windows のアクセシビリティの仕組み（UI Automation）で読みます。読めないアプリでは、"
                                 L"入力欄・文書の上で選んだときだけアイコンを出し、メニューを選んだ時点で Ctrl+C で受け取ります。");
    trigger->right = {{selTrigger, 300, 32, 160}};
    trigger->enabled = enabled;
    selCount = MakeToggle(page, kIdSelCount);
    page->AddItem(L"選択した文字数を表示する", L"選ぶと文字数を数秒出します（AI 編集が OFF でも使えます）",
                  L"マウスで選択（ドラッグ・ダブルクリック）すると、選択範囲の近くに文字数を数秒表示します（改行は数えず、"
                  L"空白があれば空白を除いた数も併記）。かな・漢字を含まない英語の文では、単語数と文字数を「152 words "
                  L"(812 chars)」のように出します。AI 編集が ON のときはアイコンに添えて表示します。キーボードでの選択には"
                  L"反応しません。")
        ->right = {{selCount, 110}};

    page->AddHeader(L"その場でAIに指示", L"選択テキストに自由な指示を出せます（選択なしで押すとテキスト生成になります）。");
    selOnDemand = MakeEdit(page, kIdSelOnDemand, L"例: Ctrl+Alt+0");
    Row* onDemand = page->AddItem(L"ショートカット", L"");
    onDemand->right = {{selOnDemand, 180, 30}};
    onDemand->enabled = enabled;

    page->AddHeader(L"プリセット",
                    L"プリセットのショートカットは、テキストを選択していないときに押すと「テキストを生成」の入力欄になり、"
                    L"結果をカーソル位置へ入れます。プロンプトに {text} と書くとその位置に選択テキストが入ります"
                    L"（無ければプロンプトに続けて渡されます）。");
    for (int i = 0; i < 5; ++i) {
        selPresetEnabled[i] = MakeToggle(page, kIdSelPresetEnabled0 + i);
        selPresetName[i] = MakeEdit(page, kIdSelPresetName0 + i, L"名前");
        selPresetHotkey[i] = MakeEdit(page, kIdSelPresetHotkey0 + i, L"ショートカット");
        selPresetPrompt[i] = MakeEdit(page, kIdSelPresetPrompt0 + i, L"プロンプト",
                                      ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL);
        Row* preset = page->AddItem(L"プリセット " + std::to_wstring(i + 1), L"");
        preset->right = {{selPresetEnabled[i], 110}};
        preset->below = {{{selPresetName[i], 200, 30}, {selPresetHotkey[i], 180, 30}}, {{selPresetPrompt[i], -1, 56}}};
        preset->enabled = enabled;
    }
    selStatusRow = page->AddNote(L"");

    page->AddHeader(L"除外するアプリ", L"ここに書いたアプリでは、選択テキストの AI 編集を使いません（ショートカットもマウスも）。",
                    /*developer=*/true)
        ->visible = dev;
    selExcluded = MakeEdit(page, kIdSelExcluded, L"例: notepad.exe, code.exe");
    Row* excluded = page->AddItem(L"使わないアプリ", L"実行ファイルの名前をカンマか改行で区切ります");
    excluded->below = {{{selExcluded, -1, 30}}};
    excluded->visible = dev;
}

void App::BuildAIPage() {
    ui::Page* page = PageFor(L"ai");
    page->AddHeader(L"AIサービス",
                    L"AI変換・AIで直す・選択テキストの AI 編集が共通で使う接続先です。Ollama・LM Studio はこのコンピュータで"
                    L"動くサーバに送ります。OpenAI 互換・Claude では、入力した文章が外部のサービスに送られます。API キーは"
                    L"config.json に書かず、Windows の資格情報マネージャーに置きます。");
    std::vector<std::wstring> names;
    aiService = ui::CreateControl(page->hwnd(), WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, kIdAIService);
    for (iroha::AIService service : kAIServices) {
        SendMessageW(aiService, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Wide(iroha::AIServiceDisplayName(service)).c_str()));
    }
    aiServiceRow = page->AddItem(L"サービス", L"");
    aiServiceRow->right = {{aiService, 240, 32, 200}};
    aiEndpoint = MakeEdit(page, kIdAIEndpoint);
    Row* endpoint = page->AddItem(L"URL", L"空なら既定の URL を使います");
    endpoint->below = {{{aiEndpoint, -1, 30}}};
    endpoint->visible = [this] {
        const auto service = iroha::ParseAIService(config.aiService);
        return service == iroha::AIService::Ollama || service == iroha::AIService::LMStudio || service == iroha::AIService::OpenAI;
    };
    aiKey = MakeEdit(page, kIdAIKey, L"新しいキーを入れて「保存」", ES_AUTOHSCROLL | ES_PASSWORD);
    aiKeySave = MakeButton(page, kIdAIKeySave, L"保存");
    aiKeyClear = MakeButton(page, kIdAIKeyClear, L"キーを消す");
    aiKeyRow = page->AddItem(L"API キー", L"");
    aiKeyRow->below = {{{aiKey, 320, 30}, {aiKeySave, 80}, {aiKeyClear, 110}}};
    aiKeyRow->visible = [this] {
        const auto service = iroha::ParseAIService(config.aiService);
        return service == iroha::AIService::OpenAI || service == iroha::AIService::Claude;
    };
    aiModel = ui::CreateControl(page->hwnd(), WC_COMBOBOXW, L"", CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_TABSTOP | WS_VSCROLL,
                                kIdAIModel);
    aiModels = MakeButton(page, kIdAIModels, L"一覧を取得");
    Row* model = page->AddItem(L"モデル", L"");
    model->right = {{aiModel, 260, 32, 260}, {aiModels, 110}};
    model->visible = [this] { return iroha::ParseAIService(config.aiService) != iroha::AIService::None; };

    page->AddHeader(L"AI変換",
                    L"入力中にプリセットのキー（既定は英訳の Shift+Space）で、未確定文字列を AI に渡します。結果が日本語なら"
                    L"文節に区切った未確定文字列で返し、もう一度 Enter で確定します。英訳など日本語を含まない結果はそのまま"
                    L"確定します。Esc で取り消します。ショートカットは「Alt+Enter」「Ctrl+Shift+T」のように、Ctrl・Alt・Shift と"
                    L"キーを + でつないで書きます。プロンプトに {text} と書くとその位置に未確定文字列が入ります。");
    for (int i = 0; i < 3; ++i) {
        aiPresetName[i] = MakeEdit(page, kIdAIPresetName0 + i, L"名前");
        aiPresetHotkey[i] = MakeEdit(page, kIdAIPresetHotkey0 + i, L"ショートカット");
        aiPresetReset[i] = MakeButton(page, kIdAIPresetReset0 + i, L"既定に戻す");
        aiPresetPrompt[i] = MakeEdit(page, kIdAIPresetPrompt0 + i, L"プロンプト",
                                     ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL);
        Row* preset = page->AddItem(L"プリセット " + std::to_wstring(i + 1), L"");
        preset->below = {{{aiPresetName[i], 200, 30}, {aiPresetHotkey[i], 180, 30}, {aiPresetReset[i], 110}},
                         {{aiPresetPrompt[i], -1, 64}}};
    }
    aiPresetStatusRow = page->AddNote(L"");
}

void App::BuildModelPage() {
    ui::Page* page = PageFor(L"model");
    const auto dev = [this] { return developerMode; };
    page->AddHeader(L"かな漢字変換モデル");
    modelCombo = ui::CreateControl(page->hwnd(), WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, kIdModelCombo);
    for (const KnownModel& model : kKnownModels) {
        SendMessageW(modelCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(model.label));
    }
    modelDownload = MakeButton(page, kIdModelDownload, L"ダウンロード");
    modelApply = MakeButton(page, kIdModelApply, L"このモデルを使う");
    modelRow = page->AddItem(L"使うモデル", L"");
    modelRow->right = {{modelCombo, 340, 32, 160}};
    modelRow->below = {{{modelDownload, 130}, {modelApply, 150}}};

    modelFile = MakeButton(page, kIdModelFile, L"ファイルを選択...");
    modelFileDefault = MakeButton(page, kIdModelFileDefault, L"既定に戻す");
    modelFolder = MakeButton(page, kIdModelFolder, L"モデルフォルダを開く");
    modelPathRow = page->AddItem(L"モデルファイル（GGUF）のパス", L"",
                                 L"一覧に無いモデル（自分で変換した GGUF など）を使うときに指定します。変えると変換サーバが"
                                 L"モデルを読み直します（数秒かかります）。",
                                 /*developer=*/true);
    modelPathRow->below = {{{modelFile, 140}, {modelFileDefault, 110}, {modelFolder, 170}}};
    modelPathRow->visible = dev;

    adapterFile = MakeButton(page, kIdAdapterFile, L"ファイルを選択...");
    adapterClear = MakeButton(page, kIdAdapterClear, L"使わない");
    adapterRow = page->AddItem(L"LoRA アダプタ（GGUF）", L"",
                               L"変換記録から追加学習した LoRA アダプタ（llama.cpp のアダプタ形式の GGUF）をモデルに重ねて使います。"
                               L"Windows 版では学習はできないので、macOS 版で作ったアダプタを指定します。モデルに合わない"
                               L"アダプタを指定すると変換できなくなります。",
                               /*developer=*/true);
    adapterRow->below = {{{adapterFile, 140}, {adapterClear, 100}}};
    adapterRow->visible = dev;
}

void App::BuildAboutPage() {
    ui::Page* page = PageFor(L"about");
    page->AddHeader(L"iroha");
    gitHub = MakeButton(page, kIdGitHub, L"GitHub を開く");
    page->AddItem(L"iroha for Windows", std::wstring(L"版: ") + Wide(std::string(IROHA_VERSION)))->right = {{gitHub, 130}};
    dataFolder = MakeButton(page, kIdDataFolder, L"フォルダを開く");
    page->AddItem(L"データの保存場所", DataDir(),
                  L"ユーザ辞書・変換ルール・学習・入力履歴・変換記録・変換モデル・設定をこのフォルダに保存します。")
        ->right = {{dataFolder, 130}};

    page->AddHeader(L"アップデート");
    autoUpdateToggle = MakeToggle(page, kIdAutoUpdate);
    page->AddItem(L"アップデートを自動で確認", L"1 日 1 回、新しい版があるかを確かめます",
                  L"ON にすると、変換サーバが 1 日 1 回 GitHub の最新のリリースを確かめ、新しい版があれば知らせます。"
                  L"「アップデート」を選ぶとインストーラーをダウンロードし、大きさとチェックサムを照合してから入れ替えます"
                  L"（管理者の許可を求めます）。同じ版は 1 度だけ知らせ、「この版をスキップ」を選んだ版は知らせません。")
        ->right = {{autoUpdateToggle, 110}};
    checkUpdate = MakeButton(page, kIdCheckUpdate, L"今すぐ確認");
    page->AddItem(L"アップデートを確認", L"新しい版があるかを今すぐ確かめます")->right = {{checkUpdate, 130}};

    page->AddHeader(L"ライセンス");
    const struct {
        const wchar_t* name;
        const wchar_t* text;
    } licenses[] = {
        {L"iroha", L"© Tetsuaki Baba  MIT License\nhttps://github.com/TetsuakiBaba/iroha"},
        {L"iroha-kkc-v1", L"既定の変換モデル  © Tetsuaki Baba  CC BY-SA 4.0\nhttps://github.com/TetsuakiBaba/iroha/releases/tag/iroha-kkc-v1"},
        {L"zenz-v2.5-dataset", L"既定の変換モデルの学習元  © Keita Miwa  CC BY-SA 4.0\nhttps://huggingface.co/datasets/Miwa-Keita/zenz-v2.5-dataset"},
        {L"llama.cpp", L"変換モデルの推論エンジン  © ggml-org  MIT License\nhttps://github.com/ggml-org/llama.cpp"},
        {L"AzooKeyKanaKanjiConverter", L"候補ウィンドウの辞書ラティス（C++ に移植）  © ensan (azooKey)  MIT License\nhttps://github.com/azooKey/AzooKeyKanaKanjiConverter"},
        {L"azooKey_dictionary_storage", L"辞書データ  © azooKey  Apache License 2.0\nhttps://github.com/azooKey/azooKey_dictionary_storage"},
        {L"Mozc", L"記号・顔文字・絵文字のデータ  © Google Inc.  BSD 3-Clause License\nhttps://github.com/google/mozc"},
    };
    for (const auto& license : licenses) page->AddItem(license.name, license.text);
    // 打ち間違い訂正のモデルと専門辞書（入れたものだけ。RefreshLicenses で中身を入れる）
    for (Row*& row : licenseTypoRows) {
        row = page->AddItem(L"", L"");
        row->visible = [row] { return !row->title.empty(); };
    }

    page->AddHeader(L"開発者向け");
    developerToggle = MakeToggle(page, kIdDeveloperMode);
    page->AddItem(L"開発者モード", L"細かな調整や開発者向けの項目を表示します",
                  L"ON にすると、細かな調整や開発者向けの項目を設定画面に表示します（入力: アプリの文章を文脈に使う・"
                  L"打ち間違いの訂正の訂正モデル／休止時間／最低文字数／確信の強さ・区切り記号で英字を入力・AIで直す、"
                  L"辞書・学習: 記号・顔文字・絵文字を候補に出す、選択テキスト: 除外するアプリ、モデル: モデルファイルの指定・"
                  L"LoRA アダプタ、情報: デバッグ表示）。開発者モードの項目には、項目名の右に紫色の「開発者」の印を付けています。見出しに印が"
                  L"あるものは、その節全体が開発者モードの項目です。OFF にしても項目が見えなくなるだけで、変えた設定はそのまま"
                  L"効きます。")
        ->right = {{developerToggle, 110}};
    developerOverlayToggle = MakeToggle(page, kIdDeveloperOverlay);
    Row* overlayRow = page->AddItem(
        L"デバッグ表示", L"推論にかかった時間と左文脈を、入力中の文字の上に表示します",
        L"入力を始めたときの左文脈（アプリの文章から読めたか・何文字か）と、かな漢字変換・打ち間違いの訂正の 1 回ごとの"
        L"時間（全体と、そのうちニューラルネットの計算。モデルの読み込みがあればその時間）を出します。ライブ変換を"
        L"結果を使わずに打ち切った数も添えます。Enter で確定したとき・ほかの場所に移ったときに閉じます。"
        L"macOS 版の「推論にかかった時間と左文脈をカーソルの右下に表示する」と同じです（「要素の文字」の行と"
        L"加速度センサのグラフは macOS 専用なのでありません）。",
        /*developer=*/true);
    overlayRow->right = {{developerOverlayToggle, 110}};
    overlayRow->visible = [this] { return developerMode; };
    page->AddNote(L"「開発者」の印の付いた項目と節は、開発者モードのときだけ表示します。")->visible = [this] {
        return developerMode;
    };
}

// 打ち間違いの訂正のモデル・専門辞書のライセンス（入れた時点のカタログの記録から）
void App::RefreshLicenses() {
    std::vector<std::pair<std::wstring, std::wstring>> entries;
    const std::filesystem::path data(DataDir());
    if (const auto record = iroha::TypoNormalizerInstall::InstalledRecord(data)) {
        entries.push_back({L"打ち間違い訂正モデル（" + Wide(record->id) + L"）",
                           L"© Tetsuaki Baba  " + Wide(record->license) + L"\n" + Wide(record->page)});
        for (const auto& source : record->sources) {
            entries.push_back({Wide(source.name), L"打ち間違い訂正モデルの学習元  © " + Wide(source.holder) + L"  " +
                                                      Wide(source.license) + L"\n" + Wide(source.url)});
        }
    }
    for (const auto& record : iroha::TermDictionaryInstall::InstalledRecords(data)) {
        for (const auto& source : record.sources) {
            entries.push_back({Wide(source.name), L"専門辞書（" + Wide(record.name) + L"）  © " + Wide(source.holder) + L"  " +
                                                      Wide(source.license) + L"\n" + Wide(source.url)});
        }
    }
    for (size_t i = 0; i < ARRAYSIZE(licenseTypoRows); ++i) {
        licenseTypoRows[i]->title = i < entries.size() ? entries[i].first : L"";
        licenseTypoRows[i]->description = i < entries.size() ? entries[i].second : L"";
    }
    PageFor(L"about")->Relayout();
}

// ---- 窓 ----

void App::Layout() {
    RECT client = {};
    GetClientRect(window, &client);
    const int navWidth = S(260);
    for (size_t i = 0; i < nav.size(); ++i) {
        nav[i].rect = {S(8), S(64) + static_cast<int>(i) * S(40), navWidth - S(8), S(64) + static_cast<int>(i) * S(40) + S(36)};
        MoveWindow(nav[i].page->hwnd(), navWidth, 0, client.right - navWidth, client.bottom, TRUE);
    }
    InvalidateRect(window, nullptr, FALSE);
}

void App::PaintNav(HDC dc) {
    RECT client = {};
    GetClientRect(window, &client);
    RECT pane = {0, 0, S(260), client.bottom};
    FillRect(dc, &pane, g_theme.backgroundBrush);
    SetBkMode(dc, TRANSPARENT);
    // 窓の題（左上）
    HGDIOBJ oldFont = SelectObject(dc, g_theme.header);
    SetTextColor(dc, g_theme.text);
    RECT title = {S(20), S(18), pane.right - S(8), S(46)};
    DrawTextW(dc, L"iroha の設定", -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    for (size_t i = 0; i < nav.size(); ++i) {
        const RECT& rect = nav[i].rect;
        const bool selected = static_cast<int>(i) == current;
        if (selected || static_cast<int>(i) == hoverNav) {
            Gdiplus::Graphics graphics(dc);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            const COLORREF fill = selected ? g_theme.navSelected : g_theme.navHover;
            Gdiplus::SolidBrush brush(Gdiplus::Color(255, GetRValue(fill), GetGValue(fill), GetBValue(fill)));
            Gdiplus::GraphicsPath path;
            const float r = static_cast<float>(S(4)) * 2;
            const float x = static_cast<float>(rect.left), y = static_cast<float>(rect.top);
            const float w = static_cast<float>(rect.right - rect.left), h = static_cast<float>(rect.bottom - rect.top);
            path.AddArc(x, y, r, r, 180, 90);
            path.AddArc(x + w - r, y, r, r, 270, 90);
            path.AddArc(x + w - r, y + h - r, r, r, 0, 90);
            path.AddArc(x, y + h - r, r, r, 90, 90);
            path.CloseFigure();
            graphics.FillPath(&brush, &path);
            if (selected) {
                // 選んでいる項目の左の印（Windows 11 と同じ、縦の短い棒）
                Gdiplus::SolidBrush accent(Gdiplus::Color(255, GetRValue(g_theme.accent), GetGValue(g_theme.accent),
                                                          GetBValue(g_theme.accent)));
                const float barHeight = static_cast<float>(S(16));
                Gdiplus::GraphicsPath bar;
                const float bw = static_cast<float>(S(3));
                const float by = y + (h - barHeight) / 2;
                bar.AddArc(x, by, bw, bw, 180, 180);
                bar.AddArc(x, by + barHeight - bw, bw, bw, 0, 180);
                bar.CloseFigure();
                graphics.FillPath(&accent, &bar);
            }
        }
        SelectObject(dc, g_theme.icon);
        SetTextColor(dc, g_theme.text);
        RECT icon = {rect.left + S(14), rect.top, rect.left + S(40), rect.bottom};
        DrawTextW(dc, nav[i].glyph, -1, &icon, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, g_theme.body);
        RECT text = {rect.left + S(46), rect.top, rect.right - S(8), rect.bottom};
        DrawTextW(dc, nav[i].title, -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(dc, oldFont);
}

void App::ShowPage(int index) {
    if (index < 0 || index >= static_cast<int>(nav.size())) return;
    current = index;
    Refresh();
    for (size_t i = 0; i < nav.size(); ++i) nav[i].page->Show(static_cast<int>(i) == index);
    const std::wstring key = nav[index].key;
    // 専門辞書の一覧は、初めて開いたときに取りにいく（入れてある辞書が作り直されていれば入れ直す）
    if (key == L"terms" && !termCatalogRequested) {
        termCatalogRequested = true;
        StartTermTask(L"辞書の一覧を取得しています…", true, {}, {});
    }
    if (key == L"about") RefreshLicenses();
    RECT pane = {0, 0, S(260), 10000};
    InvalidateRect(window, &pane, FALSE);
}

void App::RelayoutAll() {
    for (auto& entry : nav) entry.page->Relayout();
}

void App::OnDpiChanged() {
    g_theme.Rebuild(GetDpiForWindow(window));
    for (auto& entry : nav) {
        EnumChildWindows(
            entry.page->hwnd(),
            [](HWND child, LPARAM) -> BOOL {
                SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(g_theme.body), TRUE);
                return TRUE;
            },
            0);
    }
    Layout();
    RelayoutAll();
}

void App::Update(const std::function<void(iroha::Config&)>& change, bool reload) {
    iroha::Config updated = iroha::LoadConfig(ConfigPath());
    change(updated);
    if (!iroha::SaveConfig(ConfigPath(), updated)) {
        MessageBoxW(window, L"設定の保存に失敗しました", L"iroha", MB_ICONERROR);
        return;
    }
    config = updated;
    if (reload) RequestServerReload();
    nav[current].page->Relayout();
}

// 画面の部品を設定の値に合わせる
void App::Refresh() {
    config = iroha::LoadConfig(ConfigPath());
    developerMode = config.developerMode;
    // 入力
    SetCheck(liveToggle, config.liveConversion);
    SendMessageW(countCombo, CB_SETCURSEL, config.candidateCount - kMinCandidateCount, 0);
    SetCheck(contextToggle, config.documentContext);
    SetCheck(typoToggle, config.typoCorrection);
    SendMessageW(typoDelay, TBM_SETPOS, TRUE, config.typoDelayMs / 50);
    SendMessageW(typoMinLength, CB_SETCURSEL, std::max(1, std::min(20, config.typoMinimumLength)) - 1, 0);
    SendMessageW(typoThreshold, TBM_SETPOS, TRUE, static_cast<LPARAM>(std::lround(config.typoThreshold * 2)));
    PageFor(L"input")->SetDescription(typoDelayRow, std::to_wstring(config.typoDelayMs) + L" ms");
    PageFor(L"input")->SetDescription(typoThresholdRow,
                                      FormatNumber(config.typoThreshold, 1) + L"（" + TypoThresholdLabel(config.typoThreshold) + L"）");
    EnableWindow(typoThresholdDefault, std::abs(config.typoThreshold - iroha::TypoNormalizer::kDefaultThreshold) > 1e-9);
    RefreshTypoStatus();
    if (config.typoCorrection && !typoChecked && !typoBusy) {
        // ON なら、開いたときに 1 回だけ新しい版のモデルが出ていないか確かめる（macOS版と同じ）
        typoChecked = true;
        StartTypoTask(/*announce=*/false);
    }
    SetCheck(predictiveToggle, config.predictive);
    SendMessageW(punctuationCombo, CB_SETCURSEL, Wide(config.punctuation) == kPunctuations[1] ? 1 : 0, 0);
    SetCheck(spaceToggle, config.fullWidthSpace);
    SetCheck(voiceToggle, config.voiceInput);
    SendMessageW(voiceKeyCombo, CB_SETCURSEL, config.voiceInputHoldKey == "rightShift" ? 1 : 0, 0);
    if (!voiceEngineChecked) {
        // 日本語の認識エンジンがあるか（開いたときに 1 回だけ調べる）
        voiceEngineChecked = true;
        std::u32string name;
        if (iroha::VoiceRecognizer::Available(&name)) {
            PageFor(L"input")->SetDescription(voiceRow, L"キーを押している間に話した言葉を入れます（認識: " + Wide(name) + L"）");
        } else {
            PageFor(L"input")->SetDescription(
                voiceRow, L"日本語の音声認識エンジンが見つかりません（Windows の日本語の言語パックの音声認識が要ります）", true);
        }
    }
    SetCheck(delimitedToggle, config.delimitedAlphabet);
    SetWindowTextW(delimitedStart, Wide(config.delimitedAlphabetStart).c_str());
    SetWindowTextW(delimitedEnd, Wide(config.delimitedAlphabetEnd).c_str());
    SetCheck(aiFixToggle, config.aiFix);
    SendMessageW(aiFixThreshold, TBM_SETPOS, TRUE, static_cast<LPARAM>(std::lround(config.aiFixThreshold * 4)));
    PageFor(L"input")->SetDescription(aiFixThresholdRow, FormatNumber(config.aiFixThreshold, 2));
    SetWindowTextW(aiFixShortcut, Wide(config.aiFixShortcut).c_str());
    // 辞書・学習
    SetCheck(privateToggle, config.privateMode);
    SetWindowTextW(privateHotkey, Wide(config.privateModeHotkey).c_str());
    SetCheck(learningToggle, config.learningEnabled);
    SetCheck(logToggle, config.conversionLog);
    SendMessageW(logScopeCombo, CB_SETCURSEL, config.conversionLogCorrectionsOnly ? 1 : 0, 0);
    SetCheck(symbolsToggle, config.symbolCandidates);
    RefreshLearningCounts();
    // 選択テキスト
    SetCheck(selToggle, config.selectionEnabled);
    int trigger = 1;
    for (int i = 0; i < 4; ++i) {
        if (config.selectionTriggerMode == kTriggerModes[i]) trigger = i;
    }
    SendMessageW(selTrigger, CB_SETCURSEL, trigger, 0);
    SetCheck(selCount, config.selectionCharacterCount);
    SetWindowTextW(selOnDemand, Wide(config.selectionOnDemandHotkey.value_or(iroha::SelectionSettingsDefaults::kOnDemandHotkey)).c_str());
    for (size_t i = 0; i < 5; ++i) {
        const auto& stored = config.selectionPresets[i];
        const auto& fallback = iroha::SelectionSettingsDefaults::Presets()[i];
        SetCheck(selPresetEnabled[i], stored.enabled.value_or(fallback.enabled));
        SetWindowTextW(selPresetName[i], Wide(stored.name.value_or(fallback.name)).c_str());
        SetWindowTextW(selPresetHotkey[i], Wide(stored.hotkey.value_or(fallback.hotkey)).c_str());
        SetWindowTextW(selPresetPrompt[i], ToEditText(stored.prompt.value_or(fallback.prompt)).c_str());
    }
    SetWindowTextW(selExcluded, Wide(config.selectionExcludedApps).c_str());
    // AI
    const int service = AIServiceIndex(iroha::ParseAIService(config.aiService));
    SendMessageW(aiService, CB_SETCURSEL, service, 0);
    ShowAIServiceFields(service);
    for (size_t i = 0; i < 3; ++i) {
        const auto& stored = config.aiPresets[i];
        const auto& fallback = iroha::AICommitSettings::Defaults()[i];
        SetWindowTextW(aiPresetName[i], Wide(stored.name.value_or(fallback.name)).c_str());
        SetWindowTextW(aiPresetHotkey[i], Wide(stored.hotkey.value_or(fallback.shortcut)).c_str());
        SetWindowTextW(aiPresetPrompt[i], ToEditText(stored.prompt.value_or(fallback.prompt)).c_str());
    }
    // モデル
    const std::wstring currentModel = Wide(config.model);
    for (int i = 0; i < static_cast<int>(ARRAYSIZE(kKnownModels)); ++i) {
        if (currentModel == kKnownModels[i].fileName || (currentModel.empty() && i == 0)) {
            if (SendMessageW(modelCombo, CB_GETCURSEL, 0, 0) < 0) SendMessageW(modelCombo, CB_SETCURSEL, i, 0);
        }
    }
    if (SendMessageW(modelCombo, CB_GETCURSEL, 0, 0) < 0) SendMessageW(modelCombo, CB_SETCURSEL, 0, 0);
    RefreshModelPage();
    // 情報
    SetCheck(developerToggle, developerMode);
    SetCheck(autoUpdateToggle, config.autoUpdateCheck);
    SetCheck(developerOverlayToggle, config.developerOverlay);
    RelayoutAll();
}

// ---- 打ち間違いの訂正 ----

void App::RefreshTypoStatus() {
    const std::filesystem::path data(DataDir());
    const bool installed = iroha::TypoNormalizerInstall::IsInstalled(data);
    const auto record = iroha::TypoNormalizerInstall::InstalledRecord(data);
    ui::Page* page = PageFor(L"input");
    if (!typoBusy) {
        std::wstring status;
        if (installed && record) {
            status = L"入力が止まったときに読みを直します（モデル: " + Wide(record->name) + L"）";
            page->SetDescription(typoModelRow, Wide(record->name) + L"（" + Wide(record->id) + L"）  " + Wide(record->license));
        } else {
            status = L"ON にしたときに、訂正のモデル（約 23MB）を 1 回だけダウンロードします";
            page->SetDescription(typoModelRow, L"入っていません");
        }
        page->SetDescription(typoRow, status);
    }
    EnableWindow(typoToggle, !typoBusy);
    EnableWindow(typoLicense, installed && record && !record->sources.empty());
    EnableWindow(typoRemove, installed && !typoBusy);
}

// モデルの一覧を取り、入っていないか新しい版があれば取得して設置する（裏のスレッド）
void App::StartTypoTask(bool announce) {
    if (typoBusy) return;
    typoBusy = true;
    if (announce) PageFor(L"input")->SetDescription(typoRow, L"モデルを取得しています…");
    RefreshTypoStatus();
    HWND target = window;
    const auto run = [](LPVOID param) -> DWORD {
        HWND target = static_cast<HWND>(param);
        auto* result = new TypoTaskResult();
        const std::filesystem::path data(DataDir());
        iroha::TypoNormalizerCatalog catalog;
        std::string error;
        if (!iroha::TypoNormalizerFetcher::FetchCatalog(iroha::TypoNormalizerCatalog::DefaultUrl(), &catalog, &error)) {
            if (!iroha::TypoNormalizerInstall::IsInstalled(data)) {
                result->failed = true;
                result->message = L"モデルの一覧を取得できませんでした: " + Wide(error);
            }
        } else {
            const auto record = iroha::TypoNormalizerInstall::InstalledRecord(data);
            const auto* model = record ? catalog.Find(record->id) : nullptr;
            if (!model) model = &catalog.models.front();
            if (iroha::TypoNormalizerInstall::NeedsUpdate(data, *model)) {
                if (iroha::TypoNormalizerFetcher::Install(*model, catalog.LicenseFor(*model), data, nullptr, &error)) {
                    result->message = Wide(model->name) + L" のモデルを入れました";
                    if (!RequestServerReload()) result->message += L"（変換サーバが動いていないため、次に起動したときから使います）";
                } else {
                    result->failed = true;
                    result->message = L"モデルを取得できませんでした: " + Wide(error);
                }
            }
        }
        PostMessageW(target, WM_APP_TYPO_DONE, 0, reinterpret_cast<LPARAM>(result));
        return 0;
    };
    HANDLE thread = CreateThread(nullptr, 0, run, target, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        typoBusy = false;
        RefreshTypoStatus();
    }
}

void App::OnTypoTaskDone(TypoTaskResult* result) {
    typoBusy = false;
    RefreshTypoStatus();
    if (!result->message.empty()) PageFor(L"input")->SetDescription(typoRow, result->message, result->failed);
    delete result;
    PageFor(L"input")->Relayout();
}

void App::OnTypoLicense() {
    const auto record = iroha::TypoNormalizerInstall::InstalledRecord(std::filesystem::path(DataDir()));
    if (!record) return;
    std::wstring text = Wide(record->name) + L"（" + Wide(record->id) + L"）\n";
    if (!record->license.empty()) text += L"ライセンス: " + Wide(record->license) + L"\n";
    if (!record->page.empty()) text += Wide(record->page) + L"\n";
    text += L"\n学習元:\n";
    for (const auto& source : record->sources) {
        text += L"・" + Wide(source.name) + L"  © " + Wide(source.holder) + L"  " + Wide(source.license) + L"\n    " +
                Wide(source.url) + L"\n";
    }
    ui::ShowFlyout(typoLicense, text);
}

void App::OnTypoRemove() {
    if (MessageBoxW(window, L"打ち間違い訂正のモデルを削除します。よろしいですか？\n（もう一度 ON にすると取得し直します）",
                    L"iroha", MB_YESNO | MB_ICONWARNING) != IDYES) {
        return;
    }
    Update([](iroha::Config& c) { c.typoCorrection = false; });
    SetCheck(typoToggle, false);
    if (!iroha::TypoNormalizerInstall::Remove(std::filesystem::path(DataDir()))) {
        MessageBoxW(window, L"モデルを削除できませんでした", L"iroha", MB_ICONERROR);
    }
    RefreshTypoStatus();
}

// ---- 学習・記録 ----

void App::RefreshLearningCounts() {
    learning = std::make_unique<iroha::LearningStore>(std::filesystem::path(DataDir() + L"\\learning.json"));
    PageFor(L"learning")->SetDescription(learnRow, std::to_wstring(learning->Count()) + L" 件");
    EnableWindow(learnReset, learning->Count() > 0);
    inputHistory->Reload();
    PageFor(L"input")->SetDescription(historyRow, std::to_wstring(inputHistory->Count()) +
                                                      L" 語（予測変換が ON の間だけ記録します）");
    EnableWindow(historyReset, inputHistory->Count() > 0);
    const size_t count = conversionLog->Items().size();
    const uintmax_t size = conversionLog->TotalSize();
    std::wstring text = std::to_wstring(count) + L" 件（" + FormatNumber(static_cast<double>(size) / 1024.0, 1) + L" KB）";
    if (config.privateMode && config.conversionLog) text += L"  プライベートモードの間は記録しません";
    PageFor(L"learning")->SetDescription(logRow, text);
    EnableWindow(logReset, count > 0 || size > 0);
}

void App::OnHistoryReset() {
    if (MessageBoxW(window, L"入力履歴（予測変換の候補）をすべて消去します。よろしいですか？", L"iroha",
                    MB_YESNO | MB_ICONWARNING) != IDYES) {
        return;
    }
    inputHistory->Reset(iroha::InputHistoryStore::NowSeconds());
    inputHistory->Flush();
    RequestServerReload();
    RefreshLearningCounts();
}

void App::OnLearnReset() {
    if (MessageBoxW(window, L"学習した変換をすべて消去します。よろしいですか？\n（ユーザ辞書は消えません）", L"iroha",
                    MB_YESNO | MB_ICONWARNING) != IDYES) {
        return;
    }
    learning->Reset();
    RequestServerReload();
    RefreshLearningCounts();
}

void App::OnLogReset() {
    if (MessageBoxW(window,
                    L"変換記録をすべて消去します。よろしいですか？\n（データフォルダを共有しているほかのコンピュータの記録も消えます）",
                    L"iroha", MB_YESNO | MB_ICONWARNING) != IDYES) {
        return;
    }
    conversionLog->RemoveAll();
    RefreshLearningCounts();
}

bool App::SaveShortcut(HWND edit, ui::Page* page, Row* row, const std::wstring& normal,
                       const std::function<void(iroha::Config&, const std::string&)>& store) {
    const std::string text = iroha::Utf32ToUtf8(TrimmedText(edit));
    if (!text.empty() && !iroha::KeyShortcut::Parse(text)) {
        if (row) {
            page->SetDescription(row, L"「" + Wide(text) + L"」は読めません。「Ctrl+Alt+P」のように、Ctrl・Alt・Shift と 1 つのキーを"
                                          L" + でつないで書きます",
                                 true);
        }
        return false;
    }
    if (row) page->SetDescription(row, normal);
    Update([&](iroha::Config& c) { store(c, text); });
    return true;
}

// ---- ユーザ辞書 ----

void App::RefreshDictionaryList() {
    dictionary = std::make_unique<iroha::UserDictionaryStore>(std::filesystem::path(DataDir() + L"\\user-dictionary.json"));
    ListView_DeleteAllItems(dictList);
    int row = 0;
    for (const iroha::UserDictionaryEntry& entry : dictionary->Entries()) {
        std::wstring reading = Wide(entry.reading);
        std::wstring word = Wide(entry.word);
        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        item.pszText = reading.data();
        ListView_InsertItem(dictList, &item);
        ListView_SetItemText(dictList, row, 1, word.data());
        ListView_SetItemText(dictList, row, 2,
                             const_cast<wchar_t*>(entry.source == iroha::UserDictionaryEntry::Source::System ? L"取込" : L"手動"));
        ++row;
    }
}

void App::OnDictAdd() {
    const std::u32string reading = TrimmedText(dictReading);
    const std::u32string word = TrimmedText(dictWord);
    if (reading.empty() || word.empty()) {
        MessageBoxW(window, L"読みと単語を入力してください", L"iroha", MB_ICONINFORMATION);
        return;
    }
    dictionary->Add(reading, word);
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

// ---- 専門辞書 ----

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

void App::ShowTermDetail() {
    const int selected = ListView_GetNextItem(termList, -1, LVNI_SELECTED);
    const bool valid = selected >= 0 && selected < static_cast<int>(termRows.size());
    EnableWindow(termAdd, valid && !termBusy && termRows[selected].catalogEntry &&
                              (!termRows[selected].installed || termRows[selected].needsUpdate));
    EnableWindow(termRemove, valid && !termBusy && termRows[selected].installed);
    if (!valid) {
        SetWindowTextW(termDetail, L"一覧から辞書を選ぶと、説明と出典・ライセンスが出ます。");
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
        text += L"出典: " + Wide(source.name) + L"  © " + Wide(source.holder) + L"  " + Wide(source.license) + L"\r\n  " +
                Wide(source.url) + L"\r\n";
        if (!source.licenseUrl.empty()) text += L"  ライセンス文書: " + Wide(source.licenseUrl) + L"\r\n";
    }
    SetWindowTextW(termDetail, text.c_str());
}

void App::StartTermTask(std::wstring busyMessage, bool fetchCatalog, std::string installId, std::string removeId) {
    if (termBusy) return;
    termBusy = true;
    PageFor(L"terms")->SetDescription(termRowStatus, busyMessage);
    ShowTermDetail();
    struct Task {
        HWND window;
        bool fetchCatalog;
        std::string installId;
        std::string removeId;
        std::unique_ptr<iroha::TermDictionaryCatalog> catalog;
    };
    auto* task = new Task{window, fetchCatalog, std::move(installId), std::move(removeId),
                          termCatalog ? std::make_unique<iroha::TermDictionaryCatalog>(*termCatalog) : nullptr};
    const auto run = [](LPVOID param) -> DWORD {
        std::unique_ptr<Task> task(static_cast<Task*>(param));
        auto* result = new TermTaskResult();
        const std::filesystem::path data(DataDir());
        std::string error;
        bool changed = false;
        if (task->fetchCatalog) {
            auto catalog = std::make_unique<iroha::TermDictionaryCatalog>();
            if (iroha::TermDictionaryFetcher::FetchCatalog(iroha::TermDictionaryCatalog::DefaultUrl(), catalog.get(), &error)) {
                result->catalogFetched = true;
                int updated = 0;
                for (const auto& record : iroha::TermDictionaryInstall::InstalledRecords(data)) {
                    const auto* dictionary = catalog->Find(record.id);
                    if (!dictionary || !iroha::TermDictionaryInstall::NeedsUpdate(record, *dictionary)) continue;
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
        PageFor(L"terms")->SetDescription(termRowStatus, L"作業を始められませんでした", true);
    }
}

void App::OnTermTaskDone(TermTaskResult* result) {
    termBusy = false;
    if (result->catalog) termCatalog = std::move(result->catalog);
    PageFor(L"terms")->SetDescription(termRowStatus, result->message, result->failed);
    RefreshTermList();
    delete result;
}

// ---- 変換ルール ----

void App::RefreshRuleList() {
    rewriteRules->Reload();
    rules = rewriteRules->Rules();
    ruleRefreshing = true;
    ListView_DeleteAllItems(ruleList);
    const iroha::RewriteContext now = iroha::RewriteContext::Now();
    for (int i = 0; i < static_cast<int>(rules.size()); ++i) {
        const iroha::UserRewriteRule& rule = rules[i];
        std::wstring trigger = Wide(rule.trigger);
        std::wstring output = Wide(rule.output);
        std::wstring rendered = Wide(iroha::RewriteTemplate(rule.output).Render(now));
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
    const std::u32string normalized = iroha::UserRewriteRule::NormalizedTrigger(iroha::Utf16ToUtf32(WindowText(ruleTrigger)));
    const std::u32string templateText = iroha::Utf16ToUtf32(WindowText(ruleOutput));
    if (normalized.empty() || templateText.empty()) {
        MessageBoxW(window, L"読みと出力を入力してください", L"iroha", MB_ICONINFORMATION);
        return;
    }
    // 知らない {{…}} はそのまま文字として出るので、追加する前に知らせる
    const auto unknown = iroha::RewriteTemplate(templateText).UnknownPlaceholders();
    if (!unknown.empty()) {
        std::wstring names;
        for (const auto& name : unknown) names += L"{{" + Wide(name) + L"}} ";
        const std::wstring message = L"次の書き方には対応していないので、そのまま文字として出ます:\n" + names + L"\n\nこのまま追加しますか？";
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

void App::OnRuleChecked(int row) {
    if (ruleRefreshing || row < 0 || row >= static_cast<int>(rules.size())) return;
    const bool enabled = ListView_GetCheckState(ruleList, row) != FALSE;
    if (enabled == rules[row].isEnabled) return;
    rewriteRules->SetEnabled(enabled, rules[row].id);
    rules[row].isEnabled = enabled;
    RequestServerReload();
}

// ---- 選択テキスト ----

// 名前・ショートカット・プロンプト・使わないアプリの欄を保存する（欄を離れたとき）
void App::SaveSelectionTexts() {
    std::wstring invalid;
    const auto checkHotkey = [&invalid](const std::string& hotkey) {
        if (!hotkey.empty() && !iroha::KeyShortcut::Parse(hotkey)) invalid += L"「" + Wide(hotkey) + L"」";
        return hotkey.empty() || iroha::KeyShortcut::Parse(hotkey);
    };
    Update([&](iroha::Config& c) {
        const std::string onDemand = iroha::Utf32ToUtf8(TrimmedText(selOnDemand));
        if (checkHotkey(onDemand)) c.selectionOnDemandHotkey = onDemand;
        for (size_t i = 0; i < 5; ++i) {
            auto& preset = c.selectionPresets[i];
            preset.name = TrimmedText(selPresetName[i]);
            const std::string hotkey = iroha::Utf32ToUtf8(TrimmedText(selPresetHotkey[i]));
            if (checkHotkey(hotkey)) preset.hotkey = hotkey;
            preset.prompt = FromEditText(selPresetPrompt[i]);
        }
        c.selectionExcludedApps = iroha::Utf32ToUtf8(FromEditText(selExcluded));
    });
    ui::Page* page = PageFor(L"selection");
    page->SetDescription(selStatusRow,
                         invalid.empty() ? L""
                                         : L"キーの書き方が読めません: " + invalid +
                                               L"。「Ctrl+Alt+1」のように、Ctrl・Alt・Shift と 1 つのキーを + でつないで書きます",
                         true);
}

// ---- AI ----

void App::ShowAIServiceFields(int index) {
    aiShownService = index;
    const iroha::AIService service = kAIServices[index];
    std::string endpoint;
    std::string model;
    switch (service) {
    case iroha::AIService::Ollama:
        endpoint = config.ollamaEndpoint;
        model = config.ollamaModel;
        break;
    case iroha::AIService::LMStudio:
        endpoint = config.lmStudioEndpoint;
        model = config.lmStudioModel;
        break;
    case iroha::AIService::OpenAI:
        endpoint = config.openAIEndpoint;
        model = config.openAIModel;
        break;
    case iroha::AIService::Claude:
        model = config.claudeModel;
        break;
    default:
        break;
    }
    SetWindowTextW(aiEndpoint, Wide(endpoint).c_str());
    SendMessageW(aiEndpoint, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(Wide(iroha::AIClient::DefaultEndpoint(service)).c_str()));
    SendMessageW(aiModel, CB_RESETCONTENT, 0, 0);
    SetWindowTextW(aiModel, Wide(model).c_str());
    SendMessageW(aiModel, CB_SETCUEBANNER, 0,
                 reinterpret_cast<LPARAM>(service == iroha::AIService::Claude ? Wide(iroha::AIClient::kClaudeDefaultModel).c_str()
                                                                               : L""));
    SetWindowTextW(aiKey, L"");
    RefreshAIKeyStatus(service);
}

void App::RefreshAIKeyStatus(iroha::AIService service) {
    ui::Page* page = PageFor(L"ai");
    if (service == iroha::AIService::OpenAI || service == iroha::AIService::Claude) {
        std::string key;
        const bool stored = iroha::SecretStore::Get(service == iroha::AIService::Claude ? iroha::AIClient::kClaudeKeyAccount
                                                                                         : iroha::AIClient::kOpenAIKeyAccount,
                                                    &key) &&
                            !key.empty();
        std::wstring text = stored ? L"保存してあります（資格情報マネージャー）" : L"まだありません";
        if (service == iroha::AIService::OpenAI && !stored) text += L"（このコンピュータの互換サーバなら無くてもよい）";
        page->SetDescription(aiKeyRow, text, !stored && service == iroha::AIService::Claude);
        EnableWindow(aiKeyClear, stored);
    }
    page->SetDescription(aiServiceRow, service == iroha::AIService::None
                                           ? L"AI を使わない（AI変換のキーでは普通に確定します）"
                                       : service == iroha::AIService::OpenAI || service == iroha::AIService::Claude
                                           ? L"入力した文章が外部のサービスに送られます"
                                           : L"このコンピュータで動くサーバに送ります");
}

void App::StoreAIServiceFields(int index, iroha::Config* target) {
    if (index < 0) return;
    const std::string endpoint = iroha::Utf32ToUtf8(TrimmedText(aiEndpoint));
    const std::string model = iroha::Utf32ToUtf8(TrimmedText(aiModel));
    switch (kAIServices[index]) {
    case iroha::AIService::Ollama:
        target->ollamaEndpoint = endpoint;
        target->ollamaModel = model;
        break;
    case iroha::AIService::LMStudio:
        target->lmStudioEndpoint = endpoint;
        target->lmStudioModel = model;
        break;
    case iroha::AIService::OpenAI:
        target->openAIEndpoint = endpoint;
        target->openAIModel = model;
        break;
    case iroha::AIService::Claude:
        target->claudeModel = model;
        break;
    default:
        break;
    }
}

void App::OnAIServiceChanged() {
    const int index = static_cast<int>(SendMessageW(aiService, CB_GETCURSEL, 0, 0));
    if (index < 0) return;
    const int previous = aiShownService;
    Update([&](iroha::Config& c) {
        // 前の接続先の欄の値は、その接続先の設定として残す
        StoreAIServiceFields(previous, &c);
        c.aiService = iroha::AIServiceKey(kAIServices[index]);
    });
    ShowAIServiceFields(index);
    PageFor(L"ai")->Relayout();
}

void App::SaveAIPresets() {
    std::wstring invalid;
    Update([&](iroha::Config& c) {
        StoreAIServiceFields(aiShownService, &c);
        for (size_t i = 0; i < 3; ++i) {
            c.aiPresets[i].name = TrimmedText(aiPresetName[i]);
            const std::string hotkey = iroha::Utf32ToUtf8(TrimmedText(aiPresetHotkey[i]));
            if (!hotkey.empty() && !iroha::KeyShortcut::Parse(hotkey)) {
                invalid += L"「" + Wide(hotkey) + L"」";
            } else {
                c.aiPresets[i].hotkey = hotkey;
            }
            c.aiPresets[i].prompt = FromEditText(aiPresetPrompt[i]);
        }
    });
    PageFor(L"ai")->SetDescription(aiPresetStatusRow,
                                   invalid.empty() ? L""
                                                   : L"キーの書き方が読めません: " + invalid +
                                                         L"。「Alt+Enter」のように、Ctrl・Alt・Shift と 1 つのキーを + でつないで書きます",
                                   true);
}

void App::OnAIKeySave() {
    const int index = static_cast<int>(SendMessageW(aiService, CB_GETCURSEL, 0, 0));
    const iroha::AIService service = index >= 0 ? kAIServices[index] : iroha::AIService::None;
    const std::string key = iroha::Utf32ToUtf8(TrimmedText(aiKey));
    if (key.empty() || (service != iroha::AIService::OpenAI && service != iroha::AIService::Claude)) return;
    iroha::SecretStore::Set(service == iroha::AIService::Claude ? iroha::AIClient::kClaudeKeyAccount
                                                                : iroha::AIClient::kOpenAIKeyAccount,
                            key);
    SetWindowTextW(aiKey, L"");
    RequestServerReload();
    RefreshAIKeyStatus(service);
}

void App::OnAIKeyClear() {
    const int index = static_cast<int>(SendMessageW(aiService, CB_GETCURSEL, 0, 0));
    if (index < 0) return;
    const iroha::AIService service = kAIServices[index];
    if (service != iroha::AIService::OpenAI && service != iroha::AIService::Claude) return;
    iroha::SecretStore::Set(service == iroha::AIService::Claude ? iroha::AIClient::kClaudeKeyAccount
                                                                : iroha::AIClient::kOpenAIKeyAccount,
                            "");
    RequestServerReload();
    RefreshAIKeyStatus(service);
}

void App::OnAIListModels() {
    const int index = static_cast<int>(SendMessageW(aiService, CB_GETCURSEL, 0, 0));
    if (index < 0) return;
    iroha::AIConnection connection;
    connection.service = kAIServices[index];
    connection.endpoint = iroha::Utf32ToUtf8(TrimmedText(aiEndpoint));
    connection.apiKey = iroha::Utf32ToUtf8(TrimmedText(aiKey));
    if (connection.apiKey.empty()) {
        iroha::SecretStore::Get(connection.service == iroha::AIService::Claude ? iroha::AIClient::kClaudeKeyAccount
                                                                               : iroha::AIClient::kOpenAIKeyAccount,
                                &connection.apiKey);
    }
    EnableWindow(aiModels, FALSE);
    PageFor(L"ai")->SetDescription(aiServiceRow, L"モデルの一覧を取得しています…");
    struct Task {
        HWND window;
        iroha::AIConnection connection;
    };
    auto* task = new Task{window, connection};
    const auto run = [](LPVOID param) -> DWORD {
        std::unique_ptr<Task> task(static_cast<Task*>(param));
        auto* result = new AIModelsResult();
        std::string error;
        if (!iroha::AIClient::ListModels(task->connection, &result->models, &error)) result->error = Wide(error);
        PostMessageW(task->window, WM_APP_AI_MODELS, 0, reinterpret_cast<LPARAM>(result));
        return 0;
    };
    HANDLE thread = CreateThread(nullptr, 0, run, task, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        delete task;
        EnableWindow(aiModels, TRUE);
    }
}

void App::OnAIModelsDone(AIModelsResult* result) {
    EnableWindow(aiModels, TRUE);
    ui::Page* page = PageFor(L"ai");
    if (!result->error.empty()) {
        page->SetDescription(aiServiceRow, L"モデルの一覧を取得できませんでした: " + result->error, true);
    } else {
        const std::wstring typed = WindowText(aiModel);
        SendMessageW(aiModel, CB_RESETCONTENT, 0, 0);
        for (const std::string& model : result->models) {
            SendMessageW(aiModel, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Wide(model).c_str()));
        }
        SetWindowTextW(aiModel, typed.c_str());
        if (typed.empty() && !result->models.empty()) SendMessageW(aiModel, CB_SETCURSEL, 0, 0);
        SendMessageW(aiModel, CB_SHOWDROPDOWN, TRUE, 0);
        page->SetDescription(aiServiceRow, std::to_wstring(result->models.size()) + L" 個のモデルがあります");
    }
    delete result;
}

// ---- モデル ----

void App::RefreshModelPage() {
    ui::Page* page = PageFor(L"model");
    const int index = static_cast<int>(SendMessageW(modelCombo, CB_GETCURSEL, 0, 0));
    if (index >= 0 && index < static_cast<int>(ARRAYSIZE(kKnownModels))) {
        const KnownModel& model = kKnownModels[index];
        const bool downloaded = std::filesystem::exists(ModelPath(model.fileName));
        // 未設定なら既定のモデル（変換サーバと同じ決め方）
        const std::wstring inUse = config.model.empty()
                                       ? iroha::DefaultModelPath(ModelPath(L"").parent_path()).filename().wstring()
                                       : Wide(config.model);
        std::wstring status = downloading ? L"ダウンロードしています…" : downloaded ? L"ダウンロード済み" : L"まだダウンロードしていません";
        if (inUse == model.fileName) status += L"（使用中）";
        page->SetDescription(modelRow, status);
        EnableWindow(modelDownload, !downloaded && !downloading);
        EnableWindow(modelApply, downloaded && !downloading && inUse != model.fileName);
    }
    // 開発者モード: モデルファイル・アダプタのパス
    const std::wstring modelPath = config.model.empty() ? L"既定: " + iroha::DefaultModelPath(ModelPath(L"").parent_path()).wstring()
                                                        : Wide(config.model);
    const bool absolute = !config.model.empty() && std::filesystem::path(Wide(config.model)).is_absolute();
    const bool missing = absolute && !std::filesystem::exists(std::filesystem::path(Wide(config.model)));
    page->SetDescription(modelPathRow, missing ? modelPath + L"（ファイルがありません）" : modelPath, missing);
    const bool adapterMissing = !config.modelAdapterPath.empty() &&
                                !std::filesystem::exists(std::filesystem::path(Wide(config.modelAdapterPath)));
    page->SetDescription(adapterRow,
                         config.modelAdapterPath.empty() ? L"使っていません"
                                                         : Wide(config.modelAdapterPath) + (adapterMissing ? L"（ファイルがありません）" : L""),
                         adapterMissing);
    EnableWindow(adapterClear, !config.modelAdapterPath.empty());
    EnableWindow(modelFileDefault, !config.model.empty());
}

struct DownloadContext {
    HWND window;
    std::wstring url;
    std::wstring destination;
    uintmax_t bytes;
    std::string sha256;
};

DWORD WINAPI DownloadThread(LPVOID param) {
    std::unique_ptr<DownloadContext> context(static_cast<DownloadContext*>(param));
    const std::wstring temp = context->destination + L".tmp";
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(context->destination).parent_path(), ec);
    const HRESULT hr = URLDownloadToFileW(nullptr, context->url.c_str(), temp.c_str(), 0, nullptr);
    bool ok = SUCCEEDED(hr);
    if (ok && context->bytes != 0) ok = std::filesystem::file_size(temp, ec) == context->bytes && !ec;
    if (ok && !context->sha256.empty()) ok = iroha::TermDictionaryFetcher::Sha256Hex(temp) == context->sha256;
    if (ok) {
        std::filesystem::rename(temp, context->destination, ec);
        ok = !ec;
    } else {
        std::filesystem::remove(temp, ec);
    }
    PostMessageW(context->window, WM_APP_DOWNLOAD_DONE, ok ? 1 : 0, 0);
    return 0;
}

void App::OnModelDownload() {
    const int index = static_cast<int>(SendMessageW(modelCombo, CB_GETCURSEL, 0, 0));
    if (index < 0 || index >= static_cast<int>(ARRAYSIZE(kKnownModels))) return;
    downloading = true;
    RefreshModelPage();
    auto* context = new DownloadContext{window, kKnownModels[index].url, ModelPath(kKnownModels[index].fileName).wstring(),
                                        kKnownModels[index].bytes, kKnownModels[index].sha256};
    HANDLE thread = CreateThread(nullptr, 0, DownloadThread, context, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        delete context;
        downloading = false;
        RefreshModelPage();
    }
}

void App::OnModelApply() {
    const int index = static_cast<int>(SendMessageW(modelCombo, CB_GETCURSEL, 0, 0));
    if (index < 0 || index >= static_cast<int>(ARRAYSIZE(kKnownModels))) return;
    PageFor(L"model")->SetDescription(modelRow, L"変換サーバに反映しています…（モデルの読み込みに数秒かかります）");
    UpdateWindow(window);
    Update([&](iroha::Config& c) { c.model = iroha::Utf16ToUtf32(kKnownModels[index].fileName); });
    RefreshModelPage();
}

// モデルかアダプタの GGUF を選ぶ
void App::OnChooseFile(bool adapter) {
    wchar_t path[MAX_PATH] = L"";
    std::wstring folder = DataDir() + (adapter ? L"\\models\\adapters" : L"\\models");
    OPENFILENAMEW dialog = {sizeof(dialog)};
    dialog.hwndOwner = window;
    dialog.lpstrFilter = L"GGUF (*.gguf)\0*.gguf\0すべてのファイル\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrInitialDir = folder.c_str();
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) return;
    PageFor(L"model")->SetDescription(adapter ? adapterRow : modelPathRow, L"変換サーバに反映しています…");
    UpdateWindow(window);
    Update([&](iroha::Config& c) {
        if (adapter) {
            c.modelAdapterPath = Narrow(path);
        } else {
            c.model = iroha::Utf16ToUtf32(path);
        }
    });
    RefreshModelPage();
}

// ---- 操作 ----

void App::OnCommand(int id, int code, HWND control) {
    const bool clicked = code == BN_CLICKED;
    const bool selected = code == CBN_SELCHANGE;
    const bool left = code == EN_KILLFOCUS;
    const auto comboIndex = [control] { return static_cast<int>(SendMessageW(control, CB_GETCURSEL, 0, 0)); };
    ui::Page* input = PageFor(L"input");
    switch (id) {
    // 入力
    case kIdLive:
        if (clicked) Update([&](iroha::Config& c) { c.liveConversion = Checked(control); });
        return;
    case kIdCandidateCount:
        if (selected && comboIndex() >= 0) Update([&](iroha::Config& c) { c.candidateCount = kMinCandidateCount + comboIndex(); });
        return;
    case kIdDocumentContext:
        if (clicked) Update([&](iroha::Config& c) { c.documentContext = Checked(control); });
        return;
    case kIdTypo:
        if (!clicked) return;
        Update([&](iroha::Config& c) { c.typoCorrection = Checked(control); });
        if (config.typoCorrection && !typoBusy) {
            typoChecked = true;
            StartTypoTask(/*announce=*/true);
        }
        return;
    case kIdTypoLicense:
        if (clicked) OnTypoLicense();
        return;
    case kIdTypoRemove:
        if (clicked) OnTypoRemove();
        return;
    case kIdTypoMinLength:
        if (selected && comboIndex() >= 0) Update([&](iroha::Config& c) { c.typoMinimumLength = comboIndex() + 1; });
        return;
    case kIdTypoThresholdDefault:
        if (!clicked) return;
        Update([](iroha::Config& c) { c.typoThreshold = iroha::TypoNormalizer::kDefaultThreshold; });
        Refresh();
        return;
    case kIdPredictive:
        if (clicked) Update([&](iroha::Config& c) { c.predictive = Checked(control); });
        return;
    case kIdHistoryReset:
        if (clicked) OnHistoryReset();
        return;
    case kIdPunctuation:
        if (selected) Update([&](iroha::Config& c) { c.punctuation = iroha::Utf16ToUtf32(kPunctuations[comboIndex() == 1 ? 1 : 0]); });
        return;
    case kIdFullWidthSpace:
        if (clicked) Update([&](iroha::Config& c) { c.fullWidthSpace = Checked(control); });
        return;
    case kIdVoice:
        if (clicked) Update([&](iroha::Config& c) { c.voiceInput = Checked(control); });
        return;
    case kIdVoiceKey:
        if (selected) Update([&](iroha::Config& c) { c.voiceInputHoldKey = comboIndex() == 1 ? "rightShift" : "rightControl"; });
        return;
    case kIdDelimited:
        if (clicked) Update([&](iroha::Config& c) { c.delimitedAlphabet = Checked(control); });
        return;
    case kIdDelimitedStart:
    case kIdDelimitedEnd: {
        if (!left) return;
        const std::string text = iroha::Utf32ToUtf8(TrimmedText(control));
        const bool valid = text.size() == 1 && text[0] >= 0x21 && text[0] <= 0x7E && !std::isalnum(static_cast<unsigned char>(text[0]));
        Row* row = id == kIdDelimitedStart ? delimitedStartRow : delimitedEndRow;
        if (!valid) {
            input->SetDescription(row, L"英字・数字以外の ASCII の記号 1 文字を書きます（例: _ や `）", true);
            return;
        }
        input->SetDescription(row, L"英字・数字以外の ASCII の記号 1 文字");
        Update([&](iroha::Config& c) {
            (id == kIdDelimitedStart ? c.delimitedAlphabetStart : c.delimitedAlphabetEnd) = text;
        });
        return;
    }
    case kIdAIFix:
        if (clicked) Update([&](iroha::Config& c) { c.aiFix = Checked(control); });
        return;
    case kIdAIFixShortcut:
        if (left) {
            SaveShortcut(control, input, aiFixShortcutRow, L"",
                         [](iroha::Config& c, const std::string& text) { c.aiFixShortcut = text; });
        }
        return;
    // 辞書・学習
    case kIdPrivateMode:
        if (clicked) Update([&](iroha::Config& c) { c.privateMode = Checked(control); });
        return;
    case kIdPrivateHotkey:
        if (left) {
            SaveShortcut(control, PageFor(L"learning"), privateHotkeyRow, L"",
                         [](iroha::Config& c, const std::string& text) { c.privateModeHotkey = text; });
        }
        return;
    case kIdLearning:
        if (clicked) Update([&](iroha::Config& c) { c.learningEnabled = Checked(control); });
        return;
    case kIdLearnOpen:
        if (clicked) ShellExecuteW(window, L"open", DataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    case kIdLearnReset:
        if (clicked) OnLearnReset();
        return;
    case kIdLogEnabled:
        if (clicked) Update([&](iroha::Config& c) { c.conversionLog = Checked(control); });
        RefreshLearningCounts();
        return;
    case kIdLogScope:
        if (selected) Update([&](iroha::Config& c) { c.conversionLogCorrectionsOnly = comboIndex() == 1; });
        return;
    case kIdLogOpen: {
        if (!clicked) return;
        std::error_code error;
        std::filesystem::create_directories(ConversionLogDirectory(), error);
        ShellExecuteW(window, L"open", ConversionLogDirectory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    case kIdLogReset:
        if (clicked) OnLogReset();
        return;
    case kIdSymbols:
        if (clicked) Update([&](iroha::Config& c) { c.symbolCandidates = Checked(control); });
        return;
    // ユーザ辞書・専門辞書・変換ルール
    case kIdDictAdd:
        if (clicked) OnDictAdd();
        return;
    case kIdDictRemove:
        if (clicked) OnDictRemove();
        return;
    case kIdTermAdd:
    case kIdTermRemove: {
        if (!clicked) return;
        const int row = ListView_GetNextItem(termList, -1, LVNI_SELECTED);
        if (row < 0 || row >= static_cast<int>(termRows.size())) return;
        if (id == kIdTermAdd) {
            StartTermTask(termRows[row].name + L" を取得しています…", false, termRows[row].id, {});
        } else {
            StartTermTask(termRows[row].name + L" を外しています…", false, {}, termRows[row].id);
        }
        return;
    }
    case kIdRuleAdd:
        if (clicked) OnRuleAdd();
        return;
    case kIdRuleRemove:
        if (clicked) OnRuleRemove();
        return;
    // 選択テキスト
    case kIdSelEnabled:
        if (clicked) Update([&](iroha::Config& c) { c.selectionEnabled = Checked(control); });
        return;
    case kIdSelTrigger:
        if (selected && comboIndex() >= 0) Update([&](iroha::Config& c) { c.selectionTriggerMode = kTriggerModes[comboIndex()]; });
        return;
    case kIdSelCount:
        if (clicked) Update([&](iroha::Config& c) { c.selectionCharacterCount = Checked(control); });
        return;
    case kIdSelOnDemand:
    case kIdSelExcluded:
        if (left) SaveSelectionTexts();
        return;
    // AI
    case kIdAIService:
        if (selected) OnAIServiceChanged();
        return;
    case kIdAIEndpoint:
        if (left) SaveAIPresets();
        return;
    case kIdAIModel:
        if (left || selected) SaveAIPresets();
        return;
    case kIdAIModels:
        if (clicked) OnAIListModels();
        return;
    case kIdAIKeySave:
        if (clicked) OnAIKeySave();
        return;
    case kIdAIKeyClear:
        if (clicked) OnAIKeyClear();
        return;
    // モデル
    case kIdModelCombo:
        if (selected) RefreshModelPage();
        return;
    case kIdModelDownload:
        if (clicked) OnModelDownload();
        return;
    case kIdModelApply:
        if (clicked) OnModelApply();
        return;
    case kIdModelFile:
        if (clicked) OnChooseFile(false);
        return;
    case kIdModelFileDefault:
        if (!clicked) return;
        Update([](iroha::Config& c) { c.model.clear(); });
        RefreshModelPage();
        return;
    case kIdModelFolder: {
        if (!clicked) return;
        std::error_code error;
        std::filesystem::create_directories(ModelPath(L"").parent_path(), error);
        ShellExecuteW(window, L"open", ModelPath(L"").parent_path().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    case kIdAdapterFile:
        if (clicked) OnChooseFile(true);
        return;
    case kIdAdapterClear:
        if (!clicked) return;
        Update([](iroha::Config& c) { c.modelAdapterPath.clear(); });
        RefreshModelPage();
        return;
    // 情報
    case kIdGitHub:
        if (clicked) ShellExecuteW(window, L"open", L"https://github.com/TetsuakiBaba/iroha", nullptr, nullptr, SW_SHOWNORMAL);
        return;
    case kIdDataFolder:
        if (clicked) ShellExecuteW(window, L"open", DataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    case kIdDeveloperOverlay:
        if (clicked) Update([&](iroha::Config& c) { c.developerOverlay = Checked(control); });
        return;
    case kIdAutoUpdate:
        if (clicked) Update([&](iroha::Config& c) { c.autoUpdateCheck = Checked(control); }, /*reload=*/false);
        return;
    case kIdCheckUpdate:
        if (!clicked) return;
        if (updater::CheckAndOffer(window, config, std::filesystem::path(DataDir()), IROHA_VERSION, /*manual=*/true) ==
            updater::Result::Launched) {
            // インストーラーが設定画面も入れ替えるので閉じる
            DestroyWindow(window);
        }
        return;
    case kIdDeveloperMode:
        if (!clicked) return;
        // 開発者モードは表示を変え、デバッグ表示を出すかも決める（変換サーバに読み直させる）
        Update([&](iroha::Config& c) { c.developerMode = Checked(control); });
        developerMode = config.developerMode;
        RelayoutAll();
        return;
    default:
        break;
    }
    // プリセットの欄（選択テキスト・AI変換）
    if (id >= kIdSelPresetEnabled0 && id < kIdSelPresetEnabled0 + 5) {
        if (!clicked) return;
        const size_t index = static_cast<size_t>(id - kIdSelPresetEnabled0);
        Update([&](iroha::Config& c) { c.selectionPresets[index].enabled = Checked(control); });
        return;
    }
    if (id >= kIdSelPresetName0 && id < kIdSelEnd) {
        if (left) SaveSelectionTexts();
        return;
    }
    if (id >= kIdAIPresetReset0 && id < kIdAIEnd) {
        if (!clicked) return;
        const size_t index = static_cast<size_t>(id - kIdAIPresetReset0);
        const auto& fallback = iroha::AICommitSettings::Defaults()[index];
        SetWindowTextW(aiPresetName[index], Wide(fallback.name).c_str());
        SetWindowTextW(aiPresetHotkey[index], Wide(fallback.shortcut).c_str());
        SetWindowTextW(aiPresetPrompt[index], ToEditText(fallback.prompt).c_str());
        SaveAIPresets();
        return;
    }
    if (id >= kIdAIPresetName0 && id < kIdAIPresetReset0) {
        if (left) SaveAIPresets();
        return;
    }
}

// スライダーを動かした（離したときに保存する）
void App::OnSlider(HWND control, bool finished) {
    const int position = static_cast<int>(SendMessageW(control, TBM_GETPOS, 0, 0));
    ui::Page* input = PageFor(L"input");
    if (control == typoDelay) {
        const int milliseconds = position * 50;
        input->SetDescription(typoDelayRow, std::to_wstring(milliseconds) + L" ms");
        if (finished) Update([&](iroha::Config& c) { c.typoDelayMs = milliseconds; });
    } else if (control == typoThreshold) {
        const double threshold = position / 2.0;
        input->SetDescription(typoThresholdRow, FormatNumber(threshold, 1) + L"（" + TypoThresholdLabel(threshold) + L"）");
        if (finished) {
            Update([&](iroha::Config& c) { c.typoThreshold = threshold; });
            EnableWindow(typoThresholdDefault, std::abs(threshold - iroha::TypoNormalizer::kDefaultThreshold) > 1e-9);
        }
    } else if (control == aiFixThreshold) {
        const double threshold = position / 4.0;
        input->SetDescription(aiFixThresholdRow, FormatNumber(threshold, 2));
        if (finished) Update([&](iroha::Config& c) { c.aiFixThreshold = threshold; });
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    App& app = g_app;
    switch (message) {
    case WM_CREATE:
        app.window = hwnd;
        g_theme.Rebuild(GetDpiForWindow(hwnd));
        app.Build();
        app.Layout();
        app.RefreshDictionaryList();
        app.RefreshRuleList();
        app.RefreshTermList();
        app.ShowPage(0);
        return 0;
    case WM_SIZE:
        if (!app.nav.empty()) app.Layout();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize = {g_theme.S(820), g_theme.S(520)};
        return 0;
    }
    case WM_DPICHANGED: {
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        app.OnDpiChanged();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC target = BeginPaint(hwnd, &paint);
        RECT client = {};
        GetClientRect(hwnd, &client);
        const int width = g_theme.S(260);
        HDC dc = CreateCompatibleDC(target);
        HBITMAP bitmap = CreateCompatibleBitmap(target, width, client.bottom);
        HGDIOBJ old = SelectObject(dc, bitmap);
        app.PaintNav(dc);
        BitBlt(target, 0, 0, width, client.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, old);
        DeleteObject(bitmap);
        DeleteDC(dc);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        for (size_t i = 0; i < app.nav.size(); ++i) {
            if (PtInRect(&app.nav[i].rect, point)) app.ShowPage(static_cast<int>(i));
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        const POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        int hover = -1;
        for (size_t i = 0; i < app.nav.size(); ++i) {
            if (PtInRect(&app.nav[i].rect, point)) hover = static_cast<int>(i);
        }
        if (hover != app.hoverNav) {
            app.hoverNav = hover;
            TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&track);
            RECT pane = {0, 0, g_theme.S(260), 10000};
            InvalidateRect(hwnd, &pane, FALSE);
        }
        SetCursor(LoadCursorW(nullptr, hover >= 0 ? IDC_HAND : IDC_ARROW));
        return 0;
    }
    case WM_MOUSELEAVE:
        app.hoverNav = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_NOTIFY: {
        const NMHDR* header = reinterpret_cast<NMHDR*>(lParam);
        if (header->hwndFrom == app.termList && header->code == LVN_ITEMCHANGED) app.ShowTermDetail();
        if (header->hwndFrom == app.ruleList && header->code == LVN_ITEMCHANGED) {
            const auto* change = reinterpret_cast<const NMLISTVIEW*>(lParam);
            if ((change->uChanged & LVIF_STATE) && ((change->uNewState ^ change->uOldState) & LVIS_STATEIMAGEMASK)) {
                app.OnRuleChecked(change->iItem);
            }
        }
        return 0;
    }
    case WM_HSCROLL:
        if (lParam) {
            const int code = LOWORD(wParam);
            app.OnSlider(reinterpret_cast<HWND>(lParam), code == TB_ENDTRACK || code == TB_THUMBPOSITION);
        }
        return 0;
    case WM_COMMAND:
        app.OnCommand(LOWORD(wParam), HIWORD(wParam), reinterpret_cast<HWND>(lParam));
        return 0;
    case WM_APP_TERM_DONE:
        app.OnTermTaskDone(reinterpret_cast<TermTaskResult*>(lParam));
        return 0;
    case WM_APP_AI_MODELS:
        app.OnAIModelsDone(reinterpret_cast<AIModelsResult*>(lParam));
        return 0;
    case WM_APP_TYPO_DONE:
        app.OnTypoTaskDone(reinterpret_cast<TypoTaskResult*>(lParam));
        return 0;
    case WM_APP_DOWNLOAD_DONE:
        app.downloading = false;
        app.RefreshModelPage();
        if (!wParam) {
            MessageBoxW(hwnd, L"ダウンロードに失敗しました。ネットワークを確認してください。", L"iroha", MB_ICONERROR);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int showCommand) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    const std::wstring arguments = commandLine ? commandLine : L"";
    // --check-update: 変換サーバが 1 日 1 回呼ぶ自動の確認（設定の窓は開かない。新しい版があれば知らせるだけ）
    if (arguments.find(L"--check-update") != std::wstring::npos) {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        updater::CheckAndOffer(nullptr, iroha::LoadConfig(ConfigPath()), std::filesystem::path(DataDir()), IROHA_VERSION,
                               /*manual=*/false);
        if (SUCCEEDED(com)) CoUninitialize();
        return 0;
    }
    Gdiplus::GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr);
    ui::RegisterClasses(instance);

    // 変換ルールは変換サーバと同じく、初めてなら既定のルール（きょう・あした など）を用意する
    g_app.rewriteRules = std::make_unique<iroha::UserRewriteRuleStore>(
        std::filesystem::path(DataDir() + L"\\user-rewrite-rules.json"), /*seedsDefaults=*/true);
    g_app.inputHistory = std::make_unique<iroha::InputHistoryStore>(std::filesystem::path(DataDir() + L"\\input-history"),
                                                                    HostName(), /*saveDelayMs=*/0);
    g_app.conversionLog = std::make_unique<iroha::ConversionLog>(ConversionLogDirectory(), HostName());

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"IrohaSettings";
    // アイコンは版の情報と一緒に入れた 1 番（iroha_version_resource）
    LoadIconMetric(instance, MAKEINTRESOURCEW(1), LIM_LARGE, &wc.hIcon);
    LoadIconMetric(instance, MAKEINTRESOURCEW(1), LIM_SMALL, &wc.hIconSm);
    RegisterClassExW(&wc);

    const UINT dpi = GetDpiForSystem();
    HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, L"IrohaSettings", L"iroha の設定",
                                  WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                  MulDiv(1040, static_cast<int>(dpi), 96), MulDiv(760, static_cast<int>(dpi), 96), nullptr,
                                  nullptr, instance, nullptr);
    if (!window) return 1;
    // --tab <名前> で開くページを選ぶ
    if (const size_t option = arguments.find(L"--tab "); option != std::wstring::npos) {
        const std::wstring name = arguments.substr(option + 6, arguments.find(L' ', option + 6) - option - 6);
        for (size_t i = 0; i < g_app.nav.size(); ++i) {
            if (name == g_app.nav[i].key) g_app.ShowPage(static_cast<int>(i));
        }
    }
    ShowWindow(window, showCommand);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0)) {
        if (IsDialogMessageW(window, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    g_theme.Release();
    Gdiplus::GdiplusShutdown(gdiplusToken);
    return 0;
}
