#pragma once
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace iroha {

// ユーザ定義の変換ルール（User Rewriter）の1件。
// 移植元: macos/Sources/IrohaCore/UserRewriteRule.swift・UserRewriteRuleStore.swift（挙動互換を維持すること）
//
// 読み（トリガー）が一致したとき、テンプレート（出力）を展開した文字列を変換候補に加える。
// ユーザ辞書と違い出力は変換のたびに計算されるので、日付や時刻のように動的に変わる文字列を扱える
// （{{date:yyyy/MM/dd}} など。書式は RewriteTemplate を参照）。今はトリガーの完全一致だけ
struct UserRewriteRule {
    std::string id; // UUID（大文字の16進とハイフン。macOS版の UUID の JSON と同じ形）
    std::string kind = "exact";
    std::u32string trigger; // ひらがなに正規化して保持する
    std::u32string output;  // 出力テンプレート
    bool isEnabled = true;

    // トリガーの正規化はユーザ辞書の読みと同じ（カタカナ→ひらがな、前後の空白除去）
    static std::u32string NormalizedTrigger(const std::u32string& text);
    static UserRewriteRule Make(const std::u32string& trigger, const std::u32string& output,
                                bool isEnabled = true);
};

// テンプレート展開時の環境（日時はその地域の時刻で持つ）。parameters はトリガーの一致で得た値
struct RewriteContext {
    int year = 2026;
    int month = 1;
    int day = 1;
    int hour = 0;
    int minute = 0;
    int second = 0;
    std::map<std::u32string, std::u32string> parameters;

    // 今の時刻（このコンピュータの時間帯）
    static RewriteContext Now();
};

// 出力テンプレート。通常の文字列と {{名前}} / {{名前:引数}} のプレースホルダを混在できる。
//
// 対応するプレースホルダ（引数は省略可。日時の書式は Unicode/ICU のパターンのうち y M d H h m s E G a）:
// - {{date:yyyy/MM/dd}}     今日の日付（既定 yyyy/MM/dd）
// - {{time:HH:mm}}          現在時刻（既定 HH:mm）
// - {{datetime:yyyy/MM/dd HH:mm}} 日付と時刻（既定 yyyy/MM/dd HH:mm）
// - {{wareki:Gy年M月d日}}   和暦の日付（既定 Gy年M月d日 → 令和8年9月6日）
// - {{param:名前}}          トリガーの一致で得た値
// date / datetime / wareki は名前に +N / -N を付けてN日ずらせる（{{date+1}} 明日）。
// 知らないプレースホルダや閉じていない {{ は書いたまま文字列として残す
class RewriteTemplate {
public:
    struct Segment {
        bool isPlaceholder = false;
        std::u32string text; // 文字列、またはプレースホルダの名前
        std::optional<std::u32string> argument;
        bool operator==(const Segment& other) const {
            return isPlaceholder == other.isPlaceholder && text == other.text && argument == other.argument;
        }
    };

    explicit RewriteTemplate(std::u32string source);

    const std::vector<Segment>& Segments() const { return segments_; }
    // 展開結果が呼ぶたびに変わりうるか（日時などを含む）
    bool IsDynamic() const;
    // 対応していないプレースホルダ名（設定画面の警告用）
    std::vector<std::u32string> UnknownPlaceholders() const;
    std::u32string Render(const RewriteContext& context) const;

    // プレースホルダ名を基本名と日数オフセットに分ける（"date+1" → ("date", 1)）。対応していなければ nullopt
    static std::optional<std::pair<std::u32string, int>> Resolve(const std::u32string& name);

    // ICU の書式で日時を書く（暦は西暦か和暦）
    static std::u32string FormatDate(const std::u32string& format, const RewriteContext& when,
                                     bool japaneseCalendar);

private:
    std::u32string source_;
    std::vector<Segment> segments_;
};

// 候補生成時に参照する変換ルールの不変のスナップショット（有効なルールの索引つき）。
// かな漢字変換エンジンとは独立した候補生成源で、文節の候補ウィンドウを開くときに合流させる
// （第一候補の直後。ライブ変換には使わない）
class UserRewriteRuleSet {
public:
    UserRewriteRuleSet() = default;
    explicit UserRewriteRuleSet(std::vector<UserRewriteRule> rules);

    // 全ルール（無効なものも含む。設定画面の一覧用、登録順）
    const std::vector<UserRewriteRule>& Rules() const { return rules_; }
    // 有効なルールが1つもないか
    bool Empty() const { return exact_.empty(); }
    // 読みに一致するルールの出力を展開した候補（登録順、重複と空は除く）
    std::vector<std::u32string> Candidates(const std::u32string& reading,
                                           const RewriteContext& context) const;

private:
    std::vector<UserRewriteRule> rules_;
    std::unordered_map<std::u32string, std::vector<size_t>> exact_;
};

// ユーザ定義の変換ルールの永続化（<データフォルダ>/user-rewrite-rules.json。macOS版と同じ形式）。
// ルールの並び順は候補の並び順になるので、登録順をそのまま保つ
class UserRewriteRuleStore {
public:
    // 既定ルールの世代。既定に項目を足したらこの値を上げる（macOS版と同じ値）
    static constexpr int kDefaultRulesVersion = 1;
    // 初回（および世代が上がったとき）に用意する既定のルール
    static const std::vector<std::pair<std::u32string, std::u32string>>& DefaultRules();

    // seedsDefaults: 既定ルールをまだ入れていなければ追加する（IME本体は真。テストで空のストアが欲しいときは偽）
    explicit UserRewriteRuleStore(std::filesystem::path path, bool seedsDefaults = false);

    UserRewriteRuleSet Current() const;
    std::vector<UserRewriteRule> Rules() const { return Current().Rules(); }
    // 一覧をまるごと置き換えて保存する（トリガー・出力が空のものは落とす）
    void ReplaceAll(std::vector<UserRewriteRule> rules);
    void Add(const std::u32string& trigger, const std::u32string& output);
    void Remove(const std::set<std::string>& ids);
    void SetEnabled(bool isEnabled, const std::string& id);
    // ファイルを読み直す（設定画面が書き換えたあと）
    void Reload();

private:
    void Store(std::vector<UserRewriteRule> rules);

    std::filesystem::path path_;
    mutable std::mutex mutex_;
    UserRewriteRuleSet cached_;
    int defaultsVersion_ = 0;
};

} // namespace iroha
