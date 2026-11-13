// iroha-coreの単体テスト。
// 移植元: macos/Tests/IrohaCoreTests/（テストケースは1対1で対応させること）
#include <cstdio>
#include <string>

#include "iroha/kana.h"
#include "iroha/reading_aligner.h"
#include "iroha/reading_constraint.h"
#include "iroha/romaji_composer.h"
#include "iroha/unicode.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void ExpectEq(const std::u32string& actual, const std::u32string& expected,
              const char* file, int line) {
    ++g_checks;
    if (actual != expected) {
        ++g_failures;
        std::printf("FAIL %s:%d\n  expected: %s\n  actual:   %s\n", file, line,
                    iroha::Utf32ToUtf8(expected).c_str(),
                    iroha::Utf32ToUtf8(actual).c_str());
    }
}

void ExpectTrue(bool condition, const char* expr, const char* file, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL %s:%d\n  expected true: %s\n", file, line, expr);
    }
}

#define EXPECT_EQ(actual, expected) ExpectEq((actual), (expected), __FILE__, __LINE__)
#define EXPECT_TRUE(cond) ExpectTrue((cond), #cond, __FILE__, __LINE__)

std::u32string Compose(const std::u32string& input, bool flush = false) {
    iroha::RomajiComposer composer;
    composer.Input(input);
    if (flush) composer.Flush();
    return composer.Display();
}

void TestBasicGojuon() {
    EXPECT_EQ(Compose(U"aiueo"), U"あいうえお");
    EXPECT_EQ(Compose(U"kakikukeko"), U"かきくけこ");
    EXPECT_EQ(Compose(U"sashisuseso"), U"さしすせそ");
    EXPECT_EQ(Compose(U"tachitsuteto"), U"たちつてと");
    EXPECT_EQ(Compose(U"naninuneno"), U"なにぬねの");
    EXPECT_EQ(Compose(U"hahifuheho"), U"はひふへほ");
    EXPECT_EQ(Compose(U"mamimumemo"), U"まみむめも");
    EXPECT_EQ(Compose(U"yayuyo"), U"やゆよ");
    EXPECT_EQ(Compose(U"rarirurero"), U"らりるれろ");
    EXPECT_EQ(Compose(U"wawo"), U"わを");
}

void TestDakutenHandakuten() {
    EXPECT_EQ(Compose(U"gagigugego"), U"がぎぐげご");
    EXPECT_EQ(Compose(U"zajizuzezo"), U"ざじずぜぞ");
    EXPECT_EQ(Compose(U"dadedo"), U"だでど");
    EXPECT_EQ(Compose(U"babibubebo"), U"ばびぶべぼ");
    EXPECT_EQ(Compose(U"papipupepo"), U"ぱぴぷぺぽ");
}

void TestYouon() {
    EXPECT_EQ(Compose(U"kyakyukyo"), U"きゃきゅきょ");
    EXPECT_EQ(Compose(U"shashusho"), U"しゃしゅしょ");
    EXPECT_EQ(Compose(U"chachucho"), U"ちゃちゅちょ");
    EXPECT_EQ(Compose(U"jajujo"), U"じゃじゅじょ");
    EXPECT_EQ(Compose(U"ryaryuryo"), U"りゃりゅりょ");
}

void TestSokuon() {
    EXPECT_EQ(Compose(U"katta"), U"かった");
    EXPECT_EQ(Compose(U"kippu"), U"きっぷ");
    EXPECT_EQ(Compose(U"zasshi"), U"ざっし");
    EXPECT_EQ(Compose(U"matcha"), U"まっちゃ");
    EXPECT_EQ(Compose(U"ltu"), U"っ");
    EXPECT_EQ(Compose(U"xtu"), U"っ");
}

void TestHatsuon() {
    // nn は「ん」（両方消費）
    EXPECT_EQ(Compose(U"kanni"), U"かんい");
    // n + 子音 → ん
    EXPECT_EQ(Compose(U"kanji"), U"かんじ");
    EXPECT_EQ(Compose(U"sinbun"), U"しんぶn");
    EXPECT_EQ(Compose(U"sinbun", true), U"しんぶん");
    // n' → ん
    EXPECT_EQ(Compose(U"kin'en", true), U"きんえん");
    // 単独 n は次入力待ち、flushで確定
    EXPECT_EQ(Compose(U"n"), U"n");
    EXPECT_EQ(Compose(U"n", true), U"ん");
    // n + 母音は な行
    EXPECT_EQ(Compose(U"nano"), U"なの");
}

void TestSmallVowels() {
    EXPECT_EQ(Compose(U"la"), U"ぁ");
    EXPECT_EQ(Compose(U"xyaxyuxyo"), U"ゃゅょ");
    EXPECT_EQ(Compose(U"fasi"), U"ふぁし");
    EXPECT_EQ(Compose(U"thi"), U"てぃ");
    EXPECT_EQ(Compose(U"dhi"), U"でぃ");
    EXPECT_EQ(Compose(U"vaiorin"), U"ゔぁいおりn");
}

void TestSymbols() {
    EXPECT_EQ(Compose(U"ko-hi-"), U"こーひー");
    EXPECT_EQ(Compose(U"a,b."), U"あ、b。");
    EXPECT_EQ(Compose(U"!?"), U"！？");
    EXPECT_EQ(Compose(U"[a]"), U"「あ」");
}

void TestPendingDisplay() {
    EXPECT_EQ(Compose(U"k"), U"k");
    EXPECT_EQ(Compose(U"ky"), U"ky");
    EXPECT_EQ(Compose(U"kya"), U"きゃ");
    EXPECT_EQ(Compose(U"sh"), U"sh");
}

void TestDeleteBackward() {
    iroha::RomajiComposer composer;
    composer.Input(std::u32string(U"kak"));
    EXPECT_EQ(composer.Display(), U"かk");
    composer.DeleteBackward();
    EXPECT_EQ(composer.Display(), U"か");
    composer.DeleteBackward();
    EXPECT_EQ(composer.Display(), U"");
    EXPECT_TRUE(composer.Empty());
    // 拗音は表示単位で1文字ずつ消える
    composer.Input(std::u32string(U"kya"));
    EXPECT_EQ(composer.Display(), U"きゃ");
    composer.DeleteBackward();
    EXPECT_EQ(composer.Display(), U"き");
}

void TestPassthroughUnknown() {
    // 数字はそのまま通す。"b2" は b が解決不能になった時点で文字として出力される
    EXPECT_EQ(Compose(U"a1b2", true), U"あ1b2");
}

void TestRealWords() {
    EXPECT_EQ(Compose(U"kyouhaiitenki"), U"きょうはいいてんき");
    EXPECT_EQ(Compose(U"konnnichiha"), U"こんにちは");
    EXPECT_EQ(Compose(U"gakkou"), U"がっこう");
    EXPECT_EQ(Compose(U"nihongonyuuryoku"), U"にほんごにゅうりょく");
}

void TestPunctuationStyle() {
    iroha::RomajiComposer composer(U"，", U"．");
    composer.Input(std::u32string(U"a,b."));
    composer.Flush();
    EXPECT_EQ(composer.Display(), U"あ，b．");
    // デフォルトは「、。」
    EXPECT_EQ(Compose(U"a,b.", true), U"あ、b。");
}

void TestHiraganaToKatakana() {
    EXPECT_EQ(iroha::HiraganaToKatakana(U"にゅうりょく"), U"ニュウリョク");
    EXPECT_EQ(iroha::HiraganaToKatakana(U"こーひー"), U"コーヒー");
    EXPECT_EQ(iroha::HiraganaToKatakana(U"ゔぁ"), U"ヴァ");
}

// ---- ReadingConstraint（移植元: ReadingConstraintTests.swift） ----

// 出力が読みと辻褄が合うか（生成を最後まで走らせたときと同じ判定）
bool Accepts(const std::u32string& reading, const std::u32string& output) {
    const auto constraint = iroha::ReadingConstraint::Create(reading);
    if (!constraint) return true;
    const uint64_t mask = constraint->Advance(constraint->InitialMask(), output);
    return mask != 0 && constraint->IsComplete(mask);
}

void TestAcceptsPlainConversion() {
    EXPECT_TRUE(Accepts(U"こんにちはあかちゃん", U"こんにちは赤ちゃん"));
    EXPECT_TRUE(Accepts(U"きょうはいいてんきですね", U"今日はいい天気ですね"));
    EXPECT_TRUE(Accepts(U"うけたまわりました", U"承りました"));
}

// 読みにない句読点の挿入を弾く（zenzが「こんにちは。赤ちゃん」を出す問題）
void TestRejectsInsertedPunctuation() {
    EXPECT_TRUE(!Accepts(U"こんにちはあかちゃん", U"こんにちは。赤ちゃん"));
    EXPECT_TRUE(Accepts(U"こんにちは、あかちゃん", U"こんにちは、赤ちゃん"));
}

void TestRejectsMismatchedKana() {
    EXPECT_TRUE(!Accepts(U"さかなをたべる", U"魚が食べる"));
}

// 読みを使い切らない・使い切りすぎる出力を弾く
void TestRejectsIncompleteConsumption() {
    EXPECT_TRUE(!Accepts(U"あかちゃんがわらった", U"赤ちゃんが"));
    EXPECT_TRUE(!Accepts(U"あかちゃん", U"赤ちゃんが笑った"));
}

// カタカナ・英数字は読みと字数が合わないことがあるので許す
void TestAllowsLooseKatakanaAndAlphabet() {
    EXPECT_TRUE(Accepts(U"こんぴゅーた", U"コンピューター"));
    EXPECT_TRUE(Accepts(U"わうわうをみる", U"WOWOWを見る"));
    EXPECT_TRUE(Accepts(U"にせんにじゅうごねん", U"2025年"));
}

// 空白は読みになければ消費なしで通す（SentencePiece語彙の語頭マーカー「▁」がスペースになって出る）
void TestAllowsWordBoundarySpaces() {
    EXPECT_TRUE(Accepts(U"ぐるーぷにわかれて", U" グループに分かれて"));
    EXPECT_TRUE(Accepts(U"ぐるーぷにわかれて", U" グループに 分かれて"));
    EXPECT_TRUE(Accepts(U"ぐるーぷにわかれて", U"▁グループに分かれて"));
    // 読みに空白があればその位置で消費できる
    EXPECT_TRUE(Accepts(U"こんにちは せかい", U"こんにちは 世界"));
    // 空白を通しても他の制約は緩まない
    EXPECT_TRUE(!Accepts(U"ぐるーぷにわかれて", U" グループに。分かれて"));
}

// 読みにラテン文字がないとき、出力の英字を禁じる設定
void TestRestrictsLatinToReading() {
    const auto acceptsStrict = [](const std::u32string& reading, const std::u32string& output) {
        const auto constraint = iroha::ReadingConstraint::Create(reading, true);
        if (!constraint) return true;
        const uint64_t mask = constraint->Advance(constraint->InitialMask(), output);
        return mask != 0 && constraint->IsComplete(mask);
    };
    EXPECT_TRUE(!acceptsStrict(U"グループニワカレテ", U"groupに分かれて"));
    EXPECT_TRUE(acceptsStrict(U"グループニワカレテ", U"グループに分かれて"));
    EXPECT_TRUE(!acceptsStrict(U"わうわうをみる", U"WOWOWを見る")); // 代償として弾かれる
    EXPECT_TRUE(acceptsStrict(U"USBめもり", U"USBメモリ"));        // 読みに英字があれば許す
    EXPECT_TRUE(acceptsStrict(U"にせんにじゅうごねん", U"2025年")); // 数字は対象外
    EXPECT_TRUE(Accepts(U"わうわうをみる", U"WOWOWを見る"));        // 既定は従来どおり
}

// 追跡できない読み（長すぎる・空）では制約をかけない
void TestNoConstraintForUntrackableReading() {
    EXPECT_TRUE(!iroha::ReadingConstraint::Create(U"").has_value());
    EXPECT_TRUE(!iroha::ReadingConstraint::Create(std::u32string(63, U'あ')).has_value());
    EXPECT_TRUE(iroha::ReadingConstraint::Create(std::u32string(62, U'あ')).has_value());
}

void TestSpanClassification() {
    using RC = iroha::ReadingConstraint;
    EXPECT_TRUE(RC::SpanOf(U'あ') == RC::Span::Literal);
    EXPECT_TRUE(RC::SpanOf(U'。') == RC::Span::Literal);
    EXPECT_TRUE(RC::SpanOf(U'!') == RC::Span::Literal);
    EXPECT_TRUE(RC::SpanOf(U'漢') == RC::Span::OneOrMore);
    EXPECT_TRUE(RC::SpanOf(U'々') == RC::Span::OneOrMore);
    EXPECT_TRUE(RC::SpanOf(U'ア') == RC::Span::ZeroOrMore);
    EXPECT_TRUE(RC::SpanOf(U'ー') == RC::Span::ZeroOrMore);
    EXPECT_TRUE(RC::SpanOf(U'7') == RC::Span::ZeroOrMore);
}

// ---- ReadingAligner（移植元: ReadingAlignerTests.swift） ----

std::u32string JoinReadings(const std::vector<iroha::ReadingAligner::Segment>& segments,
                            bool conversion) {
    std::u32string out;
    for (size_t i = 0; i < segments.size(); ++i) {
        if (i > 0) out += U"|";
        out += conversion ? segments[i].conversion : segments[i].reading;
    }
    return out;
}

void TestBasicTwoSegments() {
    const auto result =
        iroha::ReadingAligner::SegmentReading(U"きょうはいいてんきですね", U"今日はいい天気ですね");
    EXPECT_EQ(JoinReadings(result, false), U"きょうはいい|てんきですね");
    EXPECT_EQ(JoinReadings(result, true), U"今日はいい|天気ですね");
}

void TestParticleAnchors() {
    const auto result = iroha::ReadingAligner::SegmentReading(U"さかなをたべる", U"魚を食べる");
    EXPECT_EQ(JoinReadings(result, false), U"さかなを|たべる");
    EXPECT_EQ(JoinReadings(result, true), U"魚を|食べる");
}

void TestAllKanaIsSingleSegment() {
    const auto result = iroha::ReadingAligner::SegmentReading(
        U"すもももももももものうち", U"すもももももももものうち");
    EXPECT_TRUE(result.size() == 1);
}

void TestAllKanjiIsSingleSegment() {
    const auto result = iroha::ReadingAligner::SegmentReading(U"かんじへんかん", U"漢字変換");
    EXPECT_TRUE(result.size() == 1);
    EXPECT_EQ(result[0].reading, U"かんじへんかん");
}

void TestKatakanaInConversion() {
    // カタカナ部分は読みのひらがなと対応付けられる
    const auto result =
        iroha::ReadingAligner::SegmentReading(U"こーひーをのむ", U"コーヒーを飲む");
    EXPECT_EQ(JoinReadings(result, true), U"コーヒーを|飲む");
    EXPECT_EQ(JoinReadings(result, false), U"こーひーを|のむ");
}

void TestLeadingKana() {
    const auto result = iroha::ReadingAligner::SegmentReading(U"これはほんです", U"これは本です");
    EXPECT_EQ(JoinReadings(result, true), U"これは|本です");
}

void TestMismatchFallsBackToWholeSegment() {
    // 変換結果のかなが読みに現れない場合は分割を諦めて全体を返す
    const auto result = iroha::ReadingAligner::SegmentReading(U"きょうは", U"明日も");
    EXPECT_TRUE(result.size() == 1);
    EXPECT_EQ(result[0].reading, U"きょうは");
}

void TestReadingRoundTrip() {
    // 分割結果の読みを連結すると元の読みに一致する
    const std::u32string reading = U"わたしはがっこうへいきます";
    const auto segments =
        iroha::ReadingAligner::SegmentReading(reading, U"私は学校へ行きます");
    std::u32string joinedReading, joinedConversion;
    for (const auto& segment : segments) {
        joinedReading += segment.reading;
        joinedConversion += segment.conversion;
    }
    EXPECT_EQ(joinedReading, reading);
    EXPECT_EQ(joinedConversion, U"私は学校へ行きます");
}

void TestKatakanaToHiragana() {
    EXPECT_EQ(iroha::KatakanaToHiragana(U"コーヒー"), U"こーひー");
    EXPECT_EQ(iroha::KatakanaToHiragana(U"ヴァイオリン"), U"ゔぁいおりん");
}

void TestUnicodeRoundTrip() {
    const std::u32string sample = U"にほんご、ABC！𠮷野家"; // サロゲートペア含む
    EXPECT_EQ(iroha::Utf16ToUtf32(iroha::Utf32ToUtf16(sample)), sample);
    EXPECT_EQ(iroha::Utf8ToUtf32(iroha::Utf32ToUtf8(sample)), sample);
}

} // namespace

// 学習・ユーザ辞書のテスト（EXPECTマクロを共有するためここで取り込む）
#include "test_dictionaries.h"
// 辞書ラティスのテスト
#include "test_lattice.h"
// 異体字・長い読みの分割のテスト
#include "test_engines.h"
// 学習のテスト
#include "test_learning.h"
// 入力セッションのテスト
#include "test_session.h"
#include "test_session_manager.h"
#include "test_symbols.h"

int main() {
    TestBasicGojuon();
    TestDakutenHandakuten();
    TestYouon();
    TestSokuon();
    TestHatsuon();
    TestSmallVowels();
    TestSymbols();
    TestPendingDisplay();
    TestDeleteBackward();
    TestPassthroughUnknown();
    TestRealWords();
    TestPunctuationStyle();
    TestHiraganaToKatakana();
    TestUnicodeRoundTrip();

    TestAcceptsPlainConversion();
    TestRejectsInsertedPunctuation();
    TestRejectsMismatchedKana();
    TestRejectsIncompleteConsumption();
    TestAllowsLooseKatakanaAndAlphabet();
    TestAllowsWordBoundarySpaces();
    TestRestrictsLatinToReading();
    TestNoConstraintForUntrackableReading();
    TestSpanClassification();

    TestBasicTwoSegments();
    TestParticleAnchors();
    TestAllKanaIsSingleSegment();
    TestAllKanjiIsSingleSegment();
    TestKatakanaInConversion();
    TestLeadingKana();
    TestMismatchFallsBackToWholeSegment();
    TestReadingRoundTrip();
    TestKatakanaToHiragana();

    RunDictionaryTests();
    RunLatticeTests();
    RunEngineTests();
    RunLearningTests();
    RunSessionTests();
    RunSessionManagerTests();
    RunSymbolTests();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
