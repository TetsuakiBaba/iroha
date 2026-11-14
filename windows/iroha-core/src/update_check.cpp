#include "iroha/update_check.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "iroha/json.h"
#include "iroha/unicode.h"

namespace iroha::Update {

namespace {

bool EndsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<std::string> Split(const std::string& text, char separator) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        const size_t at = text.find(separator, start);
        parts.push_back(text.substr(start, at == std::string::npos ? std::string::npos : at - start));
        if (at == std::string::npos) break;
        start = at + 1;
    }
    return parts;
}

bool IsNumber(const std::string& text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

int ToInt(const std::string& text) {
    return IsNumber(text) ? std::atoi(text.c_str()) : 0;
}

struct Parsed {
    std::vector<int> core;
    std::vector<std::string> pre;
};

Parsed ParseVersion(std::string version) {
    while (!version.empty() && (version[0] == 'v' || version[0] == 'V')) version.erase(0, 1);
    Parsed parsed;
    const size_t dash = version.find('-');
    for (const std::string& part : Split(version.substr(0, dash), '.')) parsed.core.push_back(ToInt(part));
    if (dash != std::string::npos) parsed.pre = Split(version.substr(dash + 1), '.');
    return parsed;
}

} // namespace

bool ParseRelease(std::string_view text, ReleaseInfo* release) {
    json::Value root;
    if (!json::Parse(text, &root) || root.type != json::Value::Type::Object) return false;
    ReleaseInfo parsed;
    parsed.tag = root.Find("tag_name") ? root.Find("tag_name")->AsString() : "";
    if (parsed.tag.empty()) return false;
    if (const json::Value* page = root.Find("html_url")) parsed.page = page->AsString();
    if (const json::Value* body = root.Find("body")) parsed.notes = Utf8ToUtf32(body->AsString());
    if (const json::Value* assets = root.Find("assets"); assets && assets->type == json::Value::Type::Array) {
        for (const json::Value& item : assets->array) {
            ReleaseAsset asset;
            if (const json::Value* name = item.Find("name")) asset.name = name->AsString();
            if (const json::Value* url = item.Find("browser_download_url")) asset.url = url->AsString();
            if (const json::Value* size = item.Find("size"); size && size->type == json::Value::Type::Number) {
                asset.size = static_cast<uint64_t>(size->number);
            }
            if (const json::Value* digest = item.Find("digest")) {
                const std::string value = digest->AsString();
                if (value.rfind("sha256:", 0) == 0) asset.sha256 = value.substr(7);
                std::transform(asset.sha256.begin(), asset.sha256.end(), asset.sha256.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            }
            if (!asset.name.empty() && !asset.url.empty()) parsed.assets.push_back(asset);
        }
    }
    *release = parsed;
    return true;
}

bool IsNewer(const std::string& remote, const std::string& local) {
    const Parsed r = ParseVersion(remote);
    const Parsed l = ParseVersion(local);
    for (size_t i = 0; i < std::max(r.core.size(), l.core.size()); ++i) {
        const int rv = i < r.core.size() ? r.core[i] : 0;
        const int lv = i < l.core.size() ? l.core[i] : 0;
        if (rv != lv) return rv > lv;
    }
    // コアが同じ: 正式版（プレリリースなし）はプレリリースより新しい
    if (r.pre.empty() != l.pre.empty()) return r.pre.empty();
    // 両方プレリリース: 識別子を順に比べる（数どうしは数として）
    for (size_t i = 0; i < std::max(r.pre.size(), l.pre.size()); ++i) {
        if (i >= r.pre.size()) return false;
        if (i >= l.pre.size()) return true;
        const std::string& rp = r.pre[i];
        const std::string& lp = l.pre[i];
        if (IsNumber(rp) && IsNumber(lp)) {
            if (ToInt(rp) != ToInt(lp)) return ToInt(rp) > ToInt(lp);
        } else if (rp != lp) {
            return rp > lp;
        }
    }
    return false;
}

std::string ComparableVersion(const std::string& version) {
    std::string text = version;
    if (EndsWith(text, "-dirty")) text.erase(text.size() - 6);
    // git describe の「-<コミット数>-g<16進>」
    const size_t g = text.rfind("-g");
    if (g != std::string::npos && g + 2 < text.size() &&
        std::all_of(text.begin() + static_cast<std::ptrdiff_t>(g) + 2, text.end(),
                    [](unsigned char c) { return std::isxdigit(c) != 0; })) {
        const size_t count = text.rfind('-', g - 1);
        if (count != std::string::npos && IsNumber(text.substr(count + 1, g - count - 1))) text.erase(count);
    }
    return text;
}

const ReleaseAsset* WindowsInstaller(const ReleaseInfo& release) {
    for (const ReleaseAsset& asset : release.assets) {
        if (asset.name.rfind("iroha-", 0) == 0 && EndsWith(asset.name, "-windows-x64-setup.exe")) return &asset;
    }
    return nullptr;
}

std::filesystem::path StatePath(const std::filesystem::path& dataDirectory) {
    return dataDirectory / "update.json";
}

State LoadState(const std::filesystem::path& path) {
    State state;
    std::ifstream file(path, std::ios::binary);
    if (!file) return state;
    std::stringstream buffer;
    buffer << file.rdbuf();
    json::Value root;
    if (!json::Parse(buffer.str(), &root) || root.type != json::Value::Type::Object) return state;
    if (const json::Value* last = root.Find("lastCheck"); last && last->type == json::Value::Type::Number) {
        state.lastCheck = static_cast<int64_t>(last->number);
    }
    if (const json::Value* skipped = root.Find("skippedVersion")) state.skipped = skipped->AsString();
    if (const json::Value* notified = root.Find("lastNotifiedVersion")) state.lastNotified = notified->AsString();
    return state;
}

bool SaveState(const std::filesystem::path& path, const State& state) {
    const std::string out = "{\"lastCheck\":" + std::to_string(state.lastCheck) +
                            ",\"skippedVersion\":" + json::QuoteString(state.skipped) +
                            ",\"lastNotifiedVersion\":" + json::QuoteString(state.lastNotified) + "}";
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::filesystem::path tmp = path.wstring() + L".tmp";
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file.write(out.data(), static_cast<std::streamsize>(out.size()));
        if (!file) return false;
    }
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

bool IsDue(const State& state, int64_t now) {
    return now < state.lastCheck || now - state.lastCheck >= kCheckIntervalSeconds;
}

} // namespace iroha::Update
