#pragma once
// 変換ルールのテスト。
// 移植元: macos/Tests/IrohaCoreTests/UserRewriteRuleTests.swift（テストケースは1対1で対応させること）。
// test_main.cpp からインクルードされる（テストマクロを共有するため）。

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "iroha/user_rewrite_rule.h"

namespace {

// 2026-09-06 14:05:09 に固定した展開環境
iroha::RewriteContext FixedRewriteContext(std::map<std::u32string, std::u32string> parameters = {}) {
    iroha::RewriteContext context;
    context.year = 2026;
    context.month = 9;
    context.day = 6;
    context.hour = 14;
    context.minute = 5;
    context.second = 9;
    context.parameters = std::move(parameters);
    return context;
}

std::u32string RenderTemplate(const std::u32string& source) {
    return iroha::RewriteTemplate(source).Render(FixedRewriteContext());
}

using RewriteSegment = iroha::RewriteTemplate::Segment;

void TestRewriteParse() {
    // 普通の文字列は 1 つの文字列の部分
    EXPECT_TRUE(iroha::RewriteTemplate(U"こんにちは").Segments() ==
                (std::vector<RewriteSegment>{{false, U"こんにちは", std::nullopt}}));
    EXPECT_TRUE(!iroha::RewriteTemplate(U"こんにちは").IsDynamic());
    // 名前と引数は最初の「:」で分ける
    const iroha::RewriteTemplate time(U"いま {{time:HH:mm:ss}} です");
    EXPECT_TRUE(time.Segments() == (std::vector<RewriteSegment>{{false, U"いま ", std::nullopt},
                                                                 {true, U"time", U"HH:mm:ss"},
                                                                 {false, U" です", std::nullopt}}));
    EXPECT_TRUE(time.IsDynamic());
    EXPECT_TRUE(iroha::RewriteTemplate(U"{{date}}").Segments() ==
                (std::vector<RewriteSegment>{{true, U"date", std::nullopt}}));
    EXPECT_TRUE(iroha::RewriteTemplate(U"{{ date }}").Segments() ==
                (std::vector<RewriteSegment>{{true, U"date", std::nullopt}}));
    // 閉じていない・空の {{}} は文字のまま
    EXPECT_TRUE(iroha::RewriteTemplate(U"{{date").Segments() ==
                (std::vector<RewriteSegment>{{false, U"{{date", std::nullopt}}));
    EXPECT_TRUE(iroha::RewriteTemplate(U"a{{}}b").Segments() ==
                (std::vector<RewriteSegment>{{false, U"a{{}}b", std::nullopt}}));
}

void TestRewriteRenderDates() {
    EXPECT_EQ(RenderTemplate(U"{{date}}"), U"2026/09/06");
    EXPECT_EQ(RenderTemplate(U"{{time}}"), U"14:05");
    EXPECT_EQ(RenderTemplate(U"{{datetime}}"), U"2026/09/06 14:05");
    EXPECT_EQ(RenderTemplate(U"{{date:yyyy-MM-dd}}"), U"2026-09-06");
    EXPECT_EQ(RenderTemplate(U"{{time:HH:mm:ss}}"), U"14:05:09");
    EXPECT_EQ(RenderTemplate(U"{{date:M月d日(E)}}"), U"9月6日(日)");
    EXPECT_EQ(RenderTemplate(U"{{wareki}}"), U"令和8年9月6日");
    // 日数のずらし（月・年をまたいでも暦どおり）
    EXPECT_EQ(RenderTemplate(U"{{date+1}}"), U"2026/09/07");
    EXPECT_EQ(RenderTemplate(U"{{date-2:M月d日}}"), U"9月4日");
    EXPECT_EQ(RenderTemplate(U"{{date+30}}"), U"2026/10/06");
    EXPECT_EQ(RenderTemplate(U"{{date+120}}"), U"2027/01/04");
    EXPECT_EQ(RenderTemplate(U"{{wareki+3}}"), U"令和8年9月9日");
    EXPECT_EQ(RenderTemplate(U"{{datetime-1}}"), U"2026/09/05 14:05");
    EXPECT_TRUE(iroha::RewriteTemplate(U"{{date+1}}").IsDynamic());
    EXPECT_EQ(RenderTemplate(U"本日（{{date:yyyy/MM/dd}}）{{time:HH:mm}}現在"), U"本日（2026/09/06）14:05現在");
}

// time にはオフセットを付けられず、数字でないオフセットも未対応として書いたまま残す
void TestRewriteUnknownPlaceholders() {
    EXPECT_EQ(RenderTemplate(U"{{time+1}}"), U"{{time+1}}");
    EXPECT_TRUE(iroha::RewriteTemplate(U"{{time+1}}").UnknownPlaceholders() == (std::vector<std::u32string>{U"time+1"}));
    EXPECT_TRUE(iroha::RewriteTemplate(U"{{date+}}").UnknownPlaceholders() == (std::vector<std::u32string>{U"date+"}));
    EXPECT_TRUE(iroha::RewriteTemplate(U"{{date+x}}").UnknownPlaceholders() == (std::vector<std::u32string>{U"date+x"}));
    EXPECT_TRUE(iroha::RewriteTemplate(U"{{date+1-1}}").UnknownPlaceholders() ==
                (std::vector<std::u32string>{U"date+1-1"}));
    const iroha::RewriteTemplate unknown(U"{{unknown:x}}と{{nope}}");
    EXPECT_EQ(unknown.Render(FixedRewriteContext()), U"{{unknown:x}}と{{nope}}");
    EXPECT_TRUE(unknown.UnknownPlaceholders() == (std::vector<std::u32string>{U"unknown", U"nope"}));
    EXPECT_TRUE(!unknown.IsDynamic());
    // パラメータ（無いものは書いたまま）
    const auto context = FixedRewriteContext({{U"n", U"3"}});
    EXPECT_EQ(iroha::RewriteTemplate(U"{{param:n}}日後").Render(context), U"3日後");
    EXPECT_EQ(iroha::RewriteTemplate(U"{{param:m}}").Render(context), U"{{param:m}}");
}

void TestRewriteRuleSet() {
    const auto context = FixedRewriteContext();
    const iroha::UserRewriteRuleSet rules({iroha::UserRewriteRule::Make(U"きょう", U"{{date:yyyy/MM/dd}}"),
                                           iroha::UserRewriteRule::Make(U"いま", U"{{time:HH:mm}}")});
    EXPECT_TRUE(rules.Candidates(U"きょう", context) == (std::vector<std::u32string>{U"2026/09/06"}));
    EXPECT_TRUE(rules.Candidates(U"いま", context) == (std::vector<std::u32string>{U"14:05"}));
    EXPECT_TRUE(rules.Candidates(U"きょうは", context).empty());
    EXPECT_TRUE(rules.Candidates(U"きょ", context).empty());
    // 読みはユーザ辞書と同じく正規化する
    const iroha::UserRewriteRuleSet katakana({iroha::UserRewriteRule::Make(U"キョウ", U"A")});
    EXPECT_EQ(katakana.Rules()[0].trigger, U"きょう");
    EXPECT_TRUE(katakana.Candidates(U" キョウ ", context) == (std::vector<std::u32string>{U"A"}));
    // 登録順で、同じ結果は 1 つにまとめる
    const iroha::UserRewriteRuleSet duplicated({iroha::UserRewriteRule::Make(U"きょう", U"{{date:yyyy/MM/dd}}"),
                                                iroha::UserRewriteRule::Make(U"きょう", U"{{date:M月d日}}"),
                                                iroha::UserRewriteRule::Make(U"きょう", U"{{date:yyyy/MM/dd}}")});
    EXPECT_TRUE(duplicated.Candidates(U"きょう", context) ==
                (std::vector<std::u32string>{U"2026/09/06", U"9月6日"}));
    // 無効・空のルールは使わない（一覧には残る）
    const iroha::UserRewriteRuleSet disabled({iroha::UserRewriteRule::Make(U"きょう", U"X", false),
                                              iroha::UserRewriteRule::Make(U"", U"Y"),
                                              iroha::UserRewriteRule::Make(U"あす", U"")});
    EXPECT_TRUE(disabled.Empty() && disabled.Candidates(U"きょう", context).empty());
    EXPECT_TRUE(disabled.Rules().size() == 3);
}

void TestRewriteStore() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("iroha-rewrite-" + iroha::GenerateUuid() + ".json");
    {
        iroha::UserRewriteRuleStore store(path);
        EXPECT_TRUE(store.Current().Empty());
        store.ReplaceAll({iroha::UserRewriteRule::Make(U"いま", U"{{time}}"),
                          iroha::UserRewriteRule::Make(U"きょう", U"{{date}}", false),
                          iroha::UserRewriteRule::Make(U"", U"捨てられる")});
        iroha::UserRewriteRuleStore reloaded(path);
        EXPECT_TRUE(reloaded.Rules().size() == 2);
        if (reloaded.Rules().size() == 2) {
            EXPECT_EQ(reloaded.Rules()[0].trigger, U"いま");
            EXPECT_TRUE(reloaded.Rules()[0].isEnabled && !reloaded.Rules()[1].isEnabled);
            EXPECT_TRUE(reloaded.Rules()[0].id == store.Rules()[0].id);
            reloaded.SetEnabled(true, reloaded.Rules()[1].id);
            EXPECT_TRUE(reloaded.Current().Candidates(U"きょう", FixedRewriteContext()).size() == 1);
            reloaded.Remove({reloaded.Rules()[0].id});
            EXPECT_TRUE(iroha::UserRewriteRuleStore(path).Rules().size() == 1);
        }
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    // 既定のルールは 1 回だけ足し、ユーザのルールを上書きしない。消した既定ルールは復活させない
    iroha::UserRewriteRuleStore(path).ReplaceAll({iroha::UserRewriteRule::Make(U"きょう", U"{{date:M/d}}")});
    {
        iroha::UserRewriteRuleStore seeded(path, true);
        EXPECT_EQ(seeded.Rules()[0].output, U"{{date:M/d}}");
        size_t today = 0;
        std::set<std::u32string> triggers;
        for (const auto& rule : seeded.Rules()) {
            today += rule.trigger == U"きょう" ? 1 : 0;
            triggers.insert(rule.trigger);
        }
        EXPECT_TRUE(today == 1 && triggers.size() == iroha::UserRewriteRuleStore::DefaultRules().size());
        EXPECT_TRUE(seeded.Current().Candidates(U"あした", FixedRewriteContext()) ==
                    (std::vector<std::u32string>{U"2026/09/07"}));
        std::set<std::string> tomorrow;
        for (const auto& rule : seeded.Rules()) {
            if (rule.trigger == U"あす") tomorrow.insert(rule.id);
        }
        seeded.Remove(tomorrow);
        iroha::UserRewriteRuleStore again(path, true);
        const auto againRules = again.Rules();
        EXPECT_TRUE(std::none_of(againRules.begin(), againRules.end(),
                                 [](const auto& rule) { return rule.trigger == U"あす"; }));
    }
    std::filesystem::remove(path, ignored);

    // 省略できる項目（kind・isEnabled・id）が無くても読める
    {
        std::ofstream file(path, std::ios::binary);
        file << "{\"version\":1,\"rules\":[{\"trigger\":\"キョウ\",\"output\":\"{{date}}\"}]}";
    }
    iroha::UserRewriteRuleStore tolerant(path);
    EXPECT_TRUE(tolerant.Rules().size() == 1);
    if (tolerant.Rules().size() == 1) {
        EXPECT_EQ(tolerant.Rules()[0].trigger, U"きょう");
        EXPECT_TRUE(tolerant.Rules()[0].kind == "exact" && tolerant.Rules()[0].isEnabled &&
                    !tolerant.Rules()[0].id.empty());
    }
    std::filesystem::remove(path, ignored);
}

void RunRewriteTests() {
    TestRewriteParse();
    TestRewriteRenderDates();
    TestRewriteUnknownPlaceholders();
    TestRewriteRuleSet();
    TestRewriteStore();
}

} // namespace
