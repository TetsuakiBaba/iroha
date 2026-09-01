// iroha-coreの単体テスト。
// 移植元: macos/Tests/IrohaCoreTests/（テストケースは1対1で対応させること）
#include <cstdio>
#include <string>

#include "iroha/kana.h"
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

void TestUnicodeRoundTrip() {
    const std::u32string sample = U"にほんご、ABC！𠮷野家"; // サロゲートペア含む
    EXPECT_EQ(iroha::Utf16ToUtf32(iroha::Utf32ToUtf16(sample)), sample);
    EXPECT_EQ(iroha::Utf8ToUtf32(iroha::Utf32ToUtf8(sample)), sample);
}

} // namespace

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

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
