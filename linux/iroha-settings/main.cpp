// iroha の設定画面（Linux版、GTK4）。
// 移植元: windows/iroha-settings/main.cpp（コミット c12af85。タブ・項目・保存先を合わせる）。
// タブ: モデル（選択・ダウンロード） / 入力（ライブ変換・予測変換・句読点・候補の数・プライベートモード・
// 打ち間違いの訂正） / ユーザ辞書 / 専門辞書（配布している辞書の追加・削除） / 変換ルール / AI（AI変換の接続先と
// プリセット） / 選択テキスト（選択テキストのAI編集） / 学習・記録 / 情報（版・データフォルダ・ライセンス）。
// 保存先は <データフォルダ>（config.json / user-dictionary.json / dictionaries/ / user-rewrite-rules.json /
// learning.json / input-history/ / logs/conversions/ / models/）。API キーは秘密の保管庫（libsecret）。
// 保存したら platform::RequestReload で IBus のエンジンに読み直してもらう（Windows版の IPC の Reload にあたる）。
//
// 時間のかかる処理（取得・照合・AI のモデルの一覧・秘密の保管庫の読み書き）は裏のスレッドで行い、
// 画面の更新は RunOnMain（g_idle_add）で main thread に戻す。**秘密の保管庫を main thread で読まない**
// （鍵が掛かっていると答えるまで止まる。CLAUDE.md の SecretStore の注意と同じ）

#include <gtk/gtk.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "ai_client.h"
#include "iroha/ai_conversion.h"
#include "iroha/config.h"
#include "iroha/conversion_log.h"
#include "iroha/input_history.h"
#include "iroha/learning_store.h"
#include "iroha/platform.h"
#include "iroha/term_dictionary.h"
#include "iroha/typo_normalizer_catalog.h"
#include "iroha/unicode.h"
#include "iroha/user_dictionary_store.h"
#include "iroha/user_rewrite_rule.h"
#include "key_shortcut.h"
#include "secret_store.h"
#include "settings_support.h"
#include "term_dictionary_fetcher.h"
#include "typo_normalizer_fetcher.h"

#ifndef IROHA_VERSION
#define IROHA_VERSION "dev"
#endif

namespace {

using iroha::settings::Tab;

constexpr char kApplicationId[] = "jp.iroha.Settings";

// 句読点の組（設定の値と表示）
const char* const kPunctuations[] = {"、。", "，．"};
// 候補の数の範囲（Windows版の設定画面と同じ）
constexpr int kMinCandidateCount = 3;
constexpr int kMaxCandidateCount = 16;
// AI の接続先の並び（選択欄の順）
const iroha::AIService kAIServices[] = {iroha::AIService::None, iroha::AIService::Ollama, iroha::AIService::LMStudio,
                                        iroha::AIService::OpenAI, iroha::AIService::Claude};
constexpr int kAIServiceCount = static_cast<int>(sizeof(kAIServices) / sizeof(kAIServices[0]));

// ---- 文字列の補助 ----

std::string U8(const std::u32string& text) { return iroha::Utf32ToUtf8(text); }
std::u32string U32(const std::string& text) { return iroha::Utf8ToUtf32(text); }
std::string Trimmed(const std::string& text) { return U8(iroha::TrimWhitespace(U32(text))); }

std::string Escape(const std::string& text) {
    gchar* escaped = g_markup_escape_text(text.c_str(), -1);
    std::string result = escaped;
    g_free(escaped);
    return result;
}

// ---- main thread に戻す ----

// 設定の窓があるか（窓を閉じたあとに裏のスレッドから戻ってきた知らせは、消えた部品に触らないよう捨てる）
bool g_windowAlive = false;

void RunOnMain(std::function<void()> function) {
    g_idle_add(
        [](gpointer data) -> gboolean {
            std::unique_ptr<std::function<void()>> function(static_cast<std::function<void()>*>(data));
            if (g_windowAlive) (*function)();
            return G_SOURCE_REMOVE;
        },
        new std::function<void()>(std::move(function)));
}

// ---- シグナル（std::function で受ける） ----

using Handler = std::function<void()>;

void DestroyHandler(gpointer data, GClosure*) { delete static_cast<Handler*>(data); }
// (instance, user_data) の形のシグナル（clicked・toggled・activate・selected-rows-changed）
void OnSignal(gpointer, gpointer data) { (*static_cast<Handler*>(data))(); }
// (instance, pspec, user_data) の形のシグナル（notify::…）
void OnNotify(gpointer, GParamSpec*, gpointer data) { (*static_cast<Handler*>(data))(); }

void Connect(gpointer instance, const char* signal, Handler handler) {
    g_signal_connect_data(instance, signal, G_CALLBACK(OnSignal), new Handler(std::move(handler)), DestroyHandler,
                          static_cast<GConnectFlags>(0));
}
void ConnectNotify(gpointer instance, const char* property, Handler handler) {
    const std::string signal = std::string("notify::") + property;
    g_signal_connect_data(instance, signal.c_str(), G_CALLBACK(OnNotify), new Handler(std::move(handler)),
                          DestroyHandler, static_cast<GConnectFlags>(0));
}

// ---- 部品の補助 ----

GtkWidget* Label(const std::string& text, bool wrap = false) {
    GtkWidget* label = gtk_label_new(text.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    if (wrap) {
        gtk_label_set_wrap(GTK_LABEL(label), TRUE);
        gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    }
    return label;
}

GtkWidget* Box(GtkOrientation orientation, int spacing) { return gtk_box_new(orientation, spacing); }
GtkWidget* Row() { return Box(GTK_ORIENTATION_HORIZONTAL, 8); }

void Append(GtkWidget* box, std::initializer_list<GtkWidget*> children) {
    for (GtkWidget* child : children) gtk_box_append(GTK_BOX(box), child);
}

GtkWidget* Entry(const char* placeholder = nullptr, int widthChars = -1) {
    GtkWidget* entry = gtk_entry_new();
    if (placeholder) gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder);
    if (widthChars > 0) gtk_editable_set_width_chars(GTK_EDITABLE(entry), widthChars);
    return entry;
}

std::string EditableText(GtkWidget* editable) { return gtk_editable_get_text(GTK_EDITABLE(editable)); }
void SetEditableText(GtkWidget* editable, const std::string& text) {
    gtk_editable_set_text(GTK_EDITABLE(editable), text.c_str());
}

std::string TextViewText(GtkWidget* view) {
    GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    GtkTextIter start;
    GtkTextIter end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    gchar* text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    std::string result = text;
    g_free(text);
    return result;
}
void SetTextViewText(GtkWidget* view, const std::string& text) {
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)), text.c_str(), -1);
}

GtkWidget* DropDown(const std::vector<std::string>& items) {
    std::vector<const char*> strings;
    for (const std::string& item : items) strings.push_back(item.c_str());
    strings.push_back(nullptr);
    return gtk_drop_down_new_from_strings(strings.data());
}
int Selected(GtkWidget* dropDown) {
    const guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropDown));
    return selected == GTK_INVALID_LIST_POSITION ? -1 : static_cast<int>(selected);
}

// 長い説明でも窓の幅を押し広げないよう、文字を折り返すチェックボックス
// （GTK 4.6 には gtk_check_button_set_child が無いので、中のラベルを探して折り返しを付ける）
GtkWidget* CheckButton(const char* text) {
    GtkWidget* check = gtk_check_button_new_with_label(text);
    for (GtkWidget* child = gtk_widget_get_first_child(check); child; child = gtk_widget_get_next_sibling(child)) {
        if (!GTK_IS_LABEL(child)) continue;
        gtk_label_set_wrap(GTK_LABEL(child), TRUE);
        gtk_label_set_wrap_mode(GTK_LABEL(child), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_xalign(GTK_LABEL(child), 0);
    }
    return check;
}

bool Checked(GtkWidget* check) { return gtk_check_button_get_active(GTK_CHECK_BUTTON(check)); }

// 縦に流れるタブの中身（窓が狭いときは縦に送れるように、送り枠に入れる）
GtkWidget* PageBox(GtkWidget** scrolled) {
    GtkWidget* box = Box(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 14);
    gtk_widget_set_margin_bottom(box, 14);
    *scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(*scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(*scrolled), box);
    return box;
}

// 一覧（行を選べる箱）と、その送り枠
GtkWidget* ListBox(GtkSelectionMode mode, GtkWidget** scrolled, int minimumHeight = 160) {
    GtkWidget* list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), mode);
    *scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(*scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(*scrolled), list);
    gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(*scrolled), TRUE);
    gtk_widget_set_vexpand(*scrolled, TRUE);
    gtk_widget_set_size_request(*scrolled, -1, minimumHeight);
    return list;
}

void ClearList(GtkWidget* list) {
    while (GtkWidget* child = gtk_widget_get_first_child(list)) gtk_list_box_remove(GTK_LIST_BOX(list), child);
}

std::vector<int> SelectedRows(GtkWidget* list) {
    std::vector<int> rows;
    GList* selected = gtk_list_box_get_selected_rows(GTK_LIST_BOX(list));
    for (GList* item = selected; item; item = item->next) {
        rows.push_back(gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(item->data)));
    }
    g_list_free(selected);
    std::sort(rows.begin(), rows.end());
    return rows;
}

// 一覧の 1 行（列の幅は文字数で決める。0 なら残りの幅を使う）
struct Cell {
    std::string text;
    int widthChars;
};
GtkWidget* ListRow(const std::vector<Cell>& cells, bool heading = false) {
    GtkWidget* row = Box(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(row, 6);
    gtk_widget_set_margin_end(row, 6);
    gtk_widget_set_margin_top(row, 3);
    gtk_widget_set_margin_bottom(row, 3);
    for (const Cell& cell : cells) {
        GtkWidget* label = gtk_label_new(nullptr);
        if (heading) {
            gtk_label_set_markup(GTK_LABEL(label), ("<b>" + Escape(cell.text) + "</b>").c_str());
        } else {
            gtk_label_set_text(GTK_LABEL(label), cell.text.c_str());
        }
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        if (cell.widthChars > 0) {
            gtk_label_set_width_chars(GTK_LABEL(label), cell.widthChars);
            gtk_label_set_max_width_chars(GTK_LABEL(label), cell.widthChars);
        } else {
            gtk_widget_set_hexpand(label, TRUE);
        }
        gtk_box_append(GTK_BOX(row), label);
    }
    return row;
}

// 一覧の見出し（一覧の上に置く）
GtkWidget* ListHeading(const std::vector<Cell>& cells, int leadingSpace = 0) {
    GtkWidget* row = ListRow(cells, /*heading=*/true);
    if (leadingSpace > 0) gtk_widget_set_margin_start(row, 6 + leadingSpace);
    return row;
}

std::string TermDetailHint() {
    return "候補ウィンドウに、選んだ分野の用語・人名・地名などを加えます（ライブ変換は変えません）。\n"
           "一覧から辞書を選ぶと、説明と出典・ライセンスが出ます。";
}

// 専門辞書の一覧の 1 行（カタログの辞書か、カタログに無いが入れてある辞書）
struct TermRow {
    std::string id;
    std::string name;
    std::string group;
    std::string summary;
    uint64_t entries = 0;
    std::string license;
    std::vector<iroha::TermDictionaryCatalog::Source> sources;
    std::vector<iroha::TermDictionaryCatalog::Sample> samples;
    bool inCatalog = false;
    bool installed = false;
    bool needsUpdate = false;
};

struct App {
    GtkApplication* application = nullptr;
    GtkWindow* window = nullptr;
    GtkWidget* notebook = nullptr;
    // 値を画面に写している間は、変更の知らせで保存しない
    bool updating = false;

    // モデル
    GtkWidget* modelDrop = nullptr;
    GtkWidget* modelStatus = nullptr;
    GtkWidget* modelProgress = nullptr;
    GtkWidget* modelDownload = nullptr;
    GtkWidget* modelCancel = nullptr;
    GtkWidget* modelApply = nullptr;
    bool downloading = false;
    std::shared_ptr<std::atomic<bool>> downloadCancel;

    // 入力
    GtkWidget* liveCheck = nullptr;
    GtkWidget* predictiveCheck = nullptr;
    GtkWidget* symbolsCheck = nullptr;
    GtkWidget* spaceCheck = nullptr;
    GtkWidget* punctuationDrop = nullptr;
    GtkWidget* countDrop = nullptr;
    GtkWidget* privateCheck = nullptr;
    GtkWidget* delimitedCheck = nullptr;
    GtkWidget* aiFixCheck = nullptr;
    // 打ち間違いの訂正
    GtkWidget* typoCheck = nullptr;
    GtkWidget* typoStatus = nullptr;
    GtkWidget* typoLicense = nullptr;
    GtkWidget* typoRemove = nullptr;
    bool typoBusy = false;
    bool typoChecked = false; // 設定画面を開いてから、モデルの更新を確かめたか

    // ユーザ辞書
    GtkWidget* dictList = nullptr;
    GtkWidget* dictReading = nullptr;
    GtkWidget* dictWord = nullptr;
    GtkWidget* dictRemove = nullptr;

    // 専門辞書
    GtkWidget* termList = nullptr;
    GtkWidget* termAdd = nullptr;
    GtkWidget* termRemove = nullptr;
    GtkWidget* termStatus = nullptr;
    GtkWidget* termDetail = nullptr;
    std::unique_ptr<iroha::TermDictionaryCatalog> termCatalog;
    std::vector<TermRow> termRows;
    bool termBusy = false;
    bool termCatalogRequested = false;

    // 変換ルール
    GtkWidget* ruleList = nullptr;
    GtkWidget* ruleTrigger = nullptr;
    GtkWidget* ruleOutput = nullptr;
    GtkWidget* ruleRemove = nullptr;
    std::vector<iroha::UserRewriteRule> rules; // 一覧の行と同じ並び

    // AI
    GtkWidget* aiServiceDrop = nullptr;
    GtkWidget* aiEndpoint = nullptr;
    GtkWidget* aiModel = nullptr;
    GtkWidget* aiModelChoices = nullptr;
    GtkWidget* aiModels = nullptr;
    GtkWidget* aiKey = nullptr;
    GtkWidget* aiKeyClear = nullptr;
    GtkWidget* aiStatus = nullptr;
    GtkWidget* aiPresetName[iroha::AICommitSettings::kCount] = {};
    GtkWidget* aiPresetHotkey[iroha::AICommitSettings::kCount] = {};
    GtkWidget* aiPresetPrompt[iroha::AICommitSettings::kCount] = {};
    int aiShownService = -1;     // URL・モデルの欄に出している接続先（切り替えたときに書き戻す先）
    unsigned keyGeneration = 0;  // API キーの状態の問い合わせの番号（古い答えを捨てる）
    std::vector<std::string> aiModelList;

    // 選択テキスト
    GtkWidget* selEnabled = nullptr;
    GtkWidget* selOnDemand = nullptr;
    GtkWidget* selPresetEnabled[iroha::SelectionSettingsDefaults::kCount] = {};
    GtkWidget* selPresetName[iroha::SelectionSettingsDefaults::kCount] = {};
    GtkWidget* selPresetHotkey[iroha::SelectionSettingsDefaults::kCount] = {};
    GtkWidget* selPresetPrompt[iroha::SelectionSettingsDefaults::kCount] = {};
    GtkWidget* selExcluded = nullptr;
    GtkWidget* selStatus = nullptr;

    // 学習・記録
    GtkWidget* learnStatus = nullptr;
    GtkWidget* historyStatus = nullptr;
    GtkWidget* logCheck = nullptr;
    GtkWidget* logScopeDrop = nullptr;
    GtkWidget* logStatus = nullptr;
    GtkWidget* logReset = nullptr;

    // 情報
    GtkWidget* licenseBox = nullptr;

    std::unique_ptr<iroha::UserDictionaryStore> dictionary;
    std::unique_ptr<iroha::LearningStore> learning;
    std::unique_ptr<iroha::UserRewriteRuleStore> rewriteRules;
    std::unique_ptr<iroha::InputHistoryStore> inputHistory;
    std::unique_ptr<iroha::ConversionLog> conversionLog;

    void Build();
    GtkWidget* BuildModelPage();
    GtkWidget* BuildInputPage();
    GtkWidget* BuildDictionaryPage();
    GtkWidget* BuildTermPage();
    GtkWidget* BuildRulePage();
    GtkWidget* BuildAIPage();
    GtkWidget* BuildSelectionPage();
    GtkWidget* BuildLearningPage();
    GtkWidget* BuildAboutPage();
    void OnTabShown(Tab tab);

    void ShowDialog(const std::string& text, const char* acceptLabel = nullptr, Handler onAccept = nullptr);

    void RefreshModelPage();
    void OnModelDownload();
    void OnModelApply();

    void RefreshInputPage();
    void SaveInputPage();
    void RefreshTypoStatus();
    void OnTypoToggled();
    void StartTypoTask(bool announce);
    void OnTypoLicense();
    void OnTypoRemove();

    void RefreshDictionaryList();
    void OnDictAdd();
    void OnDictRemove();

    void RefreshTermList();
    void ShowTermDetail();
    void StartTermTask(const std::string& busyMessage, bool fetchCatalog, std::string installId, std::string removeId);

    void RefreshRuleList();
    void OnRuleAdd();
    void OnRuleRemove();
    void OnRuleChecked(size_t index, bool enabled);

    void RefreshAIPage();
    void ShowAIServiceFields(int index, const iroha::Config& config);
    void StoreAIServiceFields(int index, iroha::Config* config);
    void OnAIServiceChanged();
    void OnAISave();
    void OnAIKeyClear();
    void OnAIListModels();
    void RefreshAIKeyStatus(iroha::AIService service);

    void RefreshSelectionPage();
    void OnSelectionSave();

    void RefreshLearningPage();
    void OnLearnReset();
    void OnHistoryReset();
    void SaveLogSettings();
    void OnLogReset();

    void RefreshAboutPage();
};

App* g_app = nullptr;

// フォルダをファイルマネージャで開く
void OpenFolder(const std::filesystem::path& folder) {
    std::error_code ignored;
    std::filesystem::create_directories(folder, ignored);
    gchar* uri = g_filename_to_uri(iroha::PathToUtf8(folder).c_str(), nullptr, nullptr);
    if (!uri) return;
    GError* error = nullptr;
    if (!g_app_info_launch_default_for_uri(uri, nullptr, &error)) {
        g_app->ShowDialog("フォルダを開けませんでした: " + std::string(error ? error->message : "") + "\n" +
                          iroha::PathToUtf8(folder));
        if (error) g_error_free(error);
    }
    g_free(uri);
}

gboolean OnDialogKey(GtkEventControllerKey*, guint keyval, guint, GdkModifierType, gpointer dialog) {
    if (keyval != GDK_KEY_Escape) return FALSE;
    gtk_window_destroy(GTK_WINDOW(dialog));
    return TRUE;
}

// 知らせ（acceptLabel が無ければ「閉じる」だけ）・確かめ（acceptLabel を押したら onAccept）
void App::ShowDialog(const std::string& text, const char* acceptLabel, Handler onAccept) {
    GtkWidget* dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "iroha");
    gtk_window_set_transient_for(GTK_WINDOW(dialog), window);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    GtkWidget* box = Box(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_start(box, 20);
    gtk_widget_set_margin_end(box, 20);
    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 14);
    GtkWidget* label = Label(text, /*wrap=*/true);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 60);
    gtk_label_set_width_chars(GTK_LABEL(label), 36);
    GtkWidget* buttons = Row();
    gtk_widget_set_halign(buttons, GTK_ALIGN_END);
    if (acceptLabel) {
        GtkWidget* cancel = gtk_button_new_with_label("キャンセル");
        GtkWidget* accept = gtk_button_new_with_label(acceptLabel);
        Connect(cancel, "clicked", [dialog] { gtk_window_destroy(GTK_WINDOW(dialog)); });
        Connect(accept, "clicked", [dialog, onAccept] {
            const Handler action = onAccept;
            gtk_window_destroy(GTK_WINDOW(dialog));
            if (action) action();
        });
        Append(buttons, {cancel, accept});
        gtk_window_set_default_widget(GTK_WINDOW(dialog), cancel);
        // 文の全体が選ばれた状態で開かないよう、ボタンに入力を向ける
        gtk_window_set_focus(GTK_WINDOW(dialog), cancel);
    } else {
        GtkWidget* close = gtk_button_new_with_label("閉じる");
        Connect(close, "clicked", [dialog] { gtk_window_destroy(GTK_WINDOW(dialog)); });
        Append(buttons, {close});
        gtk_window_set_default_widget(GTK_WINDOW(dialog), close);
        gtk_window_set_focus(GTK_WINDOW(dialog), close);
    }
    Append(box, {label, buttons});
    gtk_window_set_child(GTK_WINDOW(dialog), box);
    GtkEventController* keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(OnDialogKey), dialog);
    gtk_widget_add_controller(dialog, keys);
    gtk_window_present(GTK_WINDOW(dialog));
}

// ---- 組み立て ----

void App::Build() {
    window = GTK_WINDOW(gtk_application_window_new(application));
    gtk_window_set_title(window, "iroha の設定");
    gtk_window_set_default_size(window, 760, 640);
    g_windowAlive = true;
    Connect(window, "destroy", [] { g_windowAlive = false; });
    notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook), TRUE);
    GtkWidget* pages[iroha::settings::kTabCount] = {
        BuildModelPage(), BuildInputPage(),     BuildDictionaryPage(), BuildTermPage(),  BuildRulePage(),
        BuildAIPage(),    BuildSelectionPage(), BuildLearningPage(),   BuildAboutPage(),
    };
    for (int i = 0; i < iroha::settings::kTabCount; ++i) {
        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), pages[i],
                                 gtk_label_new(iroha::settings::TabTitle(static_cast<Tab>(i))));
    }
    g_signal_connect(notebook, "switch-page", G_CALLBACK(+[](GtkNotebook*, GtkWidget*, guint page, gpointer) {
                         g_app->OnTabShown(static_cast<Tab>(page));
                     }),
                     nullptr);
    gtk_window_set_child(window, notebook);

    // 開いたときに全部のページを今の値にする（Windows版と同じ）
    RefreshModelPage();
    RefreshDictionaryList();
    RefreshTermList();
    RefreshInputPage();
    RefreshRuleList();
    RefreshLearningPage();
}

// タブを開くたびに読み直す（件数・設定は、ほかのところ（IBus のエンジン）でも変わる）
void App::OnTabShown(Tab tab) {
    switch (tab) {
        case Tab::Model:
            RefreshModelPage();
            break;
        case Tab::Input:
            RefreshInputPage();
            break;
        case Tab::Learning:
            RefreshLearningPage();
            break;
        case Tab::AI:
            RefreshAIPage();
            break;
        case Tab::Selection:
            RefreshSelectionPage();
            break;
        case Tab::About:
            RefreshAboutPage();
            break;
        case Tab::TermDictionary:
            // 専門辞書の一覧は、タブを初めて開いたときに取りにいく（入れてある辞書が作り直されていれば入れ直す）
            if (!termCatalogRequested) {
                termCatalogRequested = true;
                StartTermTask("辞書の一覧を取得しています…", true, {}, {});
            }
            break;
        default:
            break;
    }
}

// ---- モデル ----

GtkWidget* App::BuildModelPage() {
    GtkWidget* scrolled = nullptr;
    GtkWidget* page = PageBox(&scrolled);
    std::vector<std::string> labels;
    for (const auto& model : iroha::settings::KnownModels()) labels.push_back(model.label);
    modelDrop = DropDown(labels);
    modelStatus = Label("", true);
    modelProgress = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(modelProgress), TRUE);
    gtk_widget_set_visible(modelProgress, FALSE);
    modelDownload = gtk_button_new_with_label("ダウンロード");
    modelCancel = gtk_button_new_with_label("取得を止める");
    gtk_widget_set_visible(modelCancel, FALSE);
    modelApply = gtk_button_new_with_label("このモデルを使う");
    GtkWidget* buttons = Row();
    Append(buttons, {modelDownload, modelCancel, modelApply});
    GtkWidget* note = Label("モデルはデータフォルダの models/ に置きます（大きさと SHA-256 を照合してから置きます）。\n"
                            "切り替えると iroha の入力エンジンがモデルを読み直します（数秒かかります）。",
                            true);
    gtk_widget_add_css_class(note, "dim-label");
    Append(page, {Label("変換モデル:"), modelDrop, modelStatus, modelProgress, buttons, note});

    const iroha::Config config = iroha::LoadConfig(iroha::settings::ConfigPath());
    if (const auto index = iroha::settings::CurrentModelIndex(config)) {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(modelDrop), static_cast<guint>(*index));
    }
    ConnectNotify(modelDrop, "selected", [this] { RefreshModelPage(); });
    Connect(modelDownload, "clicked", [this] { OnModelDownload(); });
    Connect(modelCancel, "clicked", [this] {
        if (downloadCancel) downloadCancel->store(true);
    });
    Connect(modelApply, "clicked", [this] { OnModelApply(); });
    return scrolled;
}

void App::RefreshModelPage() {
    const int index = Selected(modelDrop);
    const auto& models = iroha::settings::KnownModels();
    if (index < 0 || index >= static_cast<int>(models.size())) return;
    const auto& model = models[index];
    std::error_code ignored;
    const bool downloaded = std::filesystem::exists(iroha::settings::ModelsDirectory() / model.fileName, ignored);
    if (!downloading) {
        const iroha::Config config = iroha::LoadConfig(iroha::settings::ConfigPath());
        std::string status = downloaded ? "状態: ダウンロード済み" : "状態: 未ダウンロード";
        if (iroha::settings::CurrentModelFileName(config) == model.fileName) status += "（現在使用中）";
        gtk_label_set_text(GTK_LABEL(modelStatus), status.c_str());
    }
    gtk_widget_set_sensitive(modelDrop, !downloading);
    gtk_widget_set_sensitive(modelDownload, !downloaded && !downloading);
    gtk_widget_set_visible(modelCancel, downloading);
    gtk_widget_set_visible(modelProgress, downloading);
    gtk_widget_set_sensitive(modelApply, downloaded && !downloading);
}

// 取得と照合は裏のスレッドで行い、進み具合を 0.1 秒ごとに画面へ戻す
void App::OnModelDownload() {
    const int index = Selected(modelDrop);
    const auto& models = iroha::settings::KnownModels();
    if (downloading || index < 0 || index >= static_cast<int>(models.size())) return;
    const iroha::settings::KnownModel model = models[index];
    downloading = true;
    downloadCancel = std::make_shared<std::atomic<bool>>(false);
    gtk_label_set_text(GTK_LABEL(modelStatus), "状態: ダウンロード中…");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(modelProgress), 0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(modelProgress), "接続しています…");
    RefreshModelPage();
    std::thread([model, cancel = downloadCancel] {
        auto last = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        bool verifying = false;
        const auto progress = [&](uint64_t received, uint64_t total) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last >= std::chrono::milliseconds(100)) {
                last = now;
                const uint64_t whole = total > 0 ? total : model.bytes;
                RunOnMain([received, whole] {
                    if (!g_app->downloading) return;
                    const double fraction = whole > 0 ? std::min(1.0, static_cast<double>(received) / whole) : 0;
                    const std::string text =
                        iroha::settings::FormatBytes(received) + " / " + iroha::settings::FormatBytes(whole);
                    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(g_app->modelProgress), fraction);
                    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(g_app->modelProgress), text.c_str());
                });
            }
            if (!verifying && total > 0 && received >= total) {
                verifying = true;
                RunOnMain([] {
                    if (g_app->downloading) {
                        gtk_label_set_text(GTK_LABEL(g_app->modelStatus), "状態: 照合しています（SHA-256）…");
                    }
                });
            }
            return !cancel->load();
        };
        std::string error;
        const bool ok = iroha::settings::DownloadModel(model, iroha::settings::ModelsDirectory(), progress, &error);
        RunOnMain([ok, error] {
            g_app->downloading = false;
            g_app->downloadCancel.reset();
            g_app->RefreshModelPage();
            g_app->ShowDialog(ok ? "ダウンロードが完了しました。「このモデルを使う」で切り替えられます。"
                                 : "ダウンロードに失敗しました: " + error);
        });
    }).detach();
}

void App::OnModelApply() {
    const int index = Selected(modelDrop);
    const auto& models = iroha::settings::KnownModels();
    if (index < 0 || index >= static_cast<int>(models.size())) return;
    iroha::Config config = iroha::LoadConfig(iroha::settings::ConfigPath());
    config.model = U32(models[index].fileName);
    if (!iroha::SaveConfig(iroha::settings::ConfigPath(), config)) {
        ShowDialog("設定の保存に失敗しました");
        return;
    }
    iroha::platform::RequestReload();
    RefreshModelPage();
    ShowDialog("モデルを切り替えました。iroha の入力エンジンが読み直します（読み込みに数秒かかります）。");
}

// ---- 入力 ----

GtkWidget* App::BuildInputPage() {
    GtkWidget* scrolled = nullptr;
    GtkWidget* page = PageBox(&scrolled);
    liveCheck = CheckButton("ライブ変換（入力中に変換して見せる）");
    predictiveCheck = CheckButton("予測変換（入力履歴から続きの語を出す。Tab・↓ で選ぶ）");
    symbolsCheck = CheckButton("記号・顔文字・絵文字を候補ウィンドウに出す");
    spaceCheck =
        CheckButton("入力していないときの Space を全角スペースにする（Shift+Space は半角）");
    punctuationDrop = DropDown({kPunctuations[0], kPunctuations[1]});
    std::vector<std::string> counts;
    for (int count = kMinCandidateCount; count <= kMaxCandidateCount; ++count) counts.push_back(std::to_string(count));
    countDrop = DropDown(counts);
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_attach(GTK_GRID(grid), Label("句読点（Ctrl+. でも切り替え）:"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), punctuationDrop, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), Label("候補ウィンドウの候補の数:"), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), countDrop, 1, 1, 1, 1);
    privateCheck = CheckButton("プライベートモード（Ctrl+Alt+P でも切り替え）");
    // macOS版では開発者モードの項目。記号・キーは config.json の delimitedAlphabetStart / End・aiFixShortcut で変えられる
    delimitedCheck = CheckButton("区切り記号で囲んだ英字入力（kyouha_iPhone_wo → 今日はiPhoneを）");
    aiFixCheck = CheckButton(
        "AIで直す（誤変換の疑いがあるときに知らせ、Ctrl+Alt+A で AI に確かめさせる。接続先は AI のタブ）");

    typoCheck = CheckButton(
        "打ち間違いを直す（入力が止まったときに読みを直す。直した直後の Backspace で戻す）");
    typoStatus = Label("", true);
    gtk_widget_set_margin_start(typoStatus, 28);
    typoLicense = gtk_button_new_with_label("学習元とライセンス");
    typoRemove = gtk_button_new_with_label("モデルを削除");
    GtkWidget* typoButtons = Row();
    gtk_widget_set_margin_start(typoButtons, 28);
    Append(typoButtons, {typoLicense, typoRemove});

    GtkWidget* note = Label("プライベートモードの間は、学習・入力履歴・予測変換・変換記録・ユーザ辞書を止めます。\n"
                            "変更はすぐに反映されます（iroha の入力エンジンが読み直します）。",
                            true);
    gtk_widget_add_css_class(note, "dim-label");
    Append(page, {liveCheck, predictiveCheck, symbolsCheck, spaceCheck, grid, privateCheck, delimitedCheck, aiFixCheck,
                  typoCheck, typoStatus, typoButtons, note});

    for (GtkWidget* check : {liveCheck, predictiveCheck, symbolsCheck, spaceCheck, privateCheck, delimitedCheck,
                             aiFixCheck}) {
        Connect(check, "toggled", [this] {
            if (!updating) SaveInputPage();
        });
    }
    for (GtkWidget* drop : {punctuationDrop, countDrop}) {
        ConnectNotify(drop, "selected", [this] {
            if (!updating) SaveInputPage();
        });
    }
    Connect(typoCheck, "toggled", [this] {
        if (!updating) OnTypoToggled();
    });
    Connect(typoLicense, "clicked", [this] { OnTypoLicense(); });
    Connect(typoRemove, "clicked", [this] { OnTypoRemove(); });
    return scrolled;
}

void App::RefreshInputPage() {
    const iroha::Config config = iroha::LoadConfig(iroha::settings::ConfigPath());
    updating = true;
    gtk_check_button_set_active(GTK_CHECK_BUTTON(liveCheck), config.liveConversion);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(predictiveCheck), config.predictive);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(symbolsCheck), config.symbolCandidates);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(spaceCheck), config.fullWidthSpace);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(privateCheck), config.privateMode);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(delimitedCheck), config.delimitedAlphabet);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(aiFixCheck), config.aiFix);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(typoCheck), config.typoCorrection);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(punctuationDrop), U8(config.punctuation) == kPunctuations[1] ? 1 : 0);
    const int count = std::clamp(config.candidateCount, kMinCandidateCount, kMaxCandidateCount);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(countDrop), static_cast<guint>(count - kMinCandidateCount));
    updating = false;
    RefreshTypoStatus();
    // ON なら、開いたときに 1 回だけ新しい版のモデルが出ていないか確かめる（macOS版・Windows版と同じ）
    if (config.typoCorrection && !typoChecked && !typoBusy) {
        typoChecked = true;
        StartTypoTask(/*announce=*/false);
    }
}

// 入力のページの値を設定に書いて、IBus のエンジンに読み直してもらう（ほかの項目は今の設定のまま）
void App::SaveInputPage() {
    iroha::Config config = iroha::LoadConfig(iroha::settings::ConfigPath());
    config.liveConversion = Checked(liveCheck);
    config.predictive = Checked(predictiveCheck);
    config.symbolCandidates = Checked(symbolsCheck);
    config.fullWidthSpace = Checked(spaceCheck);
    config.privateMode = Checked(privateCheck);
    config.delimitedAlphabet = Checked(delimitedCheck);
    config.aiFix = Checked(aiFixCheck);
    config.typoCorrection = Checked(typoCheck);
    config.punctuation = U32(kPunctuations[Selected(punctuationDrop) == 1 ? 1 : 0]);
    const int count = Selected(countDrop);
    if (count >= 0) config.candidateCount = kMinCandidateCount + count;
    if (!iroha::SaveConfig(iroha::settings::ConfigPath(), config)) {
        ShowDialog("設定の保存に失敗しました");
        return;
    }
    iroha::platform::RequestReload();
}

// ---- 打ち間違いの訂正 ----

void App::RefreshTypoStatus() {
    const std::filesystem::path data = iroha::platform::DataDirectory();
    const bool installed = iroha::TypoNormalizerInstall::IsInstalled(data);
    const auto record = iroha::TypoNormalizerInstall::InstalledRecord(data);
    if (!typoBusy) {
        std::string text;
        if (installed) {
            text = "モデル: " + (record ? U8(record->name) + "（" + record->id + "）" : std::string("入れてあります"));
            if (record && !record->license.empty()) text += "  " + U8(record->license);
        } else {
            text = "ON にするとモデル（約 23MB）を取得します";
        }
        gtk_label_set_text(GTK_LABEL(typoStatus), text.c_str());
    }
    gtk_widget_set_sensitive(typoCheck, !typoBusy);
    gtk_widget_set_sensitive(typoLicense, installed && record && !record->sources.empty());
    gtk_widget_set_sensitive(typoRemove, installed && !typoBusy);
}

void App::OnTypoToggled() {
    SaveInputPage();
    if (Checked(typoCheck) && !typoBusy) {
        typoChecked = true;
        StartTypoTask(/*announce=*/true);
    }
}

// モデルの一覧を取り、入っていないか新しい版があれば取得して設置する（裏のスレッド）。
// 入れたら IBus のエンジンに読み直してもらう
void App::StartTypoTask(bool announce) {
    if (typoBusy) return;
    typoBusy = true;
    if (announce) gtk_label_set_text(GTK_LABEL(typoStatus), "モデルを取得しています…");
    RefreshTypoStatus();
    std::thread([] {
        const std::filesystem::path data = iroha::platform::DataDirectory();
        std::string message;
        bool failed = false;
        iroha::TypoNormalizerCatalog catalog;
        std::string error;
        if (!iroha::TypoNormalizerFetcher::FetchCatalog(iroha::TypoNormalizerCatalog::DefaultUrl(), &catalog, &error)) {
            // 入っていれば、そのまま使い続ける
            if (!iroha::TypoNormalizerInstall::IsInstalled(data)) {
                failed = true;
                message = "モデルの一覧を取得できませんでした: " + error;
            }
        } else {
            // 入っているモデルがカタログにあればその新しい版、無ければ一覧の先頭
            const auto record = iroha::TypoNormalizerInstall::InstalledRecord(data);
            const auto* model = record ? catalog.Find(record->id) : nullptr;
            if (!model) model = &catalog.models.front();
            if (iroha::TypoNormalizerInstall::NeedsUpdate(data, *model)) {
                if (iroha::TypoNormalizerFetcher::Install(*model, catalog.LicenseFor(*model), data, nullptr, &error)) {
                    message = U8(model->name) + " のモデルを入れました";
                    iroha::platform::RequestReload();
                } else {
                    failed = true;
                    message = "モデルを取得できませんでした: " + error;
                }
            }
        }
        RunOnMain([message, failed] {
            g_app->typoBusy = false;
            g_app->RefreshTypoStatus();
            if (!message.empty()) gtk_label_set_text(GTK_LABEL(g_app->typoStatus), message.c_str());
            if (failed) g_app->ShowDialog(message);
        });
    }).detach();
}

// 学習元とライセンス（入れた時点のカタログの記録から。重みは本体のコードと別のライセンス）
void App::OnTypoLicense() {
    const auto record = iroha::TypoNormalizerInstall::InstalledRecord(iroha::platform::DataDirectory());
    if (!record) return;
    std::string text = "打ち間違い訂正のモデル\n" + U8(record->name) + "（" + record->id + "）\n";
    if (!record->license.empty()) text += "ライセンス: " + U8(record->license) + "\n";
    if (!record->page.empty()) text += record->page + "\n";
    text += "\n学習元:\n";
    for (const auto& source : record->sources) {
        text += "・" + U8(source.name) + "  © " + U8(source.holder) + "  " + U8(source.license) + "\n    " +
                source.url + "\n";
    }
    ShowDialog(text);
}

void App::OnTypoRemove() {
    ShowDialog("打ち間違い訂正のモデルを削除します。よろしいですか？\n（もう一度 ON にすると取得し直します）", "削除",
               [this] {
                   iroha::Config config = iroha::LoadConfig(iroha::settings::ConfigPath());
                   config.typoCorrection = false;
                   iroha::SaveConfig(iroha::settings::ConfigPath(), config);
                   updating = true;
                   gtk_check_button_set_active(GTK_CHECK_BUTTON(typoCheck), FALSE);
                   updating = false;
                   iroha::platform::RequestReload();
                   if (!iroha::TypoNormalizerInstall::Remove(iroha::platform::DataDirectory())) {
                       ShowDialog("モデルを削除できませんでした");
                   }
                   RefreshTypoStatus();
               });
}

// ---- ユーザ辞書 ----

GtkWidget* App::BuildDictionaryPage() {
    GtkWidget* page = Box(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(page, 16);
    gtk_widget_set_margin_end(page, 16);
    gtk_widget_set_margin_top(page, 14);
    gtk_widget_set_margin_bottom(page, 14);
    GtkWidget* scrolled = nullptr;
    dictList = ListBox(GTK_SELECTION_SINGLE, &scrolled);
    GtkWidget* form = Row();
    dictReading = Entry("よみ", 14);
    dictWord = Entry("単語", 18);
    GtkWidget* add = gtk_button_new_with_label("追加");
    Append(form, {Label("読み:"), dictReading, Label("単語:"), dictWord, add});
    dictRemove = gtk_button_new_with_label("選択を削除");
    gtk_widget_set_halign(dictRemove, GTK_ALIGN_START);
    Append(page, {ListHeading({{"読み", 14}, {"単語", 20}, {"出所", 0}}), scrolled, form, dictRemove});
    Connect(add, "clicked", [this] { OnDictAdd(); });
    Connect(dictWord, "activate", [this] { OnDictAdd(); });
    Connect(dictRemove, "clicked", [this] { OnDictRemove(); });
    Connect(dictList, "selected-rows-changed",
            [this] { gtk_widget_set_sensitive(dictRemove, !SelectedRows(dictList).empty()); });
    gtk_widget_set_sensitive(dictRemove, FALSE);
    return page;
}

void App::RefreshDictionaryList() {
    ClearList(dictList);
    for (const iroha::UserDictionaryEntry& entry : dictionary->Entries()) {
        const char* source = entry.source == iroha::UserDictionaryEntry::Source::System ? "取込" : "手動";
        gtk_list_box_append(GTK_LIST_BOX(dictList),
                            ListRow({{U8(entry.reading), 14}, {U8(entry.word), 20}, {source, 0}}));
    }
    gtk_widget_set_sensitive(dictRemove, FALSE);
}

void App::OnDictAdd() {
    const std::string reading = Trimmed(EditableText(dictReading));
    const std::string word = Trimmed(EditableText(dictWord));
    if (reading.empty() || word.empty()) {
        ShowDialog("読みと単語を入力してください");
        return;
    }
    dictionary->Add(U32(reading), U32(word));
    SetEditableText(dictReading, "");
    SetEditableText(dictWord, "");
    RefreshDictionaryList();
    iroha::platform::RequestReload();
    gtk_widget_grab_focus(dictReading);
}

void App::OnDictRemove() {
    const std::vector<int> rows = SelectedRows(dictList);
    if (rows.empty()) return;
    std::vector<iroha::UserDictionaryEntry> entries = dictionary->Entries();
    const int selected = rows.front();
    if (selected >= static_cast<int>(entries.size())) return;
    entries.erase(entries.begin() + selected);
    dictionary->ReplaceAll(std::move(entries));
    RefreshDictionaryList();
    iroha::platform::RequestReload();
}

// ---- 専門辞書 ----

GtkWidget* App::BuildTermPage() {
    GtkWidget* page = Box(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(page, 16);
    gtk_widget_set_margin_end(page, 16);
    gtk_widget_set_margin_top(page, 14);
    gtk_widget_set_margin_bottom(page, 14);
    GtkWidget* scrolled = nullptr;
    termList = ListBox(GTK_SELECTION_SINGLE, &scrolled);
    termAdd = gtk_button_new_with_label("追加");
    termRemove = gtk_button_new_with_label("削除");
    termStatus = Label("", true);
    gtk_widget_set_hexpand(termStatus, TRUE);
    GtkWidget* buttons = Row();
    Append(buttons, {termAdd, termRemove, termStatus});
    // 出典・ライセンスの URL を写せるよう、選べる文字にする
    termDetail = Label(TermDetailHint(), true);
    gtk_label_set_selectable(GTK_LABEL(termDetail), TRUE);
    gtk_label_set_yalign(GTK_LABEL(termDetail), 0);
    GtkWidget* detailScrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(detailScrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(detailScrolled), TRUE);
    gtk_widget_set_size_request(detailScrolled, -1, 150);
    gtk_widget_set_margin_start(termDetail, 6);
    gtk_widget_set_margin_end(termDetail, 6);
    gtk_widget_set_margin_top(termDetail, 4);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(detailScrolled), termDetail);
    Append(page, {ListHeading({{"辞書", 16}, {"まとまり", 20}, {"語数", 8}, {"状態", 0}}), scrolled, buttons,
                  detailScrolled});
    Connect(termList, "selected-rows-changed", [this] { ShowTermDetail(); });
    const auto onButton = [this](bool add) {
        const std::vector<int> rows = SelectedRows(termList);
        if (rows.empty() || rows.front() >= static_cast<int>(termRows.size())) return;
        const TermRow row = termRows[rows.front()];
        if (add) {
            StartTermTask(row.name + " を取得しています…", false, row.id, {});
        } else {
            StartTermTask(row.name + " を外しています…", false, {}, row.id);
        }
    };
    Connect(termAdd, "clicked", [onButton] { onButton(true); });
    Connect(termRemove, "clicked", [onButton] { onButton(false); });
    return page;
}

// カタログの辞書と入れてある辞書を、一覧の行にまとめる（カタログが無くても、入れてある辞書は出す）
void App::RefreshTermList() {
    const std::filesystem::path data = iroha::platform::DataDirectory();
    const auto records = iroha::TermDictionaryInstall::InstalledRecords(data);
    termRows.clear();
    if (termCatalog) {
        for (const auto& entry : termCatalog->dictionaries) {
            TermRow row;
            row.id = entry.id;
            row.name = U8(entry.name);
            row.group = U8(entry.group);
            row.summary = U8(entry.summary);
            row.entries = entry.entries;
            row.license = U8(entry.license);
            row.sources = entry.sources;
            row.samples = entry.samples;
            row.inCatalog = true;
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
        row.name = U8(record.name);
        row.group = U8(record.group);
        row.entries = record.entries;
        row.license = U8(record.license);
        row.sources = record.sources;
        row.installed = true;
        termRows.push_back(std::move(row));
    }

    const std::vector<int> selected = SelectedRows(termList);
    ClearList(termList);
    for (const TermRow& row : termRows) {
        const std::string state = !row.installed  ? ""
                                  : row.needsUpdate ? "入れてある（更新あり）"
                                  : row.inCatalog   ? "入れてある"
                                                    : "入れてある（一覧に無い）";
        gtk_list_box_append(GTK_LIST_BOX(termList), ListRow({{row.name, 16},
                                                             {row.group, 20},
                                                             {std::to_string(row.entries), 8},
                                                             {state, 0}}));
    }
    if (!selected.empty() && selected.front() < static_cast<int>(termRows.size())) {
        gtk_list_box_select_row(GTK_LIST_BOX(termList),
                                gtk_list_box_get_row_at_index(GTK_LIST_BOX(termList), selected.front()));
    }
    ShowTermDetail();
}

// 選んでいる辞書の説明・語の例・出典とライセンス
void App::ShowTermDetail() {
    const std::vector<int> rows = SelectedRows(termList);
    const bool valid = !rows.empty() && rows.front() < static_cast<int>(termRows.size());
    const TermRow* row = valid ? &termRows[rows.front()] : nullptr;
    gtk_widget_set_sensitive(termAdd, row && !termBusy && row->inCatalog && (!row->installed || row->needsUpdate));
    gtk_widget_set_sensitive(termRemove, row && !termBusy && row->installed);
    if (!row) {
        gtk_label_set_text(GTK_LABEL(termDetail), TermDetailHint().c_str());
        return;
    }
    std::string text = row->name + "（" + row->group + "）  " + std::to_string(row->entries) + " 語\n";
    if (!row->summary.empty()) text += row->summary + "\n";
    if (!row->samples.empty()) {
        text += "例: ";
        for (size_t i = 0; i < row->samples.size(); ++i) {
            if (i > 0) text += "、";
            text += U8(row->samples[i].word) + "（" + U8(row->samples[i].reading) + "）";
        }
        text += "\n";
    }
    text += "ライセンス: " + row->license + "\n";
    for (const auto& source : row->sources) {
        text += "出典: " + U8(source.name) + "  © " + U8(source.holder) + "  " + U8(source.license) + "\n  " +
                source.url + "\n";
        if (!source.licenseUrl.empty()) text += "  ライセンス文書: " + source.licenseUrl + "\n";
    }
    gtk_label_set_text(GTK_LABEL(termDetail), text.c_str());
}

// 一覧の取得・入れる・外すを裏のスレッドで行う。
// 一覧を取ったときは、入れてある辞書のうち作り直されたものを入れ直す（macOS版・Windows版と同じ）
void App::StartTermTask(const std::string& busyMessage, bool fetchCatalog, std::string installId,
                        std::string removeId) {
    if (termBusy) return;
    termBusy = true;
    gtk_label_set_text(GTK_LABEL(termStatus), busyMessage.c_str());
    ShowTermDetail();
    std::shared_ptr<const iroha::TermDictionaryCatalog> current =
        termCatalog ? std::make_shared<iroha::TermDictionaryCatalog>(*termCatalog) : nullptr;
    std::thread([fetchCatalog, installId = std::move(installId), removeId = std::move(removeId), current] {
        const std::filesystem::path data = iroha::platform::DataDirectory();
        std::shared_ptr<iroha::TermDictionaryCatalog> fetched;
        std::string message;
        bool changed = false;
        std::string error;
        if (fetchCatalog) {
            auto catalog = std::make_shared<iroha::TermDictionaryCatalog>();
            if (iroha::TermDictionaryFetcher::FetchCatalog(iroha::TermDictionaryCatalog::DefaultUrl(), catalog.get(),
                                                           &error)) {
                int updated = 0;
                for (const auto& record : iroha::TermDictionaryInstall::InstalledRecords(data)) {
                    const auto* dictionary = catalog->Find(record.id);
                    if (!dictionary || !iroha::TermDictionaryInstall::NeedsUpdate(record, *dictionary)) continue;
                    if (iroha::TermDictionaryFetcher::Install(*dictionary, data, nullptr, &error)) ++updated;
                }
                changed = updated > 0;
                if (updated > 0) message = "新しい版の辞書を " + std::to_string(updated) + " つ入れ直しました";
                fetched = catalog;
            } else {
                message = "辞書の一覧を取得できませんでした: " + error;
            }
        }
        if (!installId.empty() && current) {
            if (const auto* dictionary = current->Find(installId)) {
                if (iroha::TermDictionaryFetcher::Install(*dictionary, data, nullptr, &error)) {
                    changed = true;
                    message = U8(dictionary->name) + " を入れました";
                } else {
                    message = "入れられませんでした: " + error;
                }
            }
        }
        if (!removeId.empty()) {
            if (iroha::TermDictionaryInstall::Remove(data, removeId)) {
                changed = true;
                message = "外しました";
            } else {
                message = "外せませんでした";
            }
        }
        // IBus のエンジンに読み直してもらう（数十万語で 1 秒前後）
        if (changed) iroha::platform::RequestReload();
        RunOnMain([fetched, message] {
            g_app->termBusy = false;
            if (fetched) g_app->termCatalog = std::make_unique<iroha::TermDictionaryCatalog>(*fetched);
            gtk_label_set_text(GTK_LABEL(g_app->termStatus), message.c_str());
            g_app->RefreshTermList();
        });
    }).detach();
}

// ---- 変換ルール ----

GtkWidget* App::BuildRulePage() {
    GtkWidget* page = Box(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(page, 16);
    gtk_widget_set_margin_end(page, 16);
    gtk_widget_set_margin_top(page, 14);
    gtk_widget_set_margin_bottom(page, 14);
    GtkWidget* scrolled = nullptr;
    ruleList = ListBox(GTK_SELECTION_MULTIPLE, &scrolled);
    GtkWidget* form = Row();
    ruleTrigger = Entry("よみ", 12);
    ruleOutput = Entry("{{date}}", 22);
    gtk_widget_set_hexpand(ruleOutput, TRUE);
    GtkWidget* add = gtk_button_new_with_label("追加");
    Append(form, {Label("読み:"), ruleTrigger, Label("出力:"), ruleOutput, add});
    ruleRemove = gtk_button_new_with_label("選択を削除");
    gtk_widget_set_halign(ruleRemove, GTK_ALIGN_START);
    GtkWidget* note =
        Label("読みを打って Space を押したときの候補に、出力を加えます（ライブ変換は変えません）。"
              "チェックを外したルールは使いません。\n"
              "出力に書けるもの: {{date}}（今日の日付）・{{date+1}}（明日）・{{time:HH:mm}}・"
              "{{datetime}}・{{wareki}}（和暦）。日付の書式は {{date:yyyy年M月d日（E）}} のように指定します。",
              true);
    gtk_widget_add_css_class(note, "dim-label");
    // 見出しはチェックの幅だけ右へずらす
    Append(page, {ListHeading({{"読み", 12}, {"出力", 22}, {"いま出る候補", 0}}, 30), scrolled, form, ruleRemove,
                  note});
    Connect(add, "clicked", [this] { OnRuleAdd(); });
    Connect(ruleOutput, "activate", [this] { OnRuleAdd(); });
    Connect(ruleRemove, "clicked", [this] { OnRuleRemove(); });
    Connect(ruleList, "selected-rows-changed",
            [this] { gtk_widget_set_sensitive(ruleRemove, !SelectedRows(ruleList).empty()); });
    gtk_widget_set_sensitive(ruleRemove, FALSE);
    return page;
}

void App::RefreshRuleList() {
    rewriteRules->Reload();
    rules = rewriteRules->Rules();
    ClearList(ruleList);
    const iroha::RewriteContext now = iroha::RewriteContext::Now();
    for (size_t i = 0; i < rules.size(); ++i) {
        const iroha::UserRewriteRule& rule = rules[i];
        GtkWidget* row = ListRow({{U8(rule.trigger), 12},
                                  {U8(rule.output), 22},
                                  {U8(iroha::RewriteTemplate(rule.output).Render(now)), 0}});
        GtkWidget* check = gtk_check_button_new();
        gtk_check_button_set_active(GTK_CHECK_BUTTON(check), rule.isEnabled);
        gtk_widget_set_tooltip_text(check, "使う");
        gtk_box_prepend(GTK_BOX(row), check);
        Connect(check, "toggled", [this, i, check] { OnRuleChecked(i, Checked(check)); });
        gtk_list_box_append(GTK_LIST_BOX(ruleList), row);
    }
    gtk_widget_set_sensitive(ruleRemove, FALSE);
}

void App::OnRuleAdd() {
    const std::u32string normalized = iroha::UserRewriteRule::NormalizedTrigger(U32(EditableText(ruleTrigger)));
    const std::u32string templateText = U32(EditableText(ruleOutput));
    if (normalized.empty() || iroha::TrimWhitespace(templateText).empty()) {
        ShowDialog("読みと出力を入力してください");
        return;
    }
    const auto add = [this, normalized, templateText] {
        rewriteRules->Add(normalized, templateText);
        SetEditableText(ruleTrigger, "");
        SetEditableText(ruleOutput, "");
        RefreshRuleList();
        iroha::platform::RequestReload();
    };
    // 知らない {{…}} はそのまま文字として出るので、追加する前に知らせる
    const auto unknown = iroha::RewriteTemplate(templateText).UnknownPlaceholders();
    if (!unknown.empty()) {
        std::string names;
        for (const auto& name : unknown) names += "{{" + U8(name) + "}} ";
        ShowDialog("次の書き方には対応していないので、そのまま文字として出ます:\n" + names + "\n\nこのまま追加しますか？",
                   "追加", add);
        return;
    }
    add();
}

void App::OnRuleRemove() {
    std::set<std::string> ids;
    for (int row : SelectedRows(ruleList)) {
        if (row < static_cast<int>(rules.size())) ids.insert(rules[row].id);
    }
    if (ids.empty()) return;
    rewriteRules->Remove(ids);
    RefreshRuleList();
    iroha::platform::RequestReload();
}

// 一覧のチェック（使う・使わない）を切り替えた
void App::OnRuleChecked(size_t index, bool enabled) {
    if (index >= rules.size() || enabled == rules[index].isEnabled) return;
    rewriteRules->SetEnabled(enabled, rules[index].id);
    rules[index].isEnabled = enabled;
    iroha::platform::RequestReload();
}

// ---- AI ----

int AIServiceIndex(iroha::AIService service) {
    for (int i = 0; i < kAIServiceCount; ++i) {
        if (kAIServices[i] == service) return i;
    }
    return 0;
}

const char* KeyAccount(iroha::AIService service) {
    return service == iroha::AIService::Claude ? iroha::AIClient::kClaudeKeyAccount
                                               : iroha::AIClient::kOpenAIKeyAccount;
}

bool HasKey(iroha::AIService service) {
    return service == iroha::AIService::OpenAI || service == iroha::AIService::Claude;
}

GtkWidget* App::BuildAIPage() {
    GtkWidget* scrolled = nullptr;
    GtkWidget* page = PageBox(&scrolled);
    std::vector<std::string> services;
    for (iroha::AIService service : kAIServices) services.push_back(U8(iroha::AIServiceDisplayName(service)));
    aiServiceDrop = DropDown(services);
    gtk_widget_set_halign(aiServiceDrop, GTK_ALIGN_START);
    aiEndpoint = Entry();
    gtk_widget_set_hexpand(aiEndpoint, TRUE);
    aiModel = Entry();
    gtk_widget_set_hexpand(aiModel, TRUE);
    aiModelChoices = gtk_drop_down_new(nullptr, nullptr);
    gtk_widget_set_visible(aiModelChoices, FALSE);
    gtk_widget_set_tooltip_text(aiModelChoices, "取得した一覧から選ぶ");
    aiModels = gtk_button_new_with_label("一覧を取得");
    aiKey = gtk_password_entry_new();
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(aiKey), TRUE);
    gtk_widget_set_hexpand(aiKey, TRUE);
    aiKeyClear = gtk_button_new_with_label("キーを消す");
    aiStatus = Label("", true);

    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    GtkWidget* modelRow = Row();
    Append(modelRow, {aiModel, aiModelChoices, aiModels});
    GtkWidget* keyRow = Row();
    Append(keyRow, {aiKey, aiKeyClear});
    gtk_grid_attach(GTK_GRID(grid), Label("接続先:"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), aiServiceDrop, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), Label("URL（空なら既定）:"), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), aiEndpoint, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), Label("モデル:"), 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), modelRow, 1, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), Label("API キー:"), 0, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), keyRow, 1, 3, 1, 1);
    Append(page, {grid, aiStatus, Label("プリセット（名前・キー・プロンプト）:")});

    for (size_t i = 0; i < iroha::AICommitSettings::kCount; ++i) {
        aiPresetName[i] = Entry("名前", 10);
        aiPresetHotkey[i] = Entry("Alt+Enter", 12);
        aiPresetPrompt[i] = gtk_text_view_new();
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(aiPresetPrompt[i]), GTK_WRAP_WORD_CHAR);
        gtk_text_view_set_left_margin(GTK_TEXT_VIEW(aiPresetPrompt[i]), 4);
        gtk_text_view_set_right_margin(GTK_TEXT_VIEW(aiPresetPrompt[i]), 4);
        GtkWidget* promptScrolled = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(promptScrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(promptScrolled), TRUE);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(promptScrolled), aiPresetPrompt[i]);
        gtk_widget_set_size_request(promptScrolled, -1, 64);
        gtk_widget_set_hexpand(promptScrolled, TRUE);
        GtkWidget* row = Row();
        gtk_widget_set_valign(aiPresetName[i], GTK_ALIGN_START);
        gtk_widget_set_valign(aiPresetHotkey[i], GTK_ALIGN_START);
        Append(row, {aiPresetName[i], aiPresetHotkey[i], promptScrolled});
        Append(page, {row});
    }
    GtkWidget* save = gtk_button_new_with_label("保存");
    gtk_widget_set_halign(save, GTK_ALIGN_START);
    GtkWidget* note = Label("入力中にプリセットのキー（既定は英訳の Alt+Enter）で、未確定文字列を AI に渡します。"
                            "英訳など日本語でない結果は確定し、日本語の結果は文節に区切って返します"
                            "（Enter で確定、Esc で取り消し）。"
                            "プロンプトに {text} を書くと、その位置に入力が入ります。"
                            "OpenAI互換・Claude では入力が外部に送られます。"
                            "API キーは config.json に書かず、秘密の保管庫（GNOME キーリングなど）に置きます。",
                            true);
    gtk_widget_add_css_class(note, "dim-label");
    Append(page, {save, note});

    ConnectNotify(aiServiceDrop, "selected", [this] {
        if (!updating) OnAIServiceChanged();
    });
    ConnectNotify(aiModelChoices, "selected", [this] {
        if (updating) return;
        const int index = Selected(aiModelChoices);
        if (index >= 0 && index < static_cast<int>(aiModelList.size())) SetEditableText(aiModel, aiModelList[index]);
    });
    Connect(aiModels, "clicked", [this] { OnAIListModels(); });
    Connect(aiKeyClear, "clicked", [this] { OnAIKeyClear(); });
    Connect(save, "clicked", [this] { OnAISave(); });
    return scrolled;
}

void App::RefreshAIPage() {
    const iroha::Config config = iroha::LoadConfig(iroha::settings::ConfigPath());
    const int index = AIServiceIndex(iroha::ParseAIService(config.aiService));
    updating = true;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(aiServiceDrop), static_cast<guint>(index));
    updating = false;
    ShowAIServiceFields(index, config);
    for (size_t i = 0; i < iroha::AICommitSettings::kCount; ++i) {
        const auto& stored = config.aiPresets[i];
        const auto& fallback = iroha::AICommitSettings::Defaults()[i];
        SetEditableText(aiPresetName[i], U8(stored.name.value_or(fallback.name)));
        SetEditableText(aiPresetHotkey[i], stored.hotkey.value_or(fallback.shortcut));
        SetTextViewText(aiPresetPrompt[i], U8(stored.prompt.value_or(fallback.prompt)));
    }
}

// 接続先ごとの URL・モデル・API キーの欄を出し分ける
void App::ShowAIServiceFields(int index, const iroha::Config& config) {
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
    SetEditableText(aiEndpoint, endpoint);
    SetEditableText(aiModel, model);
    aiModelList.clear();
    gtk_widget_set_visible(aiModelChoices, FALSE);
    const bool none = service == iroha::AIService::None;
    const bool hasEndpoint = service == iroha::AIService::Ollama || service == iroha::AIService::LMStudio ||
                             service == iroha::AIService::OpenAI;
    gtk_widget_set_sensitive(aiEndpoint, hasEndpoint);
    gtk_entry_set_placeholder_text(GTK_ENTRY(aiEndpoint), iroha::AIClient::DefaultEndpoint(service).c_str());
    gtk_widget_set_sensitive(aiModel, !none);
    gtk_widget_set_sensitive(aiModels, !none);
    gtk_entry_set_placeholder_text(GTK_ENTRY(aiModel),
                                   service == iroha::AIService::Claude ? iroha::AIClient::kClaudeDefaultModel : "");
    gtk_widget_set_sensitive(aiKey, HasKey(service));
    gtk_widget_set_sensitive(aiKeyClear, HasKey(service));
    SetEditableText(aiKey, "");
    RefreshAIKeyStatus(service);
}

// API キーがあるかを裏のスレッドで確かめて出す（秘密の保管庫を main thread で読まない）
void App::RefreshAIKeyStatus(iroha::AIService service) {
    const unsigned generation = ++keyGeneration;
    if (!HasKey(service)) {
        gtk_label_set_text(GTK_LABEL(aiStatus), service == iroha::AIService::None
                                                    ? "AI変換を使わない（AI変換のキーでは普通に確定します）"
                                                    : "");
        return;
    }
    gtk_label_set_text(GTK_LABEL(aiStatus), "API キーを確かめています…");
    const std::string account = KeyAccount(service);
    std::thread([generation, service, account] {
        std::string key;
        const bool stored = iroha::SecretStore::Get(account, &key) && !key.empty();
        RunOnMain([generation, service, stored] {
            if (generation != g_app->keyGeneration) return;
            std::string text = stored ? "API キー: 保存してあります（秘密の保管庫）" : "API キー: まだありません";
            if (service == iroha::AIService::OpenAI && !stored) text += "（ローカルの互換サーバなら無くてもよい）";
            gtk_label_set_text(GTK_LABEL(g_app->aiStatus), text.c_str());
        });
    }).detach();
}

// URL・モデルの欄の値を、その接続先の設定に書き戻す
void App::StoreAIServiceFields(int index, iroha::Config* config) {
    if (index < 0) return;
    const std::string endpoint = Trimmed(EditableText(aiEndpoint));
    const std::string model = Trimmed(EditableText(aiModel));
    switch (kAIServices[index]) {
        case iroha::AIService::Ollama:
            config->ollamaEndpoint = endpoint;
            config->ollamaModel = model;
            break;
        case iroha::AIService::LMStudio:
            config->lmStudioEndpoint = endpoint;
            config->lmStudioModel = model;
            break;
        case iroha::AIService::OpenAI:
            config->openAIEndpoint = endpoint;
            config->openAIModel = model;
            break;
        case iroha::AIService::Claude:
            config->claudeModel = model;
            break;
        default:
            break;
    }
}

void App::OnAIServiceChanged() {
    iroha::Config config = iroha::LoadConfig(iroha::settings::ConfigPath());
    // 前の接続先の欄の値は、その接続先の設定として残す
    StoreAIServiceFields(aiShownService, &config);
    const int index = Selected(aiServiceDrop);
    if (index < 0) return;
    config.aiService = iroha::AIServiceKey(kAIServices[index]);
    iroha::SaveConfig(iroha::settings::ConfigPath(), config);
    ShowAIServiceFields(index, config);
    iroha::platform::RequestReload();
}

void App::OnAISave() {
    iroha::Config config = iroha::LoadConfig(iroha::settings::ConfigPath());
    StoreAIServiceFields(aiShownService, &config);
    // プリセット: キーの書き方が読めなければ保存しない
    std::string invalid;
    for (size_t i = 0; i < iroha::AICommitSettings::kCount; ++i) {
        config.aiPresets[i].name = iroha::TrimWhitespace(U32(EditableText(aiPresetName[i])));
        const std::string hotkey = Trimmed(EditableText(aiPresetHotkey[i]));
        if (!hotkey.empty() && !iroha::KeyShortcut::Parse(hotkey)) invalid += "「" + hotkey + "」";
        config.aiPresets[i].hotkey = hotkey;
        config.aiPresets[i].prompt = iroha::TrimWhitespace(U32(TextViewText(aiPresetPrompt[i])));
    }
    if (!invalid.empty()) {
        ShowDialog("キーの書き方が読めません: " + invalid +
                   "\n「Alt+Enter」「Ctrl+Shift+T」のように、Ctrl・Alt・Shift のどれかと 1 つのキーを + でつないで書きます。");
        return;
    }
    if (!iroha::SaveConfig(iroha::settings::ConfigPath(), config)) {
        ShowDialog("設定の保存に失敗しました");
        return;
    }
    iroha::platform::RequestReload();
    // API キー（入力したときだけ置き換える。秘密の保管庫への書き込みは裏のスレッドで）
    const int index = Selected(aiServiceDrop);
    const iroha::AIService service = index >= 0 ? kAIServices[index] : iroha::AIService::None;
    const std::string key = Trimmed(EditableText(aiKey));
    if (key.empty() || !HasKey(service)) {
        gtk_label_set_text(GTK_LABEL(aiStatus), "保存しました");
        return;
    }
    SetEditableText(aiKey, "");
    gtk_label_set_text(GTK_LABEL(aiStatus), "API キーを保存しています…");
    const unsigned generation = ++keyGeneration;
    const std::string account = KeyAccount(service);
    std::thread([generation, account, key] {
        const bool ok = iroha::SecretStore::Set(account, key);
        if (ok) iroha::platform::RequestReload();
        RunOnMain([generation, ok] {
            if (generation != g_app->keyGeneration) return;
            gtk_label_set_text(GTK_LABEL(g_app->aiStatus),
                               ok ? "保存しました（API キーは秘密の保管庫に置きました）"
                                  : "設定は保存しました。API キーを秘密の保管庫に置けませんでした"
                                    "（GNOME キーリングなどが動いているか確かめてください）");
        });
    }).detach();
}

void App::OnAIKeyClear() {
    const int index = Selected(aiServiceDrop);
    if (index < 0) return;
    const iroha::AIService service = kAIServices[index];
    if (!HasKey(service)) return;
    gtk_label_set_text(GTK_LABEL(aiStatus), "API キーを消しています…");
    ++keyGeneration;
    const std::string account = KeyAccount(service);
    std::thread([account, service] {
        iroha::SecretStore::Set(account, "");
        iroha::platform::RequestReload();
        RunOnMain([service] { g_app->RefreshAIKeyStatus(service); });
    }).detach();
}

// 今の欄の接続先で、使えるモデルの一覧を取る（裏のスレッド。キーを入れていなければ秘密の保管庫から読む）
void App::OnAIListModels() {
    const int index = Selected(aiServiceDrop);
    if (index < 0) return;
    iroha::AIConnection connection;
    connection.service = kAIServices[index];
    connection.endpoint = Trimmed(EditableText(aiEndpoint));
    connection.apiKey = Trimmed(EditableText(aiKey));
    gtk_widget_set_sensitive(aiModels, FALSE);
    gtk_label_set_text(GTK_LABEL(aiStatus), "モデルの一覧を取得しています…");
    const unsigned generation = ++keyGeneration;
    std::thread([connection, generation]() mutable {
        if (connection.apiKey.empty()) iroha::AIClient::LoadKey(&connection);
        std::vector<std::string> models;
        std::string error;
        const bool ok = iroha::AIClient::ListModels(connection, &models, &error);
        RunOnMain([generation, ok, models, error] {
            gtk_widget_set_sensitive(g_app->aiModels, TRUE);
            if (generation != g_app->keyGeneration) return;
            if (!ok) {
                gtk_label_set_text(GTK_LABEL(g_app->aiStatus), ("モデルの一覧を取得できませんでした: " + error).c_str());
                return;
            }
            App& app = *g_app;
            app.aiModelList = models;
            std::vector<const char*> strings;
            for (const std::string& model : models) strings.push_back(model.c_str());
            strings.push_back(nullptr);
            const std::string current = Trimmed(EditableText(app.aiModel));
            app.updating = true;
            GtkStringList* list = gtk_string_list_new(strings.data());
            gtk_drop_down_set_model(GTK_DROP_DOWN(app.aiModelChoices), G_LIST_MODEL(list));
            g_object_unref(list);
            guint selected = GTK_INVALID_LIST_POSITION;
            for (size_t i = 0; i < models.size(); ++i) {
                if (models[i] == current) selected = static_cast<guint>(i);
            }
            if (current.empty() && !models.empty()) {
                selected = 0;
                SetEditableText(app.aiModel, models.front());
            }
            gtk_drop_down_set_selected(GTK_DROP_DOWN(app.aiModelChoices), selected);
            app.updating = false;
            gtk_widget_set_visible(app.aiModelChoices, !models.empty());
            gtk_label_set_text(GTK_LABEL(app.aiStatus),
                               (std::to_string(models.size()) + " 個のモデルがあります。選んで「保存」を押します").c_str());
        });
    }).detach();
}

// ---- 選択テキスト ----

GtkWidget* App::BuildSelectionPage() {
    GtkWidget* scrolled = nullptr;
    GtkWidget* page = PageBox(&scrolled);
    selEnabled = CheckButton(
        "選択テキストのAI編集を使う（ほかのアプリで選んだ文章をショートカットで AI に渡して置き換える）");
    selOnDemand = Entry(iroha::SelectionSettingsDefaults::kOnDemandHotkey, 14);
    GtkWidget* onDemandRow = Row();
    Append(onDemandRow, {Label("その場で指示するキー:"), selOnDemand});
    Append(page, {selEnabled, onDemandRow, Label("プリセット（使う・名前・キー・プロンプト）:")});
    for (size_t i = 0; i < iroha::SelectionSettingsDefaults::kCount; ++i) {
        selPresetEnabled[i] = gtk_check_button_new();
        selPresetName[i] = Entry("名前", 10);
        selPresetHotkey[i] = Entry("Ctrl+Alt+1", 12);
        selPresetPrompt[i] = Entry("プロンプト");
        gtk_widget_set_hexpand(selPresetPrompt[i], TRUE);
        GtkWidget* row = Row();
        Append(row, {selPresetEnabled[i], selPresetName[i], selPresetHotkey[i], selPresetPrompt[i]});
        Append(page, {row});
    }
    selExcluded = Entry();
    gtk_widget_set_hexpand(selExcluded, TRUE);
    GtkWidget* excludedRow = Row();
    Append(excludedRow, {Label("使わないアプリ（実行ファイルの名前か WM_CLASS・カンマ区切り）:"), selExcluded});
    GtkWidget* save = gtk_button_new_with_label("保存");
    gtk_widget_set_halign(save, GTK_ALIGN_START);
    selStatus = Label("");
    GtkWidget* saveRow = Row();
    Append(saveRow, {save, selStatus});
    GtkWidget* note = Label("キーを押すと、選んでいる文章を AI に渡し、結果を小さな窓に出します。"
                            "Enter で元の文章を置き換えます。選択が無ければ指示から文章を作ります。"
                            "入力中（未確定文字列があるとき）は動きません。AI の接続先は「AI」タブの設定を使います。",
                            true);
    gtk_widget_add_css_class(note, "dim-label");
    Append(page, {excludedRow, saveRow, note});
    Connect(save, "clicked", [this] { OnSelectionSave(); });
    return scrolled;
}

void App::RefreshSelectionPage() {
    const iroha::Config config = iroha::LoadConfig(iroha::settings::ConfigPath());
    gtk_check_button_set_active(GTK_CHECK_BUTTON(selEnabled), config.selectionEnabled);
    SetEditableText(selOnDemand,
                    config.selectionOnDemandHotkey.value_or(iroha::SelectionSettingsDefaults::kOnDemandHotkey));
    for (size_t i = 0; i < iroha::SelectionSettingsDefaults::kCount; ++i) {
        const auto& stored = config.selectionPresets[i];
        const auto& fallback = iroha::SelectionSettingsDefaults::Presets()[i];
        gtk_check_button_set_active(GTK_CHECK_BUTTON(selPresetEnabled[i]), stored.enabled.value_or(fallback.enabled));
        SetEditableText(selPresetName[i], U8(stored.name.value_or(fallback.name)));
        SetEditableText(selPresetHotkey[i], stored.hotkey.value_or(fallback.hotkey));
        SetEditableText(selPresetPrompt[i], U8(stored.prompt.value_or(fallback.prompt)));
    }
    SetEditableText(selExcluded, config.selectionExcludedApps);
    gtk_label_set_text(GTK_LABEL(selStatus), "");
}

void App::OnSelectionSave() {
    iroha::Config config = iroha::LoadConfig(iroha::settings::ConfigPath());
    std::string invalid;
    const auto checkHotkey = [&invalid](const std::string& hotkey) {
        if (!hotkey.empty() && !iroha::KeyShortcut::Parse(hotkey)) invalid += "「" + hotkey + "」";
    };
    config.selectionEnabled = Checked(selEnabled);
    config.selectionOnDemandHotkey = Trimmed(EditableText(selOnDemand));
    checkHotkey(*config.selectionOnDemandHotkey);
    for (size_t i = 0; i < iroha::SelectionSettingsDefaults::kCount; ++i) {
        auto& preset = config.selectionPresets[i];
        preset.enabled = Checked(selPresetEnabled[i]);
        preset.name = iroha::TrimWhitespace(U32(EditableText(selPresetName[i])));
        preset.hotkey = Trimmed(EditableText(selPresetHotkey[i]));
        checkHotkey(*preset.hotkey);
        preset.prompt = iroha::TrimWhitespace(U32(EditableText(selPresetPrompt[i])));
    }
    config.selectionExcludedApps = Trimmed(EditableText(selExcluded));
    if (!invalid.empty()) {
        ShowDialog("キーの書き方が読めません: " + invalid +
                   "\n「Ctrl+Alt+1」のように、Ctrl・Alt・Shift のどれかと 1 つのキーを + でつないで書きます。");
        return;
    }
    if (!iroha::SaveConfig(iroha::settings::ConfigPath(), config)) {
        ShowDialog("設定の保存に失敗しました");
        return;
    }
    iroha::platform::RequestReload();
    gtk_label_set_text(GTK_LABEL(selStatus), "保存しました");
}

// ---- 学習・記録 ----

GtkWidget* App::BuildLearningPage() {
    GtkWidget* scrolled = nullptr;
    GtkWidget* page = PageBox(&scrolled);
    learnStatus = Label("");
    GtkWidget* learnReset = gtk_button_new_with_label("学習をリセット");
    gtk_widget_set_halign(learnReset, GTK_ALIGN_START);
    historyStatus = Label("");
    GtkWidget* historyReset = gtk_button_new_with_label("入力履歴を消去");
    gtk_widget_set_halign(historyReset, GTK_ALIGN_START);
    logCheck = CheckButton("変換記録を残す（追加学習に使うデータ。カーソルの手前の文章も残ります）");
    logScopeDrop = DropDown({"すべての確定", "直した確定だけ"});
    GtkWidget* scopeRow = Row();
    gtk_widget_set_margin_start(scopeRow, 28);
    Append(scopeRow, {Label("記録する確定:"), logScopeDrop});
    logStatus = Label("", true);
    gtk_widget_set_margin_start(logStatus, 28);
    GtkWidget* logOpen = gtk_button_new_with_label("フォルダを開く");
    logReset = gtk_button_new_with_label("変換記録を消去");
    GtkWidget* logButtons = Row();
    gtk_widget_set_margin_start(logButtons, 28);
    Append(logButtons, {logOpen, logReset});
    GtkWidget* separator1 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget* separator2 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    Append(page, {learnStatus, learnReset, separator1, historyStatus, historyReset, separator2, logCheck, scopeRow,
                  logStatus, logButtons});
    Connect(learnReset, "clicked", [this] { OnLearnReset(); });
    Connect(historyReset, "clicked", [this] { OnHistoryReset(); });
    Connect(logCheck, "toggled", [this] {
        if (!updating) SaveLogSettings();
    });
    ConnectNotify(logScopeDrop, "selected", [this] {
        if (!updating) SaveLogSettings();
    });
    Connect(logOpen, "clicked", [] { OpenFolder(iroha::settings::ConversionLogDirectory()); });
    Connect(logReset, "clicked", [this] { OnLogReset(); });
    return scrolled;
}

void App::RefreshLearningPage() {
    learning = std::make_unique<iroha::LearningStore>(iroha::settings::LearningPath());
    gtk_label_set_text(GTK_LABEL(learnStatus), ("学習: " + std::to_string(learning->Count()) + " 件").c_str());
    inputHistory->Reload();
    gtk_label_set_text(GTK_LABEL(historyStatus),
                       ("入力履歴（予測変換が ON の間だけ記録）: " + std::to_string(inputHistory->Count()) + " 件").c_str());

    const iroha::Config config = iroha::LoadConfig(iroha::settings::ConfigPath());
    updating = true;
    gtk_check_button_set_active(GTK_CHECK_BUTTON(logCheck), config.conversionLog);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(logScopeDrop), config.conversionLogCorrectionsOnly ? 1 : 0);
    updating = false;
    gtk_widget_set_sensitive(logScopeDrop, config.conversionLog);
    const size_t count = conversionLog->Items().size();
    const uintmax_t size = conversionLog->TotalSize();
    std::string text = "記録: " + std::to_string(count) + " 件（" + iroha::settings::FormatBytes(size) + "）";
    if (config.privateMode && config.conversionLog) text += "  プライベートモードの間は記録しません";
    gtk_label_set_text(GTK_LABEL(logStatus), text.c_str());
    gtk_widget_set_sensitive(logReset, count > 0 || size > 0);
}

void App::OnLearnReset() {
    ShowDialog("学習した変換をすべて消去します。よろしいですか？\n（ユーザ辞書は消えません）", "消去", [this] {
        learning->Reset();
        RefreshLearningPage();
        iroha::platform::RequestReload();
    });
}

void App::OnHistoryReset() {
    ShowDialog("入力履歴（予測変換の候補）をすべて消去します。よろしいですか？", "消去", [this] {
        inputHistory->Reset(iroha::InputHistoryStore::NowSeconds());
        inputHistory->Flush();
        iroha::platform::RequestReload();
        RefreshLearningPage();
    });
}

void App::SaveLogSettings() {
    iroha::Config config = iroha::LoadConfig(iroha::settings::ConfigPath());
    config.conversionLog = Checked(logCheck);
    config.conversionLogCorrectionsOnly = Selected(logScopeDrop) == 1;
    if (!iroha::SaveConfig(iroha::settings::ConfigPath(), config)) {
        ShowDialog("設定の保存に失敗しました");
        return;
    }
    iroha::platform::RequestReload();
    RefreshLearningPage();
}

void App::OnLogReset() {
    ShowDialog("変換記録をすべて消去します。よろしいですか？\n"
               "（データフォルダを共有しているほかのコンピュータの記録も消えます）",
               "消去", [this] {
                   conversionLog->RemoveAll();
                   RefreshLearningPage();
               });
}

// ---- 情報 ----

GtkWidget* App::BuildAboutPage() {
    GtkWidget* scrolled = nullptr;
    GtkWidget* page = PageBox(&scrolled);
    GtkWidget* title = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(title), "<b>iroha for Linux</b>");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    GtkWidget* version = Label(std::string("バージョン: ") + IROHA_VERSION);
    gtk_label_set_selectable(GTK_LABEL(version), TRUE);
    GtkWidget* home = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(home), "<a href=\"https://github.com/TetsuakiBaba/iroha\">"
                                          "https://github.com/TetsuakiBaba/iroha</a>");
    gtk_label_set_xalign(GTK_LABEL(home), 0);

    const std::string data = iroha::PathToUtf8(iroha::platform::DataDirectory());
    GtkWidget* dataLabel = Label("データフォルダ: " + data, true);
    gtk_label_set_selectable(GTK_LABEL(dataLabel), TRUE);
    GtkWidget* openData = gtk_button_new_with_label("データフォルダを開く");
    gtk_widget_set_halign(openData, GTK_ALIGN_START);
    GtkWidget* dataNote = Label("場所は環境変数 IROHA_DATA_DIR > $XDG_DATA_HOME/iroha > ~/.local/share/iroha の順に決まります。"
                                "中のファイルの形式は macOS版・Windows版と同じです。",
                                true);
    gtk_widget_add_css_class(dataNote, "dim-label");

    GtkWidget* licenseTitle = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(licenseTitle), "<b>ライセンス</b>");
    gtk_label_set_xalign(GTK_LABEL(licenseTitle), 0);
    gtk_widget_set_margin_top(licenseTitle, 8);
    licenseBox = Box(GTK_ORIENTATION_VERTICAL, 8);
    Append(page, {title, version, home, gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), dataLabel, openData, dataNote,
                  gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), licenseTitle, licenseBox});
    Connect(openData, "clicked", [] { OpenFolder(iroha::platform::DataDirectory()); });
    return scrolled;
}

GtkWidget* LinkLabel(const std::string& prefix, const std::string& url) {
    GtkWidget* label = gtk_label_new(nullptr);
    std::string markup = Escape(prefix);
    if (!url.empty()) markup += "<a href=\"" + Escape(url) + "\">" + Escape(url) + "</a>";
    gtk_label_set_markup(GTK_LABEL(label), markup.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_CHAR);
    return label;
}

// ライセンスの一覧は、入れた専門辞書・打ち間違い訂正モデルで変わるので、タブを開くたびに作り直す
void App::RefreshAboutPage() {
    while (GtkWidget* child = gtk_widget_get_first_child(licenseBox)) gtk_box_remove(GTK_BOX(licenseBox), child);
    for (const auto& group : iroha::settings::Licenses(iroha::platform::DataDirectory())) {
        for (const auto& entry : group.entries) {
            GtkWidget* item = Box(GTK_ORIENTATION_VERTICAL, 2);
            GtkWidget* head = gtk_label_new(nullptr);
            std::string markup = "<b>" + Escape(entry.name) + "</b>";
            if (!entry.note.empty()) markup += "  <small>" + Escape(entry.note) + "</small>";
            markup += "    " + Escape(entry.license);
            gtk_label_set_markup(GTK_LABEL(head), markup.c_str());
            gtk_label_set_xalign(GTK_LABEL(head), 0);
            gtk_label_set_wrap(GTK_LABEL(head), TRUE);
            GtkWidget* source = LinkLabel("© " + entry.holder + "  ", entry.url);
            gtk_widget_add_css_class(source, "dim-label");
            Append(item, {head, source});
            if (!entry.licenseUrl.empty()) {
                GtkWidget* license = LinkLabel("ライセンス文書: ", entry.licenseUrl);
                gtk_widget_add_css_class(license, "dim-label");
                Append(item, {license});
            }
            gtk_box_append(GTK_BOX(licenseBox), item);
        }
        if (!group.footnote.empty()) {
            GtkWidget* footnote = Label(group.footnote, true);
            gtk_widget_add_css_class(footnote, "dim-label");
            gtk_box_append(GTK_BOX(licenseBox), footnote);
        }
    }
}

// ---- 起動 ----

void PrintUsage(std::FILE* out) {
    std::string tabs;
    for (int i = 0; i < iroha::settings::kTabCount; ++i) {
        if (i > 0) tabs += "|";
        tabs += iroha::settings::TabName(static_cast<Tab>(i));
    }
    std::fprintf(out,
                 "使い方: iroha-settings [--tab %s]\n"
                 "  iroha（かな漢字変換）の設定画面を開きます。すでに開いていれば、その窓を前に出します。\n"
                 "  --tab <名前>  開くタブ（既定は model）\n"
                 "  --help        この説明を出します\n"
                 "データフォルダ: %s\n",
                 tabs.c_str(), iroha::PathToUtf8(iroha::platform::DataDirectory()).c_str());
}

// 引数から開くタブを読む（--tab <名前> と --tab=<名前>）。名前が読めなければ error に理由
std::optional<Tab> ParseArguments(int argc, char** argv, std::string* error) {
    std::optional<Tab> tab;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        std::string name;
        if (argument == "--tab") {
            if (i + 1 >= argc) {
                *error = "--tab の後にタブの名前がありません";
                return std::nullopt;
            }
            name = argv[++i];
        } else if (argument.rfind("--tab=", 0) == 0) {
            name = argument.substr(6);
        } else {
            *error = "知らない引数です: " + argument;
            return std::nullopt;
        }
        tab = iroha::settings::ParseTabName(name);
        if (!tab) {
            *error = "知らないタブの名前です: " + name;
            return std::nullopt;
        }
    }
    return tab;
}

// 起動のたびに（2 つめの起動では、前から動いている方で）呼ばれる
int OnCommandLine(GApplication* application, GApplicationCommandLine* commandLine, gpointer) {
    int argc = 0;
    gchar** argv = g_application_command_line_get_arguments(commandLine, &argc);
    std::string error;
    const std::optional<Tab> tab = ParseArguments(argc, argv, &error);
    g_strfreev(argv);
    if (!error.empty()) {
        g_application_command_line_printerr(commandLine, "iroha-settings: %s\n", error.c_str());
        return 2;
    }
    if (!g_app->window) {
        App& app = *g_app;
        app.application = GTK_APPLICATION(application);
        app.dictionary = std::make_unique<iroha::UserDictionaryStore>(iroha::settings::UserDictionaryPath());
        app.learning = std::make_unique<iroha::LearningStore>(iroha::settings::LearningPath());
        // 変換ルールは IBus のエンジンと同じく、初めてなら既定のルール（きょう・あした など）を用意する
        app.rewriteRules =
            std::make_unique<iroha::UserRewriteRuleStore>(iroha::settings::RewriteRulesPath(), /*seedsDefaults=*/true);
        app.inputHistory = std::make_unique<iroha::InputHistoryStore>(iroha::settings::InputHistoryDirectory(),
                                                                      iroha::platform::HostName(), /*saveDelayMs=*/0);
        app.conversionLog = std::make_unique<iroha::ConversionLog>(iroha::settings::ConversionLogDirectory(),
                                                                   iroha::platform::HostName());
        app.Build();
    }
    if (tab) {
        if (gtk_notebook_get_current_page(GTK_NOTEBOOK(g_app->notebook)) == static_cast<int>(*tab)) {
            g_app->OnTabShown(*tab);
        } else {
            gtk_notebook_set_current_page(GTK_NOTEBOOK(g_app->notebook), static_cast<int>(*tab));
        }
    }
    gtk_window_present(g_app->window);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    // --help と引数の誤りは、画面につながずに答える（DISPLAY が無くても動く）
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            PrintUsage(stdout);
            return 0;
        }
    }
    std::string error;
    ParseArguments(argc, argv, &error);
    if (!error.empty()) {
        std::fprintf(stderr, "iroha-settings: %s\n", error.c_str());
        PrintUsage(stderr);
        return 2;
    }

    // App は最後まで消さない（裏のスレッドが終わる前に窓を閉じても、戻ってきた知らせが参照できるように）
    g_app = new App();
    GtkApplication* application = gtk_application_new(kApplicationId, G_APPLICATION_HANDLES_COMMAND_LINE);
    g_signal_connect(application, "command-line", G_CALLBACK(OnCommandLine), nullptr);
    const int status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    return status;
}
