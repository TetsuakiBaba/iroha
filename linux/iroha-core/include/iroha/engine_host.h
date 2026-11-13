#pragma once
// IME本体と同じ構成のエンジン一式と、セッション（SessionManager）から見た呼び出し口。
// Windows版の変換サーバ（iroha-server/main.cpp の ServerState / ServerBackend）を移したもの。
// IBus のエンジンと iroha-cli の session が使う。
//
// 構成: 学習 → ユーザ辞書 → 長い読みの区切り → 異体字 → 辞書ラティスの候補をモデルで再採点 → モデル。
// 候補を複数求められたとき（候補ウィンドウ）は辞書ラティスで候補を作ってモデルで並べ、
// 1つだけのとき（全体の変換）はモデルの生成に任せる。辞書が無ければモデル単体で動く。
// Reload で設定・辞書を読み直す（モデルは変わったときだけ読み込む）
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "iroha/chunked_conversion_engine.h"
#include "iroha/config.h"
#include "iroha/conversion_log.h"
#include "iroha/input_history.h"
#include "iroha/lattice_converter.h"
#include "iroha/lattice_rescoring_engine.h"
#include "iroha/learning_engine.h"
#include "iroha/learning_store.h"
#include "iroha/session_manager.h"
#include "iroha/symbol_candidates.h"
#include "iroha/term_dictionary.h"
#include "iroha/typo_normalizer.h"
#include "iroha/user_dictionary_engine.h"
#include "iroha/user_dictionary_store.h"
#include "iroha/user_rewrite_rule.h"
#include "iroha/variant_kanji_engine.h"
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
    };

    explicit EngineHost(Options options = {});
    ~EngineHost() override;

    // 設定・辞書を読み（直し）てエンジンを組み立てる。セッションが動いているときは
    // SessionManager::WithEngine の中で呼び、続けて ApplyTo で設定をセッションに渡すこと
    void Build();
    // 今の設定・記号の表・専門辞書の印・変換ルールをセッションに渡す
    void ApplyTo(SessionManager& sessions);

    const SessionSettings& Settings() const { return sessionSettings_; }
    const std::string& ModelPath() const { return modelPath_; }
    ConversionEngine* Engine() { return engine_.get(); }

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

private:
    void Log(const std::string& message) const;
    std::filesystem::path ConfigPath() const;
    void LoadTypoNormalizer(bool enabled);
    std::shared_ptr<TypoNormalizer> Typo();
    void LoadTermDictionaries();
    // 文節変換の結果が表示していた変換（baseline）と違っていたら、ユーザによる修正として学習する
    // （macOS版 IrohaInputController.learnIfCorrected と同じ規則）
    void RecordCorrection(const std::u32string& reading, const std::optional<std::u32string>& engineBaseline,
                          const std::u32string& baseline, const std::u32string& committed);

    Options options_;
    std::string modelPath_;
    std::unique_ptr<ZenzEngine> zenz_;
    std::unique_ptr<LatticeConverter> lattice_;
    std::unique_ptr<LatticeRescoringEngine> rescoring_;
    std::unique_ptr<VariantKanjiEngine> variants_;
    std::unique_ptr<ChunkedConversionEngine> chunked_;
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
    std::unique_ptr<UserRewriteRuleStore> rewriteRules_;
    std::shared_ptr<const UserRewriteRuleSet> rewriteRuleSet_;
    std::unique_ptr<UserDictionaryEngine> dictionaryEngine_;
    std::unique_ptr<LearningEngine> engine_;
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
};

} // namespace iroha
