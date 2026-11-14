#include "benchmark_section.h"

#include <signal.h>
#include <sys/resource.h>

#include <algorithm>
#include <cstdio>
#include <thread>

#include "benchmark_fetcher.h"
#include "iroha/platform.h"
#include "iroha/unicode.h"

namespace benchmark_section {

namespace {

using iroha::benchmark_run::Lattice;

// ---- 文字列 ----

std::string Format(const char* format, double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), format, value);
    return buffer;
}

std::string Percent(double ratio, int digits) {
    return Format(digits == 1 ? "%.1f%%" : digits == 2 ? "%.2f%%" : "%.0f%%", ratio * 100);
}

// 「173 KB」「1.6 MB」（Windows版と同じ 1000 区切り）
std::string ByteCount(uint64_t bytes) {
    if (bytes < 1000 * 1000) return Format("%.0f KB", static_cast<double>(bytes) / 1000);
    return Format("%.1f MB", static_cast<double>(bytes) / 1000 / 1000);
}

std::string Escape(const std::string& text) {
    gchar* escaped = g_markup_escape_text(text.c_str(), -1);
    std::string result = escaped;
    g_free(escaped);
    return result;
}

std::string Stem(const std::string& fileName) { return iroha::PathToUtf8(iroha::PathFromUtf8(fileName).stem()); }

std::string RecordTitle(const iroha::benchmark_run::Record& record) {
    return iroha::benchmark_run::FormatLocalDateTime(record.startedAt) + "  " + record.benchmarkName + "・" +
           iroha::benchmark_run::LatticeDisplayName(record.condition.lattice) + "・" + Stem(record.condition.model);
}

// 1 回の計測の結果（精度の表・速度の表・読みの長さ別の中央値）。表の列はタブで区切る（Windows版と同じ中身）
std::string RecordText(const iroha::benchmark_run::Record& record) {
    const auto& s = record.summary;
    const auto& e = record.environment;
    std::string text = RecordTitle(record) + "\n";
    text += record.condition.SummaryText() + "\n";
    text += e.cpu + (e.machineModel ? "（" + *e.machineModel + "）" : "") + "・" + e.osVersion + "・iroha " +
            e.appVersion + "\n";
    if (record.inputDuringRun && *record.inputDuringRun) {
        text += "※ 計測中にキーボードかマウスの入力がありました。速度の数字は入力の変換と取り合った値です。\n";
    }
    if (const auto note = e.ThrottlingNote()) text += "※ " + *note + "。速度の数字はこの影響を受けています。\n";
    text += "\n区分\t件数\tacc@1\tMinCER\n";
    const auto row = [&](const std::string& label, const iroha::benchmark::Tally& tally) {
        if (tally.count == 0) return;
        text += label + "\t" + std::to_string(tally.count) + "\t" + Percent(tally.Accuracy(), 1) + "\t" +
                Percent(tally.MeanMinCER(), 2) + "\n";
    };
    row("全体", s.total);
    row("文脈あり", s.withContext);
    row("文脈なし", s.withoutContext);
    for (const auto& source : s.perSource) row(source.source, source.tally);
    const auto& l = s.latency;
    text += "\n平均\t中央値\t90%\t99%\t最大\tNN の割合\n";
    text += Format("%.1f", l.mean) + "\t" + Format("%.1f", l.p50) + "\t" + Format("%.1f", l.p90) + "\t" +
            Format("%.1f", l.p99) + "\t" + Format("%.1f", l.max) + "\t" +
            (s.neuralNetworkShare ? Percent(*s.neuralNetworkShare, 0) : "—") + "\n";
    text += "\n読みの文字数";
    for (const auto& bin : s.lengthBins) text += "\t" + bin.Label();
    text += "\n中央値";
    for (const auto& bin : s.lengthBins) text += "\t" + Format("%.1f", bin.p50);
    text += "\n件数";
    for (const auto& bin : s.lengthBins) text += "\t" + std::to_string(bin.count);
    text += "\n\n単位はミリ秒（1 件 = 読み 1 つの変換 1 回）。モデルの読み込みを含む最初の変換 " +
            Format("%.0f", record.warmUpMilliseconds) + " ms は除いています。";
    return text;
}

std::string MissesText(const iroha::benchmark_run::Record& record) {
    std::string text = "間違えた項目（" + std::to_string(record.misses.size()) + " 件）\n";
    for (const auto& miss : record.misses) {
        std::string expected;
        for (size_t i = 0; i < miss.expected.size(); ++i) {
            if (i > 0) expected += " / ";
            expected += miss.expected[i];
        }
        text += "\n" + miss.output + "\n  正解: " + expected + "\n  読み: " + miss.input +
                (miss.context.empty() ? "" : "　文脈: " + miss.context) + "\n  " + miss.index + "\n";
    }
    return text;
}

// ---- 部品 ----

using Handler = std::function<void()>;

void DestroyHandler(gpointer data, GClosure*) { delete static_cast<Handler*>(data); }
void OnSignal(gpointer, gpointer data) { (*static_cast<Handler*>(data))(); }
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

GtkWidget* Label(const std::string& text, bool wrap = false) {
    GtkWidget* label = gtk_label_new(text.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    if (wrap) {
        gtk_label_set_wrap(GTK_LABEL(label), TRUE);
        gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    }
    return label;
}

GtkWidget* Note(const std::string& text) {
    GtkWidget* label = Label(text, true);
    gtk_widget_add_css_class(label, "dim-label");
    return label;
}

// 項目の見出し（左）と、その右に並べる部品
GtkWidget* Item(const char* title, std::initializer_list<GtkWidget*> right) {
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* label = Label(title);
    // 見出しの幅は一番長い「測る評価セット」に合わせる（右の部品の位置をそろえる）
    gtk_label_set_width_chars(GTK_LABEL(label), 16);
    gtk_widget_set_valign(label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(row), label);
    for (GtkWidget* child : right) {
        gtk_widget_set_valign(child, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(row), child);
    }
    return row;
}

// 横に並べる（隣の説明が折り返して高くなっても、ボタンは伸ばさず上に寄せる）
GtkWidget* Buttons(std::initializer_list<GtkWidget*> buttons) {
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    for (GtkWidget* button : buttons) {
        gtk_widget_set_valign(button, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(row), button);
    }
    return row;
}

struct Cell {
    std::string text;
    int widthChars;
};
GtkWidget* ListRow(const std::vector<Cell>& cells, bool heading = false) {
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
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

GtkWidget* ListBox(GtkWidget** scrolled, int height) {
    GtkWidget* list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_SINGLE);
    *scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(*scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(*scrolled), list);
    gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(*scrolled), TRUE);
    gtk_widget_set_size_request(*scrolled, -1, height);
    return list;
}

void ClearList(GtkWidget* list) {
    while (GtkWidget* child = gtk_widget_get_first_child(list)) gtk_list_box_remove(GTK_LIST_BOX(list), child);
}

int SelectedRow(GtkWidget* list) {
    GtkListBoxRow* row = gtk_list_box_get_selected_row(GTK_LIST_BOX(list));
    return row ? gtk_list_box_row_get_index(row) : -1;
}

void SetDropDownItems(GtkWidget* dropDown, const std::vector<std::string>& items) {
    std::vector<const char*> strings;
    for (const auto& item : items) strings.push_back(item.c_str());
    strings.push_back(nullptr);
    gtk_drop_down_set_model(GTK_DROP_DOWN(dropDown), G_LIST_MODEL(gtk_string_list_new(strings.data())));
}

int Selected(GtkWidget* dropDown) {
    const guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropDown));
    return selected == GTK_INVALID_LIST_POSITION ? -1 : static_cast<int>(selected);
}

void SetStatus(GtkWidget* label, const std::string& text, bool warning) {
    gtk_label_set_text(GTK_LABEL(label), text.c_str());
    if (warning) {
        gtk_widget_remove_css_class(label, "dim-label");
        gtk_widget_add_css_class(label, "error");
    } else {
        gtk_widget_remove_css_class(label, "error");
        gtk_widget_add_css_class(label, "dim-label");
    }
}

// 計測ヘルパーの子プロセス: 優先度を下げる（計測中も入力の変換を優先させる。macOS版・Windows版と同じ考え方）
void LowerPriority(gpointer) { setpriority(PRIO_PROCESS, 0, 10); }

} // namespace

struct Section::Impl {
    enum class State { Idle, Loading, Running, Failed, Cancelled };

    std::filesystem::path dataDirectory;
    std::string appVersion;
    std::function<void(const std::string&)> showDialog;
    // 裏のスレッド・子プロセスから戻ってきた知らせが、消えた部品に触らないための印（Impl が消えたら偽）
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    bool visible = false;
    std::filesystem::path modelPath;
    std::filesystem::path adapterPath;

    GtkWidget* setList = nullptr;
    GtkWidget* setFetch = nullptr;
    GtkWidget* setRemove = nullptr;
    GtkWidget* setStatus = nullptr;
    GtkWidget* setCombo = nullptr;
    GtkWidget* latticeCombo = nullptr;
    GtkWidget* target = nullptr;
    GtkWidget* start = nullptr;
    GtkWidget* cancel = nullptr;
    GtkWidget* runStatus = nullptr;
    GtkWidget* progress = nullptr;
    GtkWidget* resultBox = nullptr;
    GtkWidget* resultTitle = nullptr;
    GtkWidget* result = nullptr;
    GtkWidget* misses = nullptr;
    GtkWidget* historyBox = nullptr;
    GtkWidget* history = nullptr;
    GtkWidget* deleteRecord = nullptr;

    std::unique_ptr<iroha::BenchmarkCatalog> catalog;
    std::string catalogMessage;
    bool catalogFailed = false;
    bool catalogRequested = false;
    bool taskBusy = false;
    std::vector<iroha::BenchmarkInstall::Record> installed;
    std::vector<iroha::benchmark_run::Record> records;
    // 表の行（カタログの評価セット + カタログに無いが入れてあるもの）の ID
    std::vector<std::string> setRows;
    // 開いている結果（空なら直近のもの）と、間違えた項目を出しているか
    std::string shownRecordId;
    bool showingMisses = false;
    // 測る評価セット（設定画面を開いている間だけ覚える）
    std::string selectedSet;
    // 評価セットの選択肢を作り直している間は、選択の知らせで覚え直さない
    bool updatingCombo = false;

    State state = State::Idle;
    int done = 0;
    int total = 0;
    int hits = 0;
    std::string failure;
    GSubprocess* process = nullptr;
    GDataInputStream* output = nullptr;
    unsigned generation = 0;
    // 子プロセスの終了と標準出力の終わりの両方が揃ってから終了を扱う（終了の知らせが最後の行より先に届いても、
    // 「計測を終えた」行を読む前に失敗と扱わないため）
    bool exited = false;
    bool outputEnded = false;
    bool exitSignaled = false;
    int exitStatus = 0;

    void Build(GtkWidget* root);
    void RefreshInstalled();
    void RefreshRecords();
    void UpdateControls();
    void ShowRecord();
    void StartTask(const std::string& busyMessage, bool fetchCatalog, std::string installId);
    void Start();
    void Cancel();
    void OnLine(const std::string& line);
    void OnExit();
    void FinishIfDone();
    void ReadNextLine(unsigned generation);
    const iroha::benchmark_run::Record* ShownRecord() const;
    std::optional<std::filesystem::path> HelperPath() const;
    std::filesystem::path LogPath() const;
    Lattice SelectedLattice() const { return Selected(latticeCombo) == 1 ? Lattice::Always : Lattice::Off; }
};

void Section::Impl::Build(GtkWidget* root) {
    GtkWidget* title = gtk_label_new(nullptr);
    // 開発者モードの項目の印（macOS版の DeveloperBadge と同じ紫）
    gtk_label_set_markup(GTK_LABEL(title), "<b>変換のベンチマーク</b>  <span foreground=\"#7c3aed\" size=\"small\">"
                                           "<b>開発者</b></span>");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    GtkWidget* description = Note(
        "評価セットを 1 件ずつ、今使っているモデル・アダプタで変換し、正解との一致（acc@1 = 正解のどれかと完全に一致した割合、"
        "MinCER = 正解との文字の違いの割合）と、1 回の変換にかかった時間を測ります。"
        "ユーザ辞書・学習・専門辞書は使いません（ほかの機械の結果と比べられるようにするため）。"
        "計測は別のプロセス（iroha-bench）で行うので計測中も入力できますが、そのあいだの速度の数字は入力の変換と"
        "取り合った値になります（結果に印が付きます）。ノート PC は電源につないで測ってください。"
        "結果はデータフォルダの logs/bench/ に残ります。.tsv は testdata/speed/summarize.py でそのまま集計できます。"
        "評価セットは「取得」を押したときにダウンロードします（iroha-bench-v1 は CC BY-SA 4.0）。");

    // 評価セット
    GtkWidget* setScrolled = nullptr;
    setList = ListBox(&setScrolled, 96);
    setFetch = gtk_button_new_with_label("取得");
    setRemove = gtk_button_new_with_label("削除");
    setStatus = Note("");
    gtk_widget_set_hexpand(setStatus, TRUE);
    GtkWidget* setButtons = Buttons({setFetch, setRemove, setStatus});

    // 測る評価セット・条件・モデル
    static const char* const kNoSets[] = {nullptr};
    setCombo = gtk_drop_down_new_from_strings(kNoSets);
    gtk_widget_set_size_request(setCombo, 260, -1);
    const char* const lattices[] = {iroha::benchmark_run::LatticeDisplayName(Lattice::Off),
                                    iroha::benchmark_run::LatticeDisplayName(Lattice::Always), nullptr};
    latticeCombo = gtk_drop_down_new_from_strings(lattices);
    gtk_widget_set_size_request(latticeCombo, 260, -1);
    GtkWidget* latticeNote = Note("「モデルだけ」はライブ変換の第一候補と同じ経路（iroha-cli の IROHA_LATTICE=off）、"
                                  "「辞書ラティス + 再採点」は辞書ラティスの候補をモデルで採点し直して第一候補を選ぶ経路"
                                  "（IROHA_LATTICE=always）です。");
    target = Label("", true);
    gtk_widget_set_hexpand(target, TRUE);

    // 計測
    start = gtk_button_new_with_label("計測を開始");
    cancel = gtk_button_new_with_label("中止");
    runStatus = Note("");
    gtk_widget_set_hexpand(runStatus, TRUE);
    progress = gtk_progress_bar_new();
    gtk_widget_set_visible(progress, FALSE);

    // 結果（写せるよう、編集できない文字の欄に出す。表の列はタブで区切る）
    resultBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    resultTitle = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(resultTitle), 0);
    result = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(result), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(result), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(result), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(result), 6);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(result), 4);
    PangoTabArray* tabs = pango_tab_array_new(8, TRUE);
    for (int i = 0; i < 8; ++i) pango_tab_array_set_tab(tabs, i, PANGO_TAB_LEFT, 110 + i * 84);
    gtk_text_view_set_tabs(GTK_TEXT_VIEW(result), tabs);
    pango_tab_array_free(tabs);
    GtkWidget* resultScrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(resultScrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(resultScrolled), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(resultScrolled), result);
    gtk_widget_set_size_request(resultScrolled, -1, 330);
    GtkWidget* copy = gtk_button_new_with_label("Markdown でコピー");
    GtkWidget* folder = gtk_button_new_with_label("記録のフォルダを開く");
    misses = gtk_button_new_with_label("間違えた項目");
    gtk_box_append(GTK_BOX(resultBox), resultTitle);
    gtk_box_append(GTK_BOX(resultBox), resultScrolled);
    gtk_box_append(GTK_BOX(resultBox), Buttons({copy, folder, misses}));

    // 過去の結果
    historyBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget* historyScrolled = nullptr;
    history = ListBox(&historyScrolled, 140);
    deleteRecord = gtk_button_new_with_label("選んだ結果を削除");
    gtk_box_append(GTK_BOX(historyBox), Label("過去の結果（選ぶと上の結果に出します）"));
    gtk_box_append(GTK_BOX(historyBox), ListRow({{"日時", 14}, {"評価セット", 14}, {"条件", 16}, {"モデル", 18},
                                                 {"acc@1", 6}, {"中央値", 9}, {"機械", 0}},
                                                /*heading=*/true));
    gtk_box_append(GTK_BOX(historyBox), historyScrolled);
    gtk_box_append(GTK_BOX(historyBox), Buttons({deleteRecord}));

    for (GtkWidget* child :
         {gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), title, description,
          Label("評価セット"), ListRow({{"評価セット", 22}, {"件数", 6}, {"大きさ", 8}, {"状態", 0}}, /*heading=*/true),
          setScrolled, setButtons, Item("測る評価セット", {setCombo}), Item("条件", {latticeCombo}), latticeNote,
          Item("測るモデル", {target}), Item("計測", {start, cancel, runStatus}), progress, resultBox, historyBox}) {
        gtk_box_append(GTK_BOX(root), child);
    }

    Connect(setList, "selected-rows-changed", [this] {
        if (!taskBusy && !catalogFailed) catalogMessage.clear();
        UpdateControls();
    });
    Connect(setFetch, "clicked", [this] {
        const int row = SelectedRow(setList);
        if (row < 0 || row >= static_cast<int>(setRows.size()) || !catalog) return;
        if (const auto* entry = catalog->Find(setRows[row])) {
            StartTask(entry->name + " を取得しています…", false, entry->id);
        }
    });
    Connect(setRemove, "clicked", [this] {
        const int row = SelectedRow(setList);
        if (row < 0 || row >= static_cast<int>(setRows.size()) || process) return;
        iroha::BenchmarkInstall::Remove(dataDirectory, setRows[row]);
        catalogMessage.clear();
        RefreshInstalled();
    });
    ConnectNotify(setCombo, "selected", [this] {
        if (updatingCombo) return;
        const int index = Selected(setCombo);
        if (index >= 0 && index < static_cast<int>(installed.size())) selectedSet = installed[index].id;
    });
    Connect(start, "clicked", [this] {
        if (state == State::Failed || state == State::Cancelled) state = State::Idle;
        Start();
    });
    Connect(cancel, "clicked", [this] { Cancel(); });
    Connect(copy, "clicked", [this] {
        if (const auto* record = ShownRecord()) {
            gdk_clipboard_set_text(gtk_widget_get_clipboard(result), iroha::benchmark_run::Markdown(*record).c_str());
        }
    });
    Connect(folder, "clicked", [this] {
        const std::filesystem::path directory = iroha::benchmark_run::Directory(dataDirectory);
        std::error_code ignored;
        std::filesystem::create_directories(directory, ignored);
        gchar* uri = g_filename_to_uri(iroha::PathToUtf8(directory).c_str(), nullptr, nullptr);
        if (!uri) return;
        GError* error = nullptr;
        if (!g_app_info_launch_default_for_uri(uri, nullptr, &error)) {
            showDialog("フォルダを開けませんでした: " + std::string(error ? error->message : "") + "\n" +
                       iroha::PathToUtf8(directory));
            if (error) g_error_free(error);
        }
        g_free(uri);
    });
    Connect(misses, "clicked", [this] {
        showingMisses = !showingMisses;
        ShowRecord();
    });
    Connect(history, "selected-rows-changed", [this] {
        const int row = SelectedRow(history);
        if (row >= 0 && row < static_cast<int>(records.size())) {
            shownRecordId = records[row].id;
            showingMisses = false;
            ShowRecord();
        }
        UpdateControls();
    });
    Connect(deleteRecord, "clicked", [this] {
        const int row = SelectedRow(history);
        if (row < 0 || row >= static_cast<int>(records.size())) return;
        iroha::benchmark_run::Delete(dataDirectory, records[row].id);
        RefreshRecords();
        UpdateControls();
    });
    UpdateControls();
}

std::optional<std::filesystem::path> Section::Impl::HelperPath() const {
    const std::filesystem::path directory = iroha::platform::ExecutableDirectory();
    // インストールした版は同じフォルダ（/usr/local/lib/iroha）、開発中のビルドは linux/build/iroha-bench/ にある
    for (const std::filesystem::path& candidate :
         {directory / "iroha-bench", directory.parent_path() / "iroha-bench" / "iroha-bench"}) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error)) return candidate;
    }
    return std::nullopt;
}

std::filesystem::path Section::Impl::LogPath() const {
    return dataDirectory / "logs" / ("bench-" + iroha::platform::HostName() + ".log");
}

const iroha::benchmark_run::Record* Section::Impl::ShownRecord() const {
    for (const auto& record : records) {
        if (shownRecordId.empty() || record.id == shownRecordId) return &record;
    }
    return nullptr;
}

// ---- 評価セット ----

void Section::Impl::RefreshInstalled() {
    installed = iroha::BenchmarkInstall::InstalledRecords(dataDirectory);
    setRows.clear();
    if (catalog) {
        for (const auto& entry : catalog->benchmarks) setRows.push_back(entry.id);
    }
    // カタログから消えた・オフラインでも、入れてあるものは消せるようにする
    for (const auto& record : installed) {
        if (std::find(setRows.begin(), setRows.end(), record.id) == setRows.end()) setRows.push_back(record.id);
    }
    const int selected = SelectedRow(setList);
    ClearList(setList);
    for (const std::string& id : setRows) {
        const auto* entry = catalog ? catalog->Find(id) : nullptr;
        const auto record = std::find_if(installed.begin(), installed.end(), [&](const auto& r) { return r.id == id; });
        const bool isInstalled = record != installed.end();
        const std::string state = !isInstalled ? ""
                                  : !entry     ? "入れてある（一覧に無い）"
                                  : iroha::BenchmarkInstall::IsOutdated(*record, *entry) ? "入れてある（更新あり）"
                                                                                         : "入れてある";
        gtk_list_box_append(GTK_LIST_BOX(setList),
                            ListRow({{entry ? entry->name : record->name, 22},
                                     {std::to_string(entry ? entry->items : record->items), 6},
                                     {entry ? ByteCount(entry->file.bytes) : "", 8},
                                     {state, 0}}));
    }
    const int restore = selected >= 0 && selected < static_cast<int>(setRows.size()) ? selected
                        : setRows.empty()                                         ? -1
                                                                                  : 0;
    if (restore >= 0) {
        gtk_list_box_select_row(GTK_LIST_BOX(setList), gtk_list_box_get_row_at_index(GTK_LIST_BOX(setList), restore));
    }
    // 測る評価セットの選択肢（入れてあるもの）。選んだものが無ければ先頭
    std::vector<std::string> names;
    int chosen = 0;
    for (int i = 0; i < static_cast<int>(installed.size()); ++i) {
        names.push_back(installed[i].name + "（" + std::to_string(installed[i].items) + " 件）");
        if (installed[i].id == selectedSet) chosen = i;
    }
    updatingCombo = true;
    SetDropDownItems(setCombo, names);
    if (!installed.empty()) {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(setCombo), static_cast<guint>(chosen));
        selectedSet = installed[chosen].id;
    }
    updatingCombo = false;
    UpdateControls();
}

void Section::Impl::RefreshRecords() {
    records = iroha::benchmark_run::Records(dataDirectory);
    if (!shownRecordId.empty() &&
        std::none_of(records.begin(), records.end(), [&](const auto& r) { return r.id == shownRecordId; })) {
        shownRecordId.clear();
    }
    ClearList(history);
    for (const auto& record : records) {
        gtk_list_box_append(
            GTK_LIST_BOX(history),
            ListRow({{iroha::benchmark_run::FormatLocalDateTime(record.startedAt), 14},
                     {record.benchmarkName, 14},
                     {iroha::benchmark_run::LatticeDisplayName(record.condition.lattice), 16},
                     {Stem(record.condition.model), 18},
                     {Percent(record.summary.total.Accuracy(), 1), 6},
                     {Format("%.1f ms", record.summary.latency.p50), 9},
                     {record.environment.cpu + "（" + record.environment.hostName + "）", 0}}));
    }
    showingMisses = false;
    ShowRecord();
}

void Section::Impl::ShowRecord() {
    const iroha::benchmark_run::Record* record = ShownRecord();
    gtk_widget_set_visible(resultBox, visible && record != nullptr);
    gtk_widget_set_visible(historyBox, visible && records.size() > 1);
    GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(result));
    if (!record) {
        gtk_text_buffer_set_text(buffer, "", -1);
        gtk_widget_set_sensitive(misses, FALSE);
        return;
    }
    const std::string text = showingMisses ? MissesText(*record) : RecordText(*record);
    gtk_text_buffer_set_text(buffer, text.c_str(), -1);
    gtk_button_set_label(GTK_BUTTON(misses), showingMisses ? "結果に戻る"
                                                           : ("間違えた項目（" + std::to_string(record->misses.size()) +
                                                              " 件）")
                                                                 .c_str());
    gtk_widget_set_sensitive(misses, !record->misses.empty() || showingMisses);
    gtk_label_set_markup(GTK_LABEL(resultTitle),
                         record == &records.front() ? "<b>結果</b>（直近の結果）" : "<b>結果</b>（過去の結果）");
}

void Section::Impl::UpdateControls() {
    const bool running = process != nullptr;
    // 評価セットの表
    const int row = SelectedRow(setList);
    const bool valid = row >= 0 && row < static_cast<int>(setRows.size());
    const auto* entry = valid && catalog ? catalog->Find(setRows[row]) : nullptr;
    const auto record = valid ? std::find_if(installed.begin(), installed.end(),
                                             [&](const auto& r) { return r.id == setRows[row]; })
                              : installed.end();
    const bool isInstalled = record != installed.end();
    const bool outdated = isInstalled && entry && iroha::BenchmarkInstall::IsOutdated(*record, *entry);
    gtk_button_set_label(GTK_BUTTON(setFetch), outdated ? "更新" : "取得");
    gtk_widget_set_sensitive(setFetch, entry && !taskBusy && (!isInstalled || outdated));
    gtk_widget_set_sensitive(setRemove, isInstalled && !taskBusy && !running);
    std::string setDescription = catalogMessage;
    if (setDescription.empty()) {
        setDescription = entry && !entry->summary.empty() ? entry->summary
                         : entry                          ? "ライセンス: " + entry->license
                                                          : "";
    }
    SetStatus(setStatus, setDescription, catalogFailed);
    gtk_widget_set_sensitive(setCombo, !running && !installed.empty());
    gtk_widget_set_sensitive(latticeCombo, !running);
    // 測るモデル
    std::string targetText =
        modelPath.empty() ? "（未設定）" : iroha::PathToUtf8(modelPath.stem()) + "（llama.cpp）";
    std::error_code error;
    const bool modelMissing = !modelPath.empty() && !std::filesystem::exists(modelPath, error);
    if (modelMissing) targetText += "  モデルのファイルがありません";
    if (!adapterPath.empty()) targetText += "＋アダプタ " + iroha::PathToUtf8(adapterPath.filename());
    SetStatus(target, targetText, modelMissing);
    gtk_widget_remove_css_class(target, "dim-label");
    // 計測
    const bool helper = HelperPath().has_value();
    gtk_widget_set_sensitive(start, !running && helper && !installed.empty() && !modelPath.empty() && !modelMissing);
    gtk_widget_set_sensitive(cancel, running && state != State::Cancelled);
    gtk_widget_set_sensitive(deleteRecord, SelectedRow(history) >= 0);
    std::string status;
    bool warning = false;
    switch (state) {
    case State::Idle:
        status = !helper             ? "計測ヘルパー（iroha-bench）が見つかりません"
                 : installed.empty() ? "先に評価セットを取得してください"
                                     : "";
        warning = !helper;
        break;
    case State::Loading:
        status = "モデルを読み込み中…";
        break;
    case State::Running:
        status = std::to_string(done) + "/" + std::to_string(total) + " 件（正解 " + std::to_string(hits) +
                 " 件）。計測中に入力すると、速度の数字が乱れます。";
        break;
    case State::Failed:
        status = "計測に失敗しました: " + failure;
        warning = true;
        break;
    case State::Cancelled:
        status = "計測を中止しました。";
        break;
    }
    SetStatus(runStatus, status, warning);
    const bool measuring = running && (state == State::Loading || state == State::Running);
    gtk_widget_set_visible(progress, visible && measuring);
    if (measuring) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress),
                                      total > 0 ? std::min(1.0, static_cast<double>(done) / total) : 0);
    }
}

// 評価セットの一覧の取得・評価セットの取得は裏のスレッドで行い、終わったら main thread に戻す
void Section::Impl::StartTask(const std::string& busyMessage, bool fetchCatalog, std::string installId) {
    if (taskBusy) return;
    taskBusy = true;
    catalogMessage = busyMessage;
    catalogFailed = false;
    UpdateControls();
    std::shared_ptr<const iroha::BenchmarkCatalog> current =
        catalog ? std::make_shared<iroha::BenchmarkCatalog>(*catalog) : nullptr;
    struct Result {
        std::shared_ptr<bool> alive;
        Impl* impl;
        std::unique_ptr<iroha::BenchmarkCatalog> catalog;
        std::string message;
        bool failed = false;
    };
    auto* result = new Result{alive, this, nullptr, "", false};
    std::thread([result, fetchCatalog, installId = std::move(installId), current, data = dataDirectory] {
        std::string error;
        if (fetchCatalog) {
            auto fetched = std::make_unique<iroha::BenchmarkCatalog>();
            if (iroha::BenchmarkFetcher::FetchCatalog(iroha::BenchmarkCatalog::DefaultUrl(), fetched.get(), &error)) {
                result->catalog = std::move(fetched);
            } else {
                result->failed = true;
                result->message = "評価セットの一覧を取得できませんでした: " + error;
            }
        }
        if (!installId.empty() && current) {
            if (const auto* entry = current->Find(installId)) {
                if (iroha::BenchmarkFetcher::Install(*entry, data, nullptr, &error)) {
                    result->message = entry->name + " を取得しました";
                } else {
                    result->failed = true;
                    result->message = "取得できませんでした: " + entry->name + ": " + error;
                }
            }
        }
        g_idle_add(
            [](gpointer data) -> gboolean {
                std::unique_ptr<Result> result(static_cast<Result*>(data));
                if (!*result->alive) return G_SOURCE_REMOVE;
                Impl* impl = result->impl;
                impl->taskBusy = false;
                if (result->catalog) impl->catalog = std::move(result->catalog);
                impl->catalogMessage = result->message;
                impl->catalogFailed = result->failed;
                impl->RefreshInstalled();
                return G_SOURCE_REMOVE;
            },
            result);
    }).detach();
}

// ---- 計測 ----

void Section::Impl::Start() {
    const auto helper = HelperPath();
    if (!helper || process || installed.empty()) return;
    const auto record =
        std::find_if(installed.begin(), installed.end(), [&](const auto& r) { return r.id == selectedSet; });
    if (record == installed.end()) return;

    std::vector<std::string> arguments = {iroha::PathToUtf8(*helper), "run"};
    const auto add = [&](const char* key, const std::string& value) {
        arguments.push_back(key);
        arguments.push_back(value);
    };
    add("--set", iroha::PathToUtf8(iroha::BenchmarkInstall::FilePath(dataDirectory, record->id)));
    add("--id", record->id);
    add("--name", record->name);
    add("--model", iroha::PathToUtf8(modelPath));
    add("--backend", "llama");
    add("--lattice", iroha::benchmark_run::LatticeName(SelectedLattice()));
    add("--app-version", appVersion);
    if (!adapterPath.empty()) add("--adapter", iroha::PathToUtf8(adapterPath));
    std::vector<const char*> argv;
    for (const auto& argument : arguments) argv.push_back(argument.c_str());
    argv.push_back(nullptr);

    // 標準出力は進み具合（JSON Lines）。標準エラー（llama.cpp のログ）は logs/bench-<ホスト名>.log に残す
    GSubprocessLauncher* launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE);
    std::error_code ignored;
    std::filesystem::create_directories(LogPath().parent_path(), ignored);
    g_subprocess_launcher_set_stderr_file_path(launcher, iroha::PathToUtf8(LogPath()).c_str());
    g_subprocess_launcher_set_child_setup(launcher, LowerPriority, nullptr, nullptr);
    GError* error = nullptr;
    process = g_subprocess_launcher_spawnv(launcher, argv.data(), &error);
    g_object_unref(launcher);
    if (!process) {
        state = State::Failed;
        failure = "iroha-bench を起動できません（" + std::string(error ? error->message : "") + "）";
        if (error) g_error_free(error);
        UpdateControls();
        return;
    }
    ++generation;
    state = State::Loading;
    done = 0;
    total = record->items;
    hits = 0;
    failure.clear();
    exited = false;
    outputEnded = false;
    output = g_data_input_stream_new(g_subprocess_get_stdout_pipe(process));
    ReadNextLine(generation);

    // 終わったら終了の仕方を受け取る
    struct Waiter {
        std::shared_ptr<bool> alive;
        Impl* impl;
        unsigned generation;
    };
    g_subprocess_wait_async(
        process, nullptr,
        [](GObject* object, GAsyncResult* asyncResult, gpointer data) {
            std::unique_ptr<Waiter> waiter(static_cast<Waiter*>(data));
            GSubprocess* finished = G_SUBPROCESS(object);
            g_subprocess_wait_finish(finished, asyncResult, nullptr);
            if (!*waiter->alive || waiter->generation != waiter->impl->generation) return;
            Impl* impl = waiter->impl;
            impl->exited = true;
            impl->exitSignaled = g_subprocess_get_if_signaled(finished);
            impl->exitStatus = impl->exitSignaled ? g_subprocess_get_term_sig(finished)
                                                  : g_subprocess_get_exit_status(finished);
            impl->FinishIfDone();
        },
        new Waiter{alive, this, generation});
    UpdateControls();
}

// 標準出力を 1 行ずつ受け取る
void Section::Impl::ReadNextLine(unsigned expected) {
    struct Reader {
        std::shared_ptr<bool> alive;
        Impl* impl;
        unsigned generation;
    };
    g_data_input_stream_read_line_async(
        output, G_PRIORITY_DEFAULT, nullptr,
        [](GObject* object, GAsyncResult* asyncResult, gpointer data) {
            std::unique_ptr<Reader> reader(static_cast<Reader*>(data));
            gsize length = 0;
            gchar* line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(object), asyncResult, &length,
                                                               nullptr);
            if (!*reader->alive || reader->generation != reader->impl->generation) {
                g_free(line);
                return;
            }
            if (!line) { // 終わり（子プロセスが閉じた）
                reader->impl->outputEnded = true;
                reader->impl->FinishIfDone();
                return;
            }
            const std::string text(line, length);
            g_free(line);
            reader->impl->OnLine(text);
            reader->impl->ReadNextLine(reader->generation);
        },
        new Reader{alive, this, expected});
}

void Section::Impl::Cancel() {
    if (!process) return;
    state = State::Cancelled;
    g_subprocess_send_signal(process, SIGTERM);
    UpdateControls();
}

void Section::Impl::OnLine(const std::string& line) {
    if (state == State::Cancelled) return;
    const auto event = iroha::benchmark_run::Event::Parse(line);
    if (!event) return;
    using Kind = iroha::benchmark_run::Event::Kind;
    switch (event->kind) {
    case Kind::Loading:
        state = State::Loading;
        total = event->total.value_or(total);
        break;
    case Kind::Progress:
        state = State::Running;
        done = event->done.value_or(0);
        total = event->total.value_or(0);
        hits = event->hits.value_or(0);
        break;
    case Kind::Finished:
        // 新しい記録が一覧の先頭に来るので、それを開く
        state = State::Idle;
        shownRecordId.clear();
        RefreshRecords();
        break;
    case Kind::Failed:
        state = State::Failed;
        failure = event->message.value_or("計測に失敗しました");
        break;
    }
    UpdateControls();
}

void Section::Impl::FinishIfDone() {
    if (process && exited && outputEnded) OnExit();
}

void Section::Impl::OnExit() {
    if (output) {
        g_object_unref(output);
        output = nullptr;
    }
    if (process) {
        g_object_unref(process);
        process = nullptr;
    }
    if (state == State::Loading || state == State::Running) {
        state = State::Failed;
        failure = exitSignaled ? "計測が途中で終了しました（シグナル " + std::to_string(exitStatus) + "）"
                               : "計測が途中で終了しました（終了コード " + std::to_string(exitStatus) + "）";
        failure += "。詳細は logs/" + iroha::PathToUtf8(LogPath().filename()) + " を参照";
    }
    UpdateControls();
}

// ---- Section ----

Section::Section(std::filesystem::path dataDirectory, std::string appVersion,
                 std::function<void(const std::string&)> showDialog)
    : impl_(new Impl) {
    impl_->dataDirectory = std::move(dataDirectory);
    impl_->appVersion = std::move(appVersion);
    impl_->showDialog = std::move(showDialog);
    root_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(root_, 12);
    impl_->Build(root_);
    gtk_widget_set_visible(root_, FALSE);
}

Section::~Section() {
    Shutdown();
    *impl_->alive = false;
}

void Section::SetVisible(bool visible) {
    impl_->visible = visible;
    gtk_widget_set_visible(root_, visible);
    impl_->ShowRecord();
    impl_->UpdateControls();
}

void Section::SetTarget(const std::filesystem::path& modelPath, const std::filesystem::path& adapterPath) {
    impl_->modelPath = modelPath;
    impl_->adapterPath = adapterPath;
    impl_->UpdateControls();
}

void Section::Show() {
    if (!impl_->visible) return;
    impl_->RefreshInstalled();
    impl_->RefreshRecords();
    if (!impl_->catalogRequested) {
        impl_->catalogRequested = true;
        impl_->StartTask("評価セットの一覧を取得しています…", true, {});
    }
}

void Section::Shutdown() {
    if (impl_->process) {
        g_subprocess_force_exit(impl_->process);
        // 記録が途中で増えないよう、待つのは子プロセスが消えるまでにとどめ、知らせは捨てる
        ++impl_->generation;
        g_object_unref(impl_->process);
        impl_->process = nullptr;
    }
    if (impl_->output) {
        g_object_unref(impl_->output);
        impl_->output = nullptr;
    }
}

} // namespace benchmark_section
