#pragma once
#include <optional>
#include <string>
#include <vector>

namespace iroha {

// 音声認識の結果を分かち書きして読みを付けた 1 語（つなげると認識結果に戻る）。
// 移植元: macos/Sources/IrohaCore/DocumentVocabulary/DocumentTermExtractor.swift ReadingToken
struct ReadingToken {
    std::u32string surface;
    std::optional<std::u32string> reading;
    bool operator==(const ReadingToken& other) const {
        return surface == other.surface && reading == other.reading;
    }
};

// 音声認識の結果（漢字かな交じりの表記）を、かな漢字変換に通す「読み」と、読みに戻さない「固定部分」に
// 分けたもの（「こえ」モード）。移植元: macos/Sources/IrohaCore/Voice/VoiceTranscript.swift。
//
// 読みが付いた語はつなげて読みにし、付かなかった語（英字・数字・記号、読みを求められなかった漢字）は
// 認識結果の表記のまま固定部分にする。読みは打鍵で入れた読みと同じ形（ひらがなと、ローマ字入力で打てる記号）にする。
// 読みの部分には認識結果の表記（surface）も残す
struct VoiceTranscript {
    struct Part {
        enum class Kind { Reading, Fixed };
        Kind kind = Kind::Reading;
        // Reading: かな漢字変換に通す読み。Fixed: 認識結果の表記のまま入れる部分
        std::u32string text;
        // Reading: 音声認識が出した表記（Fixed では空）
        std::u32string surface;

        static Part Reading(std::u32string reading, std::u32string surface) {
            return Part{Kind::Reading, std::move(reading), std::move(surface)};
        }
        static Part Fixed(std::u32string text) { return Part{Kind::Fixed, std::move(text), U""}; }
        bool operator==(const Part& other) const {
            return kind == other.kind && text == other.text && surface == other.surface;
        }
    };

    std::vector<Part> parts;

    VoiceTranscript() = default;
    // comma・period は読点・句点として読みに入れる文字（句読点の組。「、。」か「，．」）
    explicit VoiceTranscript(const std::vector<ReadingToken>& tokens, const std::u32string& comma = U"、",
                             const std::u32string& period = U"。");

    bool Empty() const { return parts.empty(); }
    // 音声認識の表記（空白を除いたもの）
    std::u32string Surface() const;
    // 読みと固定部分を順につなげたもの（確認用の表示）
    std::u32string ReadingText() const;

    // Windows の音声認識（SAPI）が語ごとに返す表示形（display）と語彙形（lexical）から読みを決める（Windows版だけのもの。
    // macOS版は表記を分かち書きして読みを付け直す）:
    //   表示形がひらがな・カタカナ・長音・句読点だけ → 表示形をひらがなにしたもの（助詞の「は」は語彙形が「わ」なので、表示形を使う）
    //   そうでなく、表示形が漢字とかなだけで語彙形がかなだけ → 語彙形をひらがなにしたもの（「明日」→ あした）
    //   どちらでもない（英字・数字・記号を含む） → 読みなし
    // 語ごとの表示形は数字への置き換え（「三」→「3」）の前のもの
    static ReadingToken TokenFromRecognition(const std::u32string& display, const std::u32string& lexical);
};

// 音声認識の表記と、同じ発話を読みに戻して iroha で変換した結果の、違う箇所だけを取り出す（「こえ」モード）。
// 移植元: macos/Sources/IrohaCore/Voice/VoiceComparison.swift。
// 違う箇所を前後 1 文字つきで出す（「何時か → いつか」）。違う箇所の間に同じ文字が 1 文字しかなければ 1 か所にまとめる
namespace VoiceComparison {

struct Difference {
    std::u32string recognized;
    std::u32string converted;
    bool operator==(const Difference& other) const {
        return recognized == other.recognized && converted == other.converted;
    }
};

std::vector<Difference> Differences(const std::u32string& recognized, const std::u32string& converted);

// 小窓に出す 1 行（「音声認識と同じ」か、違う箇所を最大 limit 件）
std::u32string Summary(const std::u32string& recognized, const std::u32string& converted, size_t limit = 3);

} // namespace VoiceComparison

} // namespace iroha
