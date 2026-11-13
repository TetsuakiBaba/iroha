#pragma once
// 打ち間違いの訂正のテスト。
// 移植元: macos/Tests/IrohaCoreTests/TypoCorrectionPlacementTests.swift・TypoCorrectionFeedbackTests.swift・
// TypoNormalizerTests.swift（モデルが要るものは IROHA_TYPO_MODEL でモデルのフォルダを指したときだけ回す。
// parity.json との照合は iroha-cli typo parity）。
// test_main.cpp からインクルードされる（テストマクロを共有するため）。

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "iroha/typo_normalizer.h"
#include "iroha/typo_normalizer_catalog.h"

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#ifdef _MSC_VER
#include <intrin.h>
#endif
#endif

namespace {

iroha::TypoCorrection Fix(const std::u32string& reading, const std::u32string& corrected) {
    return iroha::TypoCorrection{reading, corrected, 5};
}

// 差分は前後の一致部分を取り除いた最小の範囲になる（置換・削除・挿入）
void TestTypoEditRange() {
    const auto replaced = Fix(U"をわぇてけいやくする", U"をわけてけいやくする").GetEdit();
    EXPECT_TRUE(replaced.start == 2 && replaced.end == 3);
    EXPECT_EQ(replaced.replacement, U"け");
    const auto deleted = Fix(U"がちがうんっだろな", U"がちがうんだろな").GetEdit();
    EXPECT_TRUE(deleted.start == 5 && deleted.end == 6);
    EXPECT_EQ(deleted.replacement, U"");
    const auto inserted = Fix(U"とうきょうえき", U"とうきょうのえき").GetEdit();
    EXPECT_TRUE(inserted.start == 5 && inserted.end == 5);
    EXPECT_EQ(inserted.replacement, U"の");
}

void TestTypoPlacement() {
    using Strings = std::vector<std::u32string>;
    // 差分が収まっている文節にだけ訂正後の読みが出る
    auto result = Fix(U"をわぇてけいやくする", U"をわけてけいやくする").PlacementIn(Strings{U"をわぇて", U"けいやくする"});
    EXPECT_TRUE(result && result->index == 0);
    EXPECT_EQ(result ? result->correctedReading : U"", U"をわけて");
    result = Fix(U"きょうはあめだっだ", U"きょうはあめだった").PlacementIn(Strings{U"きょうは", U"あめだっだ"});
    EXPECT_TRUE(result && result->index == 1);
    EXPECT_EQ(result ? result->correctedReading : U"", U"あめだった");
    // 文節境界をまたぐ差分・文節の読みが元の読みと違うときは見送る
    EXPECT_TRUE(!Fix(U"きょうはあめ", U"きょうあはめ").PlacementIn(Strings{U"きょうは", U"あめ"}));
    EXPECT_TRUE(!Fix(U"をわぇてけいやくする", U"をわけてけいやくする").PlacementIn(Strings{U"まったくちがうよみ"}));
    // 先頭に訂正対象外の固定文節があっても対応づけられる
    result = Fix(U"をわぇてけいやくする", U"をわけてけいやくする")
                 .PlacementWithFixedPrefix(Strings{U"USB", U"をわぇて", U"けいやくする"});
    EXPECT_TRUE(result && result->index == 1);
    EXPECT_EQ(result ? result->correctedReading : U"", U"をわけて");
    EXPECT_TRUE(!Fix(U"をわぇてけいやくする", U"をわけてけいやくする")
                     .PlacementWithFixedPrefix(Strings{U"USB", U"べつのよみ"}));
    // 固定部分が無いときは通常版と同じ
    const auto fix = Fix(U"きょうはあめだっだ", U"きょうはあめだった");
    const auto plain = fix.PlacementIn(Strings{U"きょうは", U"あめだっだ"});
    const auto withPrefix = fix.PlacementWithFixedPrefix(Strings{U"きょうは", U"あめだっだ"});
    EXPECT_TRUE(plain && withPrefix && plain->index == withPrefix->index &&
                plain->correctedReading == withPrefix->correctedReading);
    // 文節の端での挿入は、その位置を含む文節に付く
    result = Fix(U"とうきょうえき", U"とうきょうのえき").PlacementIn(Strings{U"とうきょう", U"えき"});
    EXPECT_TRUE(result && result->index == 0);
    EXPECT_EQ(result ? result->correctedReading : U"", U"とうきょうの");
}

// 「末尾に足しただけ」の訂正は入力中には使わない
void TestTypoTrailingInsertion() {
    EXPECT_TRUE(Fix(U"こえて", U"こえて、").IsTrailingInsertionOnly());
    EXPECT_TRUE(Fix(U"やはり", U"やはり。").IsTrailingInsertionOnly());
    EXPECT_TRUE(Fix(U"いゔ", U"いゔー").IsTrailingInsertionOnly());
    EXPECT_TRUE(!Fix(U"がちがうんっだろな", U"がちがうんだろな").IsTrailingInsertionOnly());
    EXPECT_TRUE(!Fix(U"をわぇてけいやくする", U"をわけてけいやくする").IsTrailingInsertionOnly());
    EXPECT_TRUE(!Fix(U"とらは", U"とらいは").IsTrailingInsertionOnly());
    EXPECT_TRUE(!Fix(U"こえてを", U"こえてほ").IsTrailingInsertionOnly());
    EXPECT_TRUE(!Fix(U"こえてを", U"こえて").IsTrailingInsertionOnly());
}

// 差分が文節境界をまたぐときは、訂正対象の先頭文節の位置がわかる
void TestTypoSegmentOffset() {
    using Strings = std::vector<std::u32string>;
    const auto fix = Fix(U"さsてえいただいていて", U"させていただいていて");
    const Strings segments = {U"さ", U"sて", U"えいただいていて"};
    EXPECT_TRUE(!fix.PlacementIn(segments));
    EXPECT_TRUE(fix.SegmentOffset(segments) == std::optional<size_t>(0));
    EXPECT_TRUE(fix.SegmentOffset(Strings{U"USB", U"さ", U"sて", U"えいただいていて"}) == std::optional<size_t>(1));
    EXPECT_TRUE(!fix.SegmentOffset(Strings{U"まったく", U"ちがう"}));
}

std::optional<iroha::TypoCorrectionFeedback> Feedback(const std::u32string& reading, const std::u32string& corrected,
                                                      size_t maxChanged = 12) {
    return iroha::TypoCorrectionFeedback::Make(Fix(reading, corrected), 6, maxChanged);
}

void TestTypoFeedback() {
    // 置換: 変わった1文字だけが左右で入れ替わり、前後は共通
    auto result = Feedback(U"をわぇてけいやくする", U"をわけてけいやくする");
    EXPECT_TRUE(result.has_value());
    if (result) {
        EXPECT_EQ(result->prefix, U"をわ");
        EXPECT_EQ(result->originalChanged, U"ぇ");
        EXPECT_EQ(result->correctedChanged, U"け");
        EXPECT_EQ(result->suffix, U"てけいやくす…");
    }
    // 削除・挿入
    result = Feedback(U"がちがうんっだろな", U"がちがうんだろな");
    if (result) {
        EXPECT_EQ(result->originalChanged, U"っ");
        EXPECT_EQ(result->correctedChanged, U"");
        EXPECT_EQ(result->Original(), U"がちがうんっだろな");
        EXPECT_EQ(result->Corrected(), U"がちがうんだろな");
    }
    result = Feedback(U"とうきょうえき", U"とうきょうのえき");
    if (result) {
        EXPECT_EQ(result->originalChanged, U"");
        EXPECT_EQ(result->correctedChanged, U"の");
        EXPECT_EQ(result->Corrected(), U"とうきょうのえき");
    }
    // 差分が無ければ何も出さない
    EXPECT_TRUE(!Feedback(U"こんにちは", U"こんにちは"));
    // 前後は元と訂正後で必ず同じ文字列
    const std::u32string reading = std::u32string(20, U'あ') + U"ぇ" + std::u32string(20, U'い');
    const std::u32string corrected = std::u32string(20, U'あ') + U"け" + std::u32string(20, U'い');
    result = Feedback(reading, corrected);
    if (result) {
        EXPECT_EQ(result->prefix, U"…ああああああ");
        EXPECT_EQ(result->suffix, U"いいいいいい…");
        EXPECT_EQ(result->Original(), U"…ああああああぇいいいいいい…");
        EXPECT_EQ(result->Corrected(), U"…ああああああけいいいいいい…");
    }
    // 差分が読みの先頭・末尾にあるときは、その側に省略記号を付けない
    result = Feedback(U"をんにちは", U"こんにちは");
    if (result) {
        EXPECT_EQ(result->prefix, U"");
        EXPECT_EQ(result->suffix, U"んにちは");
    }
    result = Feedback(U"こんにちわ", U"こんにちは");
    if (result) {
        EXPECT_EQ(result->prefix, U"こんにち");
        EXPECT_EQ(result->suffix, U"");
    }
    // 変わった部分が長ければ切る
    result = Feedback(std::u32string(20, U'さ'), std::u32string(20, U'し'), 4);
    if (result) {
        EXPECT_EQ(result->originalChanged, U"ささささ…");
        EXPECT_EQ(result->correctedChanged, U"しししし…");
    }
}

void TestTypoProtectedSegments() {
    const auto isDigit = [](char32_t c) { return c >= U'0' && c <= U'9'; };
    auto segments = iroha::TypoNormalizer::ProtectedSegments(U"11じにあいましょう", isDigit);
    EXPECT_TRUE(segments.size() == 2 && segments[0].text == U"11" && segments[0].isProtected &&
                segments[1].text == U"じにあいましょう" && !segments[1].isProtected);
    segments = iroha::TypoNormalizer::ProtectedSegments(U"あした3じから10ぷん", isDigit);
    EXPECT_TRUE(segments.size() == 5 && segments[2].text == U"じから" && segments[3].text == U"10");
    EXPECT_TRUE(iroha::TypoNormalizer::ProtectedSegments(U"", isDigit).empty());
    EXPECT_TRUE(iroha::TypoNormalizer::ProtectedSegments(U"かくにん", isDigit).size() == 1);
}

// 数字の並びを「なん」に置き換え、「なん」の数が合えば数字に戻す
void TestTypoNumberMask() {
    const iroha::NumberMask mask(U"2がつ3にちにかくいんします");
    EXPECT_EQ(mask.Masked(), U"なんがつなんにちにかくいんします");
    EXPECT_EQ(mask.Restore(U"なんがつなんにちにかくにんします").value_or(U"(なし)"), U"2がつ3にちにかくにんします");
    EXPECT_TRUE(!mask.Restore(U"なんがつにちにかくにんします")); // 「なん」が減ったら戻せない
    const iroha::NumberMask mixed(U"なんでも3.5じかん1,000えん");
    EXPECT_EQ(mixed.Masked(), U"なんでもなんじかんなんえん");
    EXPECT_EQ(mixed.Restore(U"なんでもなんじかんなんえん").value_or(U"(なし)"), U"なんでも3.5じかん1,000えん");
    // ローマ字の隣の数字は打ち間違いとみなして置き換えない
    const iroha::NumberMask typo(U"はつばいをk8たいしてる");
    EXPECT_TRUE(typo.DigitIsTypo());
    EXPECT_EQ(typo.Masked(), U"はつばいをk8たいしてる");
    EXPECT_TRUE(!iroha::NumberMask(U"11じにあいましょう").DigitIsTypo());
    const iroha::NumberMask plain(U"かくいん");
    EXPECT_EQ(plain.Masked(), U"かくいん");
    EXPECT_EQ(plain.Restore(U"かくにん").value_or(U"(なし)"), U"かくにん");
}

// q6 に落として戻すと、各値が区切りの倍率の半分以内に収まる。float16 の展開は 65,536 通り全部で
// CPU の命令（F16C）と一致する（macOS版は Float16 と比べている）
void TestTypoWeightConversion() {
    std::vector<float> values;
    for (int i = 0; i < 32 * 3; ++i) values.push_back(static_cast<float>(i % 17) * 0.013f - 0.1f + (i == 40 ? -0.9f : 0));
    const auto bytes = iroha::TypoWeightConversion::QuantizeQ6(values.data(), values.size());
    EXPECT_TRUE(bytes.size() == iroha::TypoWeightConversion::Q6Bytes(values.size()) && bytes.size() == 3 * 26);
    const auto restored = iroha::TypoWeightConversion::DequantizeQ6(bytes.data(), values.size());
    bool within = true;
    for (size_t block = 0; block < 3; ++block) {
        float peak = 0;
        for (size_t i = block * 32; i < block * 32 + 32; ++i) peak = std::max(peak, std::fabs(values[i]));
        const float scale = peak / 32;
        for (size_t i = block * 32; i < block * 32 + 32; ++i) {
            if (std::fabs(restored[i] - values[i]) > scale * 0.5f + 1e-3f) within = false;
        }
    }
    EXPECT_TRUE(within);
    EXPECT_TRUE(std::fabs(restored[40] - values[40]) <= 1e-3f);
    EXPECT_TRUE(iroha::TypoWeightConversion::IsQ6Target("enc.0.attn.q.weight", {640, 640}));
    EXPECT_TRUE(!iroha::TypoWeightConversion::IsQ6Target("emb.weight", {137, 640}));
    EXPECT_TRUE(!iroha::TypoWeightConversion::IsQ6Target("enc.0.attn.q.bias", {640}));

    EXPECT_TRUE(iroha::TypoWeightConversion::HalfToFloat(0x3C00) == 1.0f);
    EXPECT_TRUE(iroha::TypoWeightConversion::HalfToFloat(0x0001) == std::ldexp(1.0f, -24)); // 非正規化数の最小
    EXPECT_TRUE(iroha::TypoWeightConversion::HalfToFloat(0x0200) == std::ldexp(1.0f, -15));
    EXPECT_TRUE(iroha::TypoWeightConversion::FloatToHalf(65520.0f) == 0x7C00); // あふれて Inf
#if defined(_M_X64) || defined(__x86_64__)
#ifdef _MSC_VER
    int info[4] = {};
    __cpuid(info, 1);
    const bool f16c = (info[2] & (1 << 29)) != 0;
#else
    const bool f16c = __builtin_cpu_supports("f16c");
#endif
    if (f16c) {
        int mismatches = 0;
        for (uint32_t bits = 0; bits <= 0xFFFF; ++bits) {
            const float expected = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(static_cast<int>(bits))));
            const float actual = iroha::TypoWeightConversion::HalfToFloat(static_cast<uint16_t>(bits));
            if (std::isnan(expected) ? !std::isnan(actual) : std::memcmp(&expected, &actual, 4) != 0) ++mismatches;
        }
        EXPECT_TRUE(mismatches == 0);
        for (float value : {0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 3.14159f, -3.75f, 65504.0f, 65520.0f, 1e-4f, 6.0e-5f,
                            3.05e-5f, 1.5e-5f, 5.96e-8f, 1e-8f, -1e-8f, 1e10f, -1e10f, 0.1f, -0.2f}) {
            const int half = _mm_cvtsi128_si32(_mm_cvtps_ph(_mm_set_ss(value), 0)) & 0xFFFF;
            EXPECT_TRUE(iroha::TypoWeightConversion::FloatToHalf(value) == half);
        }
    }
#endif
}

// カタログと設置の記録（macOS版と同じキー）
void TestTypoCatalog() {
    const std::string text = R"({"formatVersion":2,"license":"CC BY-SA 4.0","attribution":"出典",
      "models":[{"id":"iroha-typo-v2","name":"標準","summary":"11.6M",
        "manifest":{"url":"https://example.com/m.json","bytes":10,"sha256":"A63302E94EEEAEC5CCE1A84186E5D4B72C7241FF08264E84035D22F0C64CA9A5"},
        "weights":{"url":"https://example.com/w.bin","bytes":20,"sha256":"fa0d4b1ccf8f33cd45e61aefc58cb6ce00864099046d3b899918afc49e8f29d0"},
        "sources":[{"name":"コーパス","holder":"作者","license":"CC BY 4.0","url":"https://example.com"}],
        "page":"https://example.com/page"},
        {"id":"broken","name":"壊れた行"}]})";
    iroha::TypoNormalizerCatalog catalog;
    std::string error;
    EXPECT_TRUE(iroha::TypoNormalizerCatalog::Parse(text, &catalog, &error));
    EXPECT_TRUE(catalog.models.size() == 1 && catalog.Find("iroha-typo-v2") != nullptr);
    if (const auto* model = catalog.Find("iroha-typo-v2")) {
        EXPECT_TRUE(model->TotalBytes() == 30 && model->manifest.sha256[0] == 'a');
        EXPECT_EQ(catalog.LicenseFor(*model), U"CC BY-SA 4.0"); // モデルに無ければカタログの既定
        EXPECT_TRUE(model->sources.size() == 1 && model->sources[0].holder == U"作者");
    }
    EXPECT_TRUE(!iroha::TypoNormalizerCatalog::Parse(R"({"formatVersion":3,"models":[]})", &catalog, &error));

    iroha::TypoNormalizerInstall::Record record;
    record.id = "iroha-typo-v2";
    record.name = U"標準";
    record.sha256 = "fa0d";
    record.installedAt = "2026-11-13T10:00:00Z";
    record.license = U"CC BY-SA 4.0";
    record.sources = {{U"コーパス", U"作者", U"CC BY 4.0", "https://example.com"}};
    const std::string encoded = iroha::TypoNormalizerInstall::EncodeRecord(record);
    EXPECT_EQ(iroha::Utf8ToUtf32(encoded),
              U"{\"id\":\"iroha-typo-v2\",\"installedAt\":\"2026-11-13T10:00:00Z\",\"license\":\"CC BY-SA 4.0\","
              U"\"name\":\"標準\",\"sha256\":\"fa0d\",\"sources\":[{\"holder\":\"作者\",\"license\":\"CC BY 4.0\","
              U"\"name\":\"コーパス\",\"url\":\"https://example.com\"}]}");
    const auto decoded = iroha::TypoNormalizerInstall::DecodeRecord(encoded);
    EXPECT_TRUE(decoded && decoded->id == record.id && decoded->sources.size() == 1 && decoded->page.empty());
}

// モデルを使うテスト（IROHA_TYPO_MODEL があるときだけ）。期待値は macOS版の testDigitsAreKept と同じ
void TestTypoModel() {
    wchar_t* value = nullptr;
    size_t length = 0;
    if (_wdupenv_s(&value, &length, L"IROHA_TYPO_MODEL") != 0 || value == nullptr) return;
    const std::filesystem::path directory(value);
    std::free(value);
    if (!iroha::TypoNormalizer::IsModelDirectory(directory)) return;
    iroha::TypoNormalizer normalizer{directory};
    std::string error;
    EXPECT_TRUE(normalizer.Load(&error));
    // 数字はモデルが出力できないので、そのまま残して前後だけを直す
    for (const std::u32string reading : {U"11じにあいましょう", U"あした3じから", U"11じに"}) {
        const auto correction = normalizer.Correction(reading, 2.0, 4);
        const std::u32string corrected = correction ? correction->corrected : reading;
        std::u32string digits, expected;
        for (char32_t c : corrected) {
            if (c >= U'0' && c <= U'9') digits += c;
        }
        for (char32_t c : reading) {
            if (c >= U'0' && c <= U'9') expected += c;
        }
        EXPECT_EQ(digits, expected);
    }
    const auto typo = normalizer.Correction(U"3じにかくいんしました", 2.0, 4);
    EXPECT_EQ(typo ? typo->corrected : U"(なし)", U"3じにかくにんしました");
    const auto date = normalizer.Correction(U"2がつ3にちにかくいんします", 2.0, 4);
    EXPECT_EQ(date ? date->corrected : U"(なし)", U"2がつ3にちにかくにんします");
    // 対象外の読み（長すぎる・語彙に無い文字）
    EXPECT_TRUE(!normalizer.Supports(std::u32string(iroha::TypoNormalizer::kMaxReadingLength + 1, U'あ')));
    for (const std::u32string reading : {U"漢字", U"カタカナ", U"ゐゑ", U""}) EXPECT_TRUE(!normalizer.Supports(reading));
    EXPECT_TRUE(normalizer.Supports(U"こんにちは"));
    // 訂正なし・しきい値
    EXPECT_TRUE(!normalizer.Correction(U"きょうはいいてんきですね", 2.0));
    const auto fixed = normalizer.Correction(U"をわぇてけいやくする", 2.0);
    EXPECT_TRUE(fixed && fixed->corrected == U"をわけてけいやくする" && fixed->margin > 2.0);
    if (fixed) EXPECT_TRUE(!normalizer.Correction(U"をわぇてけいやくする", fixed->margin + 1));
}

void RunTypoTests() {
    TestTypoEditRange();
    TestTypoPlacement();
    TestTypoTrailingInsertion();
    TestTypoSegmentOffset();
    TestTypoFeedback();
    TestTypoProtectedSegments();
    TestTypoNumberMask();
    TestTypoWeightConversion();
    TestTypoCatalog();
    TestTypoModel();
}

} // namespace
