#pragma once
// IME本体と同じ構成のエンジン一式と、セッション（SessionManager）から見た呼び出し口。
// Windows版の変換サーバ（iroha-server/main.cpp の ServerState / ServerBackend）を移したもの。
// IBus のエンジンと iroha-cli の session が使う。
//
// 構成: 学習 → ユーザ辞書 → 長い読みの区切り → 異体字 → 辞書ラティスの候補をモデルで再採点 → モデル。
// 候補を複数求められたとき（候補ウィンドウ）は辞書ラティスで候補を作ってモデルで並べ、
// 1つだけのとき（全体の変換）はモデルの生成に任せる。辞書が無ければモデル単体で動く。
// Reload で設定・辞書を読み直す（モデルは変わったときだけ読み込む）
#include <array>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "ai_client.h"
#include "input_menu.h"
#include "iroha/ai_conversion.h"
#include "iroha/config.h"
#include "iroha/conversion_log.h"
#include "iroha/engine_assembly.h"
#include "iroha/input_history.h"
#include "iroha/lattice_converter.h"
#include "iroha/learning_engine.h"
#include "iroha/learning_store.h"
#include "iroha/session_manager.h"
#include "iroha/symbol_candidates.h"
#include "iroha/term_dictionary.h"
#include "iroha/typo_normalizer.h"
#include "iroha/user_dictionary_store.h"
#include "iroha/user_rewrite_rule.h"
#include "iroha/zenz_engine.h"

namespace iroha {

class EngineHost : public SessionManager::Backend {
public:
    using Logger = std::function<void(const std::string&)>;

    struct Options {
        // 動作の記録（既定は標準エラー）
        Logger log;
        // 描けない絵文字を記号の表から除く判定（nullptr なら除かない）
        SymbolCandidateTable::RenderablePredicate isRenderableEmoji;
        // 辞書ラティスの辞書（Dictionary）・記号のデータ（Symbols）を探すフォルダ。空なら実行ファイルのフォルダ
        // （fcitx5 のエンジンは fcitx5 本体のプロセスの中で動くので、インストール先を渡す）
        std::filesystem::path resourceDirectory;
    };

    explicit EngineHost(Options options = {});
    ~EngineHost() override;

    // 設定・辞書を読み（直し）てエンジンを組み立てる。セッションが動いているときは
    // SessionManager::WithEngine の中で呼び、続けて ApplyTo で設定をセッションに渡すこと
    void Build();
    // 今の設定・記号の表・専門辞書の印・変換ルールをセッションに渡す
    void ApplyTo(SessionManager& sessions);

    const SessionSettings& Settings() const { return sessionSettings_; }
    // かな・英数を切り替えるキー（KeyShortcut::Encode。0 なら割り当てなし。設定 inputModeHotkey）
    uint32_t InputModeShortcut() const { return inputModeShortcut_; }
    // かな・英数を切り替えたときにカーソルの近くに「あ」「A」を出すか（設定 inputModeIndicator）
    bool InputModeIndicator() const { return inputModeIndicator_; }
    // 入力メニューを組むのに使う今の状態（入力モード・版は呼ぶ側で入れる）
    menu::State MenuState() const;
    // 入力メニューから設定を変える（config.json を書き換えて、読み直しを頼む。
    // 読み直しは reload-request の見張りが行う。設定画面から変えたときと同じ経路）
    void ChangeConfig(const std::function<void(Config&)>& change);
    const std::string& ModelPath() const { return modelPath_; }
    ConversionEngine* Engine() { return engine_; }

    // ---- SessionManager::Backend ----
    bool Convert(const std::u32string& reading, const std::u32string& context, int candidateCount,
                 std::vector<std::u32string>* results, std::string* error) override;
    void SetCancelCheck(std::function<bool()> check) override;
    void RecordCommit(const std::u32string& reading, const std::u32string& engineBaseline,
                      const std::u32string& baseline, const std::u32string& committed) override;
    std::optional<std::u32string> EngineResult(const std::u32string& reading,
                                               const std::u32string& shown) override;
    void ForgetLearning(const std::u32string& reading, const std::u32string& engineResult,
                        const std::u32string& shown) override;
    void PunctuationChanged(const std::u32string& punctuation) override;
    std::vector<std::u32string> CandidateOnlyWords(const std::u32string& reading) override;
    std::vector<InputHistoryEntry> PredictionCandidates(const std::u32string& prefix) override;
    void RecordInputHistory(const std::u32string& reading, const std::u32string& text) override;
    void PrivateModeChanged(bool enabled) override;
    void RecordConversion(const ConversionLogEntry& entry) override;
    bool TypoCorrectionAvailable() override;
    std::optional<TypoCorrection> CorrectTypo(const std::u32string& reading, double threshold,
                                              size_t minimumLength) override;
    ScoredText ScoreConversion(const std::u32string& reading, const std::u32string& context) override;
    AIOutcome RunAIRequest(const AIRequest& request, int stallSeconds, CancelToken* cancel) override;
    std::optional<std::u32string> RunAI(size_t preset, const std::u32string& text,
                                        const std::function<void(const std::u32string&)>& onPartial,
                                        CancelToken* cancel) override;

    // ---- 選択テキストのAI編集が使う（どのスレッドからでも呼べる） ----
    // AI に依頼する（接続先は設定のもの。API キーはここで読む）
    AIResult RunAIWith(const AIRequest& request, const std::function<void(const std::u32string&)>& onPartial,
                       CancelToken* cancel, int stallSeconds = 30);
    // AI の接続先が決まっているか（Claude は API キーを読まずに真とする。無ければ依頼のときに失敗の理由を出す）
    bool AIConfigured();
    // 今の設定（Build で読んだもの）
    Config CurrentConfig();

private:
    // AI の接続先とプリセットを読む（API キーは読まない。main thread で呼ぶので）
    void LoadAI(const Config& config);
    // 依頼に使う接続先（API キーが要る接続先なら保管庫から読む。裏のスレッドで呼ぶ）
    AIConnection ConnectionForRequest();
    void Log(const std::string& message) const;
    std::filesystem::path ConfigPath() const;
    std::filesystem::path ResourceDirectory() const;
    void LoadTypoNormalizer(bool enabled);
    std::shared_ptr<TypoNormalizer> Typo();
    void LoadTermDictionaries();
    // 文節変換の結果が表示していた変換（baseline）と違っていたら、ユーザによる修正として学習する
    // （macOS版 IrohaInputController.learnIfCorrected と同じ規則）
    void RecordCorrection(const std::u32string& reading, const std::optional<std::u32string>& engineBaseline,
                          const std::u32string& baseline, const std::u32string& committed);

    Options options_;
    std::string modelPath_;
    std::string adapterPath_;
    std::unique_ptr<ZenzEngine> zenz_;
    std::unique_ptr<LatticeConverter> lattice_;
    std::unique_ptr<UserDictionaryStore> userDictionary_;
    std::unique_ptr<LearningStore> learning_;
    // 入力履歴（予測変換。<データフォルダ>/input-history/<ホスト名>.json）
    std::unique_ptr<InputHistoryStore> inputHistory_;
    // 変換記録（学習用データ。<データフォルダ>/logs/conversions/。設定で ON のときだけ書く）
    std::unique_ptr<ConversionLog> conversionLog_;
    // 打ち間違いの訂正（設定で ON で、モデルが入っているときだけ。訂正の裏のスレッドからも使うので鍵で守る）
    std::mutex typoMutex_;
    std::shared_ptr<TypoNormalizer> typo_;
    std::string typoSignature_;
    // プライベートモードの間は、学習の差し替えとユーザ辞書を止める（変換のスレッドからも読む）
    std::atomic<bool> privateMode_{false};
    // 変換の学習を使うか（設定 learningEnabled。OFF なら記録も適用もしない）
    std::atomic<bool> learningEnabled_{true};
    uint32_t inputModeShortcut_ = 0;
    bool inputModeIndicator_ = true;
    std::string inputModeShortcutLabel_;
    std::string privateModeShortcutLabel_;
    std::unique_ptr<UserRewriteRuleStore> rewriteRules_;
    std::shared_ptr<const UserRewriteRuleSet> rewriteRuleSet_;
    // エンジンの重ね方（iroha-cli・iroha-bench と同じ）と、その一番外側（学習）
    std::unique_ptr<EngineAssembly> assembly_;
    LearningEngine* engine_ = nullptr;
    SessionSettings sessionSettings_;
    // 記号・顔文字・絵文字の表（設定で切ってあれば nullptr。読み込みは初めて要るときに 1 回だけ）
    std::shared_ptr<const SymbolCandidateTable> symbols_;
    std::shared_ptr<const SymbolCandidateTable> loadedSymbols_;
    bool symbolsLoaded_ = false;
    // 専門辞書（入れ外しされたら Reload で読み直す）
    std::filesystem::path dictionaryPath_;
    std::string termSignature_;
    bool termsLoaded_ = false;
    std::shared_ptr<const TermLatticeIndex> termIndex_;
    std::shared_ptr<const TermDictionaryLookup> termLookup_;
    // AI変換の接続先とプリセット（裏のスレッドから読むので鍵で守る）。API キーは使うときに読み、読めたら覚える
    std::mutex aiMutex_;
    AIConnection aiConnection_;
    std::array<AICommitPreset, AICommitSettings::kCount> aiPresets_;
    bool aiKeyLoaded_ = false;
    Config config_;
};

} // namespace iroha
