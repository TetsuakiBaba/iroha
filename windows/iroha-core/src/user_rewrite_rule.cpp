#include "iroha/user_rewrite_rule.h"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <sstream>
#include <tuple>

#include "iroha/json.h"
#include "iroha/unicode.h"
#include "iroha/user_dictionary.h"

namespace iroha {

namespace {

// ---- 暦の計算（年月日 ⇔ 1970-01-01 からの日数。うるう年をまたいでも正しい） ----

long long DaysFromCivil(int year, int month, int day) {
    year -= month <= 2 ? 1 : 0;
    const long long era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}

void CivilFromDays(long long days, int* year, int* month, int* day) {
    days += 719468;
    const long long era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    *month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    *year = static_cast<int>(static_cast<long long>(yoe) + era * 400 + (*month <= 2 ? 1 : 0));
}

RewriteContext AddDays(RewriteContext context, int days) {
    if (days == 0) return context;
    CivilFromDays(DaysFromCivil(context.year, context.month, context.day) + days, &context.year,
                  &context.month, &context.day);
    return context;
}

// 和暦の元号（始まりの日。新しい順）
struct Era {
    const char32_t* name;
    const char32_t* initial;
    int year;
    int month;
    int day;
};
const Era kEras[] = {
    {U"令和", U"R", 2019, 5, 1},
    {U"平成", U"H", 1989, 1, 8},
    {U"昭和", U"S", 1926, 12, 25},
    {U"大正", U"T", 1912, 7, 30},
    {U"明治", U"M", 1868, 9, 8},
};

const Era* EraOf(const RewriteContext& when) {
    for (const Era& era : kEras) {
        if (std::make_tuple(when.year, when.month, when.day) >= std::make_tuple(era.year, era.month, era.day)) {
            return &era;
        }
    }
    return nullptr;
}

std::u32string Number(long long value, size_t width) {
    std::u32string digits = Utf8ToUtf32(std::to_string(value < 0 ? -value : value));
    while (digits.size() < width) digits.insert(digits.begin(), U'0');
    return (value < 0 ? U"-" : U"") + digits;
}

bool IsPatternLetter(char32_t c) { return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z'); }

std::u32string Trimmed(const std::u32string& text) {
    size_t start = 0;
    size_t end = text.size();
    while (start < end && (text[start] == U' ' || text[start] == U'\t')) ++start;
    while (end > start && (text[end - 1] == U' ' || text[end - 1] == U'\t')) --end;
    return text.substr(start, end - start);
}

std::u32string Literal(const RewriteTemplate::Segment& segment) {
    return segment.argument ? U"{{" + segment.text + U":" + *segment.argument + U"}}"
                            : U"{{" + segment.text + U"}}";
}

} // namespace

// ---- ルール ----

std::u32string UserRewriteRule::NormalizedTrigger(const std::u32string& text) {
    return UserDictionary::NormalizedReading(text);
}

UserRewriteRule UserRewriteRule::Make(const std::u32string& trigger, const std::u32string& output,
                                      bool isEnabled) {
    UserRewriteRule rule;
    rule.id = GenerateUuid();
    rule.trigger = NormalizedTrigger(trigger);
    rule.output = output;
    rule.isEnabled = isEnabled;
    return rule;
}

RewriteContext RewriteContext::Now() {
    const std::time_t now = std::time(nullptr);
    std::tm local = {};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    RewriteContext context;
    context.year = local.tm_year + 1900;
    context.month = local.tm_mon + 1;
    context.day = local.tm_mday;
    context.hour = local.tm_hour;
    context.minute = local.tm_min;
    context.second = local.tm_sec;
    return context;
}

// ---- テンプレート ----

RewriteTemplate::RewriteTemplate(std::u32string source) : source_(std::move(source)) {
    std::u32string text;
    size_t position = 0;
    while (position < source_.size()) {
        const size_t open = source_.find(U"{{", position);
        if (open == std::u32string::npos) break;
        const size_t close = source_.find(U"}}", open + 2);
        if (close == std::u32string::npos) break;
        const std::u32string body = source_.substr(open + 2, close - open - 2);
        // 名前と引数は最初の「:」で分ける（引数側の「:」は引数の一部）
        Segment placeholder;
        placeholder.isPlaceholder = true;
        const size_t colon = body.find(U':');
        if (colon != std::u32string::npos) {
            placeholder.text = Trimmed(body.substr(0, colon));
            placeholder.argument = body.substr(colon + 1);
        } else {
            placeholder.text = Trimmed(body);
        }
        text += source_.substr(position, open - position);
        if (placeholder.text.empty()) {
            // 「{{}}」のような空のプレースホルダはそのまま文字として扱う
            text += source_.substr(open, close + 2 - open);
        } else {
            if (!text.empty()) {
                segments_.push_back({false, text, std::nullopt});
                text.clear();
            }
            segments_.push_back(std::move(placeholder));
        }
        position = close + 2;
    }
    text += source_.substr(std::min(position, source_.size()));
    if (!text.empty()) segments_.push_back({false, text, std::nullopt});
}

std::optional<std::pair<std::u32string, int>> RewriteTemplate::Resolve(const std::u32string& name) {
    static const std::set<std::u32string> known = {U"date", U"time", U"datetime", U"wareki", U"param"};
    static const std::set<std::u32string> offsettable = {U"date", U"datetime", U"wareki"};
    if (known.count(name)) return std::make_pair(name, 0);
    const size_t sign = name.find_first_of(U"+-");
    if (sign == std::u32string::npos) return std::nullopt;
    const std::u32string base = name.substr(0, sign);
    const std::u32string digits = name.substr(sign + 1);
    if (!offsettable.count(base) || digits.empty() || digits.size() > 9 ||
        !std::all_of(digits.begin(), digits.end(), [](char32_t c) { return c >= U'0' && c <= U'9'; })) {
        return std::nullopt;
    }
    const int magnitude = std::stoi(Utf32ToUtf8(digits));
    return std::make_pair(base, name[sign] == U'-' ? -magnitude : magnitude);
}

bool RewriteTemplate::IsDynamic() const {
    return std::any_of(segments_.begin(), segments_.end(), [](const Segment& segment) {
        return segment.isPlaceholder && Resolve(segment.text).has_value();
    });
}

std::vector<std::u32string> RewriteTemplate::UnknownPlaceholders() const {
    std::vector<std::u32string> names;
    for (const Segment& segment : segments_) {
        if (segment.isPlaceholder && !Resolve(segment.text)) names.push_back(segment.text);
    }
    return names;
}

std::u32string RewriteTemplate::Render(const RewriteContext& context) const {
    std::u32string result;
    for (const Segment& segment : segments_) {
        if (!segment.isPlaceholder) {
            result += segment.text;
            continue;
        }
        const auto resolved = Resolve(segment.text);
        if (!resolved) {
            result += Literal(segment);
            continue;
        }
        const std::u32string& base = resolved->first;
        const RewriteContext shifted = AddDays(context, resolved->second);
        if (base == U"date") {
            result += FormatDate(segment.argument.value_or(U"yyyy/MM/dd"), shifted, false);
        } else if (base == U"time") {
            result += FormatDate(segment.argument.value_or(U"HH:mm"), context, false);
        } else if (base == U"datetime") {
            result += FormatDate(segment.argument.value_or(U"yyyy/MM/dd HH:mm"), shifted, false);
        } else if (base == U"wareki") {
            result += FormatDate(segment.argument.value_or(U"Gy年M月d日"), shifted, true);
        } else if (base == U"param") {
            const auto found = segment.argument ? context.parameters.find(*segment.argument)
                                                : context.parameters.end();
            result += found != context.parameters.end() ? found->second : Literal(segment);
        }
    }
    return result;
}

// ICU の日付の書式のうち、よく使うものを日本語で書く（macOS版の DateFormatter（ja_JP）と同じ表記）
std::u32string RewriteTemplate::FormatDate(const std::u32string& format, const RewriteContext& when,
                                           bool japaneseCalendar) {
    static const char32_t* const kWeekdays[] = {U"日", U"月", U"火", U"水", U"木", U"金", U"土"};
    const Era* era = japaneseCalendar ? EraOf(when) : nullptr;
    const int year = era ? when.year - era->year + 1 : when.year;
    // 1970-01-01 は木曜日
    const long long days = DaysFromCivil(when.year, when.month, when.day);
    const int weekday = static_cast<int>(((days % 7) + 7 + 4) % 7);

    std::u32string out;
    size_t i = 0;
    while (i < format.size()) {
        const char32_t c = format[i];
        if (c == U'\'') {
            // 引用符の中はそのまま（'' は引用符そのもの）
            if (i + 1 < format.size() && format[i + 1] == U'\'') {
                out += U'\'';
                i += 2;
                continue;
            }
            size_t j = i + 1;
            while (j < format.size()) {
                if (format[j] == U'\'') {
                    if (j + 1 < format.size() && format[j + 1] == U'\'') {
                        out += U'\'';
                        j += 2;
                        continue;
                    }
                    break;
                }
                out += format[j++];
            }
            i = j + 1;
            continue;
        }
        if (!IsPatternLetter(c)) {
            out += c;
            ++i;
            continue;
        }
        size_t count = 1;
        while (i + count < format.size() && format[i + count] == c) ++count;
        switch (c) {
        case U'y':
            out += count == 2 ? Number(year % 100, 2) : Number(year, count);
            break;
        case U'M':
        case U'L':
            out += count >= 3 ? Number(when.month, 1) + U"月" : Number(when.month, count);
            break;
        case U'd':
            out += Number(when.day, count);
            break;
        case U'H':
            out += Number(when.hour, count);
            break;
        case U'h':
            out += Number(when.hour % 12 == 0 ? 12 : when.hour % 12, count);
            break;
        case U'm':
            out += Number(when.minute, count);
            break;
        case U's':
            out += Number(when.second, count);
            break;
        case U'E':
        case U'e':
        case U'c':
            out += kWeekdays[weekday];
            if (count == 4) out += U"曜日";
            break;
        case U'G':
            if (era) {
                out += count == 5 ? era->initial : era->name;
            } else {
                out += count == 5 ? U"AD" : U"西暦";
            }
            break;
        case U'a':
            out += when.hour < 12 ? U"午前" : U"午後";
            break;
        default:
            // 対応していない書式記号は書いたまま残す
            out += format.substr(i, count);
            break;
        }
        i += count;
    }
    return out;
}

// ---- ルール集合 ----

UserRewriteRuleSet::UserRewriteRuleSet(std::vector<UserRewriteRule> rules) : rules_(std::move(rules)) {
    for (size_t i = 0; i < rules_.size(); ++i) {
        const UserRewriteRule& rule = rules_[i];
        if (rule.isEnabled && !rule.trigger.empty() && !rule.output.empty() && rule.kind == "exact") {
            exact_[rule.trigger].push_back(i);
        }
    }
}

std::vector<std::u32string> UserRewriteRuleSet::Candidates(const std::u32string& reading,
                                                           const RewriteContext& context) const {
    std::vector<std::u32string> results;
    if (exact_.empty()) return results;
    const auto found = exact_.find(UserRewriteRule::NormalizedTrigger(reading));
    if (found == exact_.end()) return results;
    for (size_t index : found->second) {
        const std::u32string rendered = RewriteTemplate(rules_[index].output).Render(context);
        if (rendered.empty() || std::find(results.begin(), results.end(), rendered) != results.end()) {
            continue;
        }
        results.push_back(rendered);
    }
    return results;
}

// ---- 永続化 ----

const std::vector<std::pair<std::u32string, std::u32string>>& UserRewriteRuleStore::DefaultRules() {
    static const std::vector<std::pair<std::u32string, std::u32string>> rules = {
        {U"きょう", U"{{date}}"},       {U"きのう", U"{{date-1}}"},   {U"おととい", U"{{date-2}}"},
        {U"あした", U"{{date+1}}"},     {U"あす", U"{{date+1}}"},     {U"あさって", U"{{date+2}}"},
        {U"しあさって", U"{{date+3}}"}, {U"いま", U"{{time}}"},
    };
    return rules;
}

namespace {

struct RuleFile {
    std::vector<UserRewriteRule> rules;
    int defaultsVersion = 0;
};

RuleFile LoadRuleFile(const std::filesystem::path& path) {
    RuleFile file;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return file;
    std::stringstream buffer;
    buffer << stream.rdbuf();
    json::Value root;
    if (!json::Parse(buffer.str(), &root) || root.type != json::Value::Type::Object) return file;
    if (const json::Value* version = root.Find("defaultsVersion");
        version && version->type == json::Value::Type::Number) {
        file.defaultsVersion = static_cast<int>(version->number);
    }
    const json::Value* rules = root.Find("rules");
    if (!rules || rules->type != json::Value::Type::Array) return file;
    for (const json::Value& item : rules->array) {
        const json::Value* trigger = item.Find("trigger");
        const json::Value* output = item.Find("output");
        if (!trigger || !output) continue;
        UserRewriteRule rule;
        const json::Value* id = item.Find("id");
        rule.id = id && !id->AsString().empty() ? id->AsString() : GenerateUuid();
        if (const json::Value* kind = item.Find("kind"); kind && !kind->AsString().empty()) {
            rule.kind = kind->AsString();
        }
        rule.trigger = UserRewriteRule::NormalizedTrigger(Utf8ToUtf32(trigger->AsString()));
        rule.output = Utf8ToUtf32(output->AsString());
        if (const json::Value* enabled = item.Find("isEnabled");
            enabled && enabled->type == json::Value::Type::Bool) {
            rule.isEnabled = enabled->boolean;
        }
        file.rules.push_back(std::move(rule));
    }
    return file;
}

void SaveRuleFile(const std::filesystem::path& path, const std::vector<UserRewriteRule>& rules,
                  int defaultsVersion) {
    // macOS版の JSONEncoder（sortedKeys）と同じくキーの順に並べる
    std::string out = "{\"defaultsVersion\":" + std::to_string(defaultsVersion) + ",\"rules\":[";
    for (size_t i = 0; i < rules.size(); ++i) {
        const UserRewriteRule& rule = rules[i];
        if (i > 0) out += ",";
        out += "{\"id\":" + json::QuoteString(rule.id) + ",\"isEnabled\":" + (rule.isEnabled ? "true" : "false") +
               ",\"kind\":" + json::QuoteString(rule.kind) +
               ",\"output\":" + json::QuoteString(Utf32ToUtf8(rule.output)) +
               ",\"trigger\":" + json::QuoteString(Utf32ToUtf8(rule.trigger)) + "}";
    }
    out += "],\"version\":1}\n";
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    const std::filesystem::path temporary = path.wstring() + L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return;
        stream.write(out.data(), static_cast<std::streamsize>(out.size()));
    }
    std::filesystem::rename(temporary, path, error);
}

} // namespace

UserRewriteRuleStore::UserRewriteRuleStore(std::filesystem::path path, bool seedsDefaults)
    : path_(std::move(path)) {
    RuleFile file = LoadRuleFile(path_);
    cached_ = UserRewriteRuleSet(std::move(file.rules));
    defaultsVersion_ = file.defaultsVersion;
    if (seedsDefaults && defaultsVersion_ < kDefaultRulesVersion) {
        // まだ無いトリガーだけを足す（ユーザが消した既定ルールを復活させないため、世代が上がったときの1回だけ）
        std::vector<UserRewriteRule> rules = cached_.Rules();
        std::set<std::u32string> existing;
        for (const UserRewriteRule& rule : rules) existing.insert(rule.trigger);
        for (const auto& item : DefaultRules()) {
            if (!existing.count(item.first)) rules.push_back(UserRewriteRule::Make(item.first, item.second));
        }
        defaultsVersion_ = kDefaultRulesVersion;
        Store(std::move(rules));
    }
}

UserRewriteRuleSet UserRewriteRuleStore::Current() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cached_;
}

void UserRewriteRuleStore::ReplaceAll(std::vector<UserRewriteRule> rules) { Store(std::move(rules)); }

void UserRewriteRuleStore::Add(const std::u32string& trigger, const std::u32string& output) {
    std::vector<UserRewriteRule> rules = Rules();
    rules.push_back(UserRewriteRule::Make(trigger, output));
    Store(std::move(rules));
}

void UserRewriteRuleStore::Remove(const std::set<std::string>& ids) {
    std::vector<UserRewriteRule> rules = Rules();
    rules.erase(std::remove_if(rules.begin(), rules.end(),
                               [&ids](const UserRewriteRule& rule) { return ids.count(rule.id) != 0; }),
                rules.end());
    Store(std::move(rules));
}

void UserRewriteRuleStore::SetEnabled(bool isEnabled, const std::string& id) {
    std::vector<UserRewriteRule> rules = Rules();
    for (UserRewriteRule& rule : rules) {
        if (rule.id == id) rule.isEnabled = isEnabled;
    }
    Store(std::move(rules));
}

void UserRewriteRuleStore::Reload() {
    RuleFile file = LoadRuleFile(path_);
    std::lock_guard<std::mutex> lock(mutex_);
    cached_ = UserRewriteRuleSet(std::move(file.rules));
    defaultsVersion_ = std::max(defaultsVersion_, file.defaultsVersion);
}

void UserRewriteRuleStore::Store(std::vector<UserRewriteRule> rules) {
    // トリガーが空・出力が空のものは落とす（編集途中の空行は保存しない）
    std::vector<UserRewriteRule> cleaned;
    for (UserRewriteRule& rule : rules) {
        rule.trigger = UserRewriteRule::NormalizedTrigger(rule.trigger);
        if (!rule.trigger.empty() && !rule.output.empty()) cleaned.push_back(std::move(rule));
    }
    int defaultsVersion = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cached_ = UserRewriteRuleSet(cleaned);
        defaultsVersion = defaultsVersion_;
    }
    SaveRuleFile(path_, cleaned, defaultsVersion);
}

} // namespace iroha
