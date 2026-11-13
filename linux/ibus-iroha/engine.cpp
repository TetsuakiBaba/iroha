#include "engine.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>

#include "iroha/engine_host.h"
#include "iroha/session_manager.h"
#include "iroha/unicode.h"
#include "key_decision.h"

namespace {

iroha::EngineHost* sharedHost = nullptr;
iroha::SessionManager* sharedSessions = nullptr;
std::function<void(const std::string&)> sharedLog;
uint64_t nextSessionId = 1;

// 候補ウィンドウの 1 ページの数（InputSession::kCandidatePageSize と同じ。数字キーはページの中の番号）
constexpr size_t kPageSize = 9;
// ライブ変換・打ち間違いの訂正の結果待ちの間、問い合わせる間隔（Windows版の TIP と同じ）
constexpr guint kPollIntervalMs = 15;
// 句読点・プライベートモードの切り替えの知らせを出しておく時間
constexpr guint kNoticeMs = 2000;
// 左文脈として読むカーソルより前の文字数（変換側がカーソルのある行・末尾 40 文字に整える）
constexpr size_t kLeftContextLength = 64;

// 未確定文字列のうち選んでいる文節の色（背景・文字）
constexpr guint kSelectedBackground = 0xa8c8f0;
constexpr guint kSelectedForeground = 0x000000;
// 候補の右に出す専門辞書の名前の色
constexpr guint kAnnotationForeground = 0x808080;

void Log(const std::string& message) {
    if (sharedLog) sharedLog(message);
}

std::string U8(const std::u32string& text) { return iroha::Utf32ToUtf8(text); }

guint Length(const std::u32string& text) { return static_cast<guint>(text.size()); }

struct State {
    uint64_t sessionId = 0;
    // 直近に受け取った表示（commit は除く）
    iroha::SessionOutput shown;
    bool directMode = false;
    std::u32string idleUndoable;
    bool idleFullWidthSpace = true;
    bool privateMode = false;
    // 確定の取り消しで、カーソルの手前から消す文字数
    guint replaceBeforeLength = 0;
    guint pollTimer = 0;
    guint noticeTimer = 0;
    std::u32string notice;
    IBusPropList* properties = nullptr;
    IBusProperty* modeProperty = nullptr;
    IBusProperty* privateProperty = nullptr;
};

} // namespace

struct IrohaEngine {
    IBusEngine parent;
    State* state;
};

struct IrohaEngineClass {
    IBusEngineClass parent;
};

G_DEFINE_TYPE(IrohaEngine, iroha_engine, IBUS_TYPE_ENGINE)

#define IROHA_ENGINE(object) (G_TYPE_CHECK_INSTANCE_CAST((object), IROHA_TYPE_ENGINE, IrohaEngine))

namespace {

State& StateOf(IBusEngine* engine) { return *IROHA_ENGINE(engine)->state; }

bool Composing(const State& state) { return state.shown.Composing(); }

// ---- 表示 ----

void UpdatePreedit(IBusEngine* engine, const iroha::SessionOutput& output) {
    if (output.preedit.empty()) {
        ibus_engine_update_preedit_text_with_mode(engine, ibus_text_new_from_static_string(""), 0, FALSE,
                                                  IBUS_ENGINE_PREEDIT_CLEAR);
        return;
    }
    std::u32string whole;
    for (const auto& span : output.preedit) whole += span.text;
    IBusText* text = ibus_text_new_from_string(U8(whole).c_str());
    ibus_text_append_attribute(text, IBUS_ATTR_TYPE_UNDERLINE, IBUS_ATTR_UNDERLINE_SINGLE, 0,
                               static_cast<gint>(Length(whole)));
    // カーソルは、文節変換中は選んでいる文節の頭（候補ウィンドウがその文節の下に出る）、入力中は末尾
    guint cursor = Length(whole);
    guint offset = 0;
    bool selectedFound = false;
    for (const auto& span : output.preedit) {
        const guint length = Length(span.text);
        if (span.style == iroha::SessionOutput::kSelectedSegment) {
            ibus_text_append_attribute(text, IBUS_ATTR_TYPE_BACKGROUND, kSelectedBackground, offset,
                                       static_cast<gint>(offset + length));
            ibus_text_append_attribute(text, IBUS_ATTR_TYPE_FOREGROUND, kSelectedForeground, offset,
                                       static_cast<gint>(offset + length));
            if (!selectedFound && output.segmenting) cursor = offset;
            selectedFound = true;
        }
        offset += length;
    }
    // 合成中にフォーカスが外れたら、IBus が表示中の文字列をそのまま確定する（セッションは Reset で捨てる）
    ibus_engine_update_preedit_text_with_mode(engine, text, cursor, TRUE, IBUS_ENGINE_PREEDIT_COMMIT);
}

// 候補ウィンドウ。今のページの 9 件だけを渡す（クリックされた番号がページの中の番号になる）
void UpdateLookupTable(IBusEngine* engine) {
    const State& state = StateOf(engine);
    const iroha::SessionOutput& shown = state.shown;
    if (!Composing(state) || !shown.candidatesVisible || shown.candidates.empty()) {
        ibus_engine_hide_lookup_table(engine);
        return;
    }
    const size_t index = std::min<size_t>(shown.candidateIndex, shown.candidates.size() - 1);
    const size_t start = index / kPageSize * kPageSize;
    const size_t end = std::min(start + kPageSize, shown.candidates.size());
    IBusLookupTable* table =
        ibus_lookup_table_new(kPageSize, static_cast<guint>(index - start), TRUE, FALSE);
    ibus_lookup_table_set_orientation(table, IBUS_ORIENTATION_VERTICAL);
    for (size_t i = start; i < end; ++i) {
        std::u32string label = shown.candidates[i];
        const guint candidateLength = Length(label);
        const bool annotated =
            i < shown.candidateAnnotations.size() && !shown.candidateAnnotations[i].empty();
        if (annotated) label += U"  " + shown.candidateAnnotations[i];
        IBusText* text = ibus_text_new_from_string(U8(label).c_str());
        if (annotated) {
            ibus_text_append_attribute(text, IBUS_ATTR_TYPE_FOREGROUND, kAnnotationForeground, candidateLength,
                                       static_cast<gint>(Length(label)));
        }
        ibus_lookup_table_append_candidate(table, text);
        ibus_lookup_table_set_label(table, static_cast<guint>(i - start),
                                    ibus_text_new_from_string(std::to_string(i - start + 1).c_str()));
    }
    ibus_engine_update_lookup_table(engine, table, TRUE);
}

// 補助テキスト（候補ウィンドウの上の 1 行）: 知らせ > 打ち間違いの訂正 > 予測変換 > 候補の位置
void UpdateAuxiliary(IBusEngine* engine) {
    const State& state = StateOf(engine);
    const iroha::SessionOutput& shown = state.shown;
    std::string line;
    if (!state.notice.empty()) {
        line = U8(state.notice);
    } else if (Composing(state) && shown.typoFeedback) {
        line = "訂正: " + U8(shown.typoFeedback->Original()) + " → " + U8(shown.typoFeedback->Corrected()) +
               "（BackSpace で戻す）";
    } else if (Composing(state) && !shown.segmenting && !shown.predictions.empty()) {
        line = "予測:";
        for (size_t i = 0; i < shown.predictions.size(); ++i) {
            const bool selected = static_cast<int32_t>(i) == shown.predictionIndex;
            line += selected ? " [" + U8(shown.predictions[i]) + "]" : " " + U8(shown.predictions[i]);
        }
        line += "（Tab で選ぶ）";
    } else if (Composing(state) && shown.candidatesVisible && shown.candidates.size() > kPageSize) {
        line = std::to_string(shown.candidateIndex + 1) + "/" + std::to_string(shown.candidates.size());
    }
    if (line.empty()) {
        ibus_engine_hide_auxiliary_text(engine);
    } else {
        ibus_engine_update_auxiliary_text(engine, ibus_text_new_from_string(line.c_str()), TRUE);
    }
}

void UpdateProperties(IBusEngine* engine) {
    State& state = StateOf(engine);
    if (!state.modeProperty) return;
    ibus_property_set_symbol(state.modeProperty, ibus_text_new_from_static_string(state.directMode ? "A" : "あ"));
    ibus_property_set_label(state.modeProperty,
                            ibus_text_new_from_static_string(state.directMode ? "入力モード: 英数（クリックでかなに）"
                                                                              : "入力モード: かな（クリックで英数に）"));
    ibus_engine_update_property(engine, state.modeProperty);
    ibus_property_set_state(state.privateProperty, state.privateMode ? PROP_STATE_CHECKED : PROP_STATE_UNCHECKED);
    ibus_engine_update_property(engine, state.privateProperty);
}

gboolean ClearNotice(gpointer data) {
    IBusEngine* engine = IBUS_ENGINE(data);
    State& state = StateOf(engine);
    state.noticeTimer = 0;
    state.notice.clear();
    UpdateAuxiliary(engine);
    return G_SOURCE_REMOVE;
}

void ShowNotice(IBusEngine* engine, const std::u32string& notice) {
    State& state = StateOf(engine);
    state.notice = notice;
    if (state.noticeTimer) g_source_remove(state.noticeTimer);
    state.noticeTimer = g_timeout_add(kNoticeMs, ClearNotice, engine);
}

// 返ってきた表示を IBus に反映する。結果待ち（問い合わせが要る）なら真を返す
bool ApplyOutput(IBusEngine* engine, const iroha::SessionOutput& output, bool fromKey) {
    State& state = StateOf(engine);
    state.shown = output;
    state.shown.commit.clear();
    state.idleUndoable = output.undoableCommit;
    state.idleFullWidthSpace = output.fullWidthSpace;
    if (state.privateMode != output.privateMode) {
        state.privateMode = output.privateMode;
        UpdateProperties(engine);
    }
    // 確定の取り消し: カーソルの手前の確定した文字列を消し、未確定文字列に置き換える
    if (output.replacesCommitted && state.replaceBeforeLength > 0 && !output.preedit.empty()) {
        ibus_engine_delete_surrounding_text(engine, -static_cast<gint>(state.replaceBeforeLength),
                                            state.replaceBeforeLength);
    }
    state.replaceBeforeLength = 0;
    if (!output.commit.empty()) ibus_engine_commit_text(engine, ibus_text_new_from_string(U8(output.commit).c_str()));
    UpdatePreedit(engine, output);
    if (fromKey && !output.notice.empty()) ShowNotice(engine, output.notice);
    UpdateLookupTable(engine);
    UpdateAuxiliary(engine);
    return Composing(state) && (output.livePending || output.typoPending);
}

// ---- 問い合わせ（ライブ変換・打ち間違いの訂正の到着） ----

gboolean PollTick(gpointer data) {
    IBusEngine* engine = IBUS_ENGINE(data);
    State& state = StateOf(engine);
    try {
        iroha::SessionOutput output;
        bool pending = Composing(state) && (state.shown.livePending || state.shown.typoPending);
        if (sharedSessions->Poll(state.sessionId, state.shown.revision, &output)) {
            pending = ApplyOutput(engine, output, /*fromKey=*/false);
        }
        if (pending) return G_SOURCE_CONTINUE;
    } catch (const std::exception& error) {
        Log(std::string("poll failed: ") + error.what());
    }
    state.pollTimer = 0;
    return G_SOURCE_REMOVE;
}

void SetPolling(IBusEngine* engine, bool pending) {
    State& state = StateOf(engine);
    if (pending && state.pollTimer == 0) {
        state.pollTimer = g_timeout_add(kPollIntervalMs, PollTick, engine);
    } else if (!pending && state.pollTimer != 0) {
        g_source_remove(state.pollTimer);
        state.pollTimer = 0;
    }
}

// ---- 文書（周辺テキスト） ----

// カーソルより前の文字列（選択があればその頭より前）。アプリが周辺テキストを渡さないなら偽
bool TextBeforeCursor(IBusEngine* engine, std::u32string* before) {
    if ((engine->client_capabilities & IBUS_CAP_SURROUNDING_TEXT) == 0) return false;
    IBusText* text = nullptr;
    guint cursor = 0;
    guint anchor = 0;
    ibus_engine_get_surrounding_text(engine, &text, &cursor, &anchor);
    if (!text) return false;
    const std::u32string all = iroha::Utf8ToUtf32(ibus_text_get_text(text));
    const size_t end = std::min<size_t>(std::min(cursor, anchor), all.size());
    *before = all.substr(0, end);
    return true;
}

bool ReadLeftContext(IBusEngine* engine, std::u32string* context) {
    std::u32string before;
    if (!TextBeforeCursor(engine, &before)) return false;
    *context = before.size() > kLeftContextLength ? before.substr(before.size() - kLeftContextLength) : before;
    return true;
}

bool TextBeforeCursorIs(IBusEngine* engine, const std::u32string& expected) {
    std::u32string before;
    if (!TextBeforeCursor(engine, &before)) return false;
    return before.size() >= expected.size() &&
           before.compare(before.size() - expected.size(), expected.size(), expected) == 0;
}

// ---- セッションへのキー ----

void SendKey(IBusEngine* engine, iroha::SessionKey key, char32_t character, bool shift) {
    State& state = StateOf(engine);
    // 合成を始めるキーには、カーソルより前の文書のテキスト（左文脈）を付ける。合成の途中では読み直さない
    std::u32string leftContext;
    const bool hasLeftContext = !Composing(state) && ReadLeftContext(engine, &leftContext);
    const iroha::SessionOutput output =
        sharedSessions->HandleKey(state.sessionId, key, character, shift, hasLeftContext ? &leftContext : nullptr);
    SetPolling(engine, ApplyOutput(engine, output, /*fromKey=*/true));
}

void HideAll(IBusEngine* engine) {
    ibus_engine_hide_lookup_table(engine);
    ibus_engine_hide_auxiliary_text(engine);
}

// アプリの側で合成が終わった（表示中の文字列は IBus が確定した）。セッションの合成と手元の控えを捨てる
void ResetComposition(IBusEngine* engine) {
    State& state = StateOf(engine);
    SetPolling(engine, false);
    if (Composing(state)) {
        sharedSessions->HandleKey(state.sessionId, iroha::SessionKey::Reset, 0, false, nullptr);
    }
    state.shown = iroha::SessionOutput();
    HideAll(engine);
}

// 今の表示のまま確定する（入力モードの切り替え・エンジンの無効化の前）
void CommitNow(IBusEngine* engine) {
    if (Composing(StateOf(engine))) SendKey(engine, iroha::SessionKey::CommitNow, 0, false);
}

void SetDirectMode(IBusEngine* engine, bool direct) {
    State& state = StateOf(engine);
    if (state.directMode == direct) return;
    CommitNow(engine);
    state.directMode = direct;
    UpdateProperties(engine);
}

// ---- IBusEngine の仮想関数 ----

gboolean ProcessKeyEvent(IBusEngine* engine, guint keyval, guint keycode, guint modifiers) {
    (void)keycode;
    if (!sharedSessions || (modifiers & IBUS_RELEASE_MASK) != 0) return FALSE;
    // 例外をIBusの呼び出し元へ漏らさない（漏れるとエンジンのプロセスが落ちる）
    try {
        State& state = StateOf(engine);
        iroha::ibus::KeyContext context;
        context.directMode = state.directMode;
        context.composing = Composing(state);
        context.segmenting = state.shown.segmenting;
        context.hasPredictions = !state.shown.predictions.empty();
        context.idleFullWidthSpace = state.idleFullWidthSpace;
        context.idleUndoable = !state.idleUndoable.empty();
        const iroha::ibus::KeyDecision decision = iroha::ibus::DecideKey(keyval, modifiers, context);
        using Kind = iroha::ibus::KeyDecision::Kind;
        switch (decision.kind) {
            case Kind::Pass:
                return FALSE;
            case Kind::Session:
                SendKey(engine, decision.key, decision.character, decision.shift);
                return TRUE;
            case Kind::CommitThenPass:
                CommitNow(engine);
                return FALSE;
            case Kind::UndoCommit: {
                // カーソルの手前が確定した文字列のままのときだけ取り消す（違えばキーはアプリに渡す。
                // アプリでは Ctrl+Backspace で単語を消すことが多い）
                const std::u32string text = state.idleUndoable;
                state.idleUndoable.clear();
                if (text.empty() || Composing(state) || !TextBeforeCursorIs(engine, text)) return FALSE;
                state.replaceBeforeLength = Length(text);
                SendKey(engine, iroha::SessionKey::UndoCommit, 0, false);
                return TRUE;
            }
            case Kind::ToggleDirectMode:
                SetDirectMode(engine, !state.directMode);
                return TRUE;
            case Kind::SetKanaMode:
                SetDirectMode(engine, false);
                return TRUE;
            case Kind::SetDirectMode:
                SetDirectMode(engine, true);
                return TRUE;
        }
    } catch (const std::exception& error) {
        Log(std::string("key failed: ") + error.what());
    }
    return FALSE;
}

void FocusIn(IBusEngine* engine) {
    State& state = StateOf(engine);
    ibus_engine_register_properties(engine, state.properties);
    // 周辺テキストは、エンジンが一度読むとアプリに求めるようになる
    std::u32string ignored;
    TextBeforeCursor(engine, &ignored);
    IBUS_ENGINE_CLASS(iroha_engine_parent_class)->focus_in(engine);
}

void FocusOut(IBusEngine* engine) {
    try {
        ResetComposition(engine);
    } catch (const std::exception& error) {
        Log(std::string("focus out failed: ") + error.what());
    }
    IBUS_ENGINE_CLASS(iroha_engine_parent_class)->focus_out(engine);
}

void Reset(IBusEngine* engine) {
    try {
        ResetComposition(engine);
    } catch (const std::exception& error) {
        Log(std::string("reset failed: ") + error.what());
    }
    IBUS_ENGINE_CLASS(iroha_engine_parent_class)->reset(engine);
}

void Enable(IBusEngine* engine) {
    State& state = StateOf(engine);
    state.directMode = false;
    UpdateProperties(engine);
    std::u32string ignored;
    TextBeforeCursor(engine, &ignored);
    IBUS_ENGINE_CLASS(iroha_engine_parent_class)->enable(engine);
}

void Disable(IBusEngine* engine) {
    // ほかのエンジンに切り替えた: 未確定文字列は今の表示のまま確定する
    try {
        CommitNow(engine);
        ResetComposition(engine);
    } catch (const std::exception& error) {
        Log(std::string("disable failed: ") + error.what());
    }
    IBUS_ENGINE_CLASS(iroha_engine_parent_class)->disable(engine);
}

void PropertyActivate(IBusEngine* engine, const gchar* name, guint propertyState) {
    (void)propertyState;
    try {
        const std::string property = name ? name : "";
        if (property == "InputMode") {
            SetDirectMode(engine, !StateOf(engine).directMode);
        } else if (property == "PrivateMode") {
            SendKey(engine, iroha::SessionKey::TogglePrivateMode, 0, false);
        }
    } catch (const std::exception& error) {
        Log(std::string("property failed: ") + error.what());
    }
}

// 候補のクリック（index はページの中の番号。今のページだけを渡しているので）
void CandidateClicked(IBusEngine* engine, guint index, guint button, guint modifiers) {
    (void)button;
    (void)modifiers;
    try {
        const State& state = StateOf(engine);
        if (!Composing(state) || !state.shown.candidatesVisible || index >= kPageSize) return;
        SendKey(engine, iroha::SessionKey::Character, static_cast<char32_t>(U'1' + index), false);
    } catch (const std::exception& error) {
        Log(std::string("candidate click failed: ") + error.what());
    }
}

// 候補ウィンドウの矢印（↑↓・ページ送り）
void MoveCandidate(IBusEngine* engine, iroha::SessionKey key, size_t times) {
    try {
        const State& state = StateOf(engine);
        if (!Composing(state) || !state.shown.candidatesVisible) return;
        for (size_t i = 0; i < times; ++i) SendKey(engine, key, 0, false);
    } catch (const std::exception& error) {
        Log(std::string("candidate move failed: ") + error.what());
    }
}

void CursorUp(IBusEngine* engine) { MoveCandidate(engine, iroha::SessionKey::Up, 1); }
void CursorDown(IBusEngine* engine) { MoveCandidate(engine, iroha::SessionKey::Down, 1); }
void PageUp(IBusEngine* engine) { MoveCandidate(engine, iroha::SessionKey::Up, kPageSize); }
void PageDown(IBusEngine* engine) { MoveCandidate(engine, iroha::SessionKey::Down, kPageSize); }

void Destroy(IBusObject* object) {
    IBusEngine* engine = IBUS_ENGINE(object);
    State* state = IROHA_ENGINE(engine)->state;
    if (state) {
        if (state->pollTimer) g_source_remove(state->pollTimer);
        if (state->noticeTimer) g_source_remove(state->noticeTimer);
        state->pollTimer = 0;
        state->noticeTimer = 0;
        if (sharedSessions && state->sessionId != 0) sharedSessions->End(state->sessionId);
        state->sessionId = 0;
        if (state->properties) g_object_unref(state->properties);
        if (state->modeProperty) g_object_unref(state->modeProperty);
        if (state->privateProperty) g_object_unref(state->privateProperty);
        state->properties = nullptr;
        state->modeProperty = nullptr;
        state->privateProperty = nullptr;
    }
    IBUS_OBJECT_CLASS(iroha_engine_parent_class)->destroy(object);
}

void Finalize(GObject* object) {
    delete IROHA_ENGINE(object)->state;
    IROHA_ENGINE(object)->state = nullptr;
    G_OBJECT_CLASS(iroha_engine_parent_class)->finalize(object);
}

} // namespace

static void iroha_engine_class_init(IrohaEngineClass* klass) {
    IBusEngineClass* engineClass = IBUS_ENGINE_CLASS(klass);
    engineClass->process_key_event = ProcessKeyEvent;
    engineClass->focus_in = FocusIn;
    engineClass->focus_out = FocusOut;
    engineClass->reset = Reset;
    engineClass->enable = Enable;
    engineClass->disable = Disable;
    engineClass->property_activate = PropertyActivate;
    engineClass->candidate_clicked = CandidateClicked;
    engineClass->cursor_up = CursorUp;
    engineClass->cursor_down = CursorDown;
    engineClass->page_up = PageUp;
    engineClass->page_down = PageDown;
    IBUS_OBJECT_CLASS(klass)->destroy = Destroy;
    G_OBJECT_CLASS(klass)->finalize = Finalize;
}

static void iroha_engine_init(IrohaEngine* engine) {
    State* state = new State();
    engine->state = state;
    state->sessionId = nextSessionId++;
    state->modeProperty = ibus_property_new("InputMode", PROP_TYPE_NORMAL,
                                            ibus_text_new_from_static_string("入力モード"), nullptr,
                                            ibus_text_new_from_static_string("かな ⇄ 英数（半角/全角キー）"), TRUE,
                                            TRUE, PROP_STATE_UNCHECKED, nullptr);
    g_object_ref_sink(state->modeProperty);
    ibus_property_set_symbol(state->modeProperty, ibus_text_new_from_static_string("あ"));
    state->privateProperty = ibus_property_new(
        "PrivateMode", PROP_TYPE_TOGGLE, ibus_text_new_from_static_string("プライベートモード"), nullptr,
        ibus_text_new_from_static_string("学習・入力履歴・予測変換・変換記録・ユーザ辞書を止める（Ctrl+Alt+P）"), TRUE, TRUE,
        PROP_STATE_UNCHECKED, nullptr);
    g_object_ref_sink(state->privateProperty);
    state->properties = ibus_prop_list_new();
    g_object_ref_sink(state->properties);
    ibus_prop_list_append(state->properties, state->modeProperty);
    ibus_prop_list_append(state->properties, state->privateProperty);
    if (sharedHost) state->privateMode = sharedHost->Settings().privateMode;
    if (state->privateMode) ibus_property_set_state(state->privateProperty, PROP_STATE_CHECKED);
}

namespace iroha::ibus {

void SetShared(EngineHost* host, SessionManager* sessions, std::function<void(const std::string&)> log) {
    sharedHost = host;
    sharedSessions = sessions;
    sharedLog = std::move(log);
}

} // namespace iroha::ibus
