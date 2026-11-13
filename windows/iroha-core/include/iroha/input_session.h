#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "iroha/romaji_composer.h"
#include "iroha/symbol_candidates.h"
#include "iroha/term_dictionary.h"

namespace iroha {

// TIP から届くキー。キーコードから意味への読み替え（修飾キー・配列）は TIP が行い、
// そのキーで何をするかはセッションが決める
enum class SessionKey : uint32_t {
    Character = 1, // 文字（character に入る。Shift は shift に入る）
    Enter = 2,
    Escape = 3,
    Backspace = 4,
    Space = 5,
    Up = 6,
    Down = 7,
    Left = 8,
    Right = 9,
    // 今の表示のまま確定する（入力モードの切替や、IME が扱わないキーの前など、キー以外のきっかけ）
    CommitNow = 10,
    // 合成を捨てる（アプリの側で合成が終わった。表示中の文字列は文書に残っている）
    Reset = 11,
    // 表示形の指定（F6〜F10 と Ctrl+U/I/O/P/T）
    DisplayHiragana = 12,
    DisplayKatakana = 13,
    DisplayHalfKatakana = 14,
    DisplayFullAlphanumeric = 15,
    DisplayHalfAlphanumeric = 16,
    // 句読点の切り替え（、。⇄，．。Ctrl+.）
    TogglePunctuation = 17,
    // 直前の確定を取り消して、確定する前の未確定文字列に戻す（Ctrl+Backspace。TIP は、カーソルの手前が
    // 確定した文字列のまま（SessionOutput::undoableCommit）であることを確かめてから送る）
    UndoCommit = 18,
};
constexpr uint32_t kMaxSessionKey = static_cast<uint32_t>(SessionKey::UndoCommit);

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
    // 変換の結果待ちか（TIP は結果が届くまで問い合わせる）
    bool livePending = false;
    // 候補ウィンドウ（選んでいる文節の候補。全件を渡し、ページ分けは TIP が行う）
    bool candidatesVisible = false;
    std::vector<std::u32string> candidates;
    uint32_t candidateIndex = 0;
    // 候補の右に小さく出す印（専門辞書の名前。候補と同じ数か空。印の無い候補は空文字列）
    std::vector<std::u32string> candidateAnnotations;
    // 直前の確定で、まだ取り消せるもの（無ければ空）。TIP は Ctrl+Backspace のとき、カーソルの手前が
    // この文字列のままなら UndoCommit を送る
    std::u32string undoableCommit;
    // UndoCommit で、確定した文字列（カーソルの手前の undoableCommit）を未確定文字列に置き換えるか
    bool replacesCommitted = false;
    // 合成していないときの Space を全角スペースとして受けるか（設定。Shift+Space は常に半角でアプリに任せる）
    bool fullWidthSpace = true;

    bool Composing() const { return !preedit.empty(); }
    // 未確定文字列をつなげたもの
    std::u32string PreeditText() const;
};

// セッションの設定（変換サーバが config.json から作る）
struct SessionSettings {
    // ライブ変換を使うか（使わないときは Space を押すまでかなのまま）
    bool liveConversion = true;
    // 句読点（「,」「.」キーで入れる文字の組。"、。" か "，．"）
    std::u32string punctuation = U"、。";
    // 合成していないときの Space を全角にするか
    bool fullWidthSpace = true;
    // 文節の候補として求める数（macOS版の candidateCount。3〜16）
    int candidateCount = 8;
};

// 入力の状態（読み・ライブ変換の結果・固定部分・英字入力・文節・候補）を持ち、キーごとに表示を決める。
// macOS版の IrohaInputController のうち、OS に依存しない部分を移したもの
// （キーの扱い・表示と確定の規則は macOS版と同じにしてある。違うところは各関数のコメント）。
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
        // 変換（第一候補）を裏で頼む。すぐ戻ること。結果は CompleteLiveConversion に渡す。
        // 新しい依頼が来ても、前の依頼の結果は（読みが伸びただけなら）表示に使うので、
        // 始めた変換は最後まで行ってよい。まだ始めていない古い依頼は捨ててよい
        virtual void RequestLiveConversion(uint64_t generation, const std::u32string& reading,
                                           const std::u32string& context) = 0;
        // 頼んである変換がどれも要らなくなった（確定・取り消し・文字の削除・文節変換）
        virtual void CancelLiveConversion() {}
        // 文節変換で確定した（学習に使う）。baseline は文節変換に入ったときの全体の変換結果、
        // engineBaseline はそのうち学習が差し替える前のエンジンの結果（差し替えていなければ baseline と同じ）
        virtual void RecordCommit(const std::u32string& reading, const std::u32string& engineBaseline,
                                  const std::u32string& baseline, const std::u32string& committed) = 0;
        // 直近の第一候補の変換で reading に shown を返したとき、学習が差し替える前のエンジンの結果
        // （差し替えていなければ nullopt）
        virtual std::optional<std::u32string> EngineResult(const std::u32string& reading,
                                                           const std::u32string& shown) {
            (void)reading;
            (void)shown;
            return std::nullopt;
        }
        // 学習を消す（学習で差し替えた表示を、表示形の指定で退けて確定した）
        virtual void ForgetLearning(const std::u32string& reading, const std::u32string& engineResult,
                                    const std::u32string& shown) {
            (void)reading;
            (void)engineResult;
            (void)shown;
        }
        // 句読点の組を切り替えた（設定に保存して、ほかのセッションにも効かせる）
        virtual void PunctuationChanged(const std::u32string& punctuation) { (void)punctuation; }
    };

    explicit InputSession(Host* host);

    // キー 1 つを処理する。
    // leftContext は、合成していない状態でキーが届いたときの、カーソルより前の文書のテキスト
    // （合成の途中では渡さなくてよい。渡しても使わない）
    SessionOutput Handle(SessionKey key, char32_t character, bool shift,
                         const std::u32string* leftContext);

    // 裏で頼んだ変換の結果を受け取る。表示が変わったら真を返す。
    // ライブ変換は、最新の依頼の結果でなくても、その読みが今の読みの頭の部分なら使う
    // （「その変換結果 + 増えたかな」を出せる。変換が打鍵に追いつかないときも表示が進む）。
    // 要らなくなった依頼（CancelLiveConversion より前のもの）の結果は捨てる。
    // converted が nullptr なら変換できなかった（待ちだけを解く）
    bool CompleteLiveConversion(uint64_t generation, const std::u32string& reading,
                                const std::u32string* converted);

    // 今の表示（commit は空）
    SessionOutput Current() const;

    uint64_t Revision() const { return revision_; }
    bool IsComposing() const;

    void SetSettings(const SessionSettings& settings);

    // 取り消せる間持っていた確定の記録（学習）を書く（セッションを捨てる前に呼ぶ）
    void Flush() { CloseCommitUndo(); }
    // ライブ変換を使うか（既定は使う）
    void SetLiveConversionEnabled(bool enabled) { settings_.liveConversion = enabled; }

    // 文節の候補ウィンドウに合流させる記号・顔文字・絵文字の表（nullptr なら合流させない）
    void SetSymbolTable(std::shared_ptr<const SymbolCandidateTable> table) {
        symbolTable_ = std::move(table);
    }
    // 候補に専門辞書の名前の印を付けるための表（nullptr なら付けない）
    void SetTermLookup(std::shared_ptr<const TermDictionaryLookup> lookup) {
        termLookup_ = std::move(lookup);
    }

    // 候補ウィンドウの1ページの候補数（数字キー 1〜9 はページの中の番号。TIP の表示と同じにする）
    static constexpr size_t kCandidatePageSize = 9;
    // 左文脈として持つ最大文字数（モデルの学習設定に合わせる。macOS版の LeftContext.maxLength）。
    // 確定した文字列は左文脈の末尾に足しておき、次の合成でアプリのテキストを読めなかったときに使う
    // （macOS版の recentCommitted にあたる）
    static constexpr size_t kMaxContextLength = 40;

    // アプリから読んだテキストを左文脈の形にする: 最後の改行より後ろ（カーソルのある行）だけを残し、
    // 行頭の空白を除いて末尾 kMaxContextLength 文字にする（macOS版の LeftContext.normalize）
    static std::u32string NormalizeLeftContext(const std::u32string& text);

    // 英字入力の文節の候補: 打鍵通り / 小文字 / 大文字 / 先頭だけ大文字 / 全角（macOS版と同じ）
    static std::vector<std::u32string> AlphabetCandidates(const std::u32string& text);

private:
    enum class Mode { Composing, Segmenting };

    // 学習で差し替えた表示を、表示形の指定で退けたときの記録。退けた表示形のまま確定したら、その学習を消す
    struct RejectedLearning {
        std::u32string reading;
        std::u32string engineResult;
        std::u32string shown;
    };

    // F6-F10 などで表示形を指定した部分・Shift+英字で固定した部分。
    // 指定した見た目のまま未確定文字列の先頭に残し、以降の入力ではライブ変換の対象にしない
    struct FixedChunk {
        enum class Kind {
            Display,        // F6-F10 / Ctrl+U…T で指定した表示形
            LiveConversion, // Shift+英字で英字入力を始めた時点のライブ変換結果で固定したかな
            Alphabet,       // Shift+英字で入力した英字（読みも英字そのまま。かなには戻さない）
        };
        std::u32string reading; // 元の読み（Backspace・Esc でかなに戻すときに使う）
        std::u32string text;    // 固定した表示
        Kind kind = Kind::Display;
        std::optional<RejectedLearning> rejectedLearning;
    };

    struct Segment {
        std::u32string reading;
        std::u32string result;                  // 今選んでいる変換結果
        bool candidatesFetched = false;
        std::vector<std::u32string> candidates; // 取得済みの候補
        size_t candidateIndex = 0;
        // 選んで確定しても学習しない候補（記号・顔文字・絵文字）
        std::set<std::u32string> unlearnable;
        // 候補ごとの印（専門辞書の名前。candidates と同じ数か空）
        std::vector<std::u32string> annotations;
    };

    // 文節変換中に 1 文節を打ち直している状態（macOS版の SegmentEdit）
    struct SegmentEdit {
        RomajiComposer composer;
        size_t scopeStart = 0; // 変換し直す範囲の始まり（これより前の文節は変えない）
        std::vector<Segment> originalSegments;
        size_t originalIndex = 0;
        std::vector<Segment> before; // 範囲の中で、打ち直す文節の前にあった文節
        std::vector<Segment> after;  // 後ろにあった文節
        std::vector<Segment> outside; // 範囲より後ろの文節（変えない）
        bool splits = false;          // 範囲を文節に分け直すか
        // 最後に届いた変換（そのときの打ち直しの読み・範囲の文節列・打ち直しの終わりを含む文節・その文節の終わりが
        // 読みの終わりと一致するか）
        struct Shown {
            std::u32string editReading;
            std::vector<Segment> segments;
            size_t current = 0;
            bool aligned = false;
        };
        std::optional<Shown> shown;
    };

    // 確定の取り消しのために持っておく状態（キーの処理で書き換わるものすべて）
    struct State {
        Mode mode = Mode::Composing;
        RomajiComposer composer;
        std::vector<FixedChunk> fixedChunks;
        std::optional<std::u32string> alphabetRun;
        std::optional<std::u32string> displayOverride;
        std::optional<RejectedLearning> displayOverrideRejected;
        // 直近のライブ変換（読み, 変換結果）
        std::optional<std::pair<std::u32string, std::u32string>> lastConversion;
        // 文字を消したあと、次の文字を打つまで変換結果を見せずかなで出す
        bool showsKanaOnly = false;
        std::vector<Segment> segments;
        size_t currentSegment = 0;
        bool candidateListOpen = false;
        // 文節変換に入ったときの全体の変換結果と、そのうち学習が差し替える前のエンジンの結果
        std::optional<std::u32string> segmentBaseline;
        std::optional<std::u32string> segmentEngineBaseline;
        // 文節列の先頭のうち、固定部分から作った文節の数
        size_t segmentFixedCount = 0;
        // 打ち直した文節があるか（学習しない）
        bool segmentsRetyped = false;
        std::optional<SegmentEdit> segmentEdit;
        // 合成を始めたときの左文脈
        std::u32string context;
    };

    // 裏で頼んだ変換の種類
    struct Request {
        enum class Kind { Live, SegmentEdit };
        Kind kind = Kind::Live;
        // SegmentEdit: 打ち直しの範囲の始まり・打ち直しの読み・範囲の読みの中での打ち直しの終わり・分け直すか
        size_t scopeStart = 0;
        std::u32string editReading;
        size_t editEnd = 0;
        bool splits = false;
    };

    struct CommitUndo {
        std::u32string text;
        State state;
        std::vector<std::function<void()>> effects;
    };

    RomajiComposer MakeComposer() const;
    bool StateComposing() const;

    void HandleComposing(SessionKey key, char32_t character, bool shift, std::u32string* commit);
    void HandleSegmenting(SessionKey key, char32_t character, bool shift, std::u32string* commit);

    // 入力・ライブ変換
    void InputCharacter(char32_t character);
    void ComposerDidChange();
    void CancelLiveConversion();
    std::u32string FixedText() const;
    std::u32string CompositionDisplay() const;
    std::u32string KanaDisplay() const;
    std::optional<std::u32string> ResolveCommitText();
    void ApplyDisplayConversion(SessionKey key);
    std::optional<RejectedLearning> RejectedLearningFor(const std::u32string& reading,
                                                        const std::u32string& text);
    void ForgetRejectedLearnings();
    void LockDisplayOverride();
    void UnlockLastFixedChunk();
    bool UnlockFixedChunks();

    // 英字入力（Shift+英字）
    static bool StartsAlphabetRun(char32_t character);
    void InputAlphabet(char32_t character);
    void LockComposerWithLiveConversion();
    void FinishAlphabetRun();
    void DeleteAlphabetBackward();
    void EndAlphabetRun();

    // 文節変換
    void EnterSegmentMode();
    void ExitSegmentModeToKana();
    void CommitSegments(std::u32string* commit);
    bool EnsureSegmentCandidates(size_t index);
    void OpenSegmentCandidates();
    void CycleCandidate(int delta);
    void ChooseCandidateOnPage(size_t indexOnPage);
    void MoveSegment(int delta);
    void ResizeSegment(int delta);
    std::u32string JoinedResults() const;
    std::u32string LeftResults(size_t index) const;
    std::vector<Segment> SegmentsFor(const std::u32string& reading,
                                     const std::u32string& conversion) const;

    // 文節の打ち直し
    void StartSegmentEdit(std::optional<char32_t> character);
    // 処理したら真。打ち直しを終えて、いつもの文節変換のキーとして処理を続けるなら偽
    bool HandleSegmentEditKey(SessionKey key, char32_t character, bool shift);
    void EndSegmentModeIfEmptied();
    struct EditSpan {
        std::vector<Segment> segments;
        size_t current = 0;
        bool exact = false;
    };
    EditSpan SegmentEditSpan(const SegmentEdit& edit) const;
    void ShowSegmentEdit();
    void SegmentEditDidChange();
    void CancelSegmentEdit();
    void FinishSegmentEdit(bool reconverts);
    void ReconvertRetypedSpan(size_t start, size_t count, bool splits);
    void SyncCompositionWithRetypedSegment(size_t index, bool removed);
    static bool IsAlphabetText(const std::u32string& text);

    // 確定と取り消し
    void LearnIfCorrected(const std::u32string& committed);
    void CommitText(const std::u32string& text, std::u32string* commit);
    void CloseCommitUndo();
    bool UndoCommit();
    void ResetState();

    // 裏で頼む変換
    void RequestConversion(const Request& request, const std::u32string& reading,
                           const std::u32string& context);

    Host* host_;
    SessionSettings settings_;
    uint64_t revision_ = 0;
    std::shared_ptr<const SymbolCandidateTable> symbolTable_;
    std::shared_ptr<const TermDictionaryLookup> termLookup_;

    State state_;
    // 裏で頼んだ変換（番号 → 種類）。liveValidAfter_ 以下の番号の結果は受け取らない
    std::map<uint64_t, Request> requests_;
    uint64_t requestGeneration_ = 0;
    uint64_t liveValidAfter_ = 0;
    // 最新の依頼の結果を待っているか、その依頼の読み（ライブ変換）
    bool livePending_ = false;
    std::u32string liveRequestedReading_;

    // 確定に伴う記録（学習）。取り消せる間は書かずに持っておき、取り消せなくなったときに書く
    std::vector<std::function<void()>> pendingEffects_;
    std::optional<CommitUndo> commitUndo_;
    // 直前のキーが UndoCommit で、確定した文字列を未確定文字列に置き換える
    bool replacesCommitted_ = false;
};

} // namespace iroha
