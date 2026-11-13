#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace iroha {

// 打ち間違いの訂正案。margin が大きいほどモデルが「入力のままより訂正のほうがありそう」と見ている。
// 移植元: macos/Sources/IrohaCore/TypoNormalizer/（TypoNormalizer.swift・TypoCorrectionPlacement.swift）
struct TypoCorrection {
    std::u32string reading;   // 入力された読み（ひらがな）
    std::u32string corrected; // 訂正した読み
    double margin = 0;        // logP(訂正 | 入力) − logP(入力そのまま | 入力)

    // 訂正が読みのどこを書き換えるか。[start, end) は元の読みの文字位置（前後の一致部分を除いた最小の差分）、
    // replacement はそこへ入る文字列
    struct Edit {
        size_t start = 0;
        size_t end = 0;
        std::u32string replacement;
    };
    Edit GetEdit() const;

    // 「読みの末尾に文字を足しただけ」か（入力中は訂正ではなく続きの補完なので捨てる）
    bool IsTrailingInsertionOnly() const;

    // 元の読みの [start, end)（1 文節ぶん）に差分が収まっていれば、その文節の訂正後の読み
    std::optional<std::u32string> CorrectedSegment(size_t start, size_t end) const;

    // 文節の読みを並び順に渡して、訂正が当たる文節の番号と訂正後の読み（差分が境界をまたげば無し）
    struct Placement {
        size_t index = 0;
        std::u32string correctedReading;
    };
    std::optional<Placement> PlacementIn(const std::vector<std::u32string>& readings) const;
    // 文節の列のうち、訂正の対象になった読みが始まる位置（先頭の固定部分の数）
    std::optional<size_t> SegmentOffset(const std::vector<std::u32string>& readings) const;
    // 先頭に訂正の対象外だった文節（英字・取り入れた予測など）が付いていてもよい版。番号は readings 全体での番号
    std::optional<Placement> PlacementWithFixedPrefix(const std::vector<std::u32string>& readings) const;

    bool operator==(const TypoCorrection& other) const {
        return reading == other.reading && corrected == other.corrected && margin == other.margin;
    }
};

// 「打った読み → 直した読み」を小窓に出すための割り付け（macOS版 TypoCorrectionFeedback）。
// 前後の共通部分は元と訂正後で同じ文字列（省略記号込み）にする（2 つを並べたときに桁がずれないように）
struct TypoCorrectionFeedback {
    std::u32string prefix;           // 変わらなかった前の部分。頭を削ったら先頭に "…"
    std::u32string originalChanged;  // 打った読みのうち変わった部分（挿入だけなら空）
    std::u32string correctedChanged; // 直した読みのうち変わった部分（削除だけなら空）
    std::u32string suffix;           // 変わらなかった後の部分。尻を削ったら末尾に "…"

    std::u32string Original() const { return prefix + originalChanged + suffix; }
    std::u32string Corrected() const { return prefix + correctedChanged + suffix; }

    // 差分の前後 context 文字だけを残した割り付け。差分が無ければ無し
    static std::optional<TypoCorrectionFeedback> Make(const TypoCorrection& correction, size_t context = 6,
                                                      size_t maxChanged = 12);

    bool operator==(const TypoCorrectionFeedback& other) const {
        return prefix == other.prefix && originalChanged == other.originalChanged &&
               correctedChanged == other.correctedChanged && suffix == other.suffix;
    }
};

// 読みの中の数字の並び（「11」「3.5」「1,000」）を「なん」に置き換え、モデルが直した後で数字に戻す
class NumberMask {
public:
    explicit NumberMask(const std::u32string& reading);
    const std::u32string& Masked() const { return masked_; }
    // 数字がローマ字の隣にある（「k8」）。数字の段に指がかかった打ち間違いなので、置き換えない
    bool DigitIsTypo() const { return digitIsTypo_; }
    // モデルが直した Masked() を数字に戻す。「なん」の数が合わなければ無し
    std::optional<std::u32string> Restore(const std::u32string& corrected) const;

private:
    std::u32string masked_;
    // Masked() の中の「なん」を左から並べたもの。置き換えたものは元の数字、元からあるものは無し
    std::vector<std::optional<std::u32string>> occurrences_;
    bool digitIsTypo_ = false;
};

// 重みの形式の変換（float16・q6。macOS版 TypoWeightConversion）
namespace TypoWeightConversion {
float HalfToFloat(uint16_t half);
uint16_t FloatToHalf(float value);
constexpr size_t kQ6Block = 32;
bool IsQ6Target(const std::string& name, const std::vector<int64_t>& shape);
inline size_t Q6Bytes(size_t count) { return count / kQ6Block * 26; }
std::vector<uint8_t> QuantizeQ6(const float* values, size_t count);
std::vector<float> DequantizeQ6(const uint8_t* bytes, size_t count);
} // namespace TypoWeightConversion

// ローマ字入力の打ち間違いを、かな漢字変換の手前で直す「読み → 読み」のモデル
// （文字単位 Transformer encoder–decoder。移植元: macos/Sources/IrohaCore/TypoNormalizer/）。
// 計算は macOS版と同じ順で float32（行列の積だけは SIMD でまとめて足すので、足す順は違う）。
// モデルの読み込みは最初の呼び出しまで遅らせる。スレッドセーフ（呼び出しは 1 つずつ行う）
class TypoNormalizer {
public:
    // 訂正を採用するしきい値の既定値（macOS版と同じ 2.5）
    static constexpr double kDefaultThreshold = 2.5;
    // 学習データの読みの上限。これより長い読みには使わない
    static constexpr size_t kMaxReadingLength = 48;

    explicit TypoNormalizer(std::filesystem::path directory);
    ~TypoNormalizer();
    TypoNormalizer(const TypoNormalizer&) = delete;
    TypoNormalizer& operator=(const TypoNormalizer&) = delete;

    // manifest.json と weights.bin が揃っているか
    static bool IsModelDirectory(const std::filesystem::path& directory);

    // モデルを読み込む（呼ばなくても最初の推論で読まれる）。失敗は 1 回だけ記録して以後は同じ結果を返す
    bool Load(std::string* error = nullptr);

    // 訂正案（しきい値を超えたときだけ）。対象外・訂正なし・自信が足りないときは無し。
    // モデルが出力できない文字（数字・ASCII の記号・ヵヶ）は書き換えさせない（macOS版 correction と同じ規則）。
    // minimumSegmentLength は、数字などで区切った部分にも当てる最低文字数
    std::optional<TypoCorrection> Correction(const std::u32string& reading, double threshold = kDefaultThreshold,
                                             size_t minimumSegmentLength = 1, std::string* error = nullptr);

    // しきい値を通さない生の生成結果（検証用）
    std::optional<std::u32string> Generate(const std::u32string& reading, std::string* error = nullptr);
    // logP(target | source)（出力できない文字を含めば −∞）
    double LogProbability(const std::u32string& target, const std::u32string& source);
    // teacher forcing で target を食わせたときの先頭 steps ステップのロジット（照合用）
    std::vector<std::vector<float>> Logits(const std::u32string& source, const std::u32string& target, size_t steps);
    // 読みを扱えるか（空でない・48 文字以下・語彙にある文字だけ）
    bool Supports(const std::u32string& reading);

    // 読みを「そのまま残す文字の連なり」と「モデルに通す部分」に分ける（空の区間は作らない）
    struct ProtectedSegment {
        std::u32string text;
        bool isProtected = false;
    };
    static std::vector<ProtectedSegment> ProtectedSegments(const std::u32string& reading,
                                                           const std::function<bool(char32_t)>& isProtected);

    // 行列の積に SIMD の命令（AVX2）を使っているか（表示用）
    static const char* KernelName();

private:
    class Runtime;
    Runtime* LoadLocked(std::string* error);

    std::filesystem::path directory_;
    std::mutex mutex_;
    std::unique_ptr<Runtime> runtime_;
    std::optional<std::string> loadFailure_;
};

} // namespace iroha
