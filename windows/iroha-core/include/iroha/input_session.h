#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "iroha/romaji_composer.h"
#include "iroha/symbol_candidates.h"

namespace iroha {

// TIP から届くキー。キーコードから意味への読み替え（修飾キー・配列）は TIP が行い、
// そのキーで何をするかはセッションが決める
enum class SessionKey : uint32_t {
    Character = 1, // 文字（character に入る）
    Enter = 2,
    Escape = 3,
    Backspace = 4,
    Space = 5,
    Up = 6,
    Down = 7,
    Left = 8,
    Right = 9,
    // 今の表示のまま確定する（入力モードの切替など、キー以外のきっかけ）
    CommitNow = 10,
    // 合成を捨てる（アプリの側で合成が終わった。表示中の文字列は文書に残っている）
    Reset = 11,
};

// キー 1 つを処理したあと（またはライブ変換が届いたあと）に TIP が画面へ反映する内容
struct SessionOutput {
    enum SpanStyle : uint32_t {
        kInput = 0,           // 入力中・変換済み（下線）
        kSelectedSegment = 1, // 選んでいる文節（太い下線）
    };
    struct Span {
        std::u32string text;
        uint32_t style = kInput;
        bool operator==(const Span& other) const {
            return text == other.text && style == other.style;
        }
    };

    // 状態の版。ライブ変換の到着など、キー以外で表示が変わったかを TIP が確かめるのに使う
    uint64_t revision = 0;
    // 確定して文書に入れる文字列（無ければ空）。未確定文字列より先に入れる
    std::u32string commit;
    // 未確定文字列（空なら合成なし）
    std::vector<Span> preedit;
    // 文節変換中か（矢印キーを受けるかの判断に TIP が使う）
    bool segmenting = false;
    // ライブ変換の結果待ちか（TIP は結果が届くまで問い合わせる）
    bool livePending = false;
    // 候補ウィンドウ（選んでいる文節の候補。全件を渡し、ページ分けは TIP が行う）
    bool candidatesVisible = false;
    std::vector<std::u32string> candidates;
    uint32_t candidateIndex = 0;

    bool Composing() const { return !preedit.empty(); }
    // 未確定文字列をつなげたもの
    std::u32string PreeditText() const;
};

// 入力の状態（読み・ライブ変換の結果・文節・候補）を持ち、キーごとに表示を決める。
// macOS版の IrohaInputController のうち、OS に依存しない部分にあたる
// （ライブ変換の表示と確定の規則は同じにしてある。文節変換は Windows 版のこれまでの動きを移したもの）。
//
// Windows版では変換サーバが TIP（アプリごと・スレッドごと）につき 1 つ持つ。TIP はキーを送り、
// 返ってきた SessionOutput を画面に反映するだけにする（TIP はアプリのプロセスの中で動くので、
// 状態と判断をそこに置かない）。
//
// スレッドセーフではない。呼び出し側で直列化すること
class InputSession {
public:
    // 変換と学習の呼び出し口（変換サーバが実装する。テストでは偽物を渡す）
    class Host {
    public:
        virtual ~Host() = default;
        // その場で変換する（Space を押したとき・候補ウィンドウを開くとき。戻るまで待ってよい）
        virtual bool Convert(const std::u32string& reading, const std::u32string& context,
                             int candidateCount, std::vector<std::u32string>* results,
                             std::string* error) = 0;
        // ライブ変換（第一候補）を頼む。すぐ戻ること。結果は CompleteLiveConversion に渡す。
        // 新しい依頼が来ても、前の依頼の結果は（読みが伸びただけなら）表示に使うので、
        // 始めた変換は最後まで行ってよい。まだ始めていない古い依頼は捨ててよい
        virtual void RequestLiveConversion(uint64_t generation, const std::u32string& reading,
                                           const std::u32string& context) = 0;
        // 頼んであるライブ変換がどれも要らなくなった（確定・取り消し・文字の削除・文節変換）
        virtual void CancelLiveConversion() {}
        // 文節変換で確定した（学習に使う）。baseline は文節変換に入ったときの全体の変換結果
        virtual void RecordCommit(const std::u32string& reading, const std::u32string& baseline,
                                  const std::u32string& committed) = 0;
    };

    explicit InputSession(Host* host);

    // キー 1 つを処理する。
    // leftContext は、合成していない状態でキーが届いたときの、カーソルより前の文書のテキスト
    // （合成の途中では渡さなくてよい。渡しても使わない）
    SessionOutput Handle(SessionKey key, char32_t character, bool shift,
                         const std::u32string* leftContext);

    // ライブ変換の結果を受け取る。表示が変わったら真を返す。
    // 最新の依頼の結果でなくても、その読みが今の読みの頭の部分なら使う
    // （「その変換結果 + 増えたかな」を出せる。変換が打鍵に追いつかないときも表示が進む）。
    // 要らなくなった依頼（CancelLiveConversion より前のもの）の結果は捨てる。
    // converted が nullptr なら変換できなかった（待ちだけを解く）
    bool CompleteLiveConversion(uint64_t generation, const std::u32string& reading,
                                const std::u32string* converted);

    // 今の表示（commit は空）
    SessionOutput Current() const;

    uint64_t Revision() const { return revision_; }
    bool IsComposing() const { return segmenting_ || !composer_.Empty(); }

    // ライブ変換を使うか（既定は使う。使わないときは Space を押すまでかなのまま）
    void SetLiveConversionEnabled(bool enabled) { liveConversionEnabled_ = enabled; }

    // 文節の候補ウィンドウに合流させる記号・顔文字・絵文字の表（nullptr なら合流させない）
    void SetSymbolTable(std::shared_ptr<const SymbolCandidateTable> table) {
        symbolTable_ = std::move(table);
    }

    // 文節の候補として求める数
    static constexpr int kSegmentCandidateCount = 9;
    // 候補ウィンドウの1ページの候補数（数字キー 1〜9 はページの中の番号。TIP の表示と同じにする）
    static constexpr size_t kCandidatePageSize = 9;
    // 左文脈として持つ最大文字数（モデルの学習設定に合わせる。macOS版の LeftContext.maxLength）。
    // 確定した文字列は左文脈の末尾に足しておき、次の合成でアプリのテキストを読めなかったときに使う
    // （macOS版の recentCommitted にあたる）
    static constexpr size_t kMaxContextLength = 40;

    // アプリから読んだテキストを左文脈の形にする: 最後の改行より後ろ（カーソルのある行）だけを残し、
    // 行頭の空白を除いて末尾 kMaxContextLength 文字にする（macOS版の LeftContext.normalize）
    static std::u32string NormalizeLeftContext(const std::u32string& text);

private:
    struct Segment {
        std::u32string reading;
        std::u32string result;                  // 今選んでいる変換結果
        std::vector<std::u32string> candidates; // 未取得なら空
        size_t candidateIndex = 0;
        // 選んで確定しても学習しない候補（記号・顔文字・絵文字）
        std::set<std::u32string> unlearnable;
    };

    void HandleComposing(SessionKey key, char32_t character, std::u32string* commit);
    void HandleSegmenting(SessionKey key, char32_t character, bool shift, std::u32string* commit);

    void ComposerDidChange();
    void CancelLiveConversion();
    std::u32string CompositionDisplay() const;
    std::u32string ResolveCommitText();
    void ResetState();

    void EnterSegmentMode();
    std::u32string CommitSegments();
    bool EnsureSegmentCandidates(size_t index);
    void CycleCandidate(int delta);
    void SelectCandidateOnPage(size_t indexOnPage);
    void MoveSegment(int delta);
    void ResizeSegment(int delta);
    std::u32string JoinedResults() const;
    std::u32string LeftResults(size_t index) const;

    Host* host_;
    bool liveConversionEnabled_ = true;
    std::shared_ptr<const SymbolCandidateTable> symbolTable_;
    uint64_t revision_ = 0;

    // 合成（かな入力・ライブ変換）
    RomajiComposer composer_;
    // 合成を始めたときの左文脈
    std::u32string context_;
    // 直近のライブ変換（読み, 変換結果）
    std::optional<std::pair<std::u32string, std::u32string>> lastConversion_;
    // 文字を消したあと、次の文字を打つまで変換結果を見せずかなで出す
    bool showsKanaOnly_ = false;
    // ライブ変換の依頼の番号（頼むたびに増やす）と、この番号以下の依頼の結果は受け取らないという線
    uint64_t liveGeneration_ = 0;
    uint64_t liveValidAfter_ = 0;
    // 最新の依頼の結果を待っているか、その依頼の読み
    bool livePending_ = false;
    std::u32string liveRequestedReading_;

    // 文節変換
    bool segmenting_ = false;
    std::vector<Segment> segments_;
    size_t currentSegment_ = 0;
    bool candidateListOpen_ = false;
    // 学習用: 文節変換に入ったときの読み全体と、そのとき表示していた変換結果
    std::u32string conversionReading_;
    std::u32string conversionBaseline_;
};

} // namespace iroha
