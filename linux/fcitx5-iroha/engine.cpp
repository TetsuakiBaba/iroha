// iroha の fcitx5 のエンジン（fcitx5 本体のプロセスに読み込まれるアドオン）。
// IBus のエンジン（linux/ibus-iroha/engine.cpp）と同じく、キーをセッション（iroha-core の InputSession）に送り、
// 返ってきた表示（SessionOutput）を fcitx5 に反映するだけ。入力の判断と状態はセッションにある。
// キーの読み替えは IBus と共有（key_decision。fcitx5 の KeyState のビットは IBus の修飾キーと同じ並び）。
//
// IBus との違い:
// - 変換エンジン一式は fcitx5 本体のプロセスに読み込む（アドオンは最初に iroha を選んだときに読み込まれる）。
//   辞書・記号のデータはインストール先（IROHA_RESOURCE_DIR）から読む
// - フォーカスが外れた・リセットされたときは、今の表示のまま確定する（fcitx5 は未確定文字列を自動では確定しない）
// - 入力モード（あ / A）は fcitx5 の入力メソッドの補助の表示（subModeLabel）。プライベートモードと設定はメニュー
#include <sys/inotify.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <fcitx-utils/event.h>
#include <fcitx-utils/i18n.h>
#include <fcitx-utils/misc.h>
#include <fcitx/action.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputcontextproperty.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/statusarea.h>
#include <fcitx/text.h>
#include <fcitx/userinterfacemanager.h>

#include "debug_overlay_window.h"
#include "emoji_renderability.h"
#include "engine_host.h"
#include "input_menu.h"
#include "iroha/platform.h"
#include "iroha/session_manager.h"
#include "iroha/unicode.h"
#include "key_decision.h"

#ifndef IROHA_RESOURCE_DIR
#define IROHA_RESOURCE_DIR "/usr/local/lib/iroha"
#endif
#ifndef IROHA_VERSION
#define IROHA_VERSION "dev"
#endif

namespace iroha::fcitx5 {

namespace {

// 候補ウィンドウの 1 ページの数（InputSession::kCandidatePageSize と同じ。数字キーはページの中の番号）
constexpr size_t kPageSize = 9;
// ライブ変換などの結果待ちの間、問い合わせる間隔（マイクロ秒）
constexpr uint64_t kPollIntervalUs = 15000;
// 結果が届かないまま問い合わせ続ける上限（AI変換は 30 秒応答の無い依頼を打ち切るので、それより長く待つ）
constexpr auto kPollLimit = std::chrono::seconds(10);
constexpr auto kAIPollLimit = std::chrono::seconds(120);
// 句読点・プライベートモードの切り替えの知らせを出しておく時間（マイクロ秒）
constexpr uint64_t kNoticeUs = 2000000;
// 左文脈として読むカーソルより前の文字数
constexpr size_t kLeftContextLength = 64;

std::string U8(const std::u32string& text) { return Utf32ToUtf8(text); }

bool Pending(const SessionOutput& output) {
    return output.livePending || output.typoPending || output.aiPending || output.aiFixPending;
}

std::string FeedbackLine(const TypoCorrectionFeedback& feedback) {
    return U8(feedback.Original()) + " → " + U8(feedback.Corrected()) + "（BackSpace で戻す）";
}

uint64_t nextSessionId = 1;

} // namespace

class IrohaEngine;

// 入力コンテキストごとの状態（セッションの番号と、直近に受け取った表示）
class IrohaState : public fcitx::InputContextProperty {
public:
    uint64_t sessionId = nextSessionId++;
    SessionOutput shown;
    bool directMode = false;
    std::u32string idleUndoable;
    bool idleFullWidthSpace = true;
    // プライベートモードのキー（セッションの返事で受け取る。最初の返事までは既定の Ctrl+Alt+P）
    uint32_t privateModeShortcut = ibus::KeyContext().privateModeShortcut;
    unsigned int replaceBeforeLength = 0;
    std::unique_ptr<fcitx::EventSourceTime> pollTimer;
    std::chrono::steady_clock::time_point pollStarted;
    std::unique_ptr<fcitx::EventSourceTime> noticeTimer;
    std::u32string notice;
};

// 候補 1 つ（選ばれたら、ページの中の番号の数字キーをセッションに送る）
class IrohaCandidate : public fcitx::CandidateWord {
public:
    IrohaCandidate(IrohaEngine* engine, fcitx::Text text, size_t number)
        : fcitx::CandidateWord(std::move(text)), engine_(engine), number_(number) {}
    void select(fcitx::InputContext* ic) const override;

private:
    IrohaEngine* engine_;
    size_t number_;
};

// 入力メニューの項目 1 つ（並びと名前は input_menu。名前・印は押すたびにエンジンに聞く）
class IrohaAction : public fcitx::Action {
public:
    IrohaAction(IrohaEngine* engine, std::string key) : engine_(engine), key_(std::move(key)) {}
    const std::string& Key() const { return key_; }
    std::string shortText(fcitx::InputContext* ic) const override;
    std::string icon(fcitx::InputContext*) const override { return key_ == "Setup" ? "preferences-system" : ""; }
    bool isChecked(fcitx::InputContext* ic) const override;
    void activate(fcitx::InputContext* ic) override;

private:
    IrohaEngine* engine_;
    std::string key_;
};

class IrohaEngine : public fcitx::InputMethodEngineV2 {
public:
    explicit IrohaEngine(fcitx::Instance* instance)
        : instance_(instance),
          factory_([](fcitx::InputContext&) { return new IrohaState(); }),
          host_(MakeOptions()) {
        Log(std::string("start ") + IROHA_VERSION + " (fcitx5)");
        host_.Build();
        sessions_ = std::make_unique<SessionManager>(&host_);
        host_.ApplyTo(*sessions_);
        instance_->inputContextManager().registerProperty("irohaState", &factory_);
        // 入力メニューの項目は、出すことのある項目すべて（状況の行を含む）を初めに作って登録しておく
        menu::State all;
        all.modelMissing = true;
        for (const menu::Item& item : menu::Build(all)) {
            auto action = std::make_unique<IrohaAction>(this, item.key);
            action->setSeparator(item.type == menu::Item::Type::Separator);
            action->setCheckable(item.type == menu::Item::Type::Radio || item.type == menu::Item::Type::Toggle);
            instance_->userInterfaceManager().registerAction("iroha-" + item.key, action.get());
            actions_.push_back(std::move(action));
        }
        WatchReloadRequest();
        // デバッグ表示: カーソルの位置が変わったら追う
        eventHandlers_.push_back(instance_->watchEvent(
            fcitx::EventType::InputContextCursorRectChanged, fcitx::EventWatcherPhase::Default,
            [this](fcitx::Event& event) {
                auto& changed = static_cast<fcitx::InputContextEvent&>(event);
                if (overlay_.Visible()) UpdateOverlay(changed.inputContext());
            }));
    }

    ~IrohaEngine() override {
        eventHandlers_.clear();
        overlayWatch_.reset();
        reloadWatch_.reset();
        if (inotifyFd_ >= 0) close(inotifyFd_);
        platform::SetComposingFlag(false);
        // セッションのスレッドを止めてから、エンジン一式を片付ける
        sessions_.reset();
    }

    // ---- InputMethodEngine ----

    void keyEvent(const fcitx::InputMethodEntry&, fcitx::KeyEvent& event) override {
        if (event.isRelease()) return;
        fcitx::InputContext* ic = event.inputContext();
        try {
            IrohaState& state = *ic->propertyFor(&factory_);
            const fcitx::Key key = event.rawKey();
            ibus::KeyContext context;
            context.directMode = state.directMode;
            context.composing = state.shown.Composing();
            context.segmenting = state.shown.segmenting;
            context.hasPredictions = !state.shown.predictions.empty();
            context.idleFullWidthSpace = state.idleFullWidthSpace;
            context.idleUndoable = !state.idleUndoable.empty();
            context.candidatesVisible = state.shown.candidatesVisible;
            context.aiPending = state.shown.aiPending;
            context.aiFixShortcut = state.shown.aiFixShortcut;
            context.aiShortcuts = state.shown.aiShortcuts;
            context.privateModeShortcut = state.privateModeShortcut;
            context.inputModeShortcut = host_.InputModeShortcut();
            const ibus::KeyDecision decision =
                ibus::DecideKey(static_cast<uint32_t>(key.sym()), static_cast<uint32_t>(key.states()), context);
            using Kind = ibus::KeyDecision::Kind;
            switch (decision.kind) {
                case Kind::Pass:
                    return;
                case Kind::Session:
                    SendKey(ic, decision.key, decision.character, decision.shift);
                    event.filterAndAccept();
                    return;
                case Kind::CommitThenPass:
                    CommitNow(ic);
                    return;
                case Kind::UndoCommit: {
                    // カーソルの手前が確定した文字列のままのときだけ取り消す（違えばキーはアプリに渡す）
                    const std::u32string text = state.idleUndoable;
                    state.idleUndoable.clear();
                    if (text.empty() || state.shown.Composing() || !TextBeforeCursorIs(ic, text)) return;
                    state.replaceBeforeLength = static_cast<unsigned int>(text.size());
                    SendKey(ic, SessionKey::UndoCommit, 0, false);
                    event.filterAndAccept();
                    return;
                }
                case Kind::ToggleDirectMode:
                    SetDirectMode(ic, !state.directMode);
                    event.filterAndAccept();
                    return;
                case Kind::SetKanaMode:
                    SetDirectMode(ic, false);
                    event.filterAndAccept();
                    return;
                case Kind::SetDirectMode:
                    SetDirectMode(ic, true);
                    event.filterAndAccept();
                    return;
            }
        } catch (const std::exception& error) {
            Log(std::string("key failed: ") + error.what());
        }
    }

    void activate(const fcitx::InputMethodEntry&, fcitx::InputContextEvent& event) override {
        ShowMenu(event.inputContext());
    }

    void deactivate(const fcitx::InputMethodEntry& entry, fcitx::InputContextEvent& event) override {
        // ほかの入力メソッドに切り替えた・フォーカスが外れた: 今の表示のまま確定する
        reset(entry, event);
    }

    void reset(const fcitx::InputMethodEntry&, fcitx::InputContextEvent& event) override {
        fcitx::InputContext* ic = event.inputContext();
        try {
            CommitNow(ic);
            IrohaState& state = *ic->propertyFor(&factory_);
            state.pollTimer.reset();
            // デバッグ表示が出ていれば、セッションの行も捨てる（クリック・フォーカスの移動で閉じる。macOS版と同じ）
            if (!state.shown.debugOverlay.empty()) {
                sessions_->HandleKey(state.sessionId, SessionKey::Reset, 0, false, nullptr);
            }
            state.shown = SessionOutput();
            overlay_.Hide();
            ic->inputPanel().reset();
            ic->updatePreedit();
            ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
            UpdateComposingFlag();
        } catch (const std::exception& error) {
            Log(std::string("reset failed: ") + error.what());
        }
    }

    std::string subModeLabelImpl(const fcitx::InputMethodEntry&, fcitx::InputContext& ic) override {
        return ic.propertyFor(&factory_)->directMode ? "A" : "あ";
    }

    // ---- 候補・メニューから ----

    void SelectCandidate(fcitx::InputContext* ic, size_t number) {
        try {
            const IrohaState& state = *ic->propertyFor(&factory_);
            if (!state.shown.Composing() || !state.shown.candidatesVisible || number >= kPageSize) return;
            SendKey(ic, SessionKey::Character, static_cast<char32_t>(U'1' + number), false);
        } catch (const std::exception& error) {
            Log(std::string("candidate failed: ") + error.what());
        }
    }

    // ---- 入力メニュー（macOS版の入力メニューと同じ並び。input_menu） ----

    menu::State MenuStateOf(fcitx::InputContext* ic) {
        menu::State state = host_.MenuState();
        if (ic) state.directMode = ic->propertyFor(&factory_)->directMode;
        state.version = IROHA_VERSION;
        return state;
    }

    std::optional<menu::Item> MenuItem(const std::string& key, fcitx::InputContext* ic) {
        for (menu::Item& item : menu::Build(MenuStateOf(ic))) {
            if (item.key == key) return item;
        }
        return std::nullopt;
    }

    // 今の状態で出す項目を、入力コンテキストの状況の領域に並べ直す
    void ShowMenu(fcitx::InputContext* ic) {
        if (!ic) return;
        menuSignature_.clear();
        ic->statusArea().clearGroup(fcitx::StatusGroup::InputMethod);
        for (const menu::Item& item : menu::Build(MenuStateOf(ic))) {
            for (const auto& action : actions_) {
                if (action->Key() == item.key) ic->statusArea().addAction(fcitx::StatusGroup::InputMethod, action.get());
            }
        }
        ic->updateUserInterface(fcitx::UserInterfaceComponent::StatusArea);
    }

    // 名前・印が変わったかもしれない（プライベートモード・句読点・入力モード）。変わったときだけ知らせる
    // （キーのたびに全項目を知らせると、そのたびにパネルとの通信が起きる）
    void UpdateMenu(fcitx::InputContext* ic) {
        if (!ic) return;
        std::string signature;
        for (const menu::Item& item : menu::Build(MenuStateOf(ic))) {
            signature += item.key + '\t' + item.label + (item.checked ? "\t1\n" : "\t0\n");
        }
        if (signature == menuSignature_) return;
        menuSignature_ = signature;
        for (const auto& action : actions_) action->update(ic);
        ic->updateUserInterface(fcitx::UserInterfaceComponent::StatusArea);
    }

    void ActivateMenu(const std::string& key, fcitx::InputContext* ic) {
        try {
            const auto item = MenuItem(key, ic);
            if (!item) return;
            const auto settings = std::string(IROHA_RESOURCE_DIR) + "/iroha-settings";
            switch (item->command) {
                case menu::Command::None:
                    return;
                case menu::Command::KanaMode:
                    if (ic) SetDirectMode(ic, false);
                    break;
                case menu::Command::DirectMode:
                    if (ic) SetDirectMode(ic, true);
                    break;
                case menu::Command::Settings:
                    fcitx::startProcess({settings});
                    return;
                case menu::Command::UserDictionary:
                    fcitx::startProcess({settings, "--tab", "dictionary"});
                    return;
                case menu::Command::RewriteRules:
                    fcitx::startProcess({settings, "--tab", "rules"});
                    return;
                case menu::Command::PrivateMode:
                    if (ic) SendKey(ic, SessionKey::TogglePrivateMode, 0, false);
                    break;
                // ライブ変換・予測変換は設定を書き換え、読み直したときにメニューを合わせる
                case menu::Command::LiveConversion:
                    host_.ChangeConfig([on = !item->checked](Config& config) { config.liveConversion = on; });
                    return;
                case menu::Command::Predictive:
                    host_.ChangeConfig([on = !item->checked](Config& config) { config.predictive = on; });
                    return;
                case menu::Command::Punctuation:
                    if (ic) SendKey(ic, SessionKey::TogglePunctuation, 0, false);
                    break;
                case menu::Command::ModelFolder: {
                    const std::filesystem::path models = platform::DataDirectory() / "models";
                    std::error_code error;
                    std::filesystem::create_directories(models, error);
                    fcitx::startProcess({"xdg-open", PathToUtf8(models)});
                    return;
                }
            }
            UpdateMenu(ic);
        } catch (const std::exception& error) {
            Log(std::string("menu failed: ") + error.what());
        }
    }

private:
    EngineHost::Options MakeOptions() {
        EngineHost::Options options;
        options.log = [this](const std::string& message) { Log(message); };
        options.isRenderableEmoji = EmojiRenderability::MakePredicate();
        options.resourceDirectory = IROHA_RESOURCE_DIR;
        return options;
    }

    void Log(const std::string& message) const { FCITX_INFO() << "[iroha] " << message; }

    // ---- 表示 ----

    void ApplyOutput(fcitx::InputContext* ic, const SessionOutput& output, bool fromKey) {
        IrohaState& state = *ic->propertyFor(&factory_);
        state.shown = output;
        state.shown.commit.clear();
        state.idleUndoable = output.undoableCommit;
        state.idleFullWidthSpace = output.fullWidthSpace;
        state.privateModeShortcut = output.privateModeShortcut;
        // 確定の取り消し: カーソルの手前の確定した文字列を消し、未確定文字列に置き換える
        if (output.replacesCommitted && state.replaceBeforeLength > 0 && !output.preedit.empty()) {
            ic->deleteSurroundingText(-static_cast<int>(state.replaceBeforeLength), state.replaceBeforeLength);
        }
        state.replaceBeforeLength = 0;
        if (!output.commit.empty()) ic->commitString(U8(output.commit));
        if (fromKey && !output.notice.empty()) ShowNotice(ic, output.notice);

        fcitx::InputPanel& panel = ic->inputPanel();
        panel.reset();
        // 未確定文字列（選んでいる文節は強調。カーソルは、文節変換中は選んでいる文節の頭、入力中は末尾）
        fcitx::Text preedit;
        size_t bytes = 0;
        int cursor = -1;
        for (const auto& span : output.preedit) {
            const bool selected = span.style == SessionOutput::kSelectedSegment;
            if (selected && output.segmenting && cursor < 0) cursor = static_cast<int>(bytes);
            const std::string text = U8(span.text);
            preedit.append(text, selected ? fcitx::TextFormatFlags{fcitx::TextFormatFlag::Underline,
                                                                   fcitx::TextFormatFlag::HighLight}
                                          : fcitx::TextFormatFlags{fcitx::TextFormatFlag::Underline});
            bytes += text.size();
        }
        preedit.setCursor(cursor >= 0 ? cursor : static_cast<int>(bytes));
        if (ic->capabilityFlags().test(fcitx::CapabilityFlag::Preedit)) {
            panel.setClientPreedit(preedit);
        } else {
            panel.setPreedit(preedit);
        }
        // 候補ウィンドウ（今のページの 9 件だけ）
        if (output.Composing() && output.candidatesVisible && !output.candidates.empty()) {
            const size_t index = std::min<size_t>(output.candidateIndex, output.candidates.size() - 1);
            const size_t start = index / kPageSize * kPageSize;
            const size_t end = std::min(start + kPageSize, output.candidates.size());
            auto list = std::make_unique<fcitx::CommonCandidateList>();
            list->setPageSize(static_cast<int>(kPageSize));
            list->setLayoutHint(fcitx::CandidateLayoutHint::Vertical);
            std::vector<std::string> labels;
            for (size_t i = start; i < end; ++i) {
                fcitx::Text text(U8(output.candidates[i]));
                if (i < output.candidateAnnotations.size() && !output.candidateAnnotations[i].empty()) {
                    text.append("  " + U8(output.candidateAnnotations[i]));
                }
                list->append<IrohaCandidate>(this, std::move(text), i - start);
                labels.push_back(std::to_string(i - start + 1) + ".");
            }
            list->setLabels(labels);
            list->setGlobalCursorIndex(static_cast<int>(index - start));
            panel.setCandidateList(std::move(list));
        }
        const std::string aux = AuxiliaryLine(state);
        if (!aux.empty()) panel.setAuxUp(fcitx::Text(aux));
        ic->updatePreedit();
        ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        // プライベートモード・句読点（Ctrl+.）が変わっていればメニューに出す
        UpdateMenu(ic);
        UpdateComposingFlag();
        UpdateOverlay(ic);
        SetPolling(ic, output.Composing() && Pending(output));
    }

    // デバッグ表示（開発者モード。macOS版 DeveloperOverlay）: セッションの行があり、フォーカスがあってカーソルの位置が
    // 分かれば出す。候補ウィンドウが出ていれば行の上に出す
    void UpdateOverlay(fcitx::InputContext* ic) {
        const IrohaState& state = *ic->propertyFor(&factory_);
        const fcitx::Rect& rect = ic->cursorRect();
        const bool known = rect.width() > 0 || rect.height() > 0 || rect.left() != 0 || rect.top() != 0;
        if (state.shown.debugOverlay.empty() || !ic->hasFocus() || !known) {
            overlay_.Hide();
            return;
        }
        if (!overlayWatch_ && overlay_.Open()) {
            overlayWatch_ = instance_->eventLoop().addIOEvent(
                overlay_.Fd(), fcitx::IOEventFlag::In, [this](fcitx::EventSourceIO*, int, fcitx::IOEventFlags) {
                    overlay_.Dispatch();
                    return true;
                });
        }
        overlay_.Show(U8(state.shown.debugOverlay), DebugOverlayWindow::Rect{rect.left(), rect.top(), rect.width(),
                                                                              rect.height()},
                      state.shown.candidatesVisible);
    }

    // 補助テキスト: 知らせ > 打ち間違いの訂正 > AI の案 > AIで直すの窓 > AI変換の結果待ち > 表の列 > 予測変換 > 候補の位置
    std::string AuxiliaryLine(const IrohaState& state) const {
        const SessionOutput& shown = state.shown;
        if (!state.notice.empty()) return U8(state.notice);
        if (!shown.Composing()) return {};
        if (shown.typoFeedback) return "訂正: " + FeedbackLine(*shown.typoFeedback);
        if (shown.aiFixFeedback) return "AI の案: " + FeedbackLine(*shown.aiFixFeedback);
        if (!shown.aiFixNotice.empty()) return U8(shown.aiFixNotice);
        if (shown.aiPending) return "AI に依頼中…（Esc で取り消す）";
        if (shown.candidatesVisible && shown.candidatesExpanded) {
            const size_t columns = (shown.candidates.size() + kPageSize - 1) / kPageSize;
            return "列 " + std::to_string(shown.candidateIndex / kPageSize + 1) + "/" + std::to_string(columns) +
                   "（←→ で隣の列・Tab で 1 列に戻す）";
        }
        if (!shown.segmenting && !shown.predictions.empty()) {
            std::string line = "予測:";
            for (size_t i = 0; i < shown.predictions.size(); ++i) {
                const bool selected = static_cast<int32_t>(i) == shown.predictionIndex;
                line += selected ? " [" + U8(shown.predictions[i]) + "]" : " " + U8(shown.predictions[i]);
            }
            return line + "（Tab で選ぶ）";
        }
        if (shown.candidatesVisible && shown.candidates.size() > kPageSize) {
            return std::to_string(shown.candidateIndex + 1) + "/" + std::to_string(shown.candidates.size()) +
                   "（Tab で表に広げる）";
        }
        return {};
    }

    void ShowNotice(fcitx::InputContext* ic, const std::u32string& notice) {
        IrohaState& state = *ic->propertyFor(&factory_);
        state.notice = notice;
        state.noticeTimer = instance_->eventLoop().addTimeEvent(
            CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + kNoticeUs, 0,
            [this, ic](fcitx::EventSourceTime*, uint64_t) {
                IrohaState& current = *ic->propertyFor(&factory_);
                current.notice.clear();
                ApplyOutputAgain(ic);
                return true;
            });
    }

    // 知らせを消したときなど、今の表示をもう一度反映する（セッションには送らない）
    void ApplyOutputAgain(fcitx::InputContext* ic) {
        IrohaState& state = *ic->propertyFor(&factory_);
        if (!state.shown.Composing()) {
            ic->inputPanel().setAuxUp(fcitx::Text());
            ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
            return;
        }
        const SessionOutput shown = state.shown;
        ApplyOutput(ic, shown, /*fromKey=*/false);
    }

    // ---- 問い合わせ ----

    void SetPolling(fcitx::InputContext* ic, bool pending) {
        IrohaState& state = *ic->propertyFor(&factory_);
        if (!pending) {
            if (state.pollTimer) state.pollTimer->setEnabled(false);
            return;
        }
        state.pollStarted = std::chrono::steady_clock::now();
        if (state.pollTimer) {
            state.pollTimer->setNextInterval(kPollIntervalUs);
            state.pollTimer->setOneShot();
            return;
        }
        state.pollTimer = instance_->eventLoop().addTimeEvent(
            CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + kPollIntervalUs, 0,
            [this, ic](fcitx::EventSourceTime* source, uint64_t) {
                PollTick(ic, source);
                return true;
            });
    }

    void PollTick(fcitx::InputContext* ic, fcitx::EventSourceTime* source) {
        IrohaState& state = *ic->propertyFor(&factory_);
        try {
            const auto started = state.pollStarted;
            SessionOutput output;
            bool pending = state.shown.Composing() && Pending(state.shown);
            if (sessions_->Poll(state.sessionId, state.shown.revision, &output)) {
                ApplyOutput(ic, output, /*fromKey=*/false);
                pending = output.Composing() && Pending(output);
                state.pollStarted = started; // 届いた結果では上限の起点を動かさない
            }
            const auto limit = state.shown.aiPending ? kAIPollLimit : kPollLimit;
            if (pending && std::chrono::steady_clock::now() - state.pollStarted < limit) {
                source->setNextInterval(kPollIntervalUs);
                source->setOneShot();
                return;
            }
        } catch (const std::exception& error) {
            Log(std::string("poll failed: ") + error.what());
        }
        source->setEnabled(false);
    }

    // ---- 文書（周辺テキスト） ----

    bool TextBeforeCursor(fcitx::InputContext* ic, std::u32string* before) const {
        if (!ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText)) return false;
        const fcitx::SurroundingText& surrounding = ic->surroundingText();
        if (!surrounding.isValid()) return false;
        const std::u32string all = Utf8ToUtf32(surrounding.text());
        const size_t end = std::min<size_t>(std::min(surrounding.cursor(), surrounding.anchor()), all.size());
        *before = all.substr(0, end);
        return true;
    }

    bool TextBeforeCursorIs(fcitx::InputContext* ic, const std::u32string& expected) const {
        std::u32string before;
        if (!TextBeforeCursor(ic, &before)) return false;
        return before.size() >= expected.size() &&
               before.compare(before.size() - expected.size(), expected.size(), expected) == 0;
    }

    // ---- セッションへのキー ----

    void SendKey(fcitx::InputContext* ic, SessionKey key, char32_t character, bool shift) {
        IrohaState& state = *ic->propertyFor(&factory_);
        // 合成を始めるキーには、カーソルより前の文書のテキスト（左文脈）を付ける
        std::u32string leftContext;
        bool hasLeftContext = false;
        if (!state.shown.Composing()) {
            std::u32string before;
            if (TextBeforeCursor(ic, &before)) {
                leftContext = before.size() > kLeftContextLength ? before.substr(before.size() - kLeftContextLength)
                                                                 : before;
                hasLeftContext = true;
            }
        }
        const SessionOutput output =
            sessions_->HandleKey(state.sessionId, key, character, shift, hasLeftContext ? &leftContext : nullptr);
        ApplyOutput(ic, output, /*fromKey=*/true);
    }

    void CommitNow(fcitx::InputContext* ic) {
        if (ic->propertyFor(&factory_)->shown.Composing()) SendKey(ic, SessionKey::CommitNow, 0, false);
    }

    void SetDirectMode(fcitx::InputContext* ic, bool direct) {
        IrohaState& state = *ic->propertyFor(&factory_);
        if (state.directMode == direct) return;
        CommitNow(ic);
        state.directMode = direct;
        UpdateMenu(ic);
    }

    // 入力中かの目印（選択テキストのAI編集が読む）。変わったときだけ書く
    void UpdateComposingFlag() {
        const int now = sessions_ && sessions_->AnyComposing() ? 1 : 0;
        if (now == composingFlag_) return;
        composingFlag_ = now;
        platform::SetComposingFlag(now == 1);
    }

    // ---- 設定の読み直し（設定画面が <データフォルダ>/reload-request を書き換えたら） ----

    void WatchReloadRequest() {
        const std::filesystem::path path = platform::ReloadRequestPath();
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        inotifyFd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (inotifyFd_ < 0) return;
        if (inotify_add_watch(inotifyFd_, PathToUtf8(path.parent_path()).c_str(),
                              IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE) < 0) {
            return;
        }
        reloadWatch_ = instance_->eventLoop().addIOEvent(
            inotifyFd_, fcitx::IOEventFlag::In, [this, name = path.filename().string()](fcitx::EventSourceIO*, int fd,
                                                                                         fcitx::IOEventFlags) {
                alignas(inotify_event) char buffer[4096];
                bool requested = false;
                for (ssize_t length; (length = read(fd, buffer, sizeof(buffer))) > 0;) {
                    for (char* p = buffer; p < buffer + length;) {
                        const auto* event = reinterpret_cast<const inotify_event*>(p);
                        if (event->len > 0 && name == event->name) requested = true;
                        p += sizeof(inotify_event) + event->len;
                    }
                }
                if (requested) {
                    Log("reload requested");
                    sessions_->WithEngine([this] { host_.Build(); });
                    host_.ApplyTo(*sessions_);
                    // 入力メニューを合わせる（モデルが見つからない行の出し入れもあるので並べ直す）
                    ShowMenu(instance_->mostRecentInputContext());
                }
                return true;
            });
    }

    fcitx::Instance* instance_;
    fcitx::FactoryFor<IrohaState> factory_;
    EngineHost host_;
    std::unique_ptr<SessionManager> sessions_;
    std::vector<std::unique_ptr<IrohaAction>> actions_;
    std::string menuSignature_;
    int inotifyFd_ = -1;
    std::unique_ptr<fcitx::EventSourceIO> reloadWatch_;
    int composingFlag_ = -1;
    // デバッグ表示の窓（1 枚をすべての入力コンテキストで使う）
    DebugOverlayWindow overlay_;
    std::unique_ptr<fcitx::EventSourceIO> overlayWatch_;
    std::vector<std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>>> eventHandlers_;
};

void IrohaCandidate::select(fcitx::InputContext* ic) const { engine_->SelectCandidate(ic, number_); }

std::string IrohaAction::shortText(fcitx::InputContext* ic) const {
    const auto item = engine_->MenuItem(key_, ic);
    return item ? item->label : "";
}

bool IrohaAction::isChecked(fcitx::InputContext* ic) const {
    const auto item = engine_->MenuItem(key_, ic);
    return item && item->checked;
}

void IrohaAction::activate(fcitx::InputContext* ic) { engine_->ActivateMenu(key_, ic); }

class IrohaFactory : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance* create(fcitx::AddonManager* manager) override { return new IrohaEngine(manager->instance()); }
};

} // namespace iroha::fcitx5

FCITX_ADDON_FACTORY(iroha::fcitx5::IrohaFactory);
