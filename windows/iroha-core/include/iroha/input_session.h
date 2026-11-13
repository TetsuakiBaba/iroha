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

#include "iroha/ai_conversion.h"
#include "iroha/ai_fix.h"
#include "iroha/conversion_log.h"
#include "iroha/input_history.h"
#include "iroha/romaji_composer.h"
#include "iroha/symbol_candidates.h"
#include "iroha/term_dictionary.h"
#include "iroha/typo_normalizer.h"
#include "iroha/user_rewrite_rule.h"
#include "iroha/voice_transcript.h"

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
    // Tab（予測変換の候補を選ぶ。Shift+Tab は前の候補）
    Tab = 19,
    // プライベートモードの切り替え（Ctrl+Alt+P）
    TogglePrivateMode = 20,
    // AI変換（character にプリセットの番号。TIP は SessionOutput::aiShortcuts のキーで送る）
    AICommit = 21,
    // AIで直す（今の変換を AI に確かめさせる。TIP は SessionOutput::aiFixShortcut のキーで送る）
    AIFix = 22,
    // 「こえ」モード: 決めた修飾キーを押した・離した・押している間にほかのキーが来た（TIP は SessionOutput::voiceKey のキーで送る）
    VoiceStart = 23,
    VoiceStop = 24,
    VoiceCancel = 25,
};
constexpr uint32_t kMaxSessionKey = static_cast<uint32_t>(SessionKey::VoiceCancel);

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
    // 候補ウィンドウを、9 件ずつの列を横に並べた表に広げているか（Tab。macOS版の CandidateGrid）
    bool candidatesExpanded = false;
    // 直前の確定で、まだ取り消せるもの（無ければ空）。TIP は Ctrl+Backspace のとき、カーソルの手前が
    // この文字列のままなら UndoCommit を送る
    std::u32string undoableCommit;
    // UndoCommit で、確定した文字列（カーソルの手前の undoableCommit）を未確定文字列に置き換えるか
    bool replacesCommitted = false;
    // 合成していないときの Space を全角スペースとして受けるか（設定。Shift+Space は常に半角でアプリに任せる）
    bool fullWidthSpace = true;
    // 予測変換の候補（入力履歴から。無ければ空）と、Tab で選んでいる位置（-1 なら選んでいない）
    std::vector<std::u32string> predictions;
    int32_t predictionIndex = -1;
    // プライベートモードか
    bool privateMode = false;
    // カーソルの近くに少しのあいだ出す知らせ（句読点・プライベートモードの切り替え。無ければ空）
    std::u32string notice;
    // 入力の休止で打ち間違いを確かめている最中か（TIP は結果が届くまで問い合わせる）
    bool typoPending = false;
    // 打ち間違いを直したときの「打った読み → 直した読み」（小窓に出す。出ている間は Backspace で戻せる）
    std::optional<TypoCorrectionFeedback> typoFeedback;
    // AI変換の結果待ちか（TIP は結果が届くまで問い合わせ、Esc 以外のキーを飲み込む）
    bool aiPending = false;
    // AI変換のプリセットのキー（プリセットの番号の順。値は KeyShortcut::Encode、0 なら割り当てなし）
    std::vector<uint32_t> aiShortcuts;
    // AIで直す: 判定・依頼の結果待ちか（TIP は届くまで問い合わせる）・キー（KeyShortcut::Encode。0 なら使わない）・
    // カーソルの近くに出す文（「AIで直す（Ctrl+Alt+A）」・「AIで確認中…」など。無ければ空）・
    // AI の案に差し替えたときの「今の変換 → AI の案」（出ている間は Backspace で戻せる）
    bool aiFixPending = false;
    uint32_t aiFixShortcut = 0;
    std::u32string aiFixNotice;
    std::optional<TypoCorrectionFeedback> aiFixFeedback;
    // 「こえ」モード: 録音・認識の最中か（TIP は終わるまで問い合わせる。合成が無くても）・押している間録音するキー
    // （仮想キーコード。0 なら使わない）・カーソルの近くに出す文（「● 聞いています」・音声認識との違いなど。無ければ空）
    bool voicePending = false;
    uint32_t voiceKey = 0;
    std::u32string voiceNotice;
    // プライベートモードを切り替えるキー（KeyShortcut::Encode。0 なら割り当てなし）
    uint32_t privateModeShortcut = 0;

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
    // 予測変換（入力履歴から読みの先頭が一致する語を出す。入力履歴もこれが ON の間だけ記録する）
    bool predictive = false;
    // プライベートモード（学習・入力履歴・予測変換・変換記録を止める。ユーザ辞書は変換サーバが止める）
    bool privateMode = false;
    // 変換記録（学習用データ。既定 OFF）と、何を記録するか
    bool conversionLog = false;
    ConversionLogScope conversionLogScope = ConversionLogScope::All;
    // 打ち間違いの訂正（既定 OFF。macOS版 TypoNormalizerSettings と同じ既定値）: 採用する margin のしきい値・
    // 入力が止まってから確かめるまでの時間・訂正する読みの最低文字数
    bool typoCorrection = false;
    double typoThreshold = TypoNormalizer::kDefaultThreshold;
    int typoDelayMs = 500;
    int typoMinimumLength = 4;
    // AI変換: 使えるか（接続先が決まっているか）・プリセットの名前（結果待ちの表示）・キー（TIP に渡す）
    bool aiAvailable = false;
    std::vector<std::u32string> aiPresetNames;
    std::vector<uint32_t> aiShortcuts;
    // 区切り記号で囲んだ英字入力（macOS版の DelimitedAlphabetSettings。既定 OFF）: かな入力のまま、開始の記号から
    // 終わりの記号までを打ったとおりの英字にする（「kyouha_iPhone_wo」→「今日はiPhoneを」）
    bool delimitedAlphabet = false;
    char32_t delimitedStart = U'_';
    char32_t delimitedEnd = U'_';
    // AIで直す（macOS版の AIFixSettings。既定 OFF）: 疑いの閾値・キー（TIP に渡す）・窓に出すキーの名前
    bool aiFix = false;
    double aiFixThreshold = AIFixRule::kDefaultThreshold;
    uint32_t aiFixShortcut = 0;
    std::u32string aiFixShortcutLabel;
    // 「こえ」モード（macOS版の VoiceInputSettings。既定 OFF）: 押している間録音するキー（仮想キーコード。TIP に渡す）
    bool voiceInput = false;
    uint32_t voiceKey = 0;
    // アプリの文章（TIP が送る左文脈）を使うか（OFF なら直前に確定した文字列だけを文脈にする）
    bool documentContext = true;
    // プライベートモードを切り替えるキー（TIP に渡す。KeyShortcut::Encode。0 なら割り当てなし）
    uint32_t privateModeShortcut = 0;
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
        // 読み（文節全体）の中に一致する、ユーザ辞書の候補ウィンドウ専用の語（それを含む候補は学習しない）
        virtual std::vector<std::u32string> CandidateOnlyWords(const std::u32string& reading) {
            (void)reading;
            return {};
        }
        // 予測変換の候補（入力履歴から、読みが prefix で始まり、それより長い語）
        virtual std::vector<InputHistoryEntry> PredictionCandidates(const std::u32string& prefix) {
            (void)prefix;
            return {};
        }
        // 確定した「読み → 表記」を入力履歴に記録する
        virtual void RecordInputHistory(const std::u32string& reading, const std::u32string& text) {
            (void)reading;
            (void)text;
        }
        // プライベートモードを切り替えた（設定に保存して、ほかのセッションと変換エンジンにも効かせる）
        virtual void PrivateModeChanged(bool enabled) { (void)enabled; }
        // 確定を変換記録に書く（モデルの名前は変換サーバが入れる）
        virtual void RecordConversion(const ConversionLogEntry& entry) { (void)entry; }
        // 打ち間違いの訂正モデルが使えるか（入っているか）
        virtual bool TypoCorrectionAvailable() { return false; }
        // 入力の休止（delayMs）を待って、読みの訂正を裏で頼む。すぐ戻ること。結果は CompleteTypoCorrection に渡す。
        // 同じセッションの前の依頼は捨ててよい
        virtual void RequestTypoCorrection(uint64_t generation, const std::u32string& reading, int delayMs,
                                           double threshold, size_t minimumLength) {
            (void)generation;
            (void)reading;
            (void)delayMs;
            (void)threshold;
            (void)minimumLength;
        }
        // 頼んである訂正が要らなくなった
        virtual void CancelTypoCorrection() {}
        // AI変換を裏で頼む（preset 番目のプリセットのプロンプトで text を）。すぐ戻ること。
        // 途中経過は UpdateAIPartial、結果は CompleteAI に渡す
        virtual void RequestAI(uint64_t generation, size_t preset, const std::u32string& text) {
            (void)generation;
            (void)preset;
            (void)text;
        }
        // 頼んである AI変換・AIで直すの依頼が要らなくなった
        virtual void CancelAI() {}
        // AIで直す: 入力の休止（delayMs）を待って、モデル単体の変換と文字ごとのマージンを裏で求める。
        // 結果は CompleteAIFixCheck に渡す。同じセッションの前の依頼は捨ててよい
        virtual void RequestAIFixCheck(uint64_t generation, const std::u32string& reading, const std::u32string& context,
                                       int delayMs) {
            (void)generation;
            (void)reading;
            (void)context;
            (void)delayMs;
        }
        virtual void CancelAIFixCheck() {}
        // AIで直す: AI に依頼する。結果は CompleteAIFix に渡す
        virtual void RequestAIFix(uint64_t generation, const AIRequest& request) {
            (void)generation;
            (void)request;
        }
        // 「こえ」モード: 録音と認識を始める（すぐ戻ること）。経過と結果は HandleVoiceEvent に渡す（終わりの知らせ
        // Finished・Failed・Discarded のどれかを必ず 1 回）。始められなければ理由を返す（空なら始めた）
        virtual std::u32string RequestVoiceStart(uint64_t generation) {
            (void)generation;
            return U"音声認識を使えません";
        }
        // 録音をやめて、そこまでの音声の認識を終える
        virtual void RequestVoiceStop(uint64_t generation) { (void)generation; }
        // 録音と認識をやめる（結果は要らない）
        virtual void CancelVoice() {}
        // その場で訂正する（Space で文節の候補を開くとき。戻るまで待ってよい）
        virtual std::optional<TypoCorrection> CorrectTypo(const std::u32string& reading, double threshold,
                                                          size_t minimumLength) {
            (void)reading;
            (void)threshold;
            (void)minimumLength;
            return std::nullopt;
        }
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

    // 裏で頼んだ打ち間違いの訂正の結果を受け取る（訂正が無ければ correction は無し）。表示が変わったら真。
    // 読みが変わっていれば（入力が進んでいれば）使わない
    bool CompleteTypoCorrection(uint64_t generation, const std::u32string& reading,
                                const std::optional<TypoCorrection>& correction);

    // AI変換の途中経過（累積の出力）・結果（失敗なら無し）を受け取る。表示が変わったら真。
    // 結果が日本語なら文節に区切った未確定文字列にし、そうでなければ（英訳など・失敗）確定する
    bool UpdateAIPartial(uint64_t generation, const std::u32string& partial);
    bool CompleteAI(uint64_t generation, const std::optional<std::u32string>& output);

    // AIで直す: 休止で求めたモデル単体の変換と文字ごとのマージン（求められなければ text は空）・AI の答えを受け取る。
    // 表示が変わったら真
    bool CompleteAIFixCheck(uint64_t generation, const std::u32string& reading, const std::u32string& text,
                            const std::vector<float>& margins);
    bool CompleteAIFix(uint64_t generation, const AIOutcome& outcome);

    // 「こえ」モードの経過と結果（Host::RequestVoiceStart で頼んだもの）
    struct VoiceEvent {
        enum class Kind {
            Started,   // マイクを開いて録音を始めた
            Partial,   // 途中結果（text に、ここまでの認識結果の表記）
            Finished,  // 認識が終わった（tokens に語ごとの表記と読み。空なら聞き取れなかった）
            Failed,    // 失敗した（text に理由）
            Discarded, // 押していた時間が短すぎた・取り消された（何も入れない）
        };
        Kind kind = Kind::Finished;
        std::u32string text;
        std::vector<ReadingToken> tokens;
    };
    // 表示が変わったら真。認識結果は読みに戻して未確定文字列の末尾に入れる（確定はしない）
    bool HandleVoiceEvent(uint64_t generation, const VoiceEvent& event);

    // 問い合わせへの返事（今の表示と、キーの外で確定した文字列。確定した文字列は 1 回だけ渡す）
    SessionOutput TakePolledOutput();

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
    // 文節の候補ウィンドウに合流させる変換ルール（nullptr なら合流させない）
    void SetRewriteRules(std::shared_ptr<const UserRewriteRuleSet> rules) {
        rewriteRules_ = std::move(rules);
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
            Prediction,     // 予測変換で入れた入力履歴の語（読みは履歴の読み。かなには戻さない）
        };
        std::u32string reading; // 元の読み（Backspace・Esc でかなに戻すときに使う）
        std::u32string text;    // 固定した表示
        Kind kind = Kind::Display;
        std::optional<RejectedLearning> rejectedLearning;
        // Prediction で、候補を入れる前に打っていた読み（Backspace・Esc でこの読みに戻す）
        std::u32string typedReading;
        // Alphabet で、区切り記号で入れた英字か（Backspace で開き直すとき、区切り記号の英字入力に戻す）
        bool delimited = false;
    };

    // 予測変換: 出している入力履歴の候補（reading は候補を引いたときの読み）と、Tab で選んでいる候補
    struct Prediction {
        std::u32string reading;
        std::vector<InputHistoryEntry> candidates;
        std::optional<size_t> selected;
    };

    struct Segment {
        std::u32string reading;
        std::u32string result;                  // 今選んでいる変換結果
        bool candidatesFetched = false;
        std::vector<std::u32string> candidates; // 取得済みの候補
        size_t candidateIndex = 0;
        // 選んで確定しても学習しない候補（変換ルールの出力・候補ウィンドウ専用の語を含む候補・記号・顔文字・絵文字）
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
        // 今の英字入力が区切り記号で始めたものか（真の間は Shift を離しても英字が続く）
        bool alphabetRunDelimited = false;
        std::optional<std::u32string> displayOverride;
        std::optional<RejectedLearning> displayOverrideRejected;
        // 直近のライブ変換（読み, 変換結果）
        std::optional<std::pair<std::u32string, std::u32string>> lastConversion;
        // 文字を消したあと、次の文字を打つまで変換結果を見せずかなで出す
        bool showsKanaOnly = false;
        std::vector<Segment> segments;
        size_t currentSegment = 0;
        bool candidateListOpen = false;
        // 候補ウィンドウを表に広げているか（閉じたら 1 列に戻す）
        bool candidatesExpanded = false;
        // 文節変換に入ったときの全体の変換結果と、そのうち学習が差し替える前のエンジンの結果
        std::optional<std::u32string> segmentBaseline;
        std::optional<std::u32string> segmentEngineBaseline;
        // 文節列の先頭のうち、固定部分から作った文節の数
        size_t segmentFixedCount = 0;
        // 打ち直した文節があるか（学習しない）
        bool segmentsRetyped = false;
        // 文節列が AI変換の結果から作ったものか（学習にも変換記録にも入力履歴にも残さない。読みと対応しないため）
        bool segmentsFromAI = false;
        // 声で入れた読みを含むか（学習・変換記録・入力履歴に残さない。読みは認識結果から求め直したもので、打ったものではない）
        bool compositionHasVoice = false;
        // 声で入れた直後の読みと、その認識結果の表記。ライブ変換が届くまで、かなではなくこれを見せる（漢字 → かな → 漢字と
        // 表示が揺れないように）。読みが変わったら使わない
        std::optional<std::pair<std::u32string, std::u32string>> voiceSurface;
        std::optional<SegmentEdit> segmentEdit;
        // 合成を始めたときの左文脈と、それをアプリの文書から読んだか（変換記録の contextSource）
        std::u32string context;
        bool contextFromDocument = false;
        // 左文脈（確定した文字列を足したもの）に、プライベートモード中に確定した文字列が入っているか
        bool contextHasPrivateText = false;
        // 文節変換に入ったときの読み（打ち間違いの訂正候補を出す対象）と、その訂正（初めて候補を開くときに求める）
        std::optional<std::u32string> typoReading;
        bool typoResolved = false;
        std::optional<TypoCorrection> typoSegmentCorrection;
        // 差分が文節境界をまたぐ訂正: 文全体を訂正した候補を、訂正対象の先頭の文節に出してある。
        // 選んだまま候補ウィンドウを閉じたら、その文節から後ろを置き換える（macOS版 typoWholeSentence）
        struct TypoWholeSentence {
            std::u32string candidate;
            std::u32string reading;
            size_t segmentOffset = 0;
        };
        std::optional<TypoWholeSentence> typoWholeSentence;
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
    std::optional<std::u32string> ResolveCommitText(bool logging = true);
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
    // 区切り記号で囲んだ英字入力（macOS版 startDelimitedAlphabet・finishDelimitedAlphabet）
    void StartDelimitedAlphabet();
    void FinishDelimitedAlphabet();
    // 英字入力中の表示（区切り記号の英字入力なら開始の記号を前に付ける）
    std::u32string AlphabetDisplay() const;

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

    // 予測変換・入力履歴
    void RefreshPrediction();
    // 予測変換の小窓のキー。処理し終えたら真（キーを飲み込む）。選んだ候補を入れて、キーは普段どおりに処理させるなら偽
    bool HandlePredictionKey(SessionKey key, bool shift);
    void ApplyPrediction(InputHistoryEntry entry);
    void UndoLastPrediction();
    void RecordInputHistory(const std::optional<std::u32string>& composerText);
    bool PredictionActive() const { return settings_.predictive && !settings_.privateMode; }

    // 打ち間違いの訂正（macOS版 scheduleTypoCorrection・applyTypoCorrection・undoTypoCorrection など）
    void ScheduleTypoCorrection();
    void ApplyTypoCorrection(const TypoCorrection& correction);
    bool UndoTypoCorrection();
    void EndTypoUndo();
    void CancelTypoCheck();
    void CancelTypoCorrection();
    bool TypoAccepts(const std::u32string& reading) const;
    std::optional<TypoCorrection> TypoCorrectionForSegments();
    std::vector<std::u32string> TypoCandidates(size_t index);
    bool IsSelectingTypoWholeSentence() const;
    void ApplyTypoWholeSentenceIfSelected();
    // 候補ウィンドウを閉じる（文全体を訂正した候補を選んでいれば、文節を置き換える）
    void CloseCandidateList();

    // AI変換（macOS版 handleAICommit・presentAIResultAsSegments・cancelTranslation）
    void StartAICommit(size_t preset, std::u32string* commit);
    std::optional<size_t> ShiftSpaceAIPreset(SessionKey key, bool shift) const;
    // AIで直す（macOS版 scheduleAIFixCheck・handleAIFixKey・requestAIFix・finishAIFix・undoAIFix）
    struct AIFixOffer {
        std::u32string reading;
        std::u32string context;
        std::u32string shown;
    };
    void ScheduleAIFixCheck();
    void CancelAIFixCheck();
    std::u32string AIFixBlocker(const std::u32string& reading) const;
    bool HandleAIFixKey(SessionKey key, bool shift);
    void RequestAIFixNow();
    bool UndoAIFix(const std::pair<std::u32string, std::u32string>& undo);
    void CancelAIFix();
    void EndAIFix();
    void PresentAIResultAsSegments(const std::u32string& output, const std::u32string& reading);
    void CancelTranslation();

    // 「こえ」モード（macOS版 VoiceInput・insertVoiceTranscript・showVoicePartial・showVoiceComparison）
    static bool IsVoiceKey(SessionKey key) {
        return key == SessionKey::VoiceStart || key == SessionKey::VoiceStop || key == SessionKey::VoiceCancel;
    }
    void HandleVoiceKey(SessionKey key);
    void CancelVoiceInput();
    void EndVoice();
    void ShowVoicePartial(const std::u32string& text);
    // recognized は認識結果の表記（数字への置き換えの後。音声認識との違いを比べる相手。空なら語の表記をつなげたもの）
    void InsertVoiceTranscript(const std::vector<ReadingToken>& tokens, const std::u32string& recognized);
    void TryVoiceComparison();

    // 確定と取り消し
    void LearnIfCorrected(const std::u32string& committed);
    // 変換記録（macOS版 logSegmentsCommit / logConversion）
    void LogSegmentsCommit(const std::u32string& committed);
    void LogConversion(ConversionLogEntry::Mode mode, const std::u32string& context, const std::u32string& reading,
                       const std::optional<std::u32string>& proposed, const std::u32string& committed,
                       std::optional<std::vector<ConversionLogEntry::Segment>> segments = std::nullopt);
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
    std::shared_ptr<const UserRewriteRuleSet> rewriteRules_;

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
    std::optional<Prediction> prediction_;
    // このキーで出す知らせ
    std::u32string notice_;

    // 打ち間違いの訂正: 休止を待っている依頼の番号・直した「打った読み → 直した読み」・直後の Backspace で戻せるか・
    // ユーザが取り消した読み（再び直さない）・小窓に出す割り付け・休止で確かめた結果（Space の訂正候補に使い回す）
    uint64_t typoGeneration_ = 0;
    bool typoPending_ = false;
    std::optional<std::pair<std::u32string, std::u32string>> lastTypoCorrection_;
    bool typoUndoAvailable_ = false;
    std::optional<std::u32string> typoRejectedReading_;
    std::optional<TypoCorrectionFeedback> typoFeedback_;
    std::optional<std::pair<std::u32string, std::optional<TypoCorrection>>> typoChecked_;

    // AI変換の結果待ち（確定する日本語・プリセットの名前・途中経過・結果を文節に区切るための読み）
    struct Translation {
        uint64_t generation = 0;
        std::u32string japanese;
        std::u32string label;
        std::u32string partial;
        std::u32string reading;
    };
    std::optional<Translation> translation_;
    uint64_t aiGeneration_ = 0;
    // キーの外（AI変換の結果）で確定した文字列。次の返事で TIP に渡す
    std::u32string pendingCommit_;

    // AIで直す: 依頼の番号・休止の判定を待っているか・AI の答えを待っているか・出している窓・送った変換・
    // 差し替えの取り消し（読み, 元の変換）・取り消した読み（再び疑わない）・窓に出す知らせ・「今 → 案」
    uint64_t aiFixGeneration_ = 0;
    bool aiFixCheckPending_ = false;
    bool aiFixRequestPending_ = false;
    std::optional<AIFixOffer> aiFixOffer_;
    std::optional<AIFixOffer> aiFixRequested_;
    std::optional<std::pair<std::u32string, std::u32string>> aiFixUndo_;
    std::optional<std::u32string> aiFixRejectedReading_;
    std::u32string aiFixMessage_;
    std::optional<TypoCorrectionFeedback> aiFixFeedback_;

    // 「こえ」モード: キーを押してから録音を始めるまで（Waiting）・録音中・離して認識の結果を待っている間。
    // 番号は押すたびに進める（古い回の知らせを捨てる）。途中結果は未確定文字列の末尾に足して見せる（合成の状態には入れない）。
    // 入れたあと、音声認識の表記と変換結果を比べるために、比べる表記と読みを持っておく（ライブ変換が届いたら比べる）
    enum class VoicePhase { Idle, Waiting, Recording, Finishing };
    VoicePhase voicePhase_ = VoicePhase::Idle;
    uint64_t voiceGeneration_ = 0;
    std::optional<std::u32string> voicePartial_;
    std::u32string voiceMessage_;
    struct VoiceCompare {
        std::u32string recognized;
        std::u32string reading;
    };
    std::optional<VoiceCompare> voiceCompare_;
};

} // namespace iroha
