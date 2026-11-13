#include "iroha/input_session.h"

#include <algorithm>

#include "iroha/reading_aligner.h"

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

} // namespace

std::u32string SessionOutput::PreeditText() const {
    std::u32string text;
    for (const Span& span : preedit) text += span.text;
    return text;
}

InputSession::InputSession(Host* host) : host_(host) {}

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

SessionOutput InputSession::Handle(SessionKey key, char32_t character, bool shift,
                                   const std::u32string* leftContext) {
    if (leftContext && !IsComposing()) context_ = NormalizeLeftContext(*leftContext);

    std::u32string commit;
    switch (key) {
    case SessionKey::Reset:
        ResetState();
        break;
    case SessionKey::CommitNow:
        if (segmenting_) {
            commit = CommitSegments();
        } else if (!composer_.Empty()) {
            commit = ResolveCommitText();
            ResetState();
        }
        break;
    default:
        if (segmenting_) {
            HandleSegmenting(key, character, shift, &commit);
        } else {
            HandleComposing(key, character, &commit);
        }
        break;
    }
    ++revision_;
    SessionOutput output = Current();
    output.commit = std::move(commit);
    return output;
}

// ---- 入力・ライブ変換中 ----

void InputSession::HandleComposing(SessionKey key, char32_t character, std::u32string* commit) {
    switch (key) {
    case SessionKey::Character:
        composer_.Input(character);
        showsKanaOnly_ = false;
        ComposerDidChange();
        break;
    case SessionKey::Enter:
        if (composer_.Empty()) break;
        *commit = ResolveCommitText();
        ResetState();
        break;
    case SessionKey::Backspace:
        if (composer_.Empty()) break;
        composer_.DeleteBackward();
        if (composer_.Empty()) {
            ResetState();
            break;
        }
        // 削除中はライブ変換を止めてかな表示に戻す。漢字のままだと1かな消しただけで
        // 文節構造が変わり、どこを消したのか分からなくなるため。
        // 次の1文字を入力した時点でライブ変換が再開する（そのままEnterならかなで確定）
        CancelLiveConversion();
        showsKanaOnly_ = true;
        break;
    case SessionKey::Escape:
        if (composer_.Empty()) break;
        if (lastConversion_ && !showsKanaOnly_) {
            // 1回目のEsc: 変換をやめてかな表示にする
            lastConversion_.reset();
            CancelLiveConversion();
        } else {
            // 2回目のEsc: 入力自体を取り消す
            ResetState();
        }
        break;
    case SessionKey::Space:
        if (composer_.Empty()) break;
        EnterSegmentMode();
        break;
    default:
        break;
    }
}

// 入力内容が変わった: ライブ変換を頼む（結果は後から届く）
void InputSession::ComposerDidChange() {
    const std::u32string& reading = composer_.Text();
    if (!liveConversionEnabled_ || reading.empty()) {
        CancelLiveConversion();
        return;
    }
    // 変換済みの読みと同じなら再変換不要
    if (lastConversion_ && lastConversion_->first == reading) {
        CancelLiveConversion();
        return;
    }
    ++liveGeneration_;
    livePending_ = true;
    host_->RequestLiveConversion(liveGeneration_, reading, context_);
}

void InputSession::CancelLiveConversion() {
    // 版を進めて、頼んである変換の結果を受け取らないようにする
    ++liveGeneration_;
    livePending_ = false;
}

bool InputSession::CompleteLiveConversion(uint64_t generation, const std::u32string& reading,
                                          const std::u32string* converted) {
    if (generation != liveGeneration_ || !livePending_) return false;
    livePending_ = false;
    if (converted && !converted->empty()) lastConversion_.emplace(reading, *converted);
    ++revision_;
    return true;
}

// 現在表示すべき未確定文字列（変換結果があればそれ、なければかな）+ 未解決ローマ字。
//
// ちらつき防止: 入力が進んで読みが伸びた場合も、前回の変換結果を接頭辞として
// 使い続け、新しく増えたかなだけを末尾に足す（新しい変換結果が届いたら置き換わる）
std::u32string InputSession::CompositionDisplay() const {
    if (lastConversion_ && !showsKanaOnly_) {
        const std::u32string& reading = lastConversion_->first;
        const std::u32string& text = composer_.Text();
        if (reading == text) return lastConversion_->second + composer_.Pending();
        if (StartsWith(text, reading)) {
            return lastConversion_->second + text.substr(reading.size()) + composer_.Pending();
        }
    }
    return composer_.Display();
}

// 確定で文書に入る文字列（表示と同じ規則。未解決ローマ字は確定する: "n" → "ん"）。
// macOS版は、文字を消した直後でも変換済みの読みと一致すれば変換結果を確定するが、
// ここでは表示（かな）のとおりに確定する
std::u32string InputSession::ResolveCommitText() {
    const std::u32string readingBeforeFlush = composer_.Text();
    composer_.Flush();
    const std::u32string flushedSuffix = composer_.Text().substr(readingBeforeFlush.size());
    if (lastConversion_ && !showsKanaOnly_) {
        const std::u32string& reading = lastConversion_->first;
        if (reading == readingBeforeFlush) return lastConversion_->second + flushedSuffix;
        if (StartsWith(readingBeforeFlush, reading)) {
            // 変換が追いつく前の確定: 表示と同じく「変換済み接頭辞 + 追加のかな」を確定する
            return lastConversion_->second + readingBeforeFlush.substr(reading.size()) +
                   flushedSuffix;
        }
    }
    return composer_.Display();
}

void InputSession::ResetState() {
    composer_.Clear();
    lastConversion_.reset();
    showsKanaOnly_ = false;
    CancelLiveConversion();
    segmenting_ = false;
    segments_.clear();
    currentSegment_ = 0;
    candidateListOpen_ = false;
    conversionReading_.clear();
    conversionBaseline_.clear();
}

// ---- 文節変換 ----

// Space: 読み全体を変換して文節に分け、文節変換に入る
void InputSession::EnterSegmentMode() {
    composer_.Flush();
    const std::u32string reading = composer_.Text();
    if (reading.empty()) return;
    CancelLiveConversion();

    // ライブ変換が同じ読みを変換済みならその結果を使う
    std::u32string conversion;
    if (lastConversion_ && lastConversion_->first == reading) {
        conversion = lastConversion_->second;
    } else {
        std::vector<std::u32string> results;
        std::string error;
        if (!host_->Convert(reading, context_, 1, &results, &error) || results.empty()) {
            // 変換できないときは読みのまま表示を続ける（Enterでかな確定できる）
            return;
        }
        conversion = results.front();
        lastConversion_.emplace(reading, conversion);
    }
    showsKanaOnly_ = false;
    conversionReading_ = reading;
    conversionBaseline_ = conversion;
    segments_.clear();
    for (const auto& segment : ReadingAligner::SegmentReading(reading, conversion)) {
        segments_.push_back({segment.reading, segment.conversion, {}, 0});
    }
    currentSegment_ = 0;
    candidateListOpen_ = false;
    segmenting_ = !segments_.empty();
}

void InputSession::HandleSegmenting(SessionKey key, char32_t character, bool shift,
                                    std::u32string* commit) {
    switch (key) {
    case SessionKey::Enter:
        *commit = CommitSegments();
        break;
    case SessionKey::Escape:
    case SessionKey::Backspace:
        // 変換をやめて読み（かな）に戻る
        segmenting_ = false;
        segments_.clear();
        currentSegment_ = 0;
        candidateListOpen_ = false;
        showsKanaOnly_ = true;
        break;
    case SessionKey::Space:
        CycleCandidate(shift ? -1 : +1);
        break;
    case SessionKey::Down:
        CycleCandidate(+1);
        break;
    case SessionKey::Up:
        CycleCandidate(-1);
        break;
    case SessionKey::Left:
        if (shift) {
            ResizeSegment(-1);
        } else {
            MoveSegment(-1);
        }
        break;
    case SessionKey::Right:
        if (shift) {
            ResizeSegment(+1);
        } else {
            MoveSegment(+1);
        }
        break;
    case SessionKey::Character:
        if (candidateListOpen_ && character >= U'1' && character <= U'9') {
            SelectCandidateOnPage(static_cast<size_t>(character - U'1'));
            break;
        }
        // 全文節を確定して、その文字から次の入力を始める。
        // 次の入力の左文脈は、確定した文字列を足したもの
        {
            const std::u32string previousContext = context_;
            *commit = CommitSegments();
            context_ = NormalizeLeftContext(previousContext + *commit);
        }
        composer_.Input(character);
        ComposerDidChange();
        break;
    default:
        break;
    }
}

// 全文節の変換結果を結合して確定する
std::u32string InputSession::CommitSegments() {
    const std::u32string committed = JoinedResults();
    if (!conversionReading_.empty() && !committed.empty()) {
        host_->RecordCommit(conversionReading_, conversionBaseline_, committed);
    }
    ResetState();
    return committed;
}

// 文節の候補は「選択中の文節の読み + 左側の変換結果を文脈」で求める
bool InputSession::EnsureSegmentCandidates(size_t index) {
    if (index >= segments_.size()) return false;
    Segment& segment = segments_[index];
    if (!segment.candidates.empty()) return true;
    std::vector<std::u32string> candidates;
    std::string error;
    if (!host_->Convert(segment.reading, context_ + LeftResults(index), kSegmentCandidateCount,
                        &candidates, &error) ||
        candidates.empty()) {
        return false;
    }
    // 現在の結果（全体変換由来）が一覧に無ければ先頭に足す
    auto it = std::find(candidates.begin(), candidates.end(), segment.result);
    if (it == candidates.end()) {
        candidates.insert(candidates.begin(), segment.result);
        it = candidates.begin();
    }
    segment.candidateIndex = static_cast<size_t>(it - candidates.begin());
    segment.candidates = std::move(candidates);
    return true;
}

void InputSession::CycleCandidate(int delta) {
    if (segments_.empty() || !EnsureSegmentCandidates(currentSegment_)) return;
    Segment& segment = segments_[currentSegment_];
    const size_t count = segment.candidates.size();
    segment.candidateIndex = (segment.candidateIndex + count + delta) % count;
    segment.result = segment.candidates[segment.candidateIndex];
    candidateListOpen_ = true;
}

void InputSession::SelectCandidateOnPage(size_t indexOnPage) {
    if (segments_.empty() || !candidateListOpen_) return;
    Segment& segment = segments_[currentSegment_];
    const size_t index =
        segment.candidateIndex / kCandidatePageSize * kCandidatePageSize + indexOnPage;
    if (index >= segment.candidates.size()) return;
    segment.candidateIndex = index;
    segment.result = segment.candidates[index];
}

void InputSession::MoveSegment(int delta) {
    if (segments_.empty()) return;
    size_t next = currentSegment_;
    if (delta < 0 && next > 0) --next;
    if (delta > 0 && next + 1 < segments_.size()) ++next;
    if (next == currentSegment_) return;
    currentSegment_ = next;
    candidateListOpen_ = false;
}

// 選んでいる文節の読みを1文字伸ばす・縮める。その文節と後ろを変換し直す
void InputSession::ResizeSegment(int delta) {
    if (segments_.empty()) return;
    std::u32string currentReading = segments_[currentSegment_].reading;
    std::u32string remainder;
    for (size_t i = currentSegment_ + 1; i < segments_.size(); ++i) {
        remainder += segments_[i].reading;
    }
    if (delta > 0) { // 伸ばす: 次の読みの先頭1文字を取り込む
        if (remainder.empty()) return;
        currentReading.push_back(remainder.front());
        remainder.erase(0, 1);
    } else { // 縮める: 末尾1文字を残りへ返す
        if (currentReading.size() <= 1) return;
        remainder.insert(remainder.begin(), currentReading.back());
        currentReading.pop_back();
    }

    const std::u32string left = LeftResults(currentSegment_);
    std::vector<std::u32string> converted;
    std::string error;
    if (!host_->Convert(currentReading, context_ + left, 1, &converted, &error) ||
        converted.empty()) {
        return; // 変換できないときは何もしない
    }

    std::vector<Segment> rebuilt(segments_.begin(), segments_.begin() + currentSegment_);
    rebuilt.push_back({currentReading, converted.front(), {}, 0});
    if (!remainder.empty()) {
        // 残りを変換し直して文節分割もやり直す
        std::vector<std::u32string> rest;
        if (host_->Convert(remainder, context_ + left + converted.front(), 1, &rest, &error) &&
            !rest.empty()) {
            for (const auto& segment : ReadingAligner::SegmentReading(remainder, rest.front())) {
                rebuilt.push_back({segment.reading, segment.conversion, {}, 0});
            }
        } else {
            rebuilt.push_back({remainder, remainder, {}, 0});
        }
    }
    segments_ = std::move(rebuilt);
    candidateListOpen_ = false;
}

std::u32string InputSession::JoinedResults() const {
    std::u32string joined;
    for (const Segment& segment : segments_) joined += segment.result;
    return joined;
}

std::u32string InputSession::LeftResults(size_t index) const {
    std::u32string joined;
    for (size_t i = 0; i < index && i < segments_.size(); ++i) joined += segments_[i].result;
    return joined;
}

// ---- 表示 ----

SessionOutput InputSession::Current() const {
    SessionOutput output;
    output.revision = revision_;
    output.livePending = livePending_;
    if (segmenting_) {
        output.segmenting = true;
        for (size_t i = 0; i < segments_.size(); ++i) {
            output.preedit.push_back({segments_[i].result, i == currentSegment_
                                                               ? SessionOutput::kSelectedSegment
                                                               : SessionOutput::kInput});
        }
        if (candidateListOpen_ && currentSegment_ < segments_.size()) {
            const Segment& segment = segments_[currentSegment_];
            output.candidatesVisible = !segment.candidates.empty();
            output.candidates = segment.candidates;
            output.candidateIndex = static_cast<uint32_t>(segment.candidateIndex);
        }
    } else if (!composer_.Empty()) {
        output.preedit.push_back({CompositionDisplay(), SessionOutput::kInput});
    }
    return output;
}

} // namespace iroha
