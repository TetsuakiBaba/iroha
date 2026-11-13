#include "iroha/input_session.h"

#include <algorithm>

#include "iroha/candidate_grid.h"
#include "iroha/kana.h"
#include "iroha/reading_aligner.h"
#include "iroha/unicode.h"

namespace iroha {

namespace {

bool StartsWith(const std::u32string& text, const std::u32string& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool IsNewline(char32_t c) {
    return c == U'\n' || c == U'\r' || c == 0x0B || c == 0x0C || c == 0x85 || c == 0x2028 ||
           c == 0x2029;
}

bool IsWhitespace(char32_t c) {
    return IsNewline(c) || c == U' ' || c == U'\t' || c == 0xA0 || c == 0x3000 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x202F || c == 0x205F || c == 0x1680;
}

std::u32string Lowercased(std::u32string text) {
    for (char32_t& c : text) {
        if (c >= U'A' && c <= U'Z') c = c - U'A' + U'a';
    }
    return text;
}

std::u32string Uppercased(std::u32string text) {
    for (char32_t& c : text) {
        if (c >= U'a' && c <= U'z') c = c - U'a' + U'A';
    }
    return text;
}

} // namespace

std::u32string SessionOutput::PreeditText() const {
    std::u32string text;
    for (const Span& span : preedit) text += span.text;
    return text;
}

InputSession::InputSession(Host* host) : host_(host) { state_.composer = MakeComposer(); }

RomajiComposer InputSession::MakeComposer() const {
    const std::u32string& style = settings_.punctuation;
    return RomajiComposer(style.size() >= 1 ? style.substr(0, 1) : U"、",
                          style.size() >= 2 ? style.substr(1, 1) : U"。");
}

void InputSession::SetSettings(const SessionSettings& settings) {
    settings_ = settings;
    settings_.candidateCount = std::max(3, std::min(16, settings_.candidateCount));
    // 句読点の変更は、入力していなければすぐ、入力中なら次の確定から効く（macOS版と同じ）
    if (!IsComposing()) state_.composer = MakeComposer();
}

bool InputSession::StateComposing() const {
    return !state_.composer.Empty() || !state_.fixedChunks.empty() || state_.alphabetRun.has_value();
}

bool InputSession::IsComposing() const {
    return state_.mode == Mode::Segmenting || StateComposing();
}

std::u32string InputSession::NormalizeLeftContext(const std::u32string& text) {
    size_t lineStart = 0;
    for (size_t i = text.size(); i > 0; --i) {
        if (IsNewline(text[i - 1])) {
            lineStart = i;
            break;
        }
    }
    while (lineStart < text.size() && IsWhitespace(text[lineStart])) ++lineStart;
    std::u32string line = text.substr(lineStart);
    if (line.size() > kMaxContextLength) line = line.substr(line.size() - kMaxContextLength);
    return line;
}

std::vector<std::u32string> InputSession::AlphabetCandidates(const std::u32string& text) {
    std::vector<std::u32string> variants = {text, Lowercased(text), Uppercased(text)};
    if (!text.empty()) variants.push_back(Uppercased(text.substr(0, 1)) + Lowercased(text.substr(1)));
    variants.push_back(ToFullwidth(text));
    std::vector<std::u32string> results;
    for (const std::u32string& variant : variants) {
        if (std::find(results.begin(), results.end(), variant) == results.end()) results.push_back(variant);
    }
    return results;
}

SessionOutput InputSession::Handle(SessionKey key, char32_t character, bool shift,
                                   const std::u32string* leftContext) {
    replacesCommitted_ = false;
    notice_.clear();
    // 打ち間違いの自動訂正を取り消せるのは「直した直後の Backspace」だけ。ほかのキーでその窓は閉じる
    if (key != SessionKey::Backspace) EndTypoUndo();
    // AI変換の結果待ち: Esc で取り消して元の未確定文字列に戻す。それ以外のキーは飲み込む
    // （フォーカスが移るなど確定を求められたら、元の日本語のまま確定する）
    if (translation_) {
        std::u32string commit;
        if (key == SessionKey::Escape) {
            CancelTranslation();
        } else if (key == SessionKey::Reset) {
            CancelTranslation();
            ResetState();
        } else if (key == SessionKey::CommitNow) {
            const std::u32string japanese = translation_->japanese;
            CancelTranslation();
            CommitText(japanese, &commit);
        }
        ++revision_;
        SessionOutput output = Current();
        output.commit = pendingCommit_ + commit;
        pendingCommit_.clear();
        return output;
    }
    // 声の途中結果を見せている間は合成中と同じ扱い（TIP は左文脈を付けてこない。録音を始めたときに読んである）
    // アプリの文章を文脈に使わない設定なら、TIP が読んだ左文脈は捨てる（直前に確定した文字列を使う）
    if (!settings_.documentContext) leftContext = nullptr;
    if (!IsComposing() && !voicePartial_) {
        state_.contextFromDocument = leftContext != nullptr;
        if (leftContext) {
            state_.context = NormalizeLeftContext(*leftContext);
            state_.contextHasPrivateText = false;
        } else if (state_.contextHasPrivateText && !settings_.privateMode) {
            // プライベートモード中に確定した文字列を、モードを切ったあとの左文脈に残さない
            state_.context.clear();
            state_.contextHasPrivateText = false;
        }
    }

    // 「こえ」モードのキー
    if (IsVoiceKey(key)) {
        HandleVoiceKey(key);
        ++revision_;
        SessionOutput output = Current();
        output.commit = pendingCommit_;
        pendingCommit_.clear();
        return output;
    }
    // 押している間にほかのキーが来た: ショートカットに使っている（録音を捨てる）。合成を終えるキーなら認識も捨てる。
    // 認識の結果を待っている間に打ち始めたら、途中結果の表示だけ消す（結果は届いたら続きに入れる）
    if (voicePhase_ == VoicePhase::Waiting || voicePhase_ == VoicePhase::Recording || key == SessionKey::Reset ||
        key == SessionKey::CommitNow) {
        if (voicePhase_ != VoicePhase::Idle) CancelVoiceInput();
    } else {
        voicePartial_.reset();
    }
    voiceMessage_.clear();
    voiceCompare_.reset();

    // AIで直す: 窓・差し替えの取り消しのキー（ほかのキーなら窓を閉じて、処理を続ける）
    if (HandleAIFixKey(key, shift)) {
        ++revision_;
        SessionOutput output = Current();
        output.commit = pendingCommit_;
        pendingCommit_.clear();
        return output;
    }

    std::u32string commit;
    if (key == SessionKey::UndoCommit) {
        UndoCommit();
    } else {
        // 確定を取り消せるのは確定した直後だけ。ほかのキーが来たら取り消せなくする（持っていた記録をここで書く）
        CloseCommitUndo();
        // 予測変換の小窓: Tab で候補を選ぶ。選んだまま他のキーを押すと、その候補を入れてからキーを処理する
        if (prediction_ && state_.mode == Mode::Composing && HandlePredictionKey(key, shift)) {
            ++revision_;
            SessionOutput output = Current();
            return output;
        }
        switch (key) {
        case SessionKey::Reset:
            ResetState();
            break;
        case SessionKey::CommitNow:
            if (IsComposing()) {
                ForgetRejectedLearnings();
                if (const auto text = ResolveCommitText()) {
                    if (state_.mode == Mode::Segmenting) {
                        RecordInputHistory(std::nullopt);
                    } else {
                        const std::u32string prefix = FixedText();
                        RecordInputHistory(!state_.alphabetRun && text->compare(0, prefix.size(), prefix) == 0
                                               ? std::optional<std::u32string>(text->substr(prefix.size()))
                                               : std::nullopt);
                    }
                    CommitText(*text, &commit);
                }
            }
            break;
        case SessionKey::TogglePunctuation:
            settings_.punctuation = settings_.punctuation == U"、。" ? U"，．" : U"、。";
            host_->PunctuationChanged(settings_.punctuation);
            if (!IsComposing()) state_.composer = MakeComposer();
            notice_ = U"句読点: " + settings_.punctuation;
            break;
        case SessionKey::AICommit:
            if (IsComposing()) StartAICommit(static_cast<size_t>(character), &commit);
            break;
        case SessionKey::TogglePrivateMode:
            settings_.privateMode = !settings_.privateMode;
            host_->PrivateModeChanged(settings_.privateMode);
            notice_ = settings_.privateMode ? U"プライベートモード: オン（学習・入力履歴・予測・ユーザ辞書を止めます）"
                                            : U"プライベートモード: オフ";
            break;
        default:
            if (const auto preset = ShiftSpaceAIPreset(key, shift)) {
                StartAICommit(*preset, &commit);
            } else if (state_.mode == Mode::Segmenting) {
                HandleSegmenting(key, character, shift, &commit);
            } else {
                HandleComposing(key, character, shift, &commit);
            }
            break;
        }
    }
    // キーの処理で読み・状態が変わったら予測の候補を引き直す（変わっていなければ選択はそのまま）
    RefreshPrediction();
    ++revision_;
    SessionOutput output = Current();
    output.commit = pendingCommit_ + commit;
    pendingCommit_.clear();
    output.replacesCommitted = replacesCommitted_;
    return output;
}

SessionOutput InputSession::TakePolledOutput() {
    SessionOutput output = Current();
    output.commit = std::move(pendingCommit_);
    pendingCommit_.clear();
    return output;
}

// ---- 予測変換（入力履歴） ----

// 入力履歴から、いまの読みの先頭が一致する語を引く（候補が無ければ閉じる）。
// 読みが前回と同じなら引き直さず、Tab の選択もそのまま残す。英字入力中・表示形の指定中は出さない
void InputSession::RefreshPrediction() {
    const State& s = state_;
    if (!PredictionActive() || s.mode != Mode::Composing || s.alphabetRun || s.displayOverride || typoFeedback_) {
        prediction_.reset();
        return;
    }
    const std::u32string& reading = s.composer.Text();
    if (prediction_ && prediction_->reading == reading) return;
    // 候補を出し始める読みの文字数（1文字では候補が多すぎて当たらない。macOS版と同じ 2）
    std::vector<InputHistoryEntry> candidates;
    if (reading.size() >= 2) candidates = host_->PredictionCandidates(reading);
    if (candidates.empty()) {
        prediction_.reset();
        return;
    }
    prediction_ = Prediction{reading, std::move(candidates), std::nullopt};
}

bool InputSession::HandlePredictionKey(SessionKey key, bool shift) {
    Prediction& prediction = *prediction_;
    int step = 0;
    if ((key == SessionKey::Tab && !shift) || (key == SessionKey::Down && !shift)) step = 1;
    if ((key == SessionKey::Tab && shift) || (key == SessionKey::Up && !shift)) step = -1;
    if (step != 0) {
        const size_t count = prediction.candidates.size();
        if (prediction.selected) {
            prediction.selected = (*prediction.selected + (step > 0 ? 1 : count - 1)) % count;
        } else {
            prediction.selected = step > 0 ? 0 : count - 1;
        }
        return true;
    }
    if (!prediction.selected) return key == SessionKey::Tab; // 選んでいない Tab は何もしない
    if (key == SessionKey::Escape || key == SessionKey::Backspace) {
        prediction.selected.reset();
        return true;
    }
    ApplyPrediction(prediction.candidates[*prediction.selected]);
    return false;
}

// 候補を未確定文字列に入れる（確定はしない）。打っていた読みを候補の読み・表記に置き換え、
// 以降この部分はライブ変換にかけ直さない。Backspace・Esc で打っていた読みに戻せる
void InputSession::ApplyPrediction(InputHistoryEntry entry) {
    State& s = state_;
    prediction_.reset();
    FixedChunk chunk;
    chunk.reading = entry.reading;
    chunk.text = entry.text;
    chunk.kind = FixedChunk::Kind::Prediction;
    chunk.typedReading = s.composer.Text();
    s.fixedChunks.push_back(std::move(chunk));
    s.composer = MakeComposer();
    s.lastConversion.reset();
    s.showsKanaOnly = false;
    CancelLiveConversion();
}

// 直前に入れた予測の候補を取り消し、候補を入れる前に打っていた読みに戻す
void InputSession::UndoLastPrediction() {
    State& s = state_;
    if (s.fixedChunks.empty() || s.fixedChunks.back().kind != FixedChunk::Kind::Prediction) return;
    const FixedChunk last = s.fixedChunks.back();
    s.fixedChunks.pop_back();
    s.composer.PrependText(last.typedReading.empty() ? last.reading : last.typedReading);
    s.showsKanaOnly = false;
    ComposerDidChange();
}

// 確定する内容を入力履歴に記録する（予測変換が ON でプライベートモードでないときだけ。取り消せなくなったときに書く）。
// ローマ字で打った部分だけを対象にし、Shift+英字の部分・学習しない候補を選んだ文節は残さない。
// composerText は、入力中の読み（Flush 済みの composer.Text()）を確定する表記。文節変換中は使わない
void InputSession::RecordInputHistory(const std::optional<std::u32string>& composerText) {
    if (!PredictionActive()) return;
    const State& s = state_;
    if (s.compositionHasVoice) return;
    std::vector<std::pair<std::u32string, std::u32string>> pieces;
    if (s.mode == Mode::Segmenting) {
        if (s.segmentsFromAI) return;
        for (const Segment& segment : s.segments) {
            if (segment.unlearnable.count(segment.result) == 0) pieces.push_back({segment.reading, segment.result});
        }
    } else {
        for (const FixedChunk& chunk : s.fixedChunks) {
            if (chunk.kind != FixedChunk::Kind::Alphabet) pieces.push_back({chunk.reading, chunk.text});
        }
        if (composerText && !s.composer.Text().empty()) pieces.push_back({s.composer.Text(), *composerText});
    }
    if (pieces.empty()) return;
    Host* host = host_;
    pendingEffects_.push_back([host, pieces] {
        for (const auto& piece : pieces) host->RecordInputHistory(piece.first, piece.second);
    });
}

// ---- 入力・ライブ変換中 ----

void InputSession::HandleComposing(SessionKey key, char32_t character, bool shift,
                                   std::u32string* commit) {
    State& s = state_;
    switch (key) {
    case SessionKey::Character:
        // 区切り記号で囲んだ英字入力。Shift+英字より先に見る
        if (s.alphabetRunDelimited && s.alphabetRun) {
            if (character == settings_.delimitedEnd) {
                FinishDelimitedAlphabet();
            } else {
                s.alphabetRun->push_back(character);
            }
            break;
        }
        if (settings_.delimitedAlphabet && character == settings_.delimitedStart) {
            StartDelimitedAlphabet();
            break;
        }
        // Shift+英字（大文字）で英字入力を始める。Shift を押している間は記号・数字もそのまま英字に足し、
        // Shift を離して打った文字からはかな入力に戻る（それまでの英字は固定部分になる）
        if (StartsAlphabetRun(character) || (s.alphabetRun && shift)) {
            InputAlphabet(character);
            break;
        }
        FinishAlphabetRun();
        LockDisplayOverride();
        InputCharacter(character);
        break;
    case SessionKey::Enter:
        if (!StateComposing()) break;
        ForgetRejectedLearnings();
        if (const auto text = ResolveCommitText()) {
            const std::u32string prefix = FixedText();
            RecordInputHistory(!s.alphabetRun && text->compare(0, prefix.size(), prefix) == 0
                                   ? std::optional<std::u32string>(text->substr(prefix.size()))
                                   : std::nullopt);
            CommitText(*text, commit);
        }
        break;
    case SessionKey::Backspace:
        if (typoUndoAvailable_) {
            // 自動で直した直後の Backspace は、1 文字消すのではなく訂正を取り消して打った読みに戻す
            if (UndoTypoCorrection()) break;
            EndTypoUndo();
        }
        if (!StateComposing()) break;
        // 確かめている途中の訂正は要らなくなる（読みが変わる）
        CancelTypoCheck();
        if (!s.alphabetRun && s.composer.Empty() && !s.fixedChunks.empty() &&
            s.fixedChunks.back().kind == FixedChunk::Kind::Prediction) {
            // 入れた予測の候補に戻ってきた: 1文字ずつは消さず、候補を入れる前に打っていた読みに戻す
            s.displayOverride.reset();
            UndoLastPrediction();
            break;
        }
        if (!s.alphabetRun && s.composer.Empty() && !s.fixedChunks.empty() &&
            s.fixedChunks.back().kind == FixedChunk::Kind::Alphabet) {
            // 直前の英字部分に戻ってきた: 英字入力として開き直してから1文字消す
            // （区切り記号で入れた英字なら、区切り記号の英字入力として開き直す）
            s.alphabetRun = s.fixedChunks.back().text;
            s.alphabetRunDelimited = s.fixedChunks.back().delimited;
            s.fixedChunks.pop_back();
        }
        if (s.alphabetRun) {
            s.displayOverride.reset();
            s.displayOverrideRejected.reset();
            DeleteAlphabetBackward();
            if (!StateComposing()) ResetState();
            break;
        }
        s.displayOverride.reset();
        s.displayOverrideRejected.reset();
        // 固定部分しか残っていないときは、直前の固定部分を読みに戻してから削除する
        if (s.composer.Empty()) UnlockLastFixedChunk();
        s.composer.DeleteBackward();
        if (!StateComposing()) {
            ResetState();
            break;
        }
        // 削除中はライブ変換を止めてかな表示に戻す。漢字のままだと1かな消しただけで
        // 文節構造が変わり、どこを消したのか分からなくなるため。
        // 次の1文字を入力した時点でライブ変換が再開する（そのままEnterならかなで確定）
        CancelLiveConversion();
        s.showsKanaOnly = true;
        break;
    case SessionKey::Escape:
        if (!StateComposing()) break;
        if (s.alphabetRun) {
            // 英字入力を取り消して、Shiftを押す前の状態（ライブ変換表示）に戻す
            EndAlphabetRun();
        } else if (s.displayOverride) {
            // F6/F7等の上書き表示をやめてかなに戻す
            s.displayOverride.reset();
            s.displayOverrideRejected.reset();
            s.showsKanaOnly = true;
        } else if (s.composer.Empty() && !s.fixedChunks.empty() &&
                   s.fixedChunks.back().kind == FixedChunk::Kind::Prediction) {
            // 入れた予測の候補を取り消す（Backspace と同じ。入力全体を消してしまわないように）
            UndoLastPrediction();
        } else if (UnlockFixedChunks() || (s.lastConversion && !s.showsKanaOnly)) {
            // 1回目のEsc: 固定した部分をかなに戻し、変換をやめてかな表示にする
            // （英字の固定部分はかなに戻せないのでそのまま残る）
            s.lastConversion.reset();
            CancelLiveConversion();
        } else {
            // 2回目のEsc: 入力自体を取り消す
            ResetState();
        }
        break;
    case SessionKey::Space:
        // 区切り記号の英字入力中の Shift+Space は、英字の中にスペースを入れる
        if (s.alphabetRunDelimited && s.alphabetRun && shift) {
            s.alphabetRun->push_back(U' ');
            break;
        }
        if (!StateComposing()) {
            // 入力していないときのスペース。設定で常に半角にしていなければ全角を入れる
            // （Shift+スペースは半角のままアプリに任せる。TIP はそのとき送ってこない）
            if (settings_.fullWidthSpace && !shift) CommitText(U"　", commit);
            break;
        }
        EnterSegmentMode();
        break;
    case SessionKey::DisplayHiragana:
    case SessionKey::DisplayKatakana:
    case SessionKey::DisplayHalfKatakana:
    case SessionKey::DisplayFullAlphanumeric:
    case SessionKey::DisplayHalfAlphanumeric:
        ApplyDisplayConversion(key);
        break;
    default:
        // 矢印など: 未確定中はカーソル移動でテキスト側に抜けないよう握りつぶす
        break;
    }
}

void InputSession::InputCharacter(char32_t character) {
    state_.composer.Input(character);
    state_.showsKanaOnly = false;
    ComposerDidChange();
}

std::u32string InputSession::FixedText() const {
    std::u32string text;
    for (const FixedChunk& chunk : state_.fixedChunks) text += chunk.text;
    return text;
}

// 入力内容が変わった: ライブ変換を頼む（結果は後から届く）。固定部分は変換し直さず、文脈として渡す
void InputSession::ComposerDidChange() {
    // 打ち間違いの訂正はライブ変換の ON/OFF に関わらず仕掛ける（読みを直すのが仕事なので）
    ScheduleTypoCorrection();
    ScheduleAIFixCheck();
    const std::u32string& reading = state_.composer.Text();
    if (!settings_.liveConversion || reading.empty()) {
        CancelLiveConversion();
        return;
    }
    // 変換済みの読みと同じなら再変換不要
    if (state_.lastConversion && state_.lastConversion->first == reading) {
        CancelLiveConversion();
        return;
    }
    // 同じ読みの変換を頼んであれば、その結果を待つ（未解決のローマ字が増えただけ）
    if (livePending_ && liveRequestedReading_ == reading) return;
    RequestConversion(Request{}, reading, state_.context + FixedText());
    liveRequestedReading_ = reading;
}

void InputSession::RequestConversion(const Request& request, const std::u32string& reading,
                                     const std::u32string& context) {
    ++requestGeneration_;
    requests_[requestGeneration_] = request;
    livePending_ = true;
    liveRequestedReading_.clear();
    host_->RequestLiveConversion(requestGeneration_, reading, context);
}

void InputSession::CancelLiveConversion() {
    // ここまでに頼んだ変換の結果を受け取らないようにする
    liveValidAfter_ = requestGeneration_;
    if (livePending_) host_->CancelLiveConversion();
    livePending_ = false;
    liveRequestedReading_.clear();
    requests_.clear();
}

bool InputSession::CompleteLiveConversion(uint64_t generation, const std::u32string& reading,
                                          const std::u32string* converted) {
    if (generation <= liveValidAfter_ || generation > requestGeneration_) return false;
    const auto it = requests_.find(generation);
    if (it == requests_.end()) return false;
    const Request request = it->second;
    // これより前の依頼は、終わったか捨てられている
    requests_.erase(requests_.begin(), std::next(it));
    bool changed = false;
    if (generation == requestGeneration_ && livePending_) {
        livePending_ = false;
        liveRequestedReading_.clear();
        changed = true;
    }
    if (converted && !converted->empty()) {
        State& s = state_;
        if (request.kind == Request::Kind::Live) {
            // 読みが伸びたあとに届いた結果も、頭の部分の変換として使う
            if (s.mode == Mode::Composing && StartsWith(s.composer.Text(), reading)) {
                s.lastConversion.emplace(reading, *converted);
                changed = true;
                TryVoiceComparison();
            }
        } else if (s.mode == Mode::Segmenting && s.segmentEdit &&
                   s.segmentEdit->scopeStart == request.scopeStart &&
                   StartsWith(s.segmentEdit->composer.Text(), request.editReading)) {
            // 打ち直しの変換: 範囲の文節列と、打ち直しの読みの終わりを含む文節
            std::vector<Segment> pieces;
            if (request.splits) {
                pieces = SegmentsFor(reading, *converted);
            } else {
                Segment piece;
                piece.reading = reading;
                piece.result = *converted;
                pieces.push_back(std::move(piece));
            }
            size_t end = 0;
            size_t current = 0;
            bool aligned = false;
            for (size_t i = 0; i < pieces.size(); ++i) {
                end += pieces[i].reading.size();
                current = i;
                if (end >= request.editEnd) {
                    aligned = end == request.editEnd && request.editEnd > 0;
                    break;
                }
            }
            s.segmentEdit->shown = SegmentEdit::Shown{request.editReading, std::move(pieces), current, aligned};
            ShowSegmentEdit();
            changed = true;
        }
    }
    if (changed) ++revision_;
    return changed;
}

// 現在表示すべき未確定文字列（固定部分 + 変換結果があればそれ、なければかな + 未解決ローマ字 + 英字）。
// ちらつき防止: 入力が進んで読みが伸びた場合も、前回の変換結果を接頭辞として
// 使い続け、新しく増えたかなだけを末尾に足す（新しい変換結果が届いたら置き換わる）
std::u32string InputSession::CompositionDisplay() const {
    const State& s = state_;
    const std::u32string prefix = FixedText();
    if (s.alphabetRun) return prefix + s.composer.Display() + AlphabetDisplay();
    if (s.displayOverride) return prefix + *s.displayOverride;
    if (s.lastConversion && !s.showsKanaOnly) {
        const std::u32string& reading = s.lastConversion->first;
        const std::u32string& text = s.composer.Text();
        if (reading == text) return prefix + s.lastConversion->second + s.composer.Pending();
        if (StartsWith(text, reading)) {
            return prefix + s.lastConversion->second + text.substr(reading.size()) + s.composer.Pending();
        }
    }
    // 声で入れた直後（ライブ変換が届く前）は、認識結果の表記を見せる
    if (s.voiceSurface && !s.showsKanaOnly && s.voiceSurface->first == s.composer.Text() &&
        s.composer.Pending().empty()) {
        return prefix + s.voiceSurface->second;
    }
    return prefix + s.composer.Display();
}

// 変換をやめてかなで見せるときの表示（固定部分 + 入力中のかな + 英字）
std::u32string InputSession::KanaDisplay() const {
    return FixedText() + state_.composer.Display() + AlphabetDisplay();
}

std::u32string InputSession::AlphabetDisplay() const {
    if (!state_.alphabetRun) return U"";
    return state_.alphabetRunDelimited ? std::u32string(1, settings_.delimitedStart) + *state_.alphabetRun
                                       : *state_.alphabetRun;
}

// 確定で文書に入る文字列（表示と同じ規則。未解決ローマ字は確定する: "n" → "ん"）。
// macOS版は、文字を消した直後でも変換済みの読みと一致すれば変換結果を確定するが、
// ここでは表示（かな）のとおりに確定する
std::optional<std::u32string> InputSession::ResolveCommitText(bool logging) {
    State& s = state_;
    if (s.mode == Mode::Segmenting) {
        const std::u32string text = JoinedResults();
        return text.empty() ? KanaDisplay() : text;
    }
    if (!StateComposing()) return std::nullopt;
    const std::u32string prefix = FixedText();
    if (s.alphabetRun) return prefix + s.composer.Display() + *s.alphabetRun;
    if (s.displayOverride) {
        // 表示形の指定で Flush 済みなので composer.Text() がこの部分の読み。
        // ライブ変換が同じ読みを変換していれば、それを退けて表示形を選んだ記録になる
        const std::u32string& reading = s.composer.Text();
        if (logging) LogConversion(ConversionLogEntry::Mode::FunctionKey, s.context + prefix, reading,
                      s.lastConversion && s.lastConversion->first == reading
                          ? std::optional<std::u32string>(s.lastConversion->second)
                          : std::nullopt,
                      *s.displayOverride);
        return prefix + *s.displayOverride;
    }
    const std::u32string readingBeforeFlush = s.composer.Text();
    s.composer.Flush();
    const std::u32string flushedSuffix = s.composer.Text().substr(readingBeforeFlush.size());
    if (s.lastConversion && !s.showsKanaOnly) {
        const std::u32string& reading = s.lastConversion->first;
        if (reading == readingBeforeFlush) {
            // ライブ変換の表示をそのまま確定（修正なしの例）。Flush で増えたかなは読みにも結果にも付く
            const std::u32string text = s.lastConversion->second + flushedSuffix;
            if (logging) LogConversion(ConversionLogEntry::Mode::Live, s.context + prefix, s.composer.Text(), text, text);
            return prefix + text;
        }
        if (StartsWith(readingBeforeFlush, reading)) {
            // 変換が追いつく前の確定: 表示と同じく「変換済み接頭辞 + 追加のかな」を確定する
            return prefix + s.lastConversion->second + readingBeforeFlush.substr(reading.size()) +
                   flushedSuffix;
        }
    }
    return prefix + s.composer.Display();
}

// F6=ひらがな / F7=カタカナ / F8=半角カタカナ / F9=全角英数 / F10=半角英数
void InputSession::ApplyDisplayConversion(SessionKey key) {
    State& s = state_;
    if (!StateComposing() || s.alphabetRun) return;
    // 固定部分しかない（新しく打った読みがない）ときは対象がない
    if (s.composer.Empty()) return;
    CancelLiveConversion();
    s.composer.Flush();
    const std::u32string kana = s.composer.Text();
    std::optional<std::u32string> converted;
    switch (key) {
    case SessionKey::DisplayHiragana:
        converted = kana;
        break;
    case SessionKey::DisplayKatakana:
        converted = HiraganaToKatakana(kana);
        break;
    case SessionKey::DisplayHalfKatakana:
        converted = ToHalfwidth(HiraganaToKatakana(kana));
        break;
    case SessionKey::DisplayFullAlphanumeric:
        // 全角英数は打鍵通りの文字列が必要（かな削除後は復元できない）
        if (s.composer.RawIsReliable()) converted = ToFullwidth(s.composer.Raw());
        break;
    case SessionKey::DisplayHalfAlphanumeric:
        if (s.composer.RawIsReliable()) converted = s.composer.Raw();
        break;
    default:
        break;
    }
    if (converted) {
        s.displayOverride = *converted;
        s.displayOverrideRejected = RejectedLearningFor(kana, *converted);
    }
}

// ライブ変換が reading に学習で差し替えた表示を出していて、text がそれと違えば、退けた学習
std::optional<InputSession::RejectedLearning> InputSession::RejectedLearningFor(
    const std::u32string& reading, const std::u32string& text) {
    const auto& last = state_.lastConversion;
    if (!last || last->first != reading || last->second == text) return std::nullopt;
    const std::optional<std::u32string> engineResult = host_->EngineResult(reading, last->second);
    if (!engineResult || *engineResult == last->second) return std::nullopt;
    return RejectedLearning{reading, *engineResult, last->second};
}

// 表示形の指定で退けた学習を消す（確定するときに呼ぶ。取り消せなくなったときに行う）
void InputSession::ForgetRejectedLearnings() {
    if (settings_.privateMode) return;
    std::vector<RejectedLearning> rejected;
    for (const FixedChunk& chunk : state_.fixedChunks) {
        if (chunk.rejectedLearning) rejected.push_back(*chunk.rejectedLearning);
    }
    if (state_.displayOverride && state_.displayOverrideRejected) {
        rejected.push_back(*state_.displayOverrideRejected);
    }
    if (rejected.empty()) return;
    Host* host = host_;
    pendingEffects_.push_back([host, rejected] {
        for (const RejectedLearning& learning : rejected) {
            host->ForgetLearning(learning.reading, learning.engineResult, learning.shown);
        }
    });
}

// F6-F10等で指定した表示形を「固定部分」として取り込む（確定はしない）。
// 以降の入力では、この部分をライブ変換にかけ直さない
void InputSession::LockDisplayOverride() {
    State& s = state_;
    if (!s.displayOverride) return;
    const std::u32string text = *s.displayOverride;
    const std::optional<RejectedLearning> rejected = s.displayOverrideRejected;
    s.displayOverride.reset();
    s.displayOverrideRejected.reset();
    // ApplyDisplayConversion で Flush 済みなので、composer.Text() が指定した部分の読み
    const std::u32string reading = s.composer.Text();
    if (text.empty() || reading.empty()) return;
    s.fixedChunks.push_back({reading, text, FixedChunk::Kind::Display, rejected});
    s.composer = MakeComposer();
    // 残りの読みは空になったので、進行中のライブ変換も破棄する
    s.lastConversion.reset();
    s.showsKanaOnly = false;
    CancelLiveConversion();
}

// 直前の固定部分を読みに戻して composer へ返す
void InputSession::UnlockLastFixedChunk() {
    if (state_.fixedChunks.empty()) return;
    const FixedChunk last = state_.fixedChunks.back();
    state_.fixedChunks.pop_back();
    state_.composer.PrependText(last.reading);
}

// 固定部分を読みに戻して composer へ返す。英字の固定部分はかなに戻せない（読みに英字が混ざると
// エンジンの読み制約が崩れる）し、入れた予測の候補は打った読みと違う読みなので、
// 最後の英字・予測部分より後ろだけを戻す。戻したものがあれば真
bool InputSession::UnlockFixedChunks() {
    auto& chunks = state_.fixedChunks;
    size_t start = 0;
    for (size_t i = chunks.size(); i > 0; --i) {
        if (chunks[i - 1].kind == FixedChunk::Kind::Alphabet ||
            chunks[i - 1].kind == FixedChunk::Kind::Prediction) {
            start = i;
            break;
        }
    }
    if (start >= chunks.size()) return false;
    std::u32string reading;
    for (size_t i = start; i < chunks.size(); ++i) reading += chunks[i].reading;
    state_.composer.PrependText(reading);
    chunks.erase(chunks.begin() + static_cast<std::ptrdiff_t>(start), chunks.end());
    return true;
}

// ---- 英字入力（Shift+英字） ----

// 大文字の英字なら英字入力を始める（Shift・Caps Lock のどちらでも）
bool InputSession::StartsAlphabetRun(char32_t character) {
    return character >= U'A' && character <= U'Z';
}

// 英字を1文字足す。英字入力中でなければ、入力中のかなをその時点の表示で固定して英字入力を始める
void InputSession::InputAlphabet(char32_t character) {
    if (!state_.alphabetRun) {
        LockDisplayOverride();
        LockComposerWithLiveConversion();
        state_.alphabetRun = U"";
    }
    state_.alphabetRun->push_back(character);
}

// 入力中のかなを、いま表示している内容（ライブ変換結果があればそれ、なければかな）で
// 固定部分にする。英字は読みにならないので、以降この部分を変換し直すことはない
void InputSession::LockComposerWithLiveConversion() {
    State& s = state_;
    CancelLiveConversion();
    const std::u32string readingBeforeFlush = s.composer.Text();
    s.composer.Flush();
    const std::u32string reading = s.composer.Text();
    if (reading.empty()) {
        s.lastConversion.reset();
        return;
    }
    std::u32string text = reading;
    if (s.lastConversion && !s.showsKanaOnly) {
        const std::u32string& converted = s.lastConversion->first;
        if (converted == readingBeforeFlush) {
            text = s.lastConversion->second + reading.substr(readingBeforeFlush.size());
        } else if (StartsWith(readingBeforeFlush, converted)) {
            text = s.lastConversion->second + reading.substr(converted.size());
        }
    }
    s.fixedChunks.push_back({reading, text, FixedChunk::Kind::LiveConversion, std::nullopt});
    s.composer = MakeComposer();
    s.lastConversion.reset();
    s.showsKanaOnly = false;
}

// 英字入力を終えて、打った英字を固定部分にする（Shiftを離してかな入力に戻るとき）
void InputSession::FinishAlphabetRun() {
    if (!state_.alphabetRun) return;
    const std::u32string run = *state_.alphabetRun;
    const bool delimited = state_.alphabetRunDelimited;
    state_.alphabetRun.reset();
    state_.alphabetRunDelimited = false;
    if (!run.empty()) {
        FixedChunk chunk{run, run, FixedChunk::Kind::Alphabet, std::nullopt};
        chunk.delimited = delimited;
        state_.fixedChunks.push_back(std::move(chunk));
    }
}

// 英字入力の末尾1文字を削除する。空になったら英字入力をやめて元のかな入力に戻る
// （区切り記号の英字入力では、英字が空になっても開始の記号を残し、もう一度の Backspace でやめる）
void InputSession::DeleteAlphabetBackward() {
    if (!state_.alphabetRun) return;
    if (state_.alphabetRunDelimited && state_.alphabetRun->empty()) {
        EndAlphabetRun();
        return;
    }
    if (!state_.alphabetRun->empty()) state_.alphabetRun->pop_back();
    if (state_.alphabetRun->empty() && !state_.alphabetRunDelimited) EndAlphabetRun();
}

// 開始の記号: 入力中のかなを今の表示で固定し、英字入力を始める（Shift+英字の始まりと同じ）。
// Shift+英字の英字入力中なら、それを固定部分にしてから始める
void InputSession::StartDelimitedAlphabet() {
    FinishAlphabetRun();
    LockDisplayOverride();
    LockComposerWithLiveConversion();
    prediction_.reset();
    state_.alphabetRun = U"";
    state_.alphabetRunDelimited = true;
}

// 終わりの記号: 打った英字を固定部分にしてかな入力に戻る。何も打たずに終わりの記号を打ったとき（既定なら「__」）は、
// 開始の記号そのものをかな入力に入れる（区切りに使っている記号を入れる手段を残すため）
void InputSession::FinishDelimitedAlphabet() {
    if (!state_.alphabetRun || state_.alphabetRun->empty()) {
        EndAlphabetRun();
        state_.composer.Input(settings_.delimitedStart);
        state_.showsKanaOnly = false;
        ComposerDidChange();
        return;
    }
    FinishAlphabetRun();
}

// 英字入力をやめる。英字入力を始めたときに固定したかながあれば、
// そのライブ変換結果ごと元に戻す（Shiftを押す前の表示に戻る）
void InputSession::EndAlphabetRun() {
    State& s = state_;
    s.alphabetRun.reset();
    s.alphabetRunDelimited = false;
    if (s.fixedChunks.empty() || s.fixedChunks.back().kind != FixedChunk::Kind::LiveConversion) return;
    const FixedChunk last = s.fixedChunks.back();
    s.fixedChunks.pop_back();
    s.composer.PrependText(last.reading);
    s.lastConversion.emplace(last.reading, last.text);
    s.showsKanaOnly = false;
}

// ---- 文節変換 ----

std::vector<InputSession::Segment> InputSession::SegmentsFor(const std::u32string& reading,
                                                             const std::u32string& conversion) const {
    std::vector<Segment> segments;
    for (const auto& aligned : ReadingAligner::SegmentReading(reading, conversion)) {
        Segment segment;
        segment.reading = aligned.reading;
        segment.result = aligned.conversion;
        segments.push_back(std::move(segment));
    }
    return segments;
}

// Space: 読み全体を変換して文節に分け、最初の文節の候補ウィンドウまで開く
// （macOS版と同じく 1 回の Space で候補ウィンドウが開く）。
// 固定部分はそのまま先頭の文節にし（変換し直さない）、英字入力中ならその英字を末尾の文節にする
void InputSession::EnterSegmentMode() {
    State& s = state_;
    CancelLiveConversion();
    LockDisplayOverride();
    s.composer.Flush();
    const std::u32string reading = s.composer.Text();

    std::vector<Segment> fixedSegments;
    for (const FixedChunk& chunk : s.fixedChunks) {
        Segment segment;
        segment.reading = chunk.reading;
        segment.result = chunk.text;
        if (chunk.kind == FixedChunk::Kind::Alphabet) {
            segment.candidates = AlphabetCandidates(chunk.text);
            segment.candidatesFetched = true;
        }
        fixedSegments.push_back(std::move(segment));
    }
    std::optional<size_t> alphabetIndex;
    if (s.alphabetRun && !s.alphabetRun->empty()) {
        Segment segment;
        segment.reading = *s.alphabetRun;
        segment.result = *s.alphabetRun;
        segment.candidates = AlphabetCandidates(*s.alphabetRun);
        segment.candidatesFetched = true;
        alphabetIndex = fixedSegments.size();
        fixedSegments.push_back(std::move(segment));
    }
    if (reading.empty() && fixedSegments.empty()) return;

    const std::u32string fixedPrefix = FixedText();
    std::u32string full;
    if (!reading.empty()) {
        // ライブ変換が同じ読みを変換済みならその結果を使う
        if (s.lastConversion && s.lastConversion->first == reading) {
            full = s.lastConversion->second;
        } else {
            std::vector<std::u32string> results;
            std::string error;
            if (!host_->Convert(reading, s.context + fixedPrefix, 1, &results, &error) ||
                results.empty()) {
                // 変換できないときは読みのまま表示を続ける（Enterでかな確定できる）
                return;
            }
            full = results.front();
            s.lastConversion.emplace(reading, full);
        }
    }

    // 合成中の訂正（読みを直す本線）はここで終わり。休止を待たずに Space を押した場合の保険として、
    // 読みを書き換えずに訂正を候補ウィンドウに出す（休止で既に直っていれば訂正は出ない）
    CancelTypoCorrection();
    s.typoReading = reading.empty() ? std::nullopt : std::optional<std::u32string>(reading);
    s.typoResolved = false;
    s.typoSegmentCorrection.reset();
    s.typoWholeSentence.reset();
    s.mode = Mode::Segmenting;
    s.segmentEdit.reset();
    s.segmentsRetyped = false;
    s.segmentsFromAI = false;
    s.segmentFixedCount = fixedSegments.size();
    s.showsKanaOnly = false;
    s.segments = fixedSegments;
    if (!reading.empty()) {
        for (Segment& segment : SegmentsFor(reading, full)) s.segments.push_back(std::move(segment));
        s.segmentBaseline = fixedPrefix + full;
        s.segmentEngineBaseline = fixedPrefix + host_->EngineResult(reading, full).value_or(full);
        s.currentSegment = std::min(fixedSegments.size(), s.segments.size() - 1);
    } else {
        s.segmentBaseline.reset();
        s.segmentEngineBaseline.reset();
        s.currentSegment = alphabetIndex.value_or(std::min(fixedSegments.size(), s.segments.size() - 1));
    }
    s.candidateListOpen = false;
    OpenSegmentCandidates();
}

void InputSession::HandleSegmenting(SessionKey key, char32_t character, bool shift,
                                    std::u32string* commit) {
    State& s = state_;
    // 文節を打ち直している間: 文字・Backspace・Esc はその文節に効く。それ以外のキーは、
    // 打ち直しを終えてから、いつもの文節変換のキーとして処理する
    if (s.segmentEdit && HandleSegmentEditKey(key, character, shift)) return;
    switch (key) {
    case SessionKey::Space:
        // Shift+Space は前の候補（Windows の IME の慣習。macOS版には無い）
        if (s.candidateListOpen) {
            CycleCandidate(shift ? -1 : +1);
        } else {
            OpenSegmentCandidates();
        }
        break;
    case SessionKey::Down:
        if (s.candidateListOpen) {
            CycleCandidate(+1);
        } else {
            OpenSegmentCandidates();
        }
        break;
    case SessionKey::Up:
        if (s.candidateListOpen) CycleCandidate(-1);
        break;
    case SessionKey::Tab:
        // 候補が 1 列に収まらないときだけ、列を横に並べた表に広げる・1 列に戻す
        if (s.candidateListOpen && s.currentSegment < s.segments.size()) {
            CandidateGrid grid(s.segments[s.currentSegment].candidates.size(),
                               s.segments[s.currentSegment].candidateIndex, s.candidatesExpanded);
            if (grid.ToggleExpanded()) s.candidatesExpanded = grid.IsExpanded();
        }
        break;
    case SessionKey::Enter:
        // 候補ウィンドウが開いていても全文節を確定する（選んだ候補は反映済み）
        CommitSegments(commit);
        break;
    case SessionKey::Escape:
        if (s.candidateListOpen) {
            CloseCandidateList();
        } else {
            ExitSegmentModeToKana();
        }
        break;
    case SessionKey::Left:
    case SessionKey::Right: {
        const int delta = key == SessionKey::Left ? -1 : +1;
        if (shift) {
            ResizeSegment(delta);
        } else if (s.candidateListOpen && s.candidatesExpanded && s.currentSegment < s.segments.size()) {
            // 表に広げている間だけ ←→ は隣の列の候補へ移る（文節は動かさない）
            Segment& segment = s.segments[s.currentSegment];
            CandidateGrid grid(segment.candidates.size(), segment.candidateIndex, true);
            if (grid.MoveColumn(delta)) {
                segment.candidateIndex = grid.Selected();
                segment.result = segment.candidates[segment.candidateIndex];
            }
        } else {
            MoveSegment(delta);
        }
        break;
    }
    case SessionKey::Backspace:
        // 選んでいる文節の読みを後ろから 1 文字消す（かな入力に戻すのは Esc）
        CloseCandidateList();
        if (s.currentSegment < s.segments.size()) {
            StartSegmentEdit(std::nullopt);
        } else {
            ExitSegmentModeToKana();
        }
        break;
    case SessionKey::Character: {
        // 候補ウィンドウが開いていれば、番号キーはそのページの番号の候補を選ぶ
        if (s.candidateListOpen && character >= U'1' && character <= U'9') {
            ChooseCandidateOnPage(static_cast<size_t>(character - U'1'));
            break;
        }
        // 文字入力なら、選んでいる文節をその文字から打ち直す。
        // 英字入力を始める文字（Shift+英字・区切り記号）は、確定して新しい入力を始める
        const bool startsDelimited = settings_.delimitedAlphabet && character == settings_.delimitedStart;
        const bool startsAlphabet = StartsAlphabetRun(character);
        if (!startsAlphabet && !startsDelimited && s.currentSegment < s.segments.size()) {
            StartSegmentEdit(character);
            break;
        }
        CommitSegments(commit);
        if (startsDelimited) {
            StartDelimitedAlphabet();
        } else if (startsAlphabet) {
            InputAlphabet(character);
        } else {
            InputCharacter(character);
        }
        break;
    }
    default:
        break;
    }
}

// 文節変換をやめて、読み（かな）の入力状態に戻る。
// もう1度 Space を押せば、変換し直さずに文節変換へ戻れるよう、ライブ変換の結果は残す
void InputSession::ExitSegmentModeToKana() {
    State& s = state_;
    CancelLiveConversion();
    s.mode = Mode::Composing;
    s.segments.clear();
    s.currentSegment = 0;
    s.candidateListOpen = false;
    s.candidatesExpanded = false;
    s.segmentBaseline.reset();
    s.segmentEngineBaseline.reset();
    s.segmentEdit.reset();
    s.segmentsRetyped = false;
    s.segmentsFromAI = false;
    s.showsKanaOnly = true;
    s.typoReading.reset();
    s.typoResolved = false;
    s.typoSegmentCorrection.reset();
    s.typoWholeSentence.reset();
    if (!StateComposing()) ResetState();
}

// 全文節の変換結果を結合して確定する
void InputSession::CommitSegments(std::u32string* commit) {
    ApplyTypoWholeSentenceIfSelected();
    const std::u32string text = JoinedResults();
    ForgetRejectedLearnings();
    LearnIfCorrected(text);
    LogSegmentsCommit(text);
    RecordInputHistory(std::nullopt);
    CommitText(text.empty() ? KanaDisplay() : text, commit);
}

// 文節変換での確定を変換記録に書く。学習と同じく、打ち直した文節や学習しない候補（変換ルールの出力・
// 候補ウィンドウ専用の語・記号）を含む確定は残さない（毎回変わる日付や、読みと対応しない語は学習例にならない）
void InputSession::LogSegmentsCommit(const std::u32string& committed) {
    const State& s = state_;
    if (s.segmentsFromAI || s.segmentsRetyped || s.segments.empty() || committed.empty()) return;
    std::u32string reading;
    std::vector<ConversionLogEntry::Segment> segments;
    for (const Segment& segment : s.segments) {
        if (segment.unlearnable.count(segment.result) != 0) return;
        reading += segment.reading;
        segments.push_back({segment.reading, segment.result});
    }
    LogConversion(ConversionLogEntry::Mode::Segments, s.context, reading, s.segmentBaseline, committed,
                  std::move(segments));
}

// 確定1件を変換記録に書く（設定が OFF・プライベートモードなら何もしない。取り消せなくなったときに書く）。
// context はエンジンに渡した左文脈（文書側の文脈 + 未確定文字列の固定部分）、committed は確定した文字列（固定部分を除く）
void InputSession::LogConversion(ConversionLogEntry::Mode mode, const std::u32string& context,
                                 const std::u32string& reading, const std::optional<std::u32string>& proposed,
                                 const std::u32string& committed,
                                 std::optional<std::vector<ConversionLogEntry::Segment>> segments) {
    if (!settings_.conversionLog || settings_.privateMode || reading.empty() || committed.empty()) return;
    // プライベートモード中に確定した文字列が左文脈に入っている・声で入れた読みを含む
    if (state_.contextHasPrivateText || state_.compositionHasVoice) return;
    const auto source = state_.contextFromDocument ? ConversionLogEntry::ContextSource::Document
                        : state_.context.empty()   ? ConversionLogEntry::ContextSource::None
                                                   : ConversionLogEntry::ContextSource::Committed;
    ConversionLogEntry entry =
        ConversionLogEntry::Make(mode, context, source, reading, proposed, committed, std::move(segments));
    // 左文脈のない確定・読みをそのままの1文字は記録しない。「直した確定だけ」なら、そのまま確定したものも残さない
    if (!ShouldRecordConversion(entry, settings_.conversionLogScope)) return;
    Host* host = host_;
    pendingEffects_.push_back([host, entry] { host->RecordConversion(entry); });
}

// 文節変換の結果がエンジンの出力と違っていたら、ユーザによる修正として学習する（macOS版 learnIfCorrected）。
// 記号・顔文字・絵文字を選んだ文節を含む確定と、打ち直した文節を含む確定は学習しない
void InputSession::LearnIfCorrected(const std::u32string& committed) {
    const State& s = state_;
    if (settings_.privateMode || s.segmentsFromAI || s.segmentsRetyped || s.compositionHasVoice || s.segments.empty() ||
        committed.empty() || !s.segmentBaseline ||
        committed == *s.segmentBaseline) {
        return;
    }
    for (const Segment& segment : s.segments) {
        if (segment.unlearnable.count(segment.result) != 0) return;
    }
    std::u32string reading;
    for (const Segment& segment : s.segments) reading += segment.reading;
    const std::u32string baseline = *s.segmentBaseline;
    const std::u32string engineBaseline = s.segmentEngineBaseline.value_or(baseline);
    Host* host = host_;
    pendingEffects_.push_back([host, reading, engineBaseline, baseline, committed] {
        host->RecordCommit(reading, engineBaseline, baseline, committed);
    });
}

// 文節の候補は「選択中の文節の読み + 左側の変換結果を文脈」で求める。
// 並びは macOS版（IrohaInputController の候補ウィンドウを開く処理）と同じ:
// 今の表示 → 変換ルールの出力 → エンジンの候補（絵文字は 7 番目に合流）→ 顔文字・記号 → ひらがな・カタカナ
bool InputSession::EnsureSegmentCandidates(size_t index) {
    State& s = state_;
    if (index >= s.segments.size()) return false;
    Segment& segment = s.segments[index];
    if (segment.candidatesFetched) return !segment.candidates.empty();
    std::vector<std::u32string> candidates;
    std::string error;
    if (!host_->Convert(segment.reading, s.context + LeftResults(index), settings_.candidateCount,
                        &candidates, &error) ||
        candidates.empty()) {
        return false;
    }
    // 今表示している変換結果を先頭に置く（エンジンの並びが確率順で変わっても、
    // 候補ウィンドウを開いた瞬間に表示が変わらないように）
    if (!segment.result.empty()) {
        candidates.erase(std::remove(candidates.begin(), candidates.end(), segment.result),
                         candidates.end());
        candidates.insert(candidates.begin(), segment.result);
    }
    // ユーザ定義の変換ルール（日付など）の出力を、今の表示の直後に置く
    std::vector<std::u32string> rewrites;
    if (rewriteRules_) {
        for (const std::u32string& rewrite : rewriteRules_->Candidates(segment.reading, RewriteContext::Now())) {
            if (std::find(candidates.begin(), candidates.end(), rewrite) == candidates.end()) {
                rewrites.push_back(rewrite);
            }
        }
    }
    candidates.insert(candidates.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(1, candidates.size())),
                      rewrites.begin(), rewrites.end());
    // 打ち間違いの訂正も同じやり方で合流させる。変換ルールの後ろに置く（ルールはユーザが自分で書いたもの）
    std::vector<std::u32string> typoFixes;
    for (const std::u32string& fix : TypoCandidates(index)) {
        if (std::find(candidates.begin(), candidates.end(), fix) == candidates.end()) typoFixes.push_back(fix);
    }
    const size_t typoInsertIndex = std::min<size_t>(1 + rewrites.size(), candidates.size());
    candidates.insert(candidates.begin() + static_cast<std::ptrdiff_t>(typoInsertIndex), typoFixes.begin(),
                      typoFixes.end());
    // 記号・顔文字・絵文字（Mozc のデータ）。今の表示・変換ルールの出力・訂正は動かさない
    SymbolCandidates symbols;
    if (symbolTable_) symbols = symbolTable_->Candidates(segment.reading);
    candidates = SymbolCandidateMerger::Merge(candidates, symbols, typoInsertIndex + typoFixes.size());
    // 定番のフォールバック候補（ひらがな・カタカナ）を末尾に足す
    for (const std::u32string& extra : {segment.reading, HiraganaToKatakana(segment.reading)}) {
        if (std::find(candidates.begin(), candidates.end(), extra) == candidates.end()) {
            candidates.push_back(extra);
        }
    }
    // 変換ルールの出力（毎回変わる日付など）・ユーザ辞書の候補ウィンドウ専用の語を含む候補・記号は学習しない
    // （覚えるとライブ変換に戻ってくる）
    segment.unlearnable = SymbolCandidateMerger::UnlearnableCandidates(candidates, symbols);
    segment.unlearnable.insert(rewrites.begin(), rewrites.end());
    // 打ち間違いの訂正を選んだ確定も学習しない（覚えると、打ち間違えた読みがライブ変換で勝手に直るようになる）
    segment.unlearnable.insert(typoFixes.begin(), typoFixes.end());
    for (const std::u32string& word : host_->CandidateOnlyWords(segment.reading)) {
        for (const std::u32string& candidate : candidates) {
            if (candidate.find(word) != std::u32string::npos) segment.unlearnable.insert(candidate);
        }
    }
    // 専門辞書の語（か、その語＋読みの残りのかな）になっている候補に、辞書の名前の印を付ける
    segment.annotations.clear();
    if (termLookup_ && !termLookup_->Empty()) {
        bool any = false;
        for (const std::u32string& candidate : candidates) {
            std::u32string annotation;
            for (const std::u32string& label : termLookup_->Labels(segment.reading, candidate)) {
                if (!annotation.empty()) annotation += U"・";
                annotation += label;
            }
            any = any || !annotation.empty();
            segment.annotations.push_back(std::move(annotation));
        }
        if (!any) segment.annotations.clear();
    }
    segment.candidates = std::move(candidates);
    segment.candidatesFetched = true;
    segment.candidateIndex = 0;
    return true;
}

// 選んでいる文節の候補ウィンドウを開く（今の表示の候補を選んだ状態で）
void InputSession::OpenSegmentCandidates() {
    State& s = state_;
    if (!EnsureSegmentCandidates(s.currentSegment)) return;
    Segment& segment = s.segments[s.currentSegment];
    const auto it = std::find(segment.candidates.begin(), segment.candidates.end(), segment.result);
    segment.candidateIndex =
        it == segment.candidates.end() ? 0 : static_cast<size_t>(it - segment.candidates.begin());
    s.candidateListOpen = true;
}

void InputSession::CycleCandidate(int delta) {
    State& s = state_;
    if (s.currentSegment >= s.segments.size() || !EnsureSegmentCandidates(s.currentSegment)) return;
    Segment& segment = s.segments[s.currentSegment];
    const size_t count = segment.candidates.size();
    segment.candidateIndex = (segment.candidateIndex + count + delta) % count;
    segment.result = segment.candidates[segment.candidateIndex];
    s.candidateListOpen = true;
}

// 番号キーで候補を決める: その候補を文節の結果にしてウィンドウを閉じる（確定はしない。macOS版と同じ）
void InputSession::ChooseCandidateOnPage(size_t indexOnPage) {
    State& s = state_;
    if (s.currentSegment >= s.segments.size() || !s.candidateListOpen) return;
    Segment& segment = s.segments[s.currentSegment];
    const size_t index =
        segment.candidateIndex / kCandidatePageSize * kCandidatePageSize + indexOnPage;
    if (index >= segment.candidates.size()) return;
    segment.candidateIndex = index;
    segment.result = segment.candidates[index];
    CloseCandidateList();
}

void InputSession::MoveSegment(int delta) {
    State& s = state_;
    CloseCandidateList();
    if (s.segments.empty()) return;
    size_t next = s.currentSegment;
    if (delta < 0 && next > 0) --next;
    if (delta > 0 && next + 1 < s.segments.size()) ++next;
    s.currentSegment = next;
}

// 選んでいる文節の読みを1文字伸ばす・縮める。その文節と後ろを変換し直す
void InputSession::ResizeSegment(int delta) {
    State& s = state_;
    CloseCandidateList();
    if (s.currentSegment >= s.segments.size()) return;
    std::u32string currentReading = s.segments[s.currentSegment].reading;
    std::u32string remainder;
    for (size_t i = s.currentSegment + 1; i < s.segments.size(); ++i) remainder += s.segments[i].reading;
    if (delta > 0) { // 伸ばす: 次の読みの先頭1文字を取り込む
        if (remainder.empty()) return;
        currentReading.push_back(remainder.front());
        remainder.erase(0, 1);
    } else { // 縮める: 末尾1文字を残りへ返す
        if (currentReading.size() <= 1) return;
        remainder.insert(remainder.begin(), currentReading.back());
        currentReading.pop_back();
    }

    const std::u32string left = LeftResults(s.currentSegment);
    std::vector<std::u32string> converted;
    std::string error;
    if (!host_->Convert(currentReading, s.context + left, 1, &converted, &error) || converted.empty()) {
        return; // 変換できないときは何もしない
    }

    std::vector<Segment> rebuilt(s.segments.begin(),
                                 s.segments.begin() + static_cast<std::ptrdiff_t>(s.currentSegment));
    Segment current;
    current.reading = currentReading;
    current.result = converted.front();
    rebuilt.push_back(std::move(current));
    if (!remainder.empty()) {
        // 残りを変換し直して文節分割もやり直す
        std::vector<std::u32string> rest;
        if (host_->Convert(remainder, s.context + left + converted.front(), 1, &rest, &error) &&
            !rest.empty()) {
            for (Segment& segment : SegmentsFor(remainder, rest.front())) rebuilt.push_back(std::move(segment));
        } else {
            Segment segment;
            segment.reading = remainder;
            segment.result = remainder;
            rebuilt.push_back(std::move(segment));
        }
    }
    s.segments = std::move(rebuilt);
}

std::u32string InputSession::JoinedResults() const {
    std::u32string joined;
    for (const Segment& segment : state_.segments) joined += segment.result;
    return joined;
}

std::u32string InputSession::LeftResults(size_t index) const {
    std::u32string joined;
    for (size_t i = 0; i < index && i < state_.segments.size(); ++i) joined += state_.segments[i].result;
    return joined;
}

// ---- 文節の打ち直し（文節変換中に文字を打つ） ----

// 選んでいる文節を、打った文字から打ち直す（macOS版 startSegmentEdit）。
// 打つたびに、文全体（読みから変換した文節の全部）を新しい読みで変換し直す。固定部分は変えず、左文脈として使う。
// character が無ければ Backspace で始める: 文節の読みをそのまま打ち直しの読みにして、後ろから 1 文字消す
void InputSession::StartSegmentEdit(std::optional<char32_t> character) {
    State& s = state_;
    CloseCandidateList();
    if (s.currentSegment >= s.segments.size()) return;
    CancelLiveConversion();
    const size_t index = s.currentSegment;
    // 読みから変換した文節なら、読みから変換した文節の全部を変換し直す
    const bool whole = !s.segmentsFromAI && index >= s.segmentFixedCount;
    const size_t scopeStart = whole ? std::min(s.segmentFixedCount, index) : index;
    const size_t scopeEnd = whole ? s.segments.size() : index + 1;
    SegmentEdit edit;
    edit.composer = MakeComposer();
    if (character) {
        edit.composer.Input(*character);
    } else {
        edit.composer.ReplaceText(s.segments[index].reading);
        edit.composer.DeleteBackward();
    }
    edit.scopeStart = scopeStart;
    edit.originalSegments = s.segments;
    edit.originalIndex = index;
    edit.before.assign(s.segments.begin() + static_cast<std::ptrdiff_t>(scopeStart),
                       s.segments.begin() + static_cast<std::ptrdiff_t>(index));
    edit.after.assign(s.segments.begin() + static_cast<std::ptrdiff_t>(index + 1),
                      s.segments.begin() + static_cast<std::ptrdiff_t>(scopeEnd));
    edit.outside.assign(s.segments.begin() + static_cast<std::ptrdiff_t>(scopeEnd), s.segments.end());
    edit.splits = whole;
    s.segmentEdit = std::move(edit);
    s.segmentsRetyped = true;
    if (character) {
        SegmentEditDidChange();
    } else {
        ShowSegmentEdit();
        EndSegmentModeIfEmptied();
    }
}

// Backspace で最後の文節まで消して未確定文字列が空になったら、文節変換を終える
void InputSession::EndSegmentModeIfEmptied() {
    State& s = state_;
    if (!s.segmentEdit) return;
    for (const Segment& segment : s.segments) {
        if (!segment.result.empty()) return;
    }
    FinishSegmentEdit(false);
    ExitSegmentModeToKana();
}

bool InputSession::HandleSegmentEditKey(SessionKey key, char32_t character, bool shift) {
    State& s = state_;
    SegmentEdit& edit = *s.segmentEdit;
    switch (key) {
    case SessionKey::Backspace:
        // 空になったら、それ以上は何もしない（戻すのは Esc）
        if (!edit.composer.Empty()) {
            edit.composer.DeleteBackward();
            // 削除中は変換を止め、打ち直しの読みをかなで見せる（次の 1 文字で再開する）
            edit.shown.reset();
            CancelLiveConversion();
            ShowSegmentEdit();
            EndSegmentModeIfEmptied();
        }
        return true;
    case SessionKey::Escape:
        CancelSegmentEdit();
        return true;
    case SessionKey::Character:
        edit.composer.Input(character);
        SegmentEditDidChange();
        return true;
    default: {
        // 空にした文節はここで消え、選択はその場所に来た文節に移る。
        // 消した文節から見て隣へ行く ← → は、それで済んでいるので、もう 1 つ動かさない
        const bool removes = edit.composer.Empty();
        const bool wasLast = s.currentSegment + 1 == s.segments.size();
        // 表示が今の読みの変換でなければ変換し直すのは、文節変換を続ける ← → のときだけ
        // （macOS版は裏で変換し直す。Windows版はその場で変換するので、確定するキーでは確定を遅らせない）
        const bool reconverts = (key == SessionKey::Left || key == SessionKey::Right) && !shift;
        FinishSegmentEdit(reconverts);
        if (removes && !shift && ((key == SessionKey::Right && !wasLast) ||
                                  (key == SessionKey::Left && wasLast))) {
            return true;
        }
        return false;
    }
    }
}

// 打ち直している範囲の、今見せる文節列と、カーソルを置く文節（範囲の中の位置）（macOS版 segmentEditSpan）
InputSession::EditSpan InputSession::SegmentEditSpan(const SegmentEdit& edit) const {
    const std::u32string& text = edit.composer.Text();
    const std::u32string& pending = edit.composer.Pending();
    if (edit.shown && edit.shown->current < edit.shown->segments.size()) {
        const SegmentEdit::Shown& shown = *edit.shown;
        if (text == shown.editReading && pending.empty()) return {shown.segments, shown.current, true};
        if (shown.aligned && StartsWith(text, shown.editReading)) {
            std::vector<Segment> segments = shown.segments;
            const std::u32string added = text.substr(shown.editReading.size());
            segments[shown.current].reading += added;
            segments[shown.current].result += added + pending;
            segments[shown.current].candidatesFetched = false;
            segments[shown.current].candidates.clear();
            return {segments, shown.current, false};
        }
    }
    Segment editing;
    editing.reading = text;
    editing.result = text + pending;
    std::vector<Segment> segments = edit.before;
    segments.push_back(std::move(editing));
    segments.insert(segments.end(), edit.after.begin(), edit.after.end());
    return {segments, edit.before.size(), false};
}

// 打ち直しの内容を文節列に写す（どの確定の経路でも、表示のとおりに確定されるようにする）
void InputSession::ShowSegmentEdit() {
    State& s = state_;
    if (!s.segmentEdit || s.segmentEdit->scopeStart > s.segments.size()) return;
    const SegmentEdit& edit = *s.segmentEdit;
    EditSpan span = SegmentEditSpan(edit);
    std::vector<Segment> segments(s.segments.begin(),
                                  s.segments.begin() + static_cast<std::ptrdiff_t>(edit.scopeStart));
    segments.insert(segments.end(), span.segments.begin(), span.segments.end());
    segments.insert(segments.end(), edit.outside.begin(), edit.outside.end());
    s.segments = std::move(segments);
    s.currentSegment = edit.scopeStart + span.current;
}

// 打ち直しの読みが変わった: 表示を更新し、範囲の読み全体を裏で変換し直す
void InputSession::SegmentEditDidChange() {
    State& s = state_;
    ShowSegmentEdit();
    CancelLiveConversion();
    if (!s.segmentEdit || !settings_.liveConversion) return;
    const SegmentEdit& edit = *s.segmentEdit;
    const std::u32string editReading = edit.composer.Text();
    if (edit.shown && edit.shown->editReading == editReading) return;
    std::u32string beforeReading;
    for (const Segment& segment : edit.before) beforeReading += segment.reading;
    std::u32string reading = beforeReading + editReading;
    for (const Segment& segment : edit.after) reading += segment.reading;
    if (reading.empty()) return;
    Request request;
    request.kind = Request::Kind::SegmentEdit;
    request.scopeStart = edit.scopeStart;
    request.editReading = editReading;
    request.editEnd = beforeReading.size() + editReading.size();
    request.splits = edit.splits;
    RequestConversion(request, reading,
                      s.context + LeftResults(std::min(edit.scopeStart, s.segments.size())));
}

// 打ち直しをやめて、打ち直す前の文節列に戻す
void InputSession::CancelSegmentEdit() {
    State& s = state_;
    if (!s.segmentEdit) return;
    CancelLiveConversion();
    s.segments = s.segmentEdit->originalSegments;
    s.currentSegment = s.segmentEdit->originalIndex;
    s.segmentEdit.reset();
}

// 打ち直しを終える: 未解決のローマ字を確定し、文節列を表示のとおりにする。空にした文節は消す。
// 全体の読み（composer）と固定部分も合わせて、Esc でかなに戻したときに打ち直した読みが出るようにする
void InputSession::FinishSegmentEdit(bool reconverts) {
    State& s = state_;
    if (!s.segmentEdit) return;
    SegmentEdit edit = std::move(*s.segmentEdit);
    s.segmentEdit.reset();
    CancelLiveConversion();
    edit.composer.Flush();
    if (edit.scopeStart > s.segments.size()) return;
    const EditSpan span = SegmentEditSpan(edit);
    std::vector<Segment> spanSegments;
    for (const Segment& segment : span.segments) {
        if (!segment.reading.empty() || !segment.result.empty()) spanSegments.push_back(segment);
    }
    std::vector<Segment> segments(s.segments.begin(),
                                  s.segments.begin() + static_cast<std::ptrdiff_t>(edit.scopeStart));
    segments.insert(segments.end(), spanSegments.begin(), spanSegments.end());
    segments.insert(segments.end(), edit.outside.begin(), edit.outside.end());
    s.segments = std::move(segments);
    const size_t current =
        edit.scopeStart + std::min(span.current, spanSegments.empty() ? size_t(0) : spanSegments.size() - 1);
    s.currentSegment = s.segments.empty() ? 0 : std::min(current, s.segments.size() - 1);
    if (edit.splits) {
        // 全体の読みを、読みから変換した文節の読みの並びに合わせる
        std::u32string reading;
        for (size_t i = std::min(s.segmentFixedCount, s.segments.size()); i < s.segments.size(); ++i) {
            reading += s.segments[i].reading;
        }
        s.composer.ReplaceText(reading);
        s.lastConversion.reset();
    } else {
        SyncCompositionWithRetypedSegment(edit.scopeStart, spanSegments.empty());
    }
    // 英字の文節を Backspace で短くしただけなら、英字のままにする（かな漢字変換に通さない。候補は英字の変種）
    const bool alphabetOnly = !edit.splits && spanSegments.size() == 1 &&
                              IsAlphabetText(spanSegments[0].reading) &&
                              spanSegments[0].reading == spanSegments[0].result;
    if (alphabetOnly && edit.scopeStart < s.segments.size()) {
        s.segments[edit.scopeStart].candidates = AlphabetCandidates(spanSegments[0].result);
        s.segments[edit.scopeStart].candidatesFetched = true;
    }
    if (reconverts && !span.exact && !spanSegments.empty() && !alphabetOnly) {
        ReconvertRetypedSpan(edit.scopeStart, spanSegments.size(), edit.splits);
    }
}

bool InputSession::IsAlphabetText(const std::u32string& text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char32_t c) { return c < 0x80; });
}

// 打ち直しを終えたあと、範囲（start から count 文節）の読みを変換し直して文節に分け直す。
// 選んでいる文節は、読みの同じ位置を含む文節に移す
void InputSession::ReconvertRetypedSpan(size_t start, size_t count, bool splits) {
    State& s = state_;
    if (start + count > s.segments.size()) return;
    std::u32string reading;
    for (size_t i = start; i < start + count; ++i) reading += s.segments[i].reading;
    if (reading.empty()) return;
    std::vector<std::u32string> results;
    std::string error;
    if (!host_->Convert(reading, s.context + LeftResults(start), 1, &results, &error) || results.empty()) {
        return;
    }
    std::vector<Segment> pieces;
    if (splits) {
        pieces = SegmentsFor(reading, results.front());
    } else {
        Segment piece;
        piece.reading = reading;
        piece.result = results.front();
        pieces.push_back(std::move(piece));
    }
    const size_t selected = s.currentSegment;
    std::optional<size_t> offset;
    if (selected >= start && selected < start + count) {
        size_t total = 0;
        for (size_t i = start; i <= selected; ++i) total += s.segments[i].reading.size();
        offset = total;
    }
    s.segments.erase(s.segments.begin() + static_cast<std::ptrdiff_t>(start),
                     s.segments.begin() + static_cast<std::ptrdiff_t>(start + count));
    s.segments.insert(s.segments.begin() + static_cast<std::ptrdiff_t>(start), pieces.begin(), pieces.end());
    if (offset) {
        size_t end = 0;
        size_t index = 0;
        for (size_t i = 0; i < pieces.size(); ++i) {
            end += pieces[i].reading.size();
            index = i;
            if (end >= *offset) break;
        }
        s.currentSegment = start + index;
    } else if (selected >= start + count) {
        s.currentSegment = selected + pieces.size() - count;
    }
}

// 打ち直した固定部分の文節に合わせて、固定部分（fixedChunks・英字入力）を直す
void InputSession::SyncCompositionWithRetypedSegment(size_t index, bool removed) {
    State& s = state_;
    if (index >= s.segmentFixedCount) return;
    std::optional<FixedChunk> chunk;
    if (!removed && index < s.segments.size()) {
        chunk = FixedChunk{s.segments[index].reading, s.segments[index].result,
                           FixedChunk::Kind::LiveConversion, std::nullopt};
        // 英字のままの文節（英字を Backspace で短くした）は、英字の固定部分のままにする（かなに戻さない）
        if (chunk->reading == chunk->text && IsAlphabetText(chunk->text)) {
            chunk->kind = FixedChunk::Kind::Alphabet;
        }
    }
    if (index < s.fixedChunks.size()) {
        if (chunk) {
            s.fixedChunks[index] = *chunk;
        } else {
            s.fixedChunks.erase(s.fixedChunks.begin() + static_cast<std::ptrdiff_t>(index));
        }
    } else {
        // 英字入力中の英字は、固定部分の後ろに 1 つだけある
        s.alphabetRun.reset();
        if (chunk) s.fixedChunks.push_back(*chunk);
    }
    if (!chunk) --s.segmentFixedCount;
}

// ---- AI変換 ----

// AI変換のキーが Shift+Space のプリセット。TIP は Shift+Space を AICommit にせず Space として送ってくるので、ここで決める。
// 候補ウィンドウを開いているとき（前の候補）と、区切り記号の英字入力中（英字の中のスペース）は Shift+Space の元の働きにする
std::optional<size_t> InputSession::ShiftSpaceAIPreset(SessionKey key, bool shift) const {
    constexpr uint32_t kShiftSpace = 0x20 | (1u << 18); // KeyShortcut::Encode の Shift+Space
    if (key != SessionKey::Space || !shift || !IsComposing() || state_.candidateListOpen ||
        (state_.alphabetRunDelimited && state_.alphabetRun)) {
        return std::nullopt;
    }
    for (size_t i = 0; i < settings_.aiShortcuts.size(); ++i) {
        if (settings_.aiShortcuts[i] == kShiftSpace) return i;
    }
    return std::nullopt;
}

// 今の未確定文字列を、プリセットのプロンプトで AI に渡す（macOS版 handleAICommit）。
// 合成の状態は捨てずに結果を待つ（Esc で元に戻れる）。AI が使えなければ普通に確定する
void InputSession::StartAICommit(size_t preset, std::u32string* commit) {
    State& s = state_;
    if (s.mode == Mode::Segmenting) CloseCandidateList();
    const auto japanese = ResolveCommitText(/*logging=*/false);
    if (!japanese || japanese->empty()) return;
    // 結果を文節に区切るための読み（ResolveCommitText が未解決のローマ字を確定したあとの値）
    std::u32string reading;
    if (s.mode == Mode::Segmenting) {
        for (const Segment& segment : s.segments) reading += segment.reading;
    } else {
        for (const FixedChunk& chunk : s.fixedChunks) reading += chunk.reading;
        reading += s.composer.Text() + s.alphabetRun.value_or(U"");
    }
    if (!settings_.aiAvailable || preset >= settings_.aiPresetNames.size()) {
        CommitText(*japanese, commit);
        return;
    }
    CancelLiveConversion();
    CancelTypoCorrection();
    prediction_.reset();
    translation_ = Translation{++aiGeneration_, *japanese, settings_.aiPresetNames[preset], U"", reading};
    host_->RequestAI(aiGeneration_, preset, *japanese);
}

bool InputSession::UpdateAIPartial(uint64_t generation, const std::u32string& partial) {
    if (!translation_ || translation_->generation != generation) return false;
    translation_->partial = TrimWhitespace(partial);
    ++revision_;
    return true;
}

bool InputSession::CompleteAI(uint64_t generation, const std::optional<std::u32string>& output) {
    if (!translation_ || translation_->generation != generation) return false;
    const Translation translation = *translation_;
    translation_.reset();
    if (output && ContainsJapanese(*output)) {
        PresentAIResultAsSegments(*output, translation.reading);
    } else {
        // 英訳など日本語でない結果はそのまま確定する。失敗・打ち切りのときは日本語のまま確定する（テキストを失わない）
        std::u32string commit;
        CommitText(output.value_or(translation.japanese), &commit);
        pendingCommit_ += commit;
    }
    ++revision_;
    return true;
}

// AI の結果（日本語）を、確定せずに文節に区切った未確定文字列として出す（Enter で確定、Space・矢印で文節ごとに直せる）。
// 読みとの対応は ReadingAligner で取る（対応が取れなければ全体で 1 文節）
void InputSession::PresentAIResultAsSegments(const std::u32string& output, const std::u32string& reading) {
    State& s = state_;
    CancelLiveConversion();
    s.mode = Mode::Segmenting;
    s.segments = SegmentsFor(reading, output);
    if (s.segments.empty()) {
        Segment segment;
        segment.reading = reading;
        segment.result = output;
        s.segments.push_back(std::move(segment));
    }
    s.segmentBaseline = output;
    s.segmentEngineBaseline = output;
    s.segmentsFromAI = true;
    s.segmentEdit.reset();
    s.segmentsRetyped = false;
    s.segmentFixedCount = 0;
    s.currentSegment = 0;
    s.candidateListOpen = false;
    s.typoReading.reset();
    s.typoResolved = false;
    s.typoSegmentCorrection.reset();
    s.typoWholeSentence.reset();
}

// AI変換の結果待ちを取り消す（合成の状態はそのまま）
void InputSession::CancelTranslation() {
    if (!translation_) return;
    translation_.reset();
    host_->CancelAI();
}

// ---- 「こえ」モード ----

// 押した: 録音を始めてもらう（変換サーバが少し待ってからマイクを開く）。離した: そこまでの音声の認識を終えてもらう。
// 押している間にほかのキーが来た（TIP が送る）: ショートカットに使っているので捨てる
void InputSession::HandleVoiceKey(SessionKey key) {
    switch (key) {
    case SessionKey::VoiceStart: {
        if (!settings_.voiceInput || translation_ || voicePhase_ == VoicePhase::Finishing) return;
        // 録音中にまた押された（離したことが届かなかった）: 前の回は捨てて始め直す
        if (voicePhase_ != VoicePhase::Idle) CancelVoiceInput();
        ++voiceGeneration_;
        voicePhase_ = VoicePhase::Waiting;
        voicePartial_.reset();
        voiceMessage_.clear();
        voiceCompare_.reset();
        const std::u32string error = host_->RequestVoiceStart(voiceGeneration_);
        if (!error.empty()) {
            EndVoice();
            voiceMessage_ = U"こえ: " + error;
        }
        return;
    }
    case SessionKey::VoiceStop:
        if (voicePhase_ != VoicePhase::Waiting && voicePhase_ != VoicePhase::Recording) return;
        // 録音を始める前に離した（キーを単独で軽く押しただけ）なら、変換サーバが Discarded を返す
        if (voicePhase_ == VoicePhase::Recording && !voicePartial_) voiceMessage_ = U"… 認識しています";
        voicePhase_ = VoicePhase::Finishing;
        host_->RequestVoiceStop(voiceGeneration_);
        return;
    case SessionKey::VoiceCancel:
        if (voicePhase_ == VoicePhase::Waiting || voicePhase_ == VoicePhase::Recording) CancelVoiceInput();
        return;
    default:
        return;
    }
}

void InputSession::CancelVoiceInput() {
    host_->CancelVoice();
    ++voiceGeneration_;
    EndVoice();
}

void InputSession::EndVoice() {
    voicePhase_ = VoicePhase::Idle;
    voicePartial_.reset();
    voiceMessage_.clear();
}

bool InputSession::HandleVoiceEvent(uint64_t generation, const VoiceEvent& event) {
    if (generation != voiceGeneration_ || voicePhase_ == VoicePhase::Idle) return false;
    switch (event.kind) {
    case VoiceEvent::Kind::Started:
        if (voicePhase_ == VoicePhase::Waiting) voicePhase_ = VoicePhase::Recording;
        if (voicePhase_ == VoicePhase::Recording && !voicePartial_) voiceMessage_ = U"● 聞いています";
        break;
    case VoiceEvent::Kind::Partial:
        ShowVoicePartial(event.text);
        break;
    case VoiceEvent::Kind::Discarded:
        EndVoice();
        break;
    case VoiceEvent::Kind::Failed:
        EndVoice();
        voiceMessage_ = U"こえ: " + event.text;
        break;
    case VoiceEvent::Kind::Finished:
        EndVoice();
        InsertVoiceTranscript(event.tokens, event.text);
        break;
    }
    ++revision_;
    return true;
}

// 話している間の途中結果を、未確定文字列の末尾に足して見せる（macOS版 showVoicePartial）。
// 文節変換中・AI変換の結果待ちは見せられないので、末尾を小窓に出す
void InputSession::ShowVoicePartial(const std::u32string& text) {
    std::u32string surface;
    for (char32_t c : text) {
        if (c != U' ' && c != 0x3000 && c != U'\t' && c != U'\r' && c != U'\n') surface += c;
    }
    if (surface.empty()) return;
    if (state_.mode == Mode::Composing && !translation_) {
        if (!voicePartial_) {
            CloseCommitUndo();
            prediction_.reset();
            EndTypoUndo();
        }
        voicePartial_ = surface;
        voiceMessage_.clear();
        return;
    }
    // 小窓に出す途中結果の長さ（末尾から。macOS版 partialLength）
    constexpr size_t kPartialLength = 28;
    std::vector<std::u32string> graphemes = SplitGraphemes(surface);
    std::u32string shown;
    if (graphemes.size() > kPartialLength) {
        shown = U"…";
        graphemes.erase(graphemes.begin(), graphemes.end() - kPartialLength);
    }
    for (const std::u32string& g : graphemes) shown += g;
    voiceMessage_ = (voicePhase_ == VoicePhase::Recording ? U"● " : U"… ") + shown;
}

// 認識結果を読みに戻して合成の末尾に入れる（確定はしない。macOS版 insertVoiceTranscript・applyVoiceTranscript）。
// 読みの部分は今の読みに続けてライブ変換に任せ、英字・数字など読みに戻さない部分は表記のまま固定部分にする。
// 固定部分の前にある読みは、その時点の左文脈で変換して固定する（固定部分の後ろの変換の文脈にするため）
void InputSession::InsertVoiceTranscript(const std::vector<ReadingToken>& tokens, const std::u32string& recognizedText) {
    const std::u32string& style = settings_.punctuation;
    const VoiceTranscript transcript(tokens, style.size() >= 1 ? style.substr(0, 1) : U"、",
                                     style.size() >= 2 ? style.substr(1, 1) : U"。");
    if (transcript.Empty()) {
        voiceMessage_ = U"こえ: 聞き取れませんでした";
        return;
    }
    if (translation_) return;
    State& s = state_;
    // 文節変換中なら、そこまでを確定してから続きとして入れる
    if (s.mode == Mode::Segmenting) {
        std::u32string commit;
        CommitSegments(&commit);
        pendingCommit_ += commit;
    }
    CloseCommitUndo();
    EndAIFix();
    prediction_.reset();
    EndTypoUndo();
    // 合成が空のところへ入れたときだけ、あとで音声認識の表記と変換結果を比べる
    // （打った読みに続けたときは、どこからが声の分かを表示から切り分けられない）
    const bool compares = !StateComposing();
    // 次の 1 文字を打ったときと同じ準備: 英字入力を閉じ、F6 等の表示形を固定する
    FinishAlphabetRun();
    LockDisplayOverride();
    for (const VoiceTranscript::Part& part : transcript.parts) {
        s.composer.Flush();
        if (part.kind == VoiceTranscript::Part::Kind::Reading) {
            s.composer.ReplaceText(s.composer.Text() + part.text);
            continue;
        }
        const std::u32string reading = s.composer.Text();
        if (!reading.empty()) {
            std::u32string text = reading;
            if (s.lastConversion && s.lastConversion->first == reading && !s.showsKanaOnly) {
                text = s.lastConversion->second;
            } else {
                std::vector<std::u32string> results;
                std::string error;
                if (host_->Convert(reading, s.context + FixedText(), 1, &results, &error) && !results.empty() &&
                    !results.front().empty()) {
                    text = results.front();
                }
            }
            FixedChunk chunk;
            chunk.reading = reading;
            chunk.text = text;
            chunk.kind = FixedChunk::Kind::LiveConversion;
            s.fixedChunks.push_back(std::move(chunk));
            s.composer = MakeComposer();
            CancelLiveConversion();
            s.lastConversion.reset();
        }
        FixedChunk fixed;
        fixed.reading = part.text;
        fixed.text = part.text;
        fixed.kind = FixedChunk::Kind::Alphabet;
        s.fixedChunks.push_back(std::move(fixed));
    }
    s.compositionHasVoice = true;
    s.showsKanaOnly = false;
    // 最後の固定部分より後ろの読みが合成の読みのすべてなら、変換が届くまでその認識結果の表記を見せる
    {
        std::u32string trailingReading;
        std::u32string trailingSurface;
        for (const VoiceTranscript::Part& part : transcript.parts) {
            if (part.kind == VoiceTranscript::Part::Kind::Fixed) {
                trailingReading.clear();
                trailingSurface.clear();
            } else {
                trailingReading += part.text;
                trailingSurface += part.surface;
            }
        }
        if (!trailingReading.empty() && s.composer.Text() == trailingReading) {
            s.voiceSurface.emplace(trailingReading, trailingSurface);
        }
    }
    // 声で入れた読みは打ち間違いの訂正に通さない（打鍵の誤りを直すモデルで、認識の誤りは対象外）
    CancelTypoCorrection();
    typoRejectedReading_ = s.composer.Text();
    if (compares) {
        // 句読点は設定の組で読みに入れているので、比べる側もそろえる。空白は除く
        std::u32string recognized;
        for (char32_t c : recognizedText.empty() ? transcript.Surface() : recognizedText) {
            if (c != U' ' && c != 0x3000) recognized += c;
        }
        for (char32_t& c : recognized) {
            if (c == U'、' && style.size() >= 1) c = style[0];
            if (c == U'。' && style.size() >= 2) c = style[1];
        }
        voiceCompare_ = VoiceCompare{recognized, s.composer.Text()};
    }
    ComposerDidChange();
    TryVoiceComparison();
}

// 入れた発話の変換結果が出そろっていれば、音声認識の表記との違いを知らせに出す（macOS版 showVoiceComparison。
// 試作の間の確認用。変換結果はライブ変換の結果を使う）
void InputSession::TryVoiceComparison() {
    if (!voiceCompare_) return;
    const State& s = state_;
    if (s.mode != Mode::Composing || s.composer.Text() != voiceCompare_->reading) {
        voiceCompare_.reset();
        return;
    }
    std::u32string converted;
    if (!voiceCompare_->reading.empty()) {
        if (!s.lastConversion || s.lastConversion->first != voiceCompare_->reading) return; // まだ届いていない
        converted = s.lastConversion->second;
    }
    voiceMessage_ = VoiceComparison::Summary(voiceCompare_->recognized, FixedText() + converted);
    voiceCompare_.reset();
}

// ---- AIで直す ----

// 入力の休止を待って、表示しているライブ変換に誤変換の疑いがあるかを調べてもらう（macOS版 scheduleAIFixCheck）。
// 判定はモデル単体でもう一度貪欲に変換して得るマージンで行う。AI には送らない（送るのはキーを押したとき）
void InputSession::ScheduleAIFixCheck() {
    CancelAIFixCheck();
    const State& s = state_;
    if (!settings_.aiFix || !settings_.liveConversion || aiFixRequestPending_ || aiFixUndo_) return;
    const std::u32string reading = s.composer.Text();
    if (reading.empty() || GraphemeCount(reading) > AIFixRule::kMaximumReadingLength ||
        (aiFixRejectedReading_ && reading == *aiFixRejectedReading_)) {
        return;
    }
    aiFixCheckPending_ = true;
    host_->RequestAIFixCheck(aiFixGeneration_, reading, s.context + FixedText(), settings_.typoDelayMs);
}

void InputSession::CancelAIFixCheck() {
    ++aiFixGeneration_;
    if (aiFixCheckPending_) {
        aiFixCheckPending_ = false;
        host_->CancelAIFixCheck();
    }
}

// 今の未確定文字列を AI に送れないときの理由（窓に出す）。送れるなら空
std::u32string InputSession::AIFixBlocker(const std::u32string& reading) const {
    const State& s = state_;
    if (!settings_.aiFix || s.mode != Mode::Composing || translation_ || s.composer.Text() != reading) {
        return U"今の入力はAIに送れません";
    }
    if (!settings_.aiAvailable) return U"AIサービスが設定されていません（設定 > AI）";
    if (GraphemeCount(reading) > AIFixRule::kMaximumReadingLength) return U"80文字を超える読みは送れません";
    if (!s.fixedChunks.empty() || s.alphabetRun || s.displayOverride) {
        return U"固定した部分・英字・表示形の切り替えを含む入力は送れません";
    }
    if (!s.composer.Pending().empty() || !s.lastConversion || s.lastConversion->first != reading) {
        return U"変換が終わってから押してください";
    }
    return U"";
}

bool InputSession::CompleteAIFixCheck(uint64_t generation, const std::u32string& reading, const std::u32string& text,
                                      const std::vector<float>& margins) {
    if (!aiFixCheckPending_ || generation != aiFixGeneration_) return false;
    aiFixCheckPending_ = false;
    const State& s = state_;
    // 打ち間違いの訂正の知らせが出ている間は出さない（訂正の結果を先に見てもらう）
    if (AIFixBlocker(reading).empty() && !typoFeedback_ && s.lastConversion &&
        AIFixRule::IsSuspicious(text, margins, s.lastConversion->second, reading, settings_.aiFixThreshold)) {
        aiFixOffer_ = AIFixOffer{reading, s.context, s.lastConversion->second};
    }
    ++revision_;
    return true;
}

// 窓が出ている間のキーと、AI の案に差し替えた直後のキーを処理する。処理し終えたら真（キーはここで終わり）
bool InputSession::HandleAIFixKey(SessionKey key, bool shift) {
    if (aiFixUndo_) {
        const auto undo = *aiFixUndo_;
        aiFixUndo_.reset();
        aiFixFeedback_.reset();
        if (key == SessionKey::Backspace && !shift && UndoAIFix(undo)) return true;
    }
    if (key == SessionKey::AIFix) {
        // キーは窓が出ていなくても、入力中ならいつでも今の変換を送る
        const State& s = state_;
        if (!settings_.aiFix || s.mode != Mode::Composing || s.composer.Text().empty()) return true;
        if (aiFixRequestPending_) return true; // 確かめている間に押し直したときは何もしない
        if (!aiFixOffer_) {
            const std::u32string reading = s.composer.Text();
            CancelAIFix();
            const std::u32string reason = AIFixBlocker(reading);
            if (!reason.empty()) {
                aiFixMessage_ = reason;
                return true;
            }
            aiFixOffer_ = AIFixOffer{reading, s.context, s.lastConversion->second};
        }
        RequestAIFixNow();
        return true;
    }
    CancelAIFix();
    return false;
}

// 窓に出している変換（窓が出ていなければキーを押したときの変換）を AI に送る
void InputSession::RequestAIFixNow() {
    const State& s = state_;
    if (!aiFixOffer_ || !AIFixBlocker(aiFixOffer_->reading).empty() || !s.lastConversion ||
        s.lastConversion->second != aiFixOffer_->shown) {
        CancelAIFix();
        return;
    }
    const AIFixOffer offer = *aiFixOffer_;
    aiFixOffer_.reset();
    CancelAIFixCheck();
    aiFixMessage_ = U"AIで確認中…";
    aiFixRequestPending_ = true;
    aiFixRequested_ = offer;
    host_->RequestAIFix(aiFixGeneration_,
                        AIRequest{AIFixRule::kInstructions, AIFixRule::Message(offer.context, offer.reading, offer.shown)});
}

bool InputSession::CompleteAIFix(uint64_t generation, const AIOutcome& outcome) {
    if (!aiFixRequestPending_ || generation != aiFixGeneration_ || !aiFixRequested_) return false;
    aiFixRequestPending_ = false;
    const AIFixOffer offer = *aiFixRequested_;
    aiFixRequested_.reset();
    aiFixMessage_.clear();
    const State& s = state_;
    // 待っている間にキーが来ていれば番号が進んでここには来ないが、念のため状態も確かめる
    if (s.mode != Mode::Composing || s.composer.Text() != offer.reading || !s.lastConversion ||
        s.lastConversion->second != offer.shown) {
        ++revision_;
        return true;
    }
    if (!outcome.text) {
        std::u32string reason = Utf8ToUtf32(outcome.error);
        if (reason.size() > 40) reason = reason.substr(0, 40) + U"…";
        aiFixMessage_ = U"AIに頼めませんでした（" + reason + U"）";
    } else if (const auto answer = AIFixRule::Answer(*outcome.text, offer.shown); !answer) {
        aiFixMessage_ = U"AIの答えを使えませんでした";
    } else if (*answer == offer.shown) {
        aiFixMessage_ = U"AI: このままでよいようです";
    } else {
        // 文節に区切った未確定文字列にする（Enter で確定、Space で文節ごとに直せる。学習・記録に残さない）
        PresentAIResultAsSegments(*answer, offer.reading);
        aiFixUndo_.emplace(offer.reading, offer.shown);
        aiFixFeedback_ = AIFixRule::Feedback(offer.shown, *answer);
    }
    ++revision_;
    return true;
}

// AI の案に差し替えた直後の Backspace: 元のライブ変換の表示に戻す（取り消した読みは再び疑わない）
bool InputSession::UndoAIFix(const std::pair<std::u32string, std::u32string>& undo) {
    State& s = state_;
    if (s.mode != Mode::Segmenting || !s.segmentsFromAI || s.candidateListOpen || s.composer.Text() != undo.first) {
        return false;
    }
    s.mode = Mode::Composing;
    s.segments.clear();
    s.currentSegment = 0;
    s.segmentBaseline.reset();
    s.segmentEngineBaseline.reset();
    s.segmentsFromAI = false;
    s.lastConversion.emplace(undo.first, undo.second);
    s.showsKanaOnly = false;
    aiFixRejectedReading_ = undo.first;
    return true;
}

// 窓を閉じ、判定・依頼をやめる
void InputSession::CancelAIFix() {
    CancelAIFixCheck();
    if (aiFixRequestPending_) {
        aiFixRequestPending_ = false;
        host_->CancelAI();
    }
    aiFixRequested_.reset();
    aiFixOffer_.reset();
    aiFixMessage_.clear();
}

// 確定・入力の終わり: 窓も取り消しも閉じる
void InputSession::EndAIFix() {
    CancelAIFix();
    aiFixUndo_.reset();
    aiFixRejectedReading_.reset();
    aiFixFeedback_.reset();
}

// ---- 打ち間違いの訂正 ----

bool InputSession::TypoAccepts(const std::u32string& reading) const {
    return reading.size() >= static_cast<size_t>(std::max(1, settings_.typoMinimumLength)) &&
           reading.size() <= TypoNormalizer::kMaxReadingLength;
}

// 入力の休止を待って、読みそのものを直すよう頼む（macOS版 scheduleTypoCorrection）。
// 未解決のローマ字が残っている間・英字入力中・表示形の指定中は頼まない。直した直後の読みと、
// ユーザが取り消した読みも触らない（直し合いを起こさない）
void InputSession::ScheduleTypoCorrection() {
    CancelTypoCheck();
    const State& s = state_;
    if (!settings_.typoCorrection || s.mode != Mode::Composing || s.alphabetRun || s.displayOverride ||
        !s.composer.Pending().empty()) {
        return;
    }
    const std::u32string reading = s.composer.Text();
    if (!TypoAccepts(reading) || (lastTypoCorrection_ && reading == lastTypoCorrection_->second) ||
        (typoRejectedReading_ && reading == *typoRejectedReading_) || !host_->TypoCorrectionAvailable()) {
        return;
    }
    typoPending_ = true;
    host_->RequestTypoCorrection(typoGeneration_, reading, settings_.typoDelayMs, settings_.typoThreshold,
                                 static_cast<size_t>(std::max(1, settings_.typoMinimumLength)));
}

bool InputSession::CompleteTypoCorrection(uint64_t generation, const std::u32string& reading,
                                          const std::optional<TypoCorrection>& correction) {
    if (!typoPending_ || generation != typoGeneration_) return false;
    typoPending_ = false;
    typoChecked_.emplace(reading, correction);
    const State& s = state_;
    // 「読みの末尾に足しただけ」は訂正ではなく続きの補完。打ちかけの読みは常に終わりが足りなく見えるので捨てる
    if (correction && correction->reading == reading && !correction->IsTrailingInsertionOnly() &&
        s.mode == Mode::Composing && !s.alphabetRun && !s.displayOverride && s.composer.Text() == reading &&
        s.composer.Pending().empty()) {
        ApplyTypoCorrection(*correction);
    }
    ++revision_;
    return true;
}

// 読みを訂正後のものに差し替え、その読みで変換し直す
void InputSession::ApplyTypoCorrection(const TypoCorrection& correction) {
    State& s = state_;
    s.composer.ReplaceText(correction.corrected);
    s.showsKanaOnly = false;
    lastTypoCorrection_.emplace(correction.reading, correction.corrected);
    typoUndoAvailable_ = true;
    typoFeedback_ = TypoCorrectionFeedback::Make(correction);
    // 予測の候補と同じ小窓に出すので、候補は引っ込める
    prediction_.reset();
    ComposerDidChange();
}

// 直した直後の Backspace: 訂正を取り消して打った読みに戻す（取り消した読みは再び直さない）
bool InputSession::UndoTypoCorrection() {
    State& s = state_;
    if (!lastTypoCorrection_ || s.mode != Mode::Composing || s.composer.Text() != lastTypoCorrection_->second ||
        !s.composer.Pending().empty()) {
        return false;
    }
    s.composer.ReplaceText(lastTypoCorrection_->first);
    typoRejectedReading_ = lastTypoCorrection_->first;
    lastTypoCorrection_.reset();
    EndTypoUndo();
    prediction_.reset();
    ComposerDidChange();
    return true;
}

// Backspace で取り消せる状態を終える（小窓も閉じる）
void InputSession::EndTypoUndo() {
    typoUndoAvailable_ = false;
    typoFeedback_.reset();
}

// 休止を待っている訂正を捨てる
void InputSession::CancelTypoCheck() {
    ++typoGeneration_;
    if (typoPending_) {
        typoPending_ = false;
        host_->CancelTypoCorrection();
    }
}

// 訂正の状態をすべて捨てる（macOS版 cancelTypoCorrection）
void InputSession::CancelTypoCorrection() {
    CancelTypoCheck();
    lastTypoCorrection_.reset();
    EndTypoUndo();
    typoRejectedReading_.reset();
}

// 文節変換に入ったときの読みの訂正（初めて候補を開くときに 1 回だけ求める。休止で同じ読みを確かめてあれば使い回す）
std::optional<TypoCorrection> InputSession::TypoCorrectionForSegments() {
    State& s = state_;
    if (!s.typoReading) return std::nullopt;
    if (!s.typoResolved) {
        s.typoResolved = true;
        if (settings_.typoCorrection && TypoAccepts(*s.typoReading) && host_->TypoCorrectionAvailable()) {
            if (typoChecked_ && typoChecked_->first == *s.typoReading) {
                s.typoSegmentCorrection = typoChecked_->second;
            } else {
                s.typoSegmentCorrection = host_->CorrectTypo(*s.typoReading, settings_.typoThreshold,
                                                             static_cast<size_t>(std::max(1, settings_.typoMinimumLength)));
            }
        }
    }
    return s.typoSegmentCorrection;
}

// この文節に当たる訂正候補（訂正後の読みでの変換結果と、訂正後の読みそのもの）。
// 差分が文節境界をまたぐときは、文全体を訂正した候補を訂正対象の先頭の文節に出す（macOS版 typoCandidates）
std::vector<std::u32string> InputSession::TypoCandidates(size_t index) {
    const auto correction = TypoCorrectionForSegments();
    if (!correction) return {};
    State& s = state_;
    std::vector<std::u32string> readings;
    for (const Segment& segment : s.segments) readings.push_back(segment.reading);
    const std::u32string context = s.context + LeftResults(index);
    std::vector<std::u32string> converted;
    std::string error;
    const auto placement = correction->PlacementWithFixedPrefix(readings);
    if (!placement) {
        // typo のせいで文節の切り方そのものが崩れている（「さsてえいただいていて」→「さ|sて|えいただいていて」）
        const auto offset = correction->SegmentOffset(readings);
        if (!offset || *offset != index || readings.size() - *offset <= 1) return {};
        if (!host_->Convert(correction->corrected, context, 1, &converted, &error) || converted.empty() ||
            converted.front().empty()) {
            return {};
        }
        s.typoWholeSentence = State::TypoWholeSentence{converted.front(), correction->corrected, *offset};
        return {converted.front()};
    }
    if (placement->index != index) return {};
    std::vector<std::u32string> results;
    if (host_->Convert(placement->correctedReading, context, 1, &converted, &error) && !converted.empty() &&
        !converted.front().empty()) {
        results.push_back(converted.front());
    }
    // 訂正後の読みそのものも出す（ローマ字がかなにならずに残った打ち間違いは、ひらがなに直るだけで用が足りることがある）
    if (std::find(results.begin(), results.end(), placement->correctedReading) == results.end()) {
        results.push_back(placement->correctedReading);
    }
    return results;
}

bool InputSession::IsSelectingTypoWholeSentence() const {
    const State& s = state_;
    if (!s.typoWholeSentence || s.currentSegment != s.typoWholeSentence->segmentOffset ||
        s.currentSegment >= s.segments.size()) {
        return false;
    }
    return s.segments[s.currentSegment].result == s.typoWholeSentence->candidate;
}

// 「文全体を訂正した候補」を選んだまま候補ウィンドウを閉じたら、訂正対象の文節をまとめて差し替える
void InputSession::ApplyTypoWholeSentenceIfSelected() {
    State& s = state_;
    if (!IsSelectingTypoWholeSentence()) return;
    const State::TypoWholeSentence whole = *s.typoWholeSentence;
    std::vector<Segment> replacement = SegmentsFor(whole.reading, whole.candidate);
    if (replacement.empty()) return;
    // 訂正由来なので確定しても学習しない
    for (Segment& segment : replacement) segment.unlearnable.insert(segment.result);
    s.segments.resize(whole.segmentOffset);
    for (Segment& segment : replacement) s.segments.push_back(std::move(segment));
    s.currentSegment = std::min(whole.segmentOffset, s.segments.size() - 1);
    s.typoWholeSentence.reset();
}

void InputSession::CloseCandidateList() {
    ApplyTypoWholeSentenceIfSelected();
    state_.candidateListOpen = false;
    state_.candidatesExpanded = false;
}

// ---- 確定と取り消し ----

// 文字列を確定する。合成していたなら、確定する前の状態を持っておき、直後の Ctrl+Backspace で戻せるようにする
// （確定に伴う記録は、取り消せなくなったときに書く。macOS版 commitText と同じ）
void InputSession::CommitText(const std::u32string& text, std::u32string* commit) {
    std::vector<std::function<void()>> effects = std::move(pendingEffects_);
    pendingEffects_.clear();
    CloseCommitUndo();
    if (IsComposing() && !text.empty()) {
        commitUndo_ = CommitUndo{text, state_, std::move(effects)};
    } else {
        for (const auto& effect : effects) effect();
    }
    ResetState();
    // 次の合成でアプリのテキストを読めなかったときのために、確定した文字列を左文脈に足しておく
    state_.context = NormalizeLeftContext(state_.context + text);
    if (settings_.privateMode) state_.contextHasPrivateText = true;
    *commit += text;
}

// 確定を取り消せなくする。持っていた記録をここで書く
void InputSession::CloseCommitUndo() {
    std::vector<std::function<void()>> leftover = std::move(pendingEffects_);
    pendingEffects_.clear();
    for (const auto& effect : leftover) effect();
    if (!commitUndo_) return;
    const std::vector<std::function<void()>> effects = std::move(commitUndo_->effects);
    commitUndo_.reset();
    for (const auto& effect : effects) effect();
}

// 直前の確定を取り消し、確定する前の未確定文字列に戻す。取り消した確定は記録に残さない
bool InputSession::UndoCommit() {
    if (!commitUndo_ || IsComposing()) return false;
    state_ = commitUndo_->state;
    commitUndo_.reset();
    replacesCommitted_ = true;
    CancelLiveConversion();
    if (state_.mode == Mode::Composing) ComposerDidChange();
    return true;
}

// 合成の状態を捨てる（左文脈は残す）
void InputSession::ResetState() {
    const std::u32string context = state_.context;
    const bool contextHasPrivateText = state_.contextHasPrivateText;
    CancelLiveConversion();
    CancelTypoCorrection();
    CancelTranslation();
    EndAIFix();
    typoChecked_.reset();
    state_ = State();
    state_.composer = MakeComposer();
    state_.context = context;
    state_.contextHasPrivateText = contextHasPrivateText;
    prediction_.reset();
}

// ---- 表示 ----

SessionOutput InputSession::Current() const {
    const State& s = state_;
    SessionOutput output;
    output.revision = revision_;
    output.livePending = livePending_;
    output.fullWidthSpace = settings_.fullWidthSpace;
    output.privateMode = settings_.privateMode;
    output.notice = notice_;
    output.typoPending = typoPending_;
    output.aiShortcuts = settings_.aiShortcuts;
    output.voicePending = voicePhase_ != VoicePhase::Idle;
    output.voiceKey = settings_.voiceInput ? settings_.voiceKey : 0;
    output.voiceNotice = voiceMessage_;
    output.privateModeShortcut = settings_.privateModeShortcut;
    output.aiFixShortcut = settings_.aiFix ? settings_.aiFixShortcut : 0;
    output.aiFixPending = aiFixCheckPending_ || aiFixRequestPending_;
    if (aiFixOffer_ && s.mode == Mode::Composing) {
        output.aiFixNotice = U"AIで直す" +
                             (settings_.aiFixShortcutLabel.empty() ? U"" : U"（" + settings_.aiFixShortcutLabel + U"）");
    } else {
        output.aiFixNotice = aiFixMessage_;
    }
    if (aiFixUndo_) output.aiFixFeedback = aiFixFeedback_;
    if (translation_) {
        // 結果待ちの表示: 「日本語 ⇢ プリセットの名前（届いたら途中までの結果） …」
        output.aiPending = true;
        output.preedit.push_back({translation_->japanese + U" ⇢ " +
                                      (translation_->partial.empty() ? translation_->label : translation_->partial) +
                                      U" …",
                                  SessionOutput::kInput});
        return output;
    }
    if (typoUndoAvailable_ && s.mode == Mode::Composing) output.typoFeedback = typoFeedback_;
    if (commitUndo_) output.undoableCommit = commitUndo_->text;
    if (prediction_ && s.mode == Mode::Composing) {
        for (const InputHistoryEntry& entry : prediction_->candidates) output.predictions.push_back(entry.text);
        output.predictionIndex = prediction_->selected ? static_cast<int32_t>(*prediction_->selected) : -1;
    }
    if (s.mode == Mode::Segmenting) {
        output.segmenting = true;
        // 「文全体を訂正した候補」を選んでいる間は、その候補が後ろの文節ぶんまで含んでいるので後ろを隠す
        const size_t visible = IsSelectingTypoWholeSentence() ? s.currentSegment + 1 : s.segments.size();
        for (size_t i = 0; i < visible; ++i) {
            output.preedit.push_back({s.segments[i].result, i == s.currentSegment
                                                                ? SessionOutput::kSelectedSegment
                                                                : SessionOutput::kInput});
        }
        if (s.candidateListOpen && s.currentSegment < s.segments.size()) {
            const Segment& segment = s.segments[s.currentSegment];
            output.candidatesVisible = !segment.candidates.empty();
            output.candidates = segment.candidates;
            output.candidateAnnotations = segment.annotations;
            output.candidateIndex = static_cast<uint32_t>(segment.candidateIndex);
            output.candidatesExpanded = s.candidatesExpanded;
        }
        // 空の文節（打ち直しで消した途中）は出さない
        output.preedit.erase(std::remove_if(output.preedit.begin(), output.preedit.end(),
                                            [](const SessionOutput::Span& span) { return span.text.empty(); }),
                             output.preedit.end());
    } else if (StateComposing() || voicePartial_) {
        // 声の途中結果は、合成の表示の末尾に足して見せる
        const std::u32string display = (StateComposing() ? CompositionDisplay() : U"") + voicePartial_.value_or(U"");
        if (!display.empty()) output.preedit.push_back({display, SessionOutput::kInput});
    }
    return output;
}

} // namespace iroha
