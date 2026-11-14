#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "iroha/candidate_scorer.h"
#include "iroha/conversion_engine.h"

namespace iroha {

// かな漢字変換モデル（GGUF）をllama.cppで動かす変換エンジン。
// 移植元: macos/Sources/IrohaCore/ZenzEngine.swift（挙動互換を維持すること）
//
// プロンプト形式（zenz-v3）:
//   [U+EE02 + 左文脈] + U+EE00 + カタカナ読み + U+EE01 → 変換結果
//
// デコーダ専用のモデル（zenz、GPT-2系）と、エンコーダ・デコーダ型のモデル（iroha-kkc、文字単位T5）の
// 両方を同じプロンプト形式で動かす。エンコーダ・デコーダ型は、プロンプト全体（+ </s>）をエンコーダに
// 1回通し、デコーダは開始トークンから変換結果を生成する。読み制約・n-best・採点は共通。
//
// 未移植: 文字ごとの自信度（convertWithConfidence。「AIで直す」の判定に使う）
//
// スレッドセーフではない。呼び出し側（変換サーバ）で直列化すること。
// このクラスはllama.cppに依存するため iroha-engine ターゲットに属する
// （TIP DLLにはリンクしないこと）。
class ZenzEngine : public ConversionEngine, public CandidateScorer {
public:
    struct Options {
        // 変換記録から追加学習した LoRA アダプタ（GGUF、UTF-8のパス）。空ならベースモデルのまま
        std::string adapterPath;
        // 読みにラテン文字がないとき出力の英字を禁じる（ReadingConstraint 参照。既定は許す）
        bool restrictLatinToReading = false;
        // 読み制約（constrained decoding）を使うか。偽なら素の貪欲生成（実験・計測用。
        // 読みにない句読点の挿入や食い残しを防げなくなるので、IME本体では常に真）
        bool usesReadingConstraint = true;
    };

    // modelPath: GGUFファイルへのパス（UTF-8）
    explicit ZenzEngine(std::string modelPath);
    ZenzEngine(std::string modelPath, Options options);
    ~ZenzEngine() override;
    ZenzEngine(const ZenzEngine&) = delete;
    ZenzEngine& operator=(const ZenzEngine&) = delete;

    // モデルを事前にロードしておく（初回変換のもたつき防止）
    bool Prewarm(std::string* error);

    // 推論の実行先とスレッド数（macOS版 runtimeInfo。ベンチマークの記録に残す）。device は "CPU" か
    // 「ggml の装置名: 説明」（GPU に載せる層が 0 でなく、種類が GPU か IGPU の最初の装置）、threads は
    // llama_n_threads。モデルを読み込んだあとでないと分からないので、読み込む前は偽
    struct RuntimeInfo {
        std::string device;
        int threads = 0;
    };
    bool GetRuntimeInfo(RuntimeInfo* info) const;

    // ひらがな読みを仮名漢字混じり文候補（尤度順）に変換する。
    // candidateCountが1ならグリーディ、2以上で先頭トークン分岐のn-best
    // （最良の候補から kNbestLogProbWindow 以上離れた候補は捨てる）
    bool Convert(const std::u32string& reading, const std::u32string& leftContext,
                 int candidateCount, std::vector<std::u32string>* results,
                 std::string* error) override;

    // 貪欲変換の第一候補と、その系列（終端まで）の対数確率
    // 変換結果の 1 文字ぶんの自信度（macOS版 CharacterConfidence）。文字が複数トークンにまたがる・
    // 1 トークンが複数文字を含む場合は、その文字に関わるトークンのうち最も低い値を採る
    struct CharacterConfidence {
        float logProb = 0;  // この文字を出したトークンの対数確率（nat）
        float margin = 0;   // 読み制約を満たすトークンのうち 1 位と 2 位の対数確率の差（競合が無ければ +inf）
        bool relaxed = false; // 読み制約を諦めて素の最尤トークンを選んだ
    };
    struct ScoredConversion {
        std::u32string text;
        float logProb = 0;
        // text の文字（書記素）と同じ数・同じ順
        std::vector<CharacterConfidence> confidences;
    };
    // 貪欲変換の第一候補を、対数確率と文字ごとの自信度つきで返す（macOS版 convertWithConfidence）。
    // 何も生成できなければ読みをそのまま返し、対数確率は -inf（文字ごとの自信は無いものとする）
    bool ConvertScored(const std::u32string& reading, const std::u32string& leftContext,
                       ScoredConversion* result, std::string* error);

    // 候補それぞれの対数確率 log P(候補 + 終端 | プロンプト) を返す（teacher forcing）。
    // 終端は「生成を終わらせるトークンのどれか」が出る確率で測る。
    //
    // プロンプトは1回だけ評価し、候補を1つずつ、プロンプトの続きに並べて採点する。
    // macOS版はプロンプトのKVキャッシュを候補ごとの系列へ複製し、全候補を1バッチで採点するが、
    // llama.cppのCPU版は1バッチに載せるトークン数やキャッシュ上の位置で計算の経路が変わり、
    // 同じ候補でも対数確率が 0.1〜0.9 ほど動く（2026-11-13 実測。候補どうしが見えているわけではない）。
    // 1つずつなら生成と同じ経路になり、生成した文の採点が生成のときの対数確率と一致する
    // （`iroha-cli model-selfcheck` で確かめる）
    bool Score(const std::vector<std::u32string>& candidates, const std::u32string& reading,
               const std::u32string& leftContext, std::vector<float>* scores,
               std::string* error) override;

    // 変換・採点を途中でやめるかの問い合わせ口（空の関数で外す）。渡されている間、生成の1トークンごと・
    // 採点の1候補ごとに呼び、真なら失敗（error は kCancelledError）として戻る。
    // KVキャッシュは評価したところまで正しい状態で残るので、次の変換はそのまま行える。
    // ライブ変換を裏で進めている間に、キーの処理が変換を必要としたときに使う
    void SetCancelCheck(std::function<bool()> check) { cancelCheck_ = std::move(check); }
    static constexpr char kCancelledError[] = "変換を途中でやめました";

    // n-best候補を残す対数確率の幅（最良候補との差、nat単位）
    static constexpr float kNbestLogProbWindow = 8;

    static std::u32string BuildPrompt(const std::u32string& reading,
                                      const std::u32string& leftContext,
                                      size_t maxContextLength);

    // 生成バイト列をUTF-8として文字列化する。末尾の不完全なバイト列（多バイト文字の途中で
    // 終端や生成上限に達したもの）は捨て、途中の不正なバイトは取り除く
    static std::u32string DecodeUtf8DroppingFragments(const std::string& bytes);

private:
    struct Runtime;
    struct Generated;
    struct FirstToken;

    bool EnsureLoadedMeasured(std::string* error);
    bool EnsureLoaded(std::string* error);
    bool TokenizePrompt(const std::u32string& prompt, std::vector<int32_t>* tokens,
                        std::string* error);
    // KVキャッシュを「プロンプトだけを評価した状態」にし、その直後のlogitsを返す。
    // primedCount はその状態でデコーダ側に載っているトークン数（続きのトークンの位置はここから）。
    // 直前と同じプロンプトなら評価し直さず、続きのトークンを外して前の状態に戻す
    bool PrimePrompt(const std::vector<int32_t>& promptTokens, size_t* primedCount,
                     const float** logits, std::string* error);
    // KVキャッシュとの共通プレフィックスを再利用してトークン列を評価する（デコーダ専用モデル）。
    // 成功時は末尾トークンのlogitsが有効。失敗時はキャッシュを破棄してfalse
    bool DecodePrompt(const std::vector<int32_t>& tokens, std::string* error);
    bool Generate(const std::vector<int32_t>& promptTokens, const FirstToken* forcedFirstToken,
                  const std::u32string& reading, Generated* out, std::string* error);
    void ResetMemory();
    // 途中でやめるよう言われていれば error を入れて真を返す
    bool Cancelled(std::string* error) const;

    // 左文脈として与える最大文字数（zenz-v3の学習設定に合わせる）
    static constexpr size_t kMaxContextLength = 40;

    std::string modelPath_;
    Options options_;
    std::function<bool()> cancelCheck_;
    std::unique_ptr<Runtime> runtime_;
};

} // namespace iroha
