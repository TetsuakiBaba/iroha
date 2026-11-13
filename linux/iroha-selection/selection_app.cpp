#include "selection_app.h"

// iroha のヘッダは X のヘッダより先に読む（X は None・Status などをマクロにしていて、AIService::None と重なる）
#include "ai_client.h"
#include "iroha/ai_conversion.h"
#include "iroha/character_count.h"
#include "iroha/config.h"
#include "iroha/platform.h"
#include "iroha/unicode.h"
#include "selection_rules.h"

#include <glib-unix.h>
#include <gtk/gtk.h>
#include <gdk/x11/gdkx.h>

#include <algorithm>
#include <csignal>
#include <functional>
#include <memory>
#include <optional>
#include <thread>

#include "x11_desktop.h"

namespace iroha::selection {

namespace {

constexpr char kApplicationId[] = "jp.iroha.Selection";
// 結果の窓の大きさ（論理ピクセル。Windows版と同じ）
constexpr int kPanelWidth = 480;
constexpr int kPanelHeight = 330;
// PRIMARY 選択・CLIPBOARD を読むのを待つ上限（持ち主のアプリが答えないとき）
constexpr guint kSelectionReadTimeoutMs = 1000;
constexpr guint kClipboardReadTimeoutMs = 500;
// 貼る前に待つ最短の時間（元の窓が前面に戻るのを待つ。Windows版の Sleep(150) にあたる）
constexpr gint64 kPasteMinimumWaitUs = 150 * 1000;
// 元の窓が前面に戻るのを待つ上限・キーが離されるのを待つ上限
constexpr gint64 kActivateWaitUs = 700 * 1000;
constexpr gint64 kKeyReleaseWaitUs = 1500 * 1000;
// 貼ってから前のクリップボードの文字列を戻すまでの時間。X の選択は貼る側が読みに来たときに渡すので、
// 早く戻すと戻した文字列が貼られる（Windows版の 300ms より長くする）
constexpr guint kRestoreDelayMs = 1000;
// マウスで選んでから PRIMARY 選択を読むまでの待ち（macOS版・Windows版と同じ 160ms）
constexpr guint kMouseCaptureDelayMs = 160;
// アイコン（文字数）を出しておく時間・メニューを出しておく上限（Windows版のアイコンと同じ 4 秒）
constexpr guint kBubbleDurationMs = 4000;
constexpr guint kMenuDurationMs = 20000;
// 同じ選択を続けて拾わない間隔（macOS版と同じ 0.75 秒）
constexpr gint64 kSameSelectionUs = 750 * 1000;
// ドラッグとみなす距離（macOS版・Windows版と同じ 4 ピクセル）
constexpr int kDragDistance = 4;

constexpr char kBubbleCss[] =
    "window.iroha-bubble { border: 1px solid alpha(currentColor, 0.28); }"
    ".iroha-bubble-icon { color: #4f5bd5; font-size: 18px; }"
    ".iroha-bubble-label { font-weight: 600; }"
    ".iroha-menu-heading { font-size: smaller; }"
    "button.iroha-menu-item { padding: 4px 14px; border-radius: 0; }";

std::string Utf8(const std::u32string& text) { return Utf32ToUtf8(text); }

enum class Phase { Hidden, PromptInput, Processing, Done };

class App {
public:
    explicit App(Logger log) : log_(std::move(log)) {}
    ~App() { StopWorker(); }

    int Run() {
        app_ = gtk_application_new(kApplicationId, G_APPLICATION_FLAGS_NONE);
        g_signal_connect(app_, "startup", G_CALLBACK(+[](GApplication*, gpointer self) {
                             static_cast<App*>(self)->Startup();
                         }),
                         this);
        g_signal_connect(app_, "activate", G_CALLBACK(+[](GApplication*, gpointer self) {
                             static_cast<App*>(self)->Activate();
                         }),
                         this);
        g_signal_connect(app_, "shutdown", G_CALLBACK(+[](GApplication*, gpointer self) {
                             static_cast<App*>(self)->Shutdown();
                         }),
                         this);
        char* argv[] = {const_cast<char*>("iroha-selection"), nullptr};
        const int status = g_application_run(G_APPLICATION(app_), 1, argv);
        g_object_unref(app_);
        app_ = nullptr;
        return status;
    }

private:
    // ---- 起動と設定 ----

    void Startup() {
        GdkDisplay* display = gdk_display_get_default();
        if (!display || !GDK_IS_X11_DISPLAY(display)) {
            // Wayland ではほかのアプリのキーをつかめない（グローバルショートカットが無い）・前面の窓も調べられない
            const auto wayland = platform::GetEnv("WAYLAND_DISPLAY");
            log_("not an X11 session (WAYLAND_DISPLAY=" + wayland.value_or("") +
                 "): global shortcuts are not available, doing nothing");
            return;
        }
        x11_ = std::make_unique<X11Desktop>(log_, [this](int id, unsigned long time) { OnHotkey(id, time); });
        if (!x11_->Open()) {
            log_("cannot open the X display, doing nothing");
            x11_.reset();
            return;
        }
        running_ = true;
        g_application_hold(G_APPLICATION(app_)); // 窓を出していないときも常駐する
        g_unix_signal_add(SIGTERM, &App::OnSignal, this);
        g_unix_signal_add(SIGINT, &App::OnSignal, this);
        GtkCssProvider* css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(css, kBubbleCss, -1);
        gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(css),
                                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(css);
        // ダブルクリックの間隔と距離はデスクトップの設定に合わせる
        int doubleClickTime = 400;
        int doubleClickDistance = 5;
        g_object_get(gtk_settings_get_default(), "gtk-double-click-time", &doubleClickTime, "gtk-double-click-distance",
                     &doubleClickDistance, nullptr);
        gestures_.emplace(kDragDistance, static_cast<unsigned long>(std::max(1, doubleClickTime)),
                          std::max(1, doubleClickDistance));
        WatchSettings();
        LoadSettings();
    }

    void Activate() {
        // 2 回目以降の起動（すでに動いているところへの知らせ）は何もしない
        if (++activations_ > 1) log_("already running");
    }

    void Shutdown() {
        if (!running_) return;
        HideBubble();
        if (captureTimer_) g_source_remove(captureTimer_);
        captureTimer_ = 0;
        HidePanel();
        StopWorker();
        if (reloadTimer_) g_source_remove(reloadTimer_);
        reloadTimer_ = 0;
        for (GFileMonitor* monitor : monitors_) {
            g_file_monitor_cancel(monitor);
            g_object_unref(monitor);
        }
        monitors_.clear();
        x11_.reset(); // つかんだキーを放す
        running_ = false;
        log_("stop");
    }

    static gboolean OnSignal(gpointer self) {
        auto* app = static_cast<App*>(self);
        app->log_("signal received");
        g_application_quit(G_APPLICATION(app->app_));
        return G_SOURCE_REMOVE;
    }

    // config.json と reload-request（設定画面が書く）の変化を見張る
    void WatchSettings() {
        std::error_code error;
        std::filesystem::create_directories(platform::DataDirectory(), error);
        const std::filesystem::path paths[] = {platform::DataDirectory() / "config.json", platform::ReloadRequestPath()};
        for (const auto& path : paths) {
            GFile* file = g_file_new_for_path(PathToUtf8(path).c_str());
            GFileMonitor* monitor = g_file_monitor_file(file, G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr);
            g_object_unref(file);
            if (!monitor) {
                log_("cannot watch " + PathToUtf8(path));
                continue;
            }
            g_signal_connect(monitor, "changed",
                             G_CALLBACK(+[](GFileMonitor*, GFile*, GFile*, GFileMonitorEvent, gpointer self) {
                                 static_cast<App*>(self)->ScheduleReload();
                             }),
                             this);
            monitors_.push_back(monitor);
        }
    }

    // 書き込みの途中で読まないよう、変化が落ち着いてから読む
    void ScheduleReload() {
        if (reloadTimer_) g_source_remove(reloadTimer_);
        reloadTimer_ = g_timeout_add(300, +[](gpointer self) -> gboolean {
            auto* app = static_cast<App*>(self);
            app->reloadTimer_ = 0;
            app->LoadSettings();
            return G_SOURCE_REMOVE;
        }, this);
    }

    void LoadSettings() {
        config_ = LoadConfig(platform::DataDirectory() / "config.json");
        settings_ = SettingsFromConfig(config_);
        const std::vector<HotkeyBinding> bindings = PlanHotkeys(settings_);
        const size_t grabbed = x11_ ? x11_->Grab(bindings) : 0;
        // マウスで選んだとき（AI編集のアイコン・メニューか、文字数を出すときだけ見る）
        if (x11_) {
            x11_->WatchMouse(settings_.WatchesMouse(), [this](const X11Desktop::MouseEvent& event) { OnMouse(event); });
        }
        if (!settings_.WatchesMouse()) HideBubble();
        if (!settings_.enabled) {
            log_("selection: disabled");
        } else {
            log_("selection: " + std::to_string(grabbed) + " of " + std::to_string(bindings.size()) + " hotkeys grabbed");
        }
    }

    // ---- ショートカット ----

    void OnHotkey(int id, unsigned long time) {
        if (!settings_.enabled || phase_ != Phase::Hidden || capturing_ || pasting_) return;
        HideBubble();
        // 入力中（未確定文字列がある）は使わない（macOS版と同じ）
        if (platform::IsComposing()) {
            log_("hotkey ignored while composing");
            return;
        }
        if (id != kOnDemandId && (id < 0 || static_cast<size_t>(id) >= settings_.presets.size())) return;
        const Window window = x11_->ActiveWindow();
        if (window == 0) return;
        const AppIdentity identity = x11_->Identify(window);
        if (IsExcluded(settings_.excludedApps, identity)) {
            log_("hotkey ignored in " + identity.command + " (" + identity.classInstance + ")");
            return;
        }
        target_ = window;
        hotkeyTime_ = time;
        pendingId_ = id;
        ReadSelection();
    }

    // 選択している文章を PRIMARY 選択から読む（Ctrl+C を送らない。クリップボードを書き換えない）
    void ReadSelection() {
        ReadPrimary([this](const std::optional<std::string>& text) { OnSelectionRead(text); });
    }

    // PRIMARY 選択を読んで done に渡す（持ち主が答えなければ無し）
    void ReadPrimary(std::function<void(const std::optional<std::string>&)> done) {
        GdkClipboard* primary = gdk_display_get_primary_clipboard(gdk_display_get_default());
        // 自分の窓（結果の窓の入力欄など）で選んだものは、ほかのアプリの選択ではない
        if (gdk_clipboard_is_local(primary)) {
            done(std::nullopt);
            return;
        }
        capturing_ = true;
        GCancellable* cancellable = g_cancellable_new();
        // 持ち主のアプリが答えなければ、選択なしとして進む
        const guint timeout = g_timeout_add(kSelectionReadTimeoutMs, +[](gpointer cancellable) -> gboolean {
            g_cancellable_cancel(G_CANCELLABLE(cancellable));
            return G_SOURCE_REMOVE;
        }, cancellable);
        struct Read {
            App* app;
            GCancellable* cancellable;
            guint timeout;
            std::function<void(const std::optional<std::string>&)> done;
        };
        auto* read = new Read{this, cancellable, timeout, std::move(done)};
        gdk_clipboard_read_text_async(
            primary, cancellable,
            +[](GObject* source, GAsyncResult* result, gpointer data) {
                std::unique_ptr<Read> read(static_cast<Read*>(data));
                GError* error = nullptr;
                char* text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), result, &error);
                if (!g_cancellable_is_cancelled(read->cancellable)) g_source_remove(read->timeout);
                g_object_unref(read->cancellable);
                std::optional<std::string> value;
                if (text) {
                    value = text;
                    g_free(text);
                }
                if (error) g_error_free(error);
                read->app->capturing_ = false;
                if (read->app->running_) read->done(value);
            },
            read);
    }

    void OnSelectionRead(const std::optional<std::string>& primaryText) {
        const auto selected = SelectionFromPrimary(primaryText);
        sourceText_ = selected.value_or(U"");
        generation_ = !selected;
        if (pendingId_ == kOnDemandId || !selected) {
            // その場で指示を入れる（選択が無ければ文章を作る）
            ShowPanel(generation_ ? U"テキストを生成" : U"AIに指示", Phase::PromptInput);
            gtk_widget_grab_focus(prompt_);
            return;
        }
        if (pendingId_ < 0 || static_cast<size_t>(pendingId_) >= settings_.presets.size()) return;
        const size_t index = static_cast<size_t>(pendingId_);
        const auto& preset = settings_.presets[index];
        const std::u32string heading = PresetHeading(preset, index);
        ShowPanel(heading, Phase::Processing);
        StartRequest(heading, PresetRequest(preset, index, sourceText_));
    }

    // ---- マウスで選んだとき（macOS版 MouseSelectionMonitor・SelectionBubbleController・SelectionMenuController） ----

    void OnMouse(const X11Desktop::MouseEvent& event) {
        if (!gestures_) return;
        if (event.press) {
            // 押した: 読みかけの選択は捨て、アイコン・メニューの外を押したら閉じる
            ++captureSerial_;
            if (captureTimer_) g_source_remove(captureTimer_);
            captureTimer_ = 0;
            ownerChangesAtPress_ = x11_->PrimaryOwnerChanges();
            gestures_->Press(event.x, event.y);
            if (!InsideBubble(event.x, event.y)) HideBubble();
            return;
        }
        if (gestures_->Release(event.x, event.y, event.time)) OnGesture(event.x, event.y, event.alt);
    }

    // ドラッグかダブルクリックで選んだ。選択が落ち着くまで少し待ってから読む
    void OnGesture(int x, int y, bool alt) {
        if (phase_ != Phase::Hidden || capturing_ || pasting_ || InsideBubble(x, y)) return;
        if (!settings_.MouseTriggersAI(alt) && !settings_.characterCount) return;
        gestureX_ = x;
        gestureY_ = y;
        gestureAlt_ = alt;
        const uint64_t serial = captureSerial_;
        if (captureTimer_) g_source_remove(captureTimer_);
        struct Wait {
            App* app;
            uint64_t serial;
        };
        captureTimer_ = g_timeout_add_full(
            G_PRIORITY_DEFAULT, kMouseCaptureDelayMs,
            +[](gpointer data) -> gboolean {
                auto* wait = static_cast<Wait*>(data);
                wait->app->captureTimer_ = 0;
                wait->app->CaptureMouseSelection(wait->serial);
                return G_SOURCE_REMOVE;
            },
            new Wait{this, serial}, +[](gpointer data) { delete static_cast<Wait*>(data); });
    }

    void CaptureMouseSelection(uint64_t serial) {
        if (serial != captureSerial_ || phase_ != Phase::Hidden || capturing_ || pasting_) return;
        // 入力中（未確定文字列がある）は出さない（macOS版と同じ）
        if (platform::IsComposing()) return;
        Window window = x11_->ActiveWindow();
        if (window == 0) window = x11_->WindowAtPointer();
        const AppIdentity identity = x11_->Identify(window);
        if (IsExcluded(settings_.excludedApps, identity)) return;
        ReadPrimary([this, serial, window, identity](const std::optional<std::string>& utf8) {
            if (serial != captureSerial_ || phase_ != Phase::Hidden) return;
            std::optional<std::u32string> text;
            if (utf8) text = Utf8ToUtf32(*utf8);
            const bool changed = x11_->PrimaryOwnerChanges() != ownerChangesAtPress_;
            if (!ShouldUseMouseSelection(text, x11_->CanWatchSelectionOwner(), changed, lastMouseText_)) return;
            lastMouseText_ = *text;
            const std::u32string fingerprint = Utf8ToUtf32(identity.command + "|" + identity.classInstance) + U"|" + *text;
            const gint64 now = g_get_monotonic_time();
            if (fingerprint == lastFingerprint_ && now - lastFingerprintTime_ < kSameSelectionUs) return;
            lastFingerprint_ = fingerprint;
            lastFingerprintTime_ = now;
            OnMouseSelection(MouseCapture{window, *text, gestureX_, gestureY_});
        });
    }

    struct MouseCapture {
        Window window = 0;
        std::u32string text;
        int x = 0;
        int y = 0;
    };

    void OnMouseSelection(const MouseCapture& capture) {
        const bool ai = settings_.MouseTriggersAI(gestureAlt_);
        const std::u32string label = settings_.characterCount ? CharacterCount(capture.text).Summary() : U"";
        bubbleCapture_ = capture;
        if (!ai) {
            if (!label.empty()) ShowBubble(label, /*clickable=*/false);
            return;
        }
        if (settings_.trigger == Settings::Trigger::Bubble) {
            ShowBubble(label, /*clickable=*/true);
            return;
        }
        // すぐにメニューを出す（文字数はメニューの上に添える）
        ShowMenu(label);
    }

    // アイコン・メニューの窓（ウィンドウマネージャに扱わせない窓。押しても元のアプリのフォーカスと選択はそのまま）
    void EnsureBubble() {
        if (bubble_) return;
        bubble_ = gtk_window_new();
        gtk_window_set_application(GTK_WINDOW(bubble_), app_);
        gtk_window_set_decorated(GTK_WINDOW(bubble_), FALSE);
        gtk_window_set_resizable(GTK_WINDOW(bubble_), FALSE);
        gtk_window_set_title(GTK_WINDOW(bubble_), "iroha");
        gtk_widget_add_css_class(bubble_, "iroha-bubble");
        gtk_widget_realize(bubble_);
        GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(bubble_));
        if (surface && GDK_IS_X11_SURFACE(surface)) {
            bubbleXid_ = gdk_x11_surface_get_xid(surface);
            x11_->MakeOverlay(bubbleXid_);
        }
        // アイコンを押したらメニューを開く
        GtkGesture* click = gtk_gesture_click_new();
        g_signal_connect(click, "released", G_CALLBACK(+[](GtkGestureClick*, int, double, double, gpointer self) {
                             auto* app = static_cast<App*>(self);
                             if (app->bubbleClickable_ && !app->bubbleIsMenu_) app->ShowMenu(app->bubbleLabel_);
                         }),
                         this);
        gtk_widget_add_controller(bubble_, GTK_EVENT_CONTROLLER(click));
    }

    // アイコン（clickable）か文字数だけを、マウスの右下に出す
    void ShowBubble(const std::u32string& label, bool clickable) {
        EnsureBubble();
        bubbleClickable_ = clickable;
        bubbleIsMenu_ = false;
        bubbleLabel_ = label;
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_margin_start(box, 10);
        gtk_widget_set_margin_end(box, 10);
        gtk_widget_set_margin_top(box, 5);
        gtk_widget_set_margin_bottom(box, 5);
        if (clickable) {
            // macOS版の sparkles・Windows版の ✦ にあたる記号
            GtkWidget* icon = gtk_label_new("\u2726");
            gtk_widget_add_css_class(icon, "iroha-bubble-icon");
            gtk_box_append(GTK_BOX(box), icon);
            gtk_widget_set_tooltip_text(bubble_, "iroha: クリックでメニュー");
        } else {
            gtk_widget_set_tooltip_text(bubble_, nullptr);
        }
        if (!label.empty()) {
            GtkWidget* text = gtk_label_new(Utf8(label).c_str());
            gtk_widget_add_css_class(text, "iroha-bubble-label");
            gtk_box_append(GTK_BOX(box), text);
        }
        PresentBubble(box, 10, 14, kBubbleDurationMs);
    }

    // プリセットのメニュー（「AIに指示…」と有効なプリセット）。heading は文字数（無ければ空）
    void ShowMenu(const std::u32string& heading) {
        if (!bubbleCapture_) return;
        EnsureBubble();
        bubbleIsMenu_ = true;
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_margin_top(box, 4);
        gtk_widget_set_margin_bottom(box, 4);
        GtkWidget* title = gtk_label_new(heading.empty() ? "iroha" : ("iroha・" + Utf8(heading)).c_str());
        gtk_label_set_xalign(GTK_LABEL(title), 0);
        gtk_widget_set_margin_start(title, 14);
        gtk_widget_set_margin_end(title, 14);
        gtk_widget_set_margin_bottom(title, 2);
        gtk_widget_add_css_class(title, "dim-label");
        gtk_widget_add_css_class(title, "iroha-menu-heading");
        gtk_box_append(GTK_BOX(box), title);
        gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
        AppendMenuItem(box, "AIに指示…", kOnDemandId);
        bool separated = false;
        for (size_t i = 0; i < settings_.presets.size(); ++i) {
            const auto& preset = settings_.presets[i];
            if (!preset.enabled) continue;
            if (!separated) gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
            separated = true;
            std::u32string name = PresetHeading(preset, i);
            std::replace(name.begin(), name.end(), U'\n', U' ');
            if (name.size() > 42) name = name.substr(0, 42) + U"…";
            AppendMenuItem(box, Utf8(name), static_cast<int>(i));
        }
        gtk_widget_set_tooltip_text(bubble_, nullptr);
        PresentBubble(box, 4, 10, kMenuDurationMs);
    }

    void AppendMenuItem(GtkWidget* box, const std::string& text, int id) {
        GtkWidget* button = gtk_button_new();
        GtkWidget* label = gtk_label_new(text.c_str());
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_button_set_child(GTK_BUTTON(button), label);
        gtk_widget_add_css_class(button, "flat");
        gtk_widget_add_css_class(button, "iroha-menu-item");
        gtk_widget_set_focusable(button, FALSE);
        g_object_set_data(G_OBJECT(button), "iroha-id", GINT_TO_POINTER(id + 1));
        g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkButton* button, gpointer self) {
                             const int id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "iroha-id")) - 1;
                             static_cast<App*>(self)->OnMenuChoice(id);
                         }),
                         this);
        gtk_box_append(GTK_BOX(box), button);
    }

    // 中身を差し替えて、選んだ位置の右下（dx, dy は論理ピクセル）に出す。画面の作業領域からはみ出さないようにする
    void PresentBubble(GtkWidget* content, int dx, int dy, guint durationMs) {
        gtk_widget_set_visible(bubble_, FALSE);
        gtk_window_set_child(GTK_WINDOW(bubble_), content);
        int width = 0;
        int height = 0;
        gtk_widget_measure(bubble_, GTK_ORIENTATION_HORIZONTAL, -1, nullptr, &width, nullptr, nullptr);
        gtk_widget_measure(bubble_, GTK_ORIENTATION_VERTICAL, width, nullptr, &height, nullptr, nullptr);
        gtk_window_set_default_size(GTK_WINDOW(bubble_), width, height);
        const int px = bubbleCapture_ ? bubbleCapture_->x : 0;
        const int py = bubbleCapture_ ? bubbleCapture_->y : 0;
        const auto [scale, area] = WorkArea(px, py);
        int x = px + dx * scale;
        int y = py + dy * scale;
        if (area.width > 0) {
            x = std::max(area.x, std::min(x, area.x + area.width - width * scale));
            y = std::max(area.y, std::min(y, area.y + area.height - height * scale));
        }
        if (bubbleXid_) x11_->PlaceWindow(bubbleXid_, x, y);
        gtk_widget_set_visible(bubble_, TRUE);
        if (bubbleXid_) x11_->PlaceWindow(bubbleXid_, x, y);
        if (bubbleTimer_) g_source_remove(bubbleTimer_);
        bubbleTimer_ = g_timeout_add(durationMs, +[](gpointer self) -> gboolean {
            auto* app = static_cast<App*>(self);
            app->bubbleTimer_ = 0;
            app->HideBubble();
            return G_SOURCE_REMOVE;
        }, this);
    }

    bool BubbleVisible() const { return bubble_ && gtk_widget_get_visible(bubble_); }

    bool InsideBubble(int x, int y) {
        if (!BubbleVisible() || !bubbleXid_) return false;
        const auto rect = x11_->WindowRect(bubbleXid_);
        return rect && rect->Contains(x, y);
    }

    void HideBubble() {
        if (bubbleTimer_) g_source_remove(bubbleTimer_);
        bubbleTimer_ = 0;
        bubbleIsMenu_ = false;
        if (bubble_) gtk_widget_set_visible(bubble_, FALSE);
    }

    // メニューで選んだ: 選んでいた文章をプリセット（か、その場の指示）で AI に渡す
    void OnMenuChoice(int id) {
        if (!bubbleCapture_) return;
        const MouseCapture capture = *bubbleCapture_;
        HideBubble();
        if (phase_ != Phase::Hidden || pasting_) return;
        target_ = capture.window != 0 ? capture.window : x11_->ActiveWindow();
        // メニューを押したクリックの時刻（ウィンドウマネージャが結果の窓へフォーカスを移すのを断らないように）
        hotkeyTime_ = x11_->LastButtonTime();
        pendingId_ = id;
        OnSelectionRead(Utf8(capture.text));
    }

    // ---- 結果の窓 ----

    void BuildPanel() {
        panel_ = gtk_window_new();
        gtk_window_set_application(GTK_WINDOW(panel_), app_);
        gtk_window_set_title(GTK_WINDOW(panel_), "iroha");
        gtk_window_set_default_size(GTK_WINDOW(panel_), kPanelWidth, kPanelHeight);
        g_signal_connect(panel_, "close-request", G_CALLBACK(+[](GtkWindow*, gpointer self) -> gboolean {
                             static_cast<App*>(self)->HidePanel();
                             return TRUE; // 窓は捨てずに隠す
                         }),
                         this);

        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_set_margin_start(box, 12);
        gtk_widget_set_margin_end(box, 12);
        gtk_widget_set_margin_top(box, 10);
        gtk_widget_set_margin_bottom(box, 12);
        gtk_window_set_child(GTK_WINDOW(panel_), box);

        title_ = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(title_), 0);
        gtk_widget_add_css_class(title_, "heading");
        gtk_box_append(GTK_BOX(box), title_);

        source_ = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(source_), 0);
        gtk_label_set_single_line_mode(GTK_LABEL(source_), TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(source_), PANGO_ELLIPSIZE_END);
        gtk_widget_add_css_class(source_, "dim-label");
        gtk_box_append(GTK_BOX(box), source_);

        prompt_ = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(prompt_), "AIへの指示を入れて Enter");
        g_signal_connect(prompt_, "activate", G_CALLBACK(+[](GtkEntry*, gpointer self) {
                             static_cast<App*>(self)->SubmitPrompt();
                         }),
                         this);
        gtk_box_append(GTK_BOX(box), prompt_);

        GtkWidget* scroller = gtk_scrolled_window_new();
        gtk_widget_set_vexpand(scroller, TRUE);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        output_ = gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(output_), FALSE);
        gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(output_), FALSE);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(output_), GTK_WRAP_WORD_CHAR);
        gtk_text_view_set_left_margin(GTK_TEXT_VIEW(output_), 6);
        gtk_text_view_set_right_margin(GTK_TEXT_VIEW(output_), 6);
        gtk_text_view_set_top_margin(GTK_TEXT_VIEW(output_), 4);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), output_);
        gtk_box_append(GTK_BOX(box), scroller);

        notice_ = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(notice_), 0);
        gtk_label_set_wrap(GTK_LABEL(notice_), TRUE);
        gtk_box_append(GTK_BOX(box), notice_);

        GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_halign(buttons, GTK_ALIGN_END);
        primary_ = gtk_button_new_with_label("置き換える（Enter）");
        gtk_widget_add_css_class(primary_, "suggested-action");
        copy_ = gtk_button_new_with_label("コピー");
        close_ = gtk_button_new_with_label("閉じる（Esc）");
        g_signal_connect(primary_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer self) {
                             static_cast<App*>(self)->OnPrimary();
                         }),
                         this);
        g_signal_connect(copy_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer self) {
                             static_cast<App*>(self)->CopyResult();
                         }),
                         this);
        g_signal_connect(close_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer self) {
                             static_cast<App*>(self)->HidePanel();
                         }),
                         this);
        gtk_box_append(GTK_BOX(buttons), primary_);
        gtk_box_append(GTK_BOX(buttons), copy_);
        gtk_box_append(GTK_BOX(buttons), close_);
        gtk_box_append(GTK_BOX(box), buttons);

        // Esc で閉じる・結果が出たら Enter で置き換える（コピー・閉じるのボタンにいるときはそのボタン）
        GtkEventController* keys = gtk_event_controller_key_new();
        gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
        g_signal_connect(keys, "key-pressed",
                         G_CALLBACK(+[](GtkEventControllerKey*, guint keyval, guint, GdkModifierType, gpointer data) -> gboolean {
                             auto* self = static_cast<App*>(data);
                             if (keyval == GDK_KEY_Escape) {
                                 self->HidePanel();
                                 return TRUE;
                             }
                             if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) && self->phase_ == Phase::Done) {
                                 GtkWidget* focus = gtk_root_get_focus(GTK_ROOT(self->panel_));
                                 if (focus == self->copy_ || focus == self->close_) return FALSE;
                                 self->Apply();
                                 return TRUE;
                             }
                             return FALSE;
                         }),
                         this);
        gtk_widget_add_controller(panel_, keys);
    }

    // 結果の窓を出す（first: 出したときの段階。指示を入れる欄を出すかが決まる）
    void ShowPanel(const std::u32string& heading, Phase first) {
        if (!panel_) BuildPanel();
        gtk_label_set_text(GTK_LABEL(title_), Utf8(heading).c_str());
        gtk_label_set_text(GTK_LABEL(source_), Utf8(SourceLine(sourceText_, generation_)).c_str());
        gtk_editable_set_text(GTK_EDITABLE(prompt_), "");
        SetOutput(U"");
        gtk_label_set_text(GTK_LABEL(notice_), "");
        resultText_.clear();
        SetPhase(first);
        ComputePanelPosition();
        // 隠してある窓（2 回目以降）は、出す前に置き場所を決めておく
        PlacePanel();
        // ショートカットのキーの時刻を渡す（ウィンドウマネージャがフォーカスを移すのを断らないように）
        gtk_window_present_with_time(GTK_WINDOW(panel_), static_cast<guint32>(hotkeyTime_));
        PlacePanel();
        if (Window xid = PanelXid()) x11_->KeepAbove(xid);
        // ウィンドウマネージャが表示のときに置き場所を決め直すことがあるので、少し後にもう一度動かす
        g_timeout_add(60, +[](gpointer self) -> gboolean {
            auto* app = static_cast<App*>(self);
            if (app->phase_ != Phase::Hidden) app->PlacePanel();
            return G_SOURCE_REMOVE;
        }, this);
    }

    Window PanelXid() const {
        if (!panel_) return 0;
        GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(panel_));
        if (!surface || !GDK_IS_X11_SURFACE(surface)) return 0;
        return gdk_x11_surface_get_xid(surface);
    }

    // マウスポインタの近く（画面の作業領域からはみ出さないように）。X の座標（実際のピクセル）で求める
    void ComputePanelPosition() {
        const auto pointer = x11_->PointerPosition();
        if (!pointer) {
            panelPosition_.reset();
            return;
        }
        const int px = pointer->first;
        const int py = pointer->second;
        const auto [scale, area] = WorkArea(px, py);
        const int width = kPanelWidth * scale;
        const int height = kPanelHeight * scale;
        int x = px + 12 * scale;
        int y = py + 16 * scale;
        if (area.width > 0) {
            x = std::max(area.x, std::min(x, area.x + area.width - width));
            y = std::max(area.y, std::min(y, area.y + area.height - height));
        }
        panelPosition_ = std::make_pair(x, y);
    }

    // (px, py)（X の座標）を含む画面の倍率と作業領域（X の座標）。見つからなければ倍率 1・幅 0
    static std::pair<int, GdkRectangle> WorkArea(int px, int py) {
        int scale = 1;
        GdkRectangle area = {px, py, 0, 0};
        GListModel* monitors = gdk_display_get_monitors(gdk_display_get_default());
        for (guint i = 0; i < g_list_model_get_n_items(monitors); ++i) {
            auto* monitor = static_cast<GdkMonitor*>(g_list_model_get_item(monitors, i));
            const int factor = std::max(1, gdk_monitor_get_scale_factor(monitor));
            GdkRectangle geometry;
            gdk_monitor_get_geometry(monitor, &geometry);
            const bool contains = px >= geometry.x * factor && px < (geometry.x + geometry.width) * factor &&
                                  py >= geometry.y * factor && py < (geometry.y + geometry.height) * factor;
            if (contains) {
                GdkRectangle workarea;
                gdk_x11_monitor_get_workarea(monitor, &workarea);
                scale = factor;
                area = {workarea.x * factor, workarea.y * factor, workarea.width * factor, workarea.height * factor};
            }
            g_object_unref(monitor);
            if (contains) break;
        }
        return {scale, area};
    }

    void PlacePanel() {
        const Window xid = PanelXid();
        if (xid && panelPosition_) x11_->PlaceWindow(xid, panelPosition_->first, panelPosition_->second);
    }

    void SetOutput(const std::u32string& text) {
        GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(output_));
        gtk_text_buffer_set_text(buffer, Utf8(text).c_str(), -1);
    }

    void SetPhase(Phase next) {
        phase_ = next;
        const bool done = phase_ == Phase::Done && !resultText_.empty();
        gtk_button_set_label(GTK_BUTTON(primary_), phase_ == Phase::PromptInput ? "実行（Enter）"
                                                   : generation_                ? "入れる（Enter）"
                                                                                : "置き換える（Enter）");
        gtk_widget_set_sensitive(primary_, phase_ == Phase::PromptInput || done);
        gtk_widget_set_sensitive(copy_, done);
        gtk_widget_set_visible(prompt_, phase_ == Phase::PromptInput);
    }

    void HidePanel() {
        if (cancel_) cancel_->Cancel();
        ++requestNumber_; // 遅れて届いた途中経過・結果を捨てる
        phase_ = Phase::Hidden;
        if (panel_) gtk_widget_set_visible(panel_, FALSE);
    }

    void OnPrimary() {
        if (phase_ == Phase::PromptInput) {
            SubmitPrompt();
        } else {
            Apply();
        }
    }

    void SubmitPrompt() {
        if (phase_ != Phase::PromptInput) return;
        const std::u32string instruction = TrimWhitespace(Utf8ToUtf32(gtk_editable_get_text(GTK_EDITABLE(prompt_))));
        if (instruction.empty()) return;
        StartRequest(generation_ ? U"テキストを生成" : U"AIに指示", PromptRequest(instruction, sourceText_, generation_));
    }

    // ---- AI への依頼（裏のスレッド） ----

    struct Message {
        App* app;
        uint64_t number;
        bool done;
        std::u32string partial;
        AIResult result;
        bool notConfigured = false;
    };

    void StartRequest(const std::u32string& heading, const AIRequest& request) {
        gtk_label_set_text(GTK_LABEL(title_), Utf8(heading).c_str());
        resultText_.clear();
        SetOutput(U"");
        gtk_label_set_text(GTK_LABEL(notice_), "AIで処理しています…（Esc で閉じる）");
        SetPhase(Phase::Processing);
        StopWorker();
        const uint64_t number = ++requestNumber_;
        cancel_ = std::make_shared<CancelToken>();
        // API キーは秘密の保管庫から裏のスレッドで読む（main thread で読むと、保管庫の答えを待つ間止まる）
        worker_ = std::thread([this, number, request, config = config_, token = cancel_] {
            auto* done = new Message{this, number, true, U"", AIResult{}, false};
            const AIConnection connection = AIClient::FromConfig(config, /*withKey=*/true);
            if (!AIClient::IsConfigured(connection)) {
                done->notConfigured = true;
            } else {
                done->result = AIClient::Run(
                    connection, request,
                    [this, number](const std::u32string& partial) {
                        Post(new Message{this, number, false, partial, AIResult{}, false});
                    },
                    token.get());
            }
            Post(done);
        });
    }

    static void Post(Message* message) {
        g_idle_add_full(G_PRIORITY_DEFAULT, +[](gpointer data) -> gboolean {
            std::unique_ptr<Message> message(static_cast<Message*>(data));
            if (message->done) {
                message->app->OnDone(*message);
            } else {
                message->app->OnPartial(*message);
            }
            return G_SOURCE_REMOVE;
        }, message, nullptr);
    }

    void OnPartial(const Message& message) {
        if (message.number != requestNumber_ || phase_ != Phase::Processing) return;
        SetOutput(message.partial);
    }

    void OnDone(const Message& message) {
        if (message.number != requestNumber_ || phase_ != Phase::Processing) return;
        if (message.notConfigured) {
            gtk_label_set_text(GTK_LABEL(notice_), "AIサービスが設定されていません（設定 > AI）");
        } else if (message.result.text) {
            resultText_ = CleanAIOutput(*message.result.text);
            SetOutput(resultText_);
            gtk_label_set_text(GTK_LABEL(notice_),
                               generation_ ? "Enter で入れる・Esc で閉じる" : "Enter で置き換える・Esc で閉じる");
        } else {
            gtk_label_set_text(GTK_LABEL(notice_), ("AIの処理に失敗しました（" + message.result.error + "）").c_str());
            log_("selection: AI request failed: " + message.result.error);
        }
        SetPhase(Phase::Done);
        gtk_widget_grab_focus(resultText_.empty() ? close_ : primary_);
    }

    void StopWorker() {
        if (cancel_) cancel_->Cancel();
        if (worker_.joinable()) worker_.join();
    }

    // ---- クリップボードと貼り付け ----

    // CLIPBOARD に文字列を置く。temporary なら、クリップボードの履歴に残さない印（KDE の Klipper などが見る
    // x-kde-passwordManagerHint）を付ける（Windows版の ExcludeClipboardContentFromMonitorProcessing にあたる）
    static void SetClipboardText(const std::u32string& text, bool temporary) {
        GdkClipboard* clipboard = gdk_display_get_clipboard(gdk_display_get_default());
        const std::string utf8 = Utf8(text);
        if (!temporary) {
            gdk_clipboard_set_text(clipboard, utf8.c_str());
            return;
        }
        GValue value = G_VALUE_INIT;
        g_value_init(&value, G_TYPE_STRING);
        g_value_set_string(&value, utf8.c_str());
        static const char kHint[] = "secret";
        GBytes* hint = g_bytes_new_static(kHint, sizeof(kHint) - 1);
        GdkContentProvider* providers[] = {
            gdk_content_provider_new_for_value(&value),
            gdk_content_provider_new_for_bytes("x-kde-passwordManagerHint", hint),
        };
        g_value_unset(&value);
        g_bytes_unref(hint);
        GdkContentProvider* provider = gdk_content_provider_new_union(providers, G_N_ELEMENTS(providers));
        gdk_clipboard_set_content(clipboard, provider);
        g_object_unref(provider);
    }

    void CopyResult() {
        if (phase_ != Phase::Done || resultText_.empty()) return;
        SetClipboardText(resultText_, /*temporary=*/false);
        gtk_label_set_text(GTK_LABEL(notice_), "コピーしました");
    }

    // 結果を元のアプリに戻って貼る（選択していれば置き換わる。選択が無ければカーソルの位置に入る）。
    // 前のクリップボードの文字列を控える → 結果を置く → 元の窓を前面に戻す → キーが離されるのを待って Ctrl+V →
    // 少し待って控えた文字列を戻す
    void Apply() {
        if (phase_ != Phase::Done || resultText_.empty() || pasting_) return;
        pasteText_ = resultText_;
        pasteTarget_ = target_;
        HidePanel();
        if (!x11_->Exists(pasteTarget_)) {
            log_("selection: the target window is gone");
            return;
        }
        pasting_ = true;
        GdkClipboard* clipboard = gdk_display_get_clipboard(gdk_display_get_default());
        GCancellable* cancellable = g_cancellable_new();
        const guint timeout = g_timeout_add(kClipboardReadTimeoutMs, +[](gpointer cancellable) -> gboolean {
            g_cancellable_cancel(G_CANCELLABLE(cancellable));
            return G_SOURCE_REMOVE;
        }, cancellable);
        struct Read {
            App* app;
            GCancellable* cancellable;
            guint timeout;
        };
        auto* read = new Read{this, cancellable, timeout};
        gdk_clipboard_read_text_async(
            clipboard, cancellable,
            +[](GObject* source, GAsyncResult* result, gpointer data) {
                std::unique_ptr<Read> read(static_cast<Read*>(data));
                GError* error = nullptr;
                char* text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), result, &error);
                if (!g_cancellable_is_cancelled(read->cancellable)) g_source_remove(read->timeout);
                g_object_unref(read->cancellable);
                App* app = read->app;
                app->previousClipboard_.reset();
                if (text) {
                    app->previousClipboard_ = std::string(text);
                    g_free(text);
                }
                if (error) g_error_free(error);
                if (app->running_) app->StartPaste();
            },
            read);
    }

    void StartPaste() {
        if (!previousClipboard_) log_("selection: the clipboard had no text (it will not be restored)");
        SetClipboardText(pasteText_, /*temporary=*/true);
        // 時刻は 0 にする（ショートカットのキーの時刻は、結果の窓を操作したあとでは古いとみなされる）
        x11_->Activate(pasteTarget_, 0);
        pasteStarted_ = g_get_monotonic_time();
        g_timeout_add(30, +[](gpointer self) -> gboolean { return static_cast<App*>(self)->PasteStep(); }, this);
    }

    // 元の窓が前面に戻り、キーが離されるのを待ってから Ctrl+V を送る
    gboolean PasteStep() {
        if (!running_) return G_SOURCE_REMOVE;
        const gint64 elapsed = g_get_monotonic_time() - pasteStarted_;
        const bool activated = x11_->ActiveWindow() == pasteTarget_ || elapsed > kActivateWaitUs;
        const bool released = !x11_->KeysHeld() || elapsed > kKeyReleaseWaitUs;
        if (elapsed < kPasteMinimumWaitUs || !activated || !released) return G_SOURCE_CONTINUE;
        if (x11_->ActiveWindow() != pasteTarget_) log_("selection: the target window did not come to the front");
        if (!x11_->SendPaste()) {
            // 貼れなければ結果はクリップボードに残す（手で貼れる）
            log_("selection: cannot send Ctrl+V");
            pasting_ = false;
            return G_SOURCE_REMOVE;
        }
        g_timeout_add(kRestoreDelayMs, +[](gpointer self) -> gboolean {
            auto* app = static_cast<App*>(self);
            if (app->running_ && app->previousClipboard_) {
                SetClipboardText(Utf8ToUtf32(*app->previousClipboard_), /*temporary=*/false);
            }
            app->previousClipboard_.reset();
            app->pasting_ = false;
            return G_SOURCE_REMOVE;
        }, this);
        return G_SOURCE_REMOVE;
    }

    Logger log_;
    GtkApplication* app_ = nullptr;
    std::unique_ptr<X11Desktop> x11_;
    bool running_ = false;
    int activations_ = 0;
    std::vector<GFileMonitor*> monitors_;
    guint reloadTimer_ = 0;
    Config config_;
    Settings settings_;

    // 結果の窓
    GtkWidget* panel_ = nullptr;
    GtkWidget* title_ = nullptr;
    GtkWidget* source_ = nullptr;
    GtkWidget* prompt_ = nullptr;
    GtkWidget* output_ = nullptr;
    GtkWidget* notice_ = nullptr;
    GtkWidget* primary_ = nullptr;
    GtkWidget* copy_ = nullptr;
    GtkWidget* close_ = nullptr;
    std::optional<std::pair<int, int>> panelPosition_;

    Phase phase_ = Phase::Hidden;
    bool capturing_ = false;       // PRIMARY 選択を読んでいる
    Window target_ = 0;            // ショートカットを押したときの前面の窓（結果を貼る先）
    unsigned long hotkeyTime_ = 0; // ショートカットのキーの時刻
    int pendingId_ = -1;
    std::u32string sourceText_;   // 選択していた文章（無ければ空）
    std::u32string resultText_;
    bool generation_ = false;     // 選択が無く、文章を作る
    uint64_t requestNumber_ = 0;
    std::shared_ptr<CancelToken> cancel_;
    std::thread worker_;

    // マウスで選んだとき
    std::optional<GestureTracker> gestures_;
    uint64_t captureSerial_ = 0;
    guint captureTimer_ = 0;
    uint64_t ownerChangesAtPress_ = 0;
    int gestureX_ = 0;
    int gestureY_ = 0;
    bool gestureAlt_ = false;
    std::u32string lastMouseText_;    // 前に使った文章（XFixes が無いときの比較用）
    std::u32string lastFingerprint_;  // 同じ選択を続けて拾わないための印と時刻
    gint64 lastFingerprintTime_ = 0;
    std::optional<MouseCapture> bubbleCapture_;
    GtkWidget* bubble_ = nullptr;
    Window bubbleXid_ = 0;
    bool bubbleClickable_ = false;
    bool bubbleIsMenu_ = false;
    std::u32string bubbleLabel_;
    guint bubbleTimer_ = 0;

    // 貼り付け
    bool pasting_ = false;
    std::u32string pasteText_;
    Window pasteTarget_ = 0;
    std::optional<std::string> previousClipboard_;
    gint64 pasteStarted_ = 0;
};

} // namespace

int RunApplication(const Logger& log) {
    App app(log);
    return app.Run();
}

} // namespace iroha::selection
