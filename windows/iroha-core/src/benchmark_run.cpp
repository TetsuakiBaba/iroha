#include "iroha/benchmark_run.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

#include "iroha/json.h"
#include "iroha/unicode.h"

namespace iroha::benchmark_run {

namespace {

using json::MakeArray;
using json::MakeBool;
using json::MakeNumber;
using json::MakeObject;
using json::MakeString;
using json::Value;

// ---- 値を読む小道具 ----

std::optional<std::string> StringOf(const Value& object, const char* key) {
    const Value* value = object.Find(key);
    if (!value || value->type != Value::Type::String) return std::nullopt;
    return value->string;
}

std::optional<double> NumberOf(const Value& object, const char* key) {
    const Value* value = object.Find(key);
    if (!value || value->type != Value::Type::Number) return std::nullopt;
    return value->number;
}

std::optional<int> IntOf(const Value& object, const char* key) {
    const auto number = NumberOf(object, key);
    if (!number) return std::nullopt;
    return static_cast<int>(*number);
}

std::optional<bool> BoolOf(const Value& object, const char* key) {
    const Value* value = object.Find(key);
    if (!value || value->type != Value::Type::Bool) return std::nullopt;
    return value->boolean;
}

void SetIf(Value* object, const char* key, const std::optional<std::string>& value) {
    if (value) object->Set(key, MakeString(*value));
}

// ---- 日付 ----

// 1970-01-01 からの日数（グレゴリオ暦。Howard Hinnant の days_from_civil）
int64_t DaysFromCivil(int64_t year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
    const unsigned dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097 + static_cast<int64_t>(dayOfEra) - 719468;
}

void CivilFromDays(int64_t days, int64_t* year, unsigned* month, unsigned* day) {
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned dayOfEra = static_cast<unsigned>(days - era * 146097);
    const unsigned yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    const unsigned dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const unsigned mp = (5 * dayOfYear + 2) / 153;
    *day = dayOfYear - (153 * mp + 2) / 5 + 1;
    *month = mp < 10 ? mp + 3 : mp - 9;
    *year = static_cast<int64_t>(yearOfEra) + era * 400 + (*month <= 2);
}

bool LocalTime(int64_t seconds, std::tm* out) {
    const std::time_t time = static_cast<std::time_t>(seconds);
#ifdef _WIN32
    return localtime_s(out, &time) == 0;
#else
    return localtime_r(&time, out) != nullptr;
#endif
}

// ---- 集計の JSON ----

Value TallyValue(const benchmark::Tally& tally) {
    Value value = MakeObject();
    value.Set("count", MakeNumber(tally.count));
    value.Set("hits", MakeNumber(tally.hits));
    value.Set("minCERSum", MakeNumber(tally.minCERSum));
    return value;
}

benchmark::Tally ReadTally(const Value& value) {
    benchmark::Tally tally;
    tally.count = IntOf(value, "count").value_or(0);
    tally.hits = IntOf(value, "hits").value_or(0);
    tally.minCERSum = NumberOf(value, "minCERSum").value_or(0);
    return tally;
}

Value SummaryValue(const benchmark::Summary& summary) {
    Value value = MakeObject();
    value.Set("total", TallyValue(summary.total));
    value.Set("withContext", TallyValue(summary.withContext));
    value.Set("withoutContext", TallyValue(summary.withoutContext));
    Value perSource = MakeArray();
    for (const auto& source : summary.perSource) {
        Value entry = MakeObject();
        entry.Set("source", MakeString(source.source));
        entry.Set("tally", TallyValue(source.tally));
        perSource.array.push_back(std::move(entry));
    }
    value.Set("perSource", std::move(perSource));
    Value latency = MakeObject();
    latency.Set("count", MakeNumber(summary.latency.count));
    latency.Set("mean", MakeNumber(summary.latency.mean));
    latency.Set("p50", MakeNumber(summary.latency.p50));
    latency.Set("p90", MakeNumber(summary.latency.p90));
    latency.Set("p99", MakeNumber(summary.latency.p99));
    latency.Set("max", MakeNumber(summary.latency.max));
    value.Set("latency", std::move(latency));
    Value bins = MakeArray();
    for (const auto& bin : summary.lengthBins) {
        Value entry = MakeObject();
        entry.Set("lower", MakeNumber(bin.lower));
        if (bin.upper) entry.Set("upper", MakeNumber(*bin.upper));
        entry.Set("count", MakeNumber(bin.count));
        entry.Set("p50", MakeNumber(bin.p50));
        bins.array.push_back(std::move(entry));
    }
    value.Set("lengthBins", std::move(bins));
    if (summary.neuralNetworkShare) value.Set("neuralNetworkShare", MakeNumber(*summary.neuralNetworkShare));
    return value;
}

std::optional<benchmark::Summary> ReadSummary(const Value& value) {
    if (value.type != Value::Type::Object) return std::nullopt;
    const Value* total = value.Find("total");
    const Value* latency = value.Find("latency");
    if (!total || !latency) return std::nullopt;
    benchmark::Summary summary;
    summary.total = ReadTally(*total);
    if (const Value* with = value.Find("withContext")) summary.withContext = ReadTally(*with);
    if (const Value* without = value.Find("withoutContext")) summary.withoutContext = ReadTally(*without);
    if (const Value* perSource = value.Find("perSource")) {
        for (const Value& entry : perSource->array) {
            const auto source = StringOf(entry, "source");
            const Value* tally = entry.Find("tally");
            if (source && tally) summary.perSource.push_back({*source, ReadTally(*tally)});
        }
    }
    summary.latency.count = IntOf(*latency, "count").value_or(0);
    summary.latency.mean = NumberOf(*latency, "mean").value_or(0);
    summary.latency.p50 = NumberOf(*latency, "p50").value_or(0);
    summary.latency.p90 = NumberOf(*latency, "p90").value_or(0);
    summary.latency.p99 = NumberOf(*latency, "p99").value_or(0);
    summary.latency.max = NumberOf(*latency, "max").value_or(0);
    if (const Value* bins = value.Find("lengthBins")) {
        for (const Value& entry : bins->array) {
            benchmark::LengthBin bin;
            bin.lower = IntOf(entry, "lower").value_or(0);
            bin.upper = IntOf(entry, "upper");
            bin.count = IntOf(entry, "count").value_or(0);
            bin.p50 = NumberOf(entry, "p50").value_or(0);
            summary.lengthBins.push_back(bin);
        }
    }
    summary.neuralNetworkShare = NumberOf(value, "neuralNetworkShare");
    return summary;
}

Value EnvironmentValue(const Environment& environment) {
    Value value = MakeObject();
    value.Set("platform", MakeString(environment.platform));
    value.Set("appVersion", MakeString(environment.appVersion));
    value.Set("hostName", MakeString(environment.hostName));
    SetIf(&value, "machineModel", environment.machineModel);
    value.Set("cpu", MakeString(environment.cpu));
    if (environment.cpuCores) value.Set("cpuCores", MakeNumber(*environment.cpuCores));
    value.Set("osVersion", MakeString(environment.osVersion));
    SetIf(&value, "powerSource", environment.powerSource);
    if (environment.powerSaving) value.Set("powerSaving", MakeBool(*environment.powerSaving));
    SetIf(&value, "thermalStateBefore", environment.thermalStateBefore);
    SetIf(&value, "thermalStateAfter", environment.thermalStateAfter);
    return value;
}

std::optional<Environment> ReadEnvironment(const Value& value) {
    if (value.type != Value::Type::Object) return std::nullopt;
    Environment environment;
    // 形式 1（macOS だけ）は platform が無く、cpu を chip、powerSaving を lowPowerMode と書いていた
    environment.platform = StringOf(value, "platform").value_or("macos");
    const auto appVersion = StringOf(value, "appVersion");
    const auto hostName = StringOf(value, "hostName");
    auto cpu = StringOf(value, "cpu");
    if (!cpu) cpu = StringOf(value, "chip");
    const auto osVersion = StringOf(value, "osVersion");
    if (!appVersion || !hostName || !cpu || !osVersion) return std::nullopt;
    environment.appVersion = *appVersion;
    environment.hostName = *hostName;
    environment.machineModel = StringOf(value, "machineModel");
    environment.cpu = *cpu;
    environment.cpuCores = IntOf(value, "cpuCores");
    environment.osVersion = *osVersion;
    environment.powerSource = StringOf(value, "powerSource");
    environment.powerSaving = BoolOf(value, "powerSaving");
    if (!environment.powerSaving) environment.powerSaving = BoolOf(value, "lowPowerMode");
    environment.thermalStateBefore = StringOf(value, "thermalStateBefore");
    environment.thermalStateAfter = StringOf(value, "thermalStateAfter");
    return environment;
}

bool WriteAtomically(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!file) return false;
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

std::string Format(const char* format, double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), format, value);
    return buffer;
}

} // namespace

// ---- 条件・環境 ----

const char* LatticeName(Lattice lattice) { return lattice == Lattice::Always ? "always" : "off"; }

std::optional<Lattice> ParseLattice(const std::string& name) {
    if (name == "off") return Lattice::Off;
    if (name == "always") return Lattice::Always;
    return std::nullopt;
}

const char* LatticeDisplayName(Lattice lattice) {
    return lattice == Lattice::Always ? "辞書ラティス + 再採点" : "モデルだけ";
}

std::string Condition::SummaryText() const {
    std::string engine = backend == "mlx" ? "MLX" : "llama.cpp";
    std::vector<std::string> detail;
    if (device) detail.push_back(*device);
    if (threads) detail.push_back(std::to_string(*threads) + " スレッド");
    if (!detail.empty()) {
        engine += "（";
        for (size_t i = 0; i < detail.size(); ++i) {
            if (i > 0) engine += "、";
            engine += detail[i];
        }
        engine += "）";
    }
    return std::string(LatticeDisplayName(lattice)) + "、" + engine + "、モデル " + model +
           (adapter ? "＋アダプタ " + *adapter : std::string());
}

std::optional<std::string> Environment::ThrottlingNote() const {
    std::vector<std::string> notes;
    if (powerSource && *powerSource == "battery") notes.push_back("電源 バッテリー");
    if (powerSaving && *powerSaving) notes.push_back("省電力の設定");
    if (thermalStateBefore && thermalStateAfter &&
        (*thermalStateBefore != "nominal" || *thermalStateAfter != "nominal")) {
        notes.push_back("発熱の状態 " + *thermalStateBefore + " → " + *thermalStateAfter);
    }
    if (notes.empty()) return std::nullopt;
    std::string text;
    for (size_t i = 0; i < notes.size(); ++i) {
        if (i > 0) text += "・";
        text += notes[i];
    }
    return text;
}

std::vector<Miss> Record::Misses(const std::vector<benchmark::Item>& items,
                                 const std::vector<benchmark::ItemResult>& results) {
    std::vector<Miss> misses;
    for (size_t i = 0; i < items.size() && i < results.size(); ++i) {
        if (results[i].hit) continue;
        Miss miss;
        miss.index = items[i].index;
        miss.context = Utf32ToUtf8(items[i].contextText);
        miss.input = Utf32ToUtf8(items[i].input);
        miss.output = Utf32ToUtf8(results[i].output);
        for (const auto& expected : items[i].expectedOutput) miss.expected.push_back(Utf32ToUtf8(expected));
        misses.push_back(std::move(miss));
    }
    return misses;
}

// ---- JSON ----

std::string EncodeSummary(const benchmark::Summary& summary) { return json::Serialize(SummaryValue(summary), true); }

std::string EncodeEnvironment(const Environment& environment) {
    return json::Serialize(EnvironmentValue(environment), true);
}

std::optional<Environment> DecodeEnvironment(const std::string& text) {
    Value root;
    if (!json::Parse(text, &root)) return std::nullopt;
    return ReadEnvironment(root);
}

std::string EncodeRecord(const Record& record) {
    Value root = MakeObject();
    if (record.formatVersion) root.Set("formatVersion", MakeNumber(*record.formatVersion));
    root.Set("id", MakeString(record.id));
    root.Set("benchmarkID", MakeString(record.benchmarkID));
    root.Set("benchmarkName", MakeString(record.benchmarkName));
    Value condition = MakeObject();
    condition.Set("lattice", MakeString(LatticeName(record.condition.lattice)));
    condition.Set("backend", MakeString(record.condition.backend));
    condition.Set("model", MakeString(record.condition.model));
    SetIf(&condition, "adapter", record.condition.adapter);
    SetIf(&condition, "device", record.condition.device);
    if (record.condition.threads) condition.Set("threads", MakeNumber(*record.condition.threads));
    root.Set("condition", std::move(condition));
    root.Set("environment", EnvironmentValue(record.environment));
    root.Set("startedAt", MakeString(FormatIso8601(record.startedAt)));
    root.Set("finishedAt", MakeString(FormatIso8601(record.finishedAt)));
    root.Set("warmUpMilliseconds", MakeNumber(record.warmUpMilliseconds));
    if (record.inputDuringRun) root.Set("inputDuringRun", MakeBool(*record.inputDuringRun));
    root.Set("summary", SummaryValue(record.summary));
    Value misses = MakeArray();
    for (const Miss& miss : record.misses) {
        Value entry = MakeObject();
        entry.Set("index", MakeString(miss.index));
        entry.Set("context", MakeString(miss.context));
        entry.Set("input", MakeString(miss.input));
        entry.Set("output", MakeString(miss.output));
        Value expected = MakeArray();
        for (const std::string& reference : miss.expected) expected.array.push_back(MakeString(reference));
        entry.Set("expected", std::move(expected));
        misses.array.push_back(std::move(entry));
    }
    root.Set("misses", std::move(misses));
    return json::Serialize(root, true);
}

std::optional<Record> DecodeRecord(const std::string& text) {
    Value root;
    if (!json::Parse(text, &root) || root.type != Value::Type::Object) return std::nullopt;
    Record record;
    record.formatVersion = IntOf(root, "formatVersion");
    const auto id = StringOf(root, "id");
    const auto benchmarkID = StringOf(root, "benchmarkID");
    const auto benchmarkName = StringOf(root, "benchmarkName");
    const Value* condition = root.Find("condition");
    const Value* environment = root.Find("environment");
    const auto startedAt = StringOf(root, "startedAt");
    const auto finishedAt = StringOf(root, "finishedAt");
    const Value* summary = root.Find("summary");
    if (!id || !benchmarkID || !benchmarkName || !condition || !environment || !startedAt || !finishedAt ||
        !summary) {
        return std::nullopt;
    }
    record.id = *id;
    record.benchmarkID = *benchmarkID;
    record.benchmarkName = *benchmarkName;
    const auto lattice = ParseLattice(StringOf(*condition, "lattice").value_or(""));
    const auto backend = StringOf(*condition, "backend");
    const auto model = StringOf(*condition, "model");
    if (!lattice || !backend || !model) return std::nullopt;
    record.condition.lattice = *lattice;
    record.condition.backend = *backend;
    record.condition.model = *model;
    record.condition.adapter = StringOf(*condition, "adapter");
    record.condition.device = StringOf(*condition, "device");
    record.condition.threads = IntOf(*condition, "threads");
    const auto readEnvironment = ReadEnvironment(*environment);
    const auto started = ParseIso8601(*startedAt);
    const auto finished = ParseIso8601(*finishedAt);
    const auto readSummary = ReadSummary(*summary);
    if (!readEnvironment || !started || !finished || !readSummary) return std::nullopt;
    record.environment = *readEnvironment;
    record.startedAt = *started;
    record.finishedAt = *finished;
    record.warmUpMilliseconds = NumberOf(root, "warmUpMilliseconds").value_or(0);
    record.inputDuringRun = BoolOf(root, "inputDuringRun");
    record.summary = *readSummary;
    if (const Value* misses = root.Find("misses")) {
        for (const Value& entry : misses->array) {
            Miss miss;
            miss.index = StringOf(entry, "index").value_or("");
            miss.context = StringOf(entry, "context").value_or("");
            miss.input = StringOf(entry, "input").value_or("");
            miss.output = StringOf(entry, "output").value_or("");
            if (const Value* expected = entry.Find("expected")) {
                for (const Value& reference : expected->array) miss.expected.push_back(reference.AsString());
            }
            record.misses.push_back(std::move(miss));
        }
    }
    return record;
}

std::string FormatIso8601(int64_t seconds) {
    const int64_t days = (seconds >= 0 ? seconds : seconds - 86399) / 86400;
    const int64_t rest = seconds - days * 86400;
    int64_t year = 0;
    unsigned month = 0;
    unsigned day = 0;
    CivilFromDays(days, &year, &month, &day);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04lld-%02u-%02uT%02lld:%02lld:%02lldZ", static_cast<long long>(year),
                  month, day, static_cast<long long>(rest / 3600), static_cast<long long>(rest / 60 % 60),
                  static_cast<long long>(rest % 60));
    return buffer;
}

std::optional<int64_t> ParseIso8601(const std::string& text) {
    size_t position = 0;
    // 決まった桁数の数字を読む
    const auto digits = [&](size_t count, int* out) {
        if (position + count > text.size()) return false;
        int value = 0;
        for (size_t i = 0; i < count; ++i) {
            const char c = text[position + i];
            if (c < '0' || c > '9') return false;
            value = value * 10 + (c - '0');
        }
        position += count;
        *out = value;
        return true;
    };
    const auto expect = [&](const char* allowed) {
        if (position >= text.size()) return false;
        for (const char* c = allowed; *c; ++c) {
            if (text[position] == *c) {
                ++position;
                return true;
            }
        }
        return false;
    };
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (!digits(4, &year) || !expect("-") || !digits(2, &month) || !expect("-") || !digits(2, &day) ||
        !expect("Tt ") || !digits(2, &hour) || !expect(":") || !digits(2, &minute) || !expect(":") ||
        !digits(2, &second)) {
        return std::nullopt;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31) return std::nullopt;
    // 秒の小数部は捨てる
    if (position < text.size() && text[position] == '.') {
        ++position;
        while (position < text.size() && text[position] >= '0' && text[position] <= '9') ++position;
    }
    int64_t offset = 0;
    if (position < text.size() && (text[position] == '+' || text[position] == '-')) {
        const int sign = text[position] == '-' ? -1 : 1;
        ++position;
        int offsetHour = 0, offsetMinute = 0;
        if (!digits(2, &offsetHour)) return std::nullopt;
        if (expect(":")) {
            if (!digits(2, &offsetMinute)) return std::nullopt;
        } else if (!digits(2, &offsetMinute)) {
            offsetMinute = 0;
        }
        offset = sign * (offsetHour * 3600 + offsetMinute * 60);
    } else if (!expect("Zz")) {
        return std::nullopt;
    }
    return DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400 + hour * 3600 +
           minute * 60 + second - offset;
}

// ---- 置き場所 ----

std::filesystem::path Directory(const std::filesystem::path& dataDirectory) {
    return dataDirectory / "logs" / "bench";
}

std::filesystem::path RecordPath(const std::filesystem::path& dataDirectory, const std::string& id) {
    return Directory(dataDirectory) / std::filesystem::u8path(id + ".json");
}

std::filesystem::path TimingsPath(const std::filesystem::path& dataDirectory, const std::string& id) {
    return Directory(dataDirectory) / std::filesystem::u8path(id + ".tsv");
}

std::string MakeId(int64_t seconds, const std::string& hostName) {
    std::tm local = {};
    char date[32] = "00000000-000000";
    if (LocalTime(seconds, &local)) std::strftime(date, sizeof(date), "%Y%m%d-%H%M%S", &local);
    // ホスト名の英数字とハイフン以外は「_」（macOS版の CharacterSet.alphanumerics。英字以外の文字も字ならそのまま）
    std::u32string host;
    for (char32_t c : Utf8ToUtf32(hostName)) {
        const bool asciiAlphanumeric = (c >= U'0' && c <= U'9') || (c >= U'A' && c <= U'Z') || (c >= U'a' && c <= U'z');
        const bool fullWidthAlphanumeric =
            (c >= 0xFF10 && c <= 0xFF19) || (c >= 0xFF21 && c <= 0xFF3A) || (c >= 0xFF41 && c <= 0xFF5A);
        const bool symbolOrSpace = c < 0x80 || (c >= 0x2000 && c <= 0x206F) || (c >= 0x3000 && c <= 0x303F) ||
                                   (c >= 0xFF00 && c <= 0xFF65);
        host += asciiAlphanumeric || c == U'-' || fullWidthAlphanumeric || !symbolOrSpace ? c : U'_';
    }
    return std::string(date) + "-" + Utf32ToUtf8(host);
}

bool Write(const std::filesystem::path& dataDirectory, const Record& record,
           const std::vector<benchmark::ItemResult>& results, std::string* error) {
    std::error_code ignored;
    std::filesystem::create_directories(Directory(dataDirectory), ignored);
    if (!WriteAtomically(RecordPath(dataDirectory, record.id), EncodeRecord(record)) ||
        !WriteAtomically(TimingsPath(dataDirectory, record.id), benchmark::TimingsTsv(results))) {
        if (error) *error = "記録を書けませんでした";
        return false;
    }
    return true;
}

std::vector<Record> Records(const std::filesystem::path& dataDirectory) {
    std::vector<Record> records;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(Directory(dataDirectory), error)) {
        if (entry.path().extension() != ".json") continue;
        std::ifstream file(entry.path(), std::ios::binary);
        std::stringstream buffer;
        buffer << file.rdbuf();
        if (auto record = DecodeRecord(buffer.str())) records.push_back(std::move(*record));
    }
    std::stable_sort(records.begin(), records.end(),
                     [](const Record& a, const Record& b) { return a.startedAt > b.startedAt; });
    return records;
}

void Delete(const std::filesystem::path& dataDirectory, const std::string& id) {
    std::error_code ignored;
    std::filesystem::remove(RecordPath(dataDirectory, id), ignored);
    std::filesystem::remove(TimingsPath(dataDirectory, id), ignored);
}

// ---- 進み具合 ----

namespace {

const char* KindName(Event::Kind kind) {
    switch (kind) {
    case Event::Kind::Loading: return "loading";
    case Event::Kind::Progress: return "progress";
    case Event::Kind::Finished: return "finished";
    case Event::Kind::Failed: return "failed";
    }
    return "progress";
}

} // namespace

std::string Event::JsonLine() const {
    Value value = MakeObject();
    value.Set("kind", MakeString(KindName(kind)));
    if (done) value.Set("done", MakeNumber(*done));
    if (total) value.Set("total", MakeNumber(*total));
    if (hits) value.Set("hits", MakeNumber(*hits));
    SetIf(&value, "recordID", recordID);
    SetIf(&value, "message", message);
    return json::Serialize(value) + "\n";
}

std::optional<Event> Event::Parse(const std::string& line) {
    Value value;
    if (!json::Parse(line, &value) || value.type != Value::Type::Object) return std::nullopt;
    const auto kind = StringOf(value, "kind");
    if (!kind) return std::nullopt;
    Event event;
    if (*kind == "loading") {
        event.kind = Kind::Loading;
    } else if (*kind == "progress") {
        event.kind = Kind::Progress;
    } else if (*kind == "finished") {
        event.kind = Kind::Finished;
    } else if (*kind == "failed") {
        event.kind = Kind::Failed;
    } else {
        return std::nullopt;
    }
    event.done = IntOf(value, "done");
    event.total = IntOf(value, "total");
    event.hits = IntOf(value, "hits");
    event.recordID = StringOf(value, "recordID");
    event.message = StringOf(value, "message");
    return event;
}

// ---- Markdown ----

std::string FormatLocalDateTime(int64_t seconds) {
    std::tm local = {};
    char text[32] = "";
    if (LocalTime(seconds, &local)) std::strftime(text, sizeof(text), "%Y/%m/%d %H:%M", &local);
    return text;
}

std::string Markdown(const Record& record) {
    const benchmark::Summary& s = record.summary;
    const Environment& e = record.environment;
    std::vector<std::string> lines;
    lines.push_back("## iroha ベンチマーク（" + record.benchmarkName + "、" + FormatLocalDateTime(record.startedAt) +
                    "）");
    lines.push_back("");
    lines.push_back("- 条件: " + record.condition.SummaryText());
    std::string machine = "- 機械: " + e.cpu + (e.machineModel ? "（" + *e.machineModel + "）" : std::string()) +
                          "、" + e.osVersion + "、iroha " + e.appVersion + "、電源 " +
                          (e.powerSource && *e.powerSource == "battery" ? "バッテリー"
                           : e.powerSource && *e.powerSource == "ac"    ? "AC"
                                                                        : "不明") +
                          (e.powerSaving && *e.powerSaving ? "（省電力の設定）" : "");
    if (e.thermalStateBefore) {
        machine += "、発熱の状態 " + *e.thermalStateBefore + " → " + e.thermalStateAfter.value_or("?");
    }
    lines.push_back(machine);
    if (!record.inputDuringRun) {
        lines.push_back("- 計測中のキーボードの入力は確かめられない");
    } else if (*record.inputDuringRun) {
        lines.push_back("- 計測中にキーボードの入力があった");
    }
    lines.push_back("");
    lines.push_back("| 区分 | 件数 | acc@1 | MinCER |");
    lines.push_back("|---|---:|---:|---:|");
    const auto row = [&](const std::string& label, const benchmark::Tally& t) {
        if (t.count == 0) return;
        char text[160];
        std::snprintf(text, sizeof(text), " | %d | %d (%.1f%%) | %.2f%% |", t.count, t.hits, t.Accuracy() * 100,
                      t.MeanMinCER() * 100);
        lines.push_back("| " + label + text);
    };
    row("全体", s.total);
    row("文脈あり", s.withContext);
    row("文脈なし", s.withoutContext);
    for (const auto& source : s.perSource) row(source.source, source.tally);
    lines.push_back("");
    lines.push_back("| 件数 | 平均 | 中央値 | 90% | 99% | 最大 | NN の割合 |");
    lines.push_back("|---:|---:|---:|---:|---:|---:|---:|");
    const benchmark::Latency& l = s.latency;
    const std::string share = s.neuralNetworkShare ? Format("%.0f%%", *s.neuralNetworkShare * 100) : "—";
    char latency[200];
    std::snprintf(latency, sizeof(latency), "| %d | %.1f | %.1f | %.1f | %.1f | %.1f | ", l.count, l.mean, l.p50,
                  l.p90, l.p99, l.max);
    lines.push_back(latency + share + " |");
    lines.push_back("");
    lines.push_back("（単位はミリ秒。1 件 = 読み 1 つの変換 1 回。モデルの読み込みを含む最初の変換 " +
                    Format("%.0f", record.warmUpMilliseconds) + " ms は除く）");
    lines.push_back("");
    lines.push_back("読みの文字数別の中央値（括弧内は件数）");
    lines.push_back("");
    std::string labels = "|";
    std::string rule = "|";
    std::string values = "|";
    for (const auto& bin : s.lengthBins) {
        labels += " " + bin.Label() + " |";
        rule += "---:|";
        char cell[64];
        std::snprintf(cell, sizeof(cell), " %.1f (%d) |", bin.p50, bin.count);
        values += cell;
    }
    lines.push_back(labels);
    lines.push_back(rule);
    lines.push_back(values);
    std::string text;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) text += "\n";
        text += lines[i];
    }
    return text + "\n";
}

} // namespace iroha::benchmark_run
