#include "updater.h"

#include <sys/wait.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

#include "iroha/config.h"
#include "iroha/unicode.h"
#include "iroha/update_check.h"
#include "settings_support.h"
#include "term_dictionary_fetcher.h"

namespace updater {

namespace {

int64_t Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string WithoutV(const std::string& tag) {
    return !tag.empty() && (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag;
}

std::string Escape(const std::string& text) {
    gchar* escaped = g_markup_escape_text(text.c_str(), -1);
    std::string result = escaped;
    g_free(escaped);
    return result;
}

// 最後の lines 行（失敗したときに出す）
std::string Tail(const std::string& text, size_t lines) {
    size_t position = text.size();
    while (position > 0 && (text[position - 1] == '\n' || text[position - 1] == ' ')) --position;
    const size_t end = position;
    size_t count = 0;
    while (position > 0) {
        if (text[position - 1] == '\n' && ++count >= lines) break;
        --position;
    }
    return text.substr(position, end - position);
}

// main thread で動かす（窓の有無にかかわらず。設定の窓が無いときも知らせの窓を出すため）
void OnMain(std::function<void()> function) {
    g_idle_add(
        [](gpointer data) -> gboolean {
            std::unique_ptr<std::function<void()>> function(static_cast<std::function<void()>*>(data));
            (*function)();
            return G_SOURCE_REMOVE;
        },
        new std::function<void()>(std::move(function)));
}

using Handler = std::function<void()>;
void DestroyHandler(gpointer data, GClosure*) { delete static_cast<Handler*>(data); }
void OnSignal(gpointer, gpointer data) { (*static_cast<Handler*>(data))(); }
void Connect(gpointer instance, const char* signal, Handler handler) {
    g_signal_connect_data(instance, signal, G_CALLBACK(OnSignal), new Handler(std::move(handler)), DestroyHandler,
                          static_cast<GConnectFlags>(0));
}

GtkWidget* Label(const std::string& text) {
    GtkWidget* label = gtk_label_new(text.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 60);
    return label;
}

GtkWidget* Title(const std::string& text) {
    GtkWidget* label = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(label), ("<b>" + Escape(text) + "</b>").c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    return label;
}

struct CommandResult {
    bool started = false;
    int exitCode = -1;   // 終了コード（シグナルで終わったら -1）
    std::string output;  // 標準出力と標準エラー
    std::string error;   // 起動できなかった理由
};

// コマンドを動かして終わるのを待つ（裏のスレッドから呼ぶ）
CommandResult RunCommand(const std::vector<std::string>& arguments) {
    std::vector<char*> argv;
    for (const std::string& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    CommandResult result;
    gchar* out = nullptr;
    gchar* err = nullptr;
    gint status = 0;
    GError* error = nullptr;
    result.started = g_spawn_sync(nullptr, argv.data(), nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, &out, &err,
                                  &status, &error);
    if (!result.started) {
        result.error = error ? error->message : "起動できません";
        if (error) g_error_free(error);
        return result;
    }
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    result.output = std::string(out ? out : "") + std::string(err ? err : "");
    g_free(out);
    g_free(err);
    return result;
}

// 確認から入れ替えまでの 1 回分（同時に 1 つだけ）
struct Job {
    Host host;
    std::filesystem::path data;
    std::string currentVersion;
    bool manual = false;
    iroha::ReleaseInfo release;
    iroha::ReleaseAsset asset;
    std::filesystem::path work;  // 取得・展開の一時フォルダ（0700）
    GtkWindow* window = nullptr; // 今出している窓
    // 取得の進み具合
    std::atomic<bool> cancel{false};
    std::atomic<uint64_t> received{0};
    std::atomic<uint64_t> total{0};
    bool installing = false; // 入れ替えの途中（窓を閉じさせない）
    GtkWidget* status = nullptr;
    GtkWidget* progress = nullptr;
    GtkWidget* cancelButton = nullptr;
    guint timer = 0;
};

std::shared_ptr<Job> g_job;

void Finish(const std::shared_ptr<Job>& job) {
    if (g_job != job) return;
    g_job.reset();
    g_application_release(G_APPLICATION(job->host.application));
}

GtkWindow* NewWindow(const std::shared_ptr<Job>& job) {
    GtkWindow* window = GTK_WINDOW(gtk_window_new());
    gtk_window_set_title(window, "iroha のアップデート");
    gtk_window_set_application(window, job->host.application);
    if (GtkWindow* parent = job->host.parent ? job->host.parent() : nullptr) gtk_window_set_transient_for(window, parent);
    gtk_window_set_resizable(window, FALSE);
    job->window = window;
    return window;
}

GtkWidget* Content(GtkWindow* window) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(box, 20);
    gtk_widget_set_margin_end(box, 20);
    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 14);
    gtk_widget_set_size_request(box, 420, -1);
    gtk_window_set_child(window, box);
    return box;
}

GtkWidget* ButtonRow(GtkWidget* box) {
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(row, GTK_ALIGN_END);
    gtk_widget_set_margin_top(row, 6);
    gtk_box_append(GTK_BOX(box), row);
    return row;
}

// 知らせ（閉じたら onClose）
void ShowInfo(const std::shared_ptr<Job>& job, const std::string& title, const std::string& text, Handler onClose) {
    GtkWindow* window = NewWindow(job);
    GtkWidget* box = Content(window);
    gtk_box_append(GTK_BOX(box), Title(title));
    if (!text.empty()) {
        GtkWidget* label = Label(text);
        gtk_label_set_selectable(GTK_LABEL(label), TRUE);
        gtk_box_append(GTK_BOX(box), label);
    }
    GtkWidget* close = gtk_button_new_with_label("閉じる");
    gtk_box_append(GTK_BOX(ButtonRow(box)), close);
    Connect(close, "clicked", [window] { gtk_window_destroy(window); });
    Connect(window, "destroy", [onClose] {
        if (onClose) onClose();
    });
    gtk_window_set_default_widget(window, close);
    gtk_window_set_focus(window, close);
    gtk_window_present(window);
}

void ShowOffer(const std::shared_ptr<Job>& job);
void StartInstall(const std::shared_ptr<Job>& job);

bool FetchLatest(const std::filesystem::path& data, iroha::ReleaseInfo* release, std::string* error) {
    const iroha::Config config = iroha::LoadConfig(data / "config.json");
    const std::string url = config.updateCheckURL.empty() ? iroha::Update::kLatestReleaseUrl : config.updateCheckURL;
    gchar* directory = g_dir_make_tmp("iroha-check-XXXXXX", nullptr);
    if (!directory) {
        *error = "一時フォルダを作れません";
        return false;
    }
    const std::filesystem::path folder = directory;
    g_free(directory);
    const std::filesystem::path path = folder / "latest.json";
    bool ok = iroha::TermDictionaryFetcher::Download(url, path, error);
    std::string text;
    if (ok) {
        std::ifstream file(path, std::ios::binary);
        std::stringstream buffer;
        buffer << file.rdbuf();
        text = buffer.str();
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
    if (ok && !iroha::Update::ParseRelease(text, release)) {
        *error = "返事を読めません";
        ok = false;
    }
    return ok;
}

void OnFetched(const std::shared_ptr<Job>& job, bool ok, const std::string& error) {
    const auto done = [job] { Finish(job); };
    const std::string current = "現在の版: " + job->currentVersion;
    if (!ok) {
        if (job->manual) {
            ShowInfo(job, "アップデートを確認できませんでした", "ネットワーク接続を確認してください。\n（" + error + "）", done);
        } else {
            Finish(job);
        }
        return;
    }
    if (!iroha::Update::IsNewer(job->release.tag, iroha::Update::ComparableVersion(job->currentVersion))) {
        if (job->manual) {
            ShowInfo(job, "iroha は最新です", current, done);
        } else {
            Finish(job);
        }
        return;
    }
#if defined(__x86_64__)
    const iroha::ReleaseAsset* bundle = iroha::Update::LinuxBundle(job->release);
#else
    const iroha::ReleaseAsset* bundle = nullptr; // 配布物は x86_64 だけ
#endif
    if (!bundle) {
        if (job->manual) {
            ShowInfo(job, "新しい版に Linux版の配布物がありません",
                     "iroha " + WithoutV(job->release.tag) + " のリリースに、この機械で使える Linux版が含まれていません。",
                     done);
        } else {
            Finish(job);
        }
        return;
    }
    job->asset = *bundle;
    // 同じ版を何度も知らせない（スキップした版・自動で知らせた版）
    if (!job->manual) {
        const std::filesystem::path statePath = iroha::Update::StatePath(job->data);
        iroha::Update::State state = iroha::Update::LoadState(statePath);
        if (job->release.tag == state.skipped || job->release.tag == state.lastNotified) {
            Finish(job);
            return;
        }
        state.lastNotified = job->release.tag;
        iroha::Update::SaveState(statePath, state);
    }
    ShowOffer(job);
}

void ShowOffer(const std::shared_ptr<Job>& job) {
    GtkWindow* window = NewWindow(job);
    GtkWidget* box = Content(window);
    gtk_box_append(GTK_BOX(box), Title("iroha " + WithoutV(job->release.tag) + " が利用可能です"));
    gtk_box_append(GTK_BOX(box), Label("現在の版: " + job->currentVersion));
    std::u32string notes = job->release.notes;
    if (notes.size() > 2000) notes = notes.substr(0, 2000) + U"…";
    if (!notes.empty()) {
        GtkWidget* text = Label(iroha::Utf32ToUtf8(notes));
        gtk_label_set_selectable(GTK_LABEL(text), TRUE);
        gtk_widget_set_valign(text, GTK_ALIGN_START);
        GtkWidget* scrolled = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scrolled), 180);
        gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scrolled), 180);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), text);
        gtk_widget_add_css_class(scrolled, "frame");
        gtk_box_append(GTK_BOX(box), scrolled);
    }
    GtkWidget* note = Label("「アップデート」を押すと配布物（" + job->asset.name +
                            "）をダウンロードし、大きさとチェックサムを照合してから入れ替えます。入れ替えるときに"
                            "管理者のパスワードを求めます。そのあと IBus を起動し直します（入力中の文字は確定されます）。");
    gtk_widget_add_css_class(note, "dim-label");
    gtk_box_append(GTK_BOX(box), note);
    // 「この版をスキップ」の状態は変わるたびに写しておく（窓の destroy の時点では、中の部品はもう片付いている）
    auto skipped = std::make_shared<bool>(false);
    if (!job->manual) {
        GtkWidget* skip = gtk_check_button_new_with_label("この版をスキップ");
        gtk_box_append(GTK_BOX(box), skip);
        Connect(skip, "toggled", [skip, skipped] { *skipped = gtk_check_button_get_active(GTK_CHECK_BUTTON(skip)); });
    }
    GtkWidget* row = ButtonRow(box);
    GtkWidget* notesButton = gtk_button_new_with_label("リリースノート");
    GtkWidget* later = gtk_button_new_with_label("後で");
    GtkWidget* update = gtk_button_new_with_label("アップデート");
    gtk_widget_add_css_class(update, "suggested-action");
    gtk_box_append(GTK_BOX(row), notesButton);
    gtk_box_append(GTK_BOX(row), later);
    gtk_box_append(GTK_BOX(row), update);

    auto proceed = std::make_shared<bool>(false);
    Connect(notesButton, "clicked", [window, job] {
        if (!job->release.page.empty()) gtk_show_uri(window, job->release.page.c_str(), GDK_CURRENT_TIME);
    });
    Connect(later, "clicked", [window] { gtk_window_destroy(window); });
    Connect(update, "clicked", [window, proceed] {
        *proceed = true;
        gtk_window_destroy(window);
    });
    Connect(window, "destroy", [job, proceed, skipped] {
        if (*proceed) {
            StartInstall(job);
            return;
        }
        if (*skipped) {
            const std::filesystem::path statePath = iroha::Update::StatePath(job->data);
            iroha::Update::State state = iroha::Update::LoadState(statePath);
            state.skipped = job->release.tag;
            iroha::Update::SaveState(statePath, state);
        }
        Finish(job);
    });
    gtk_window_set_default_widget(window, update);
    gtk_window_set_focus(window, update);
    gtk_window_present(window);
}

gboolean OnProgressClose(GtkWindow*, gpointer data) {
    auto* job = static_cast<Job*>(data);
    if (job->installing) return TRUE; // 入れ替えの途中は閉じさせない
    job->cancel = true;
    return TRUE; // 取得が止まったら Done が閉じる
}

void SetStage(const std::shared_ptr<Job>& job, const std::string& text) {
    if (!job->status) return;
    gtk_label_set_text(GTK_LABEL(job->status), text.c_str());
    if (job->timer) {
        g_source_remove(job->timer);
        job->timer = 0;
    }
    job->installing = true;
    gtk_widget_set_sensitive(job->cancelButton, FALSE);
    gtk_window_set_deletable(job->window, FALSE);
    // 先が見えない段階は、行ったり来たりする表示にする
    job->timer = g_timeout_add(
        120,
        [](gpointer data) -> gboolean {
            gtk_progress_bar_pulse(GTK_PROGRESS_BAR(data));
            return G_SOURCE_CONTINUE;
        },
        job->progress);
}

struct InstallResult {
    bool ok = false;
    bool canceled = false;
    bool keepFolder = false; // 展開したものを残す（手で入れられるように）
    std::string message;
    std::string notices; // 入れ替えたときの「注意:」の行
};

InstallResult Install(const std::shared_ptr<Job>& job) {
    InstallResult result;
    gchar* directory = g_dir_make_tmp("iroha-update-XXXXXX", nullptr);
    if (!directory) {
        result.message = "一時フォルダを作れません。";
        return result;
    }
    job->work = directory;
    g_free(directory);

    // 1. 取得と照合（大きさ・SHA-256。GitHub の digest が無ければ大きさだけ）
    const iroha::settings::KnownModel file{job->asset.name.c_str(), job->asset.name.c_str(), job->asset.url.c_str(),
                                           job->asset.size, job->asset.sha256.c_str()};
    std::string error;
    const bool downloaded = iroha::settings::DownloadModel(
        file, job->work,
        [job](uint64_t received, uint64_t total) {
            job->received = received;
            if (total != 0) job->total = total;
            return !job->cancel.load();
        },
        &error);
    if (!downloaded) {
        result.canceled = job->cancel.load();
        result.message = "ダウンロードまたは照合に失敗したので、入れ替えませんでした。\n（" + error + "）";
        return result;
    }

    // 2. 展開
    OnMain([job] { SetStage(job, "展開しています"); });
    const std::filesystem::path archive = job->work / job->asset.name;
    const CommandResult extracted =
        RunCommand({"tar", "-xzf", iroha::PathToUtf8(archive), "-C", iroha::PathToUtf8(job->work)});
    std::error_code ignored;
    std::filesystem::remove(archive, ignored);
    const std::filesystem::path bundle = job->work / iroha::Update::BundleFolderName(job->asset.name);
    const std::filesystem::path script = bundle / "install.sh";
    if (!extracted.started || extracted.exitCode != 0 || !std::filesystem::exists(script)) {
        result.message = "配布物を展開できませんでした。\n" +
                         (extracted.started ? Tail(extracted.output, 6) : extracted.error);
        return result;
    }

    // 3. 管理者の部分（ファイルを置く）
    result.keepFolder = true;
    gchar* pkexec = g_find_program_in_path("pkexec");
    if (!pkexec) {
        result.message = "管理者の許可を求める pkexec が見つかりません（Ubuntu なら policykit-1 のパッケージ）。";
        return result;
    }
    const std::string pkexecPath = pkexec;
    g_free(pkexec);
    OnMain([job] { SetStage(job, "入れ替えています。管理者のパスワードを求める窓が出たら入力してください"); });
    const CommandResult system = RunCommand({pkexecPath, iroha::PathToUtf8(script), "--system"});
    if (!system.started) {
        result.message = "pkexec を起動できませんでした。\n（" + system.error + "）";
        return result;
    }
    // pkexec: 126 = 認証の窓を閉じた、127 = 許可されなかった・認証できなかった
    if (system.exitCode == 126 || system.exitCode == 127) {
        result.canceled = system.exitCode == 126;
        result.message = system.exitCode == 126 ? "管理者の認証を取り消したので、入れ替えませんでした。"
                                                : "管理者の許可が得られなかったので、入れ替えませんでした。\n" +
                                                      Tail(system.output, 4);
        return result;
    }
    if (system.exitCode != 0) {
        result.message = "入れ替えに失敗しました。\n" + Tail(system.output, 8);
        return result;
    }

    // 4. 利用者の部分（古いエンジンを止めて IBus を起動し直す）
    result.keepFolder = false;
    OnMain([job] { SetStage(job, "IBus を起動し直しています"); });
    const CommandResult session = RunCommand({"/bin/bash", iroha::PathToUtf8(script), "--session"});
    std::istringstream lines(session.output);
    for (std::string line; std::getline(lines, line);) {
        if (line.rfind("注意", 0) == 0 || line.rfind("error", 0) == 0) result.notices += line + "\n";
    }
    if (!session.started || session.exitCode != 0) {
        result.notices += "IBus を起動し直せませんでした。ログインし直すと新しい版になります。\n";
    }
    result.ok = true;
    return result;
}

void Done(const std::shared_ptr<Job>& job, const InstallResult& result) {
    if (job->timer) {
        g_source_remove(job->timer);
        job->timer = 0;
    }
    job->installing = false;
    job->status = nullptr;
    if (job->window) {
        g_signal_handlers_disconnect_by_data(job->window, job.get());
        gtk_window_destroy(job->window);
        job->window = nullptr;
    }
    std::error_code ignored;
    if (!result.keepFolder && !job->work.empty()) std::filesystem::remove_all(job->work, ignored);
    const auto done = [job] { Finish(job); };
    if (result.ok) {
        const bool settingsOpen = job->host.parent && job->host.parent();
        std::string text = result.notices;
        if (settingsOpen) text += "閉じると設定画面を新しい版で開き直します。";
        ShowInfo(job, "iroha を " + WithoutV(job->release.tag) + " に更新しました", text, [job, settingsOpen] {
            Finish(job);
            if (settingsOpen && job->host.relaunch) job->host.relaunch();
        });
        return;
    }
    if (result.canceled && !result.keepFolder) {
        Finish(job); // 取得を止めた
        return;
    }
    std::string text = result.message;
    if (result.keepFolder) {
        const std::filesystem::path bundle = job->work / iroha::Update::BundleFolderName(job->asset.name);
        text += "\n\n展開したものを次のフォルダに残してあります。端末でこのフォルダの ./install.sh を実行しても入れられます:\n" +
                iroha::PathToUtf8(bundle);
    }
    ShowInfo(job, result.canceled ? "アップデートを中止しました" : "アップデートに失敗しました", text, done);
}

void StartInstall(const std::shared_ptr<Job>& job) {
    GtkWindow* window = NewWindow(job);
    GtkWidget* box = Content(window);
    gtk_box_append(GTK_BOX(box), Title("iroha " + WithoutV(job->release.tag) + " に更新しています"));
    job->status = Label("ダウンロードしています（" + job->asset.name + "）");
    job->progress = gtk_progress_bar_new();
    gtk_box_append(GTK_BOX(box), job->status);
    gtk_box_append(GTK_BOX(box), job->progress);
    job->cancelButton = gtk_button_new_with_label("中止");
    gtk_box_append(GTK_BOX(ButtonRow(box)), job->cancelButton);
    Connect(job->cancelButton, "clicked", [job] { job->cancel = true; });
    g_signal_connect(window, "close-request", G_CALLBACK(OnProgressClose), job.get());
    job->timer = g_timeout_add(
        100,
        [](gpointer data) -> gboolean {
            auto* job = static_cast<Job*>(data);
            const uint64_t total = job->total != 0 ? job->total.load() : job->asset.size;
            if (total != 0 && job->progress) {
                gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(job->progress),
                                              std::min(1.0, static_cast<double>(job->received) / total));
            }
            return G_SOURCE_CONTINUE;
        },
        job.get());
    gtk_window_present(window);
    std::thread([job] {
        const InstallResult result = Install(job);
        OnMain([job, result] { Done(job, result); });
    }).detach();
}

} // namespace

void CheckAndOffer(const Host& host, const std::filesystem::path& dataDirectory, const std::string& currentVersion,
                   bool manual) {
    if (g_job) {
        if (manual && g_job->window) gtk_window_present(g_job->window);
        return;
    }
    const iroha::Config config = iroha::LoadConfig(dataDirectory / "config.json");
    const std::filesystem::path statePath = iroha::Update::StatePath(dataDirectory);
    iroha::Update::State state = iroha::Update::LoadState(statePath);
    if (!manual && (!config.autoUpdateCheck || !iroha::Update::IsDue(state, Now()))) return;
    // 先に記録する（確認に失敗しても、次は 24 時間後）
    state.lastCheck = Now();
    iroha::Update::SaveState(statePath, state);

    auto job = std::make_shared<Job>();
    job->host = host;
    job->data = dataDirectory;
    job->currentVersion = currentVersion;
    job->manual = manual;
    g_job = job;
    // 確認している間（窓がまだ無い間）に設定画面のプロセスが終わらないように
    g_application_hold(G_APPLICATION(host.application));
    std::thread([job] {
        std::string error;
        const bool ok = FetchLatest(job->data, &job->release, &error);
        OnMain([job, ok, error] { OnFetched(job, ok, error); });
    }).detach();
}

} // namespace updater
