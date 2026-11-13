#include "ai_client.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <winhttp.h>

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>

#include "iroha/json.h"
#include "iroha/unicode.h"

#pragma comment(lib, "winhttp.lib")

namespace iroha {

namespace {

constexpr char kClaudeEndpoint[] = "https://api.anthropic.com/v1";
constexpr char kClaudeApiVersion[] = "2023-06-01";

std::wstring Wide(const std::string& utf8) { return Utf32ToUtf16(Utf8ToUtf32(utf8)); }
std::string Quote(const std::u32string& text) { return json::QuoteString(Utf32ToUtf8(text)); }

std::string Trim(const std::string& text) {
    size_t start = 0;
    size_t end = text.size();
    while (start < end && (text[start] == ' ' || text[start] == '\t' || text[start] == '\r' || text[start] == '\n')) {
        ++start;
    }
    while (end > start &&
           (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' || text[end - 1] == '\n')) {
        --end;
    }
    return text.substr(start, end - start);
}

// 通信の取っ手。止める合図から閉じられることがあるので、閉じるのは 1 回だけにする
struct Handles {
    std::mutex mutex;
    HINTERNET session = nullptr;
    HINTERNET connection = nullptr;
    HINTERNET request = nullptr;

    void CloseAll() {
        std::lock_guard<std::mutex> lock(mutex);
        for (HINTERNET* handle : {&request, &connection, &session}) {
            if (*handle) {
                WinHttpCloseHandle(*handle);
                *handle = nullptr;
            }
        }
    }
};

struct HttpOutcome {
    DWORD status = 0;
    std::string errorBody; // 200 以外のときの本文（先頭 4000 バイトまで）
    std::string error;     // 通信できなかった理由（空なら status が有効）
    bool cancelled = false;
};

std::string ErrorText(DWORD code, int timeoutSeconds, const std::string& host) {
    switch (code) {
    case ERROR_WINHTTP_TIMEOUT:
        return std::to_string(timeoutSeconds) + "秒間応答がありませんでした";
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_CONNECTION_ERROR:
        return "接続できません（" + host + "）";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        return "名前を引けません（" + host + "）";
    case ERROR_WINHTTP_OPERATION_CANCELLED:
        return "取り消しました";
    case ERROR_WINHTTP_SECURE_FAILURE:
        return "安全な接続を確立できません（" + host + "）";
    default:
        return "通信エラー（" + std::to_string(code) + "）";
    }
}

// 1 回の HTTP 要求。応答の本文を 1 行ずつ onLine に渡す（偽を返すと読むのをやめる）
HttpOutcome Request(const wchar_t* method, const std::string& url, const std::vector<std::wstring>& headers,
                    const std::string& body, int timeoutSeconds, CancelToken* cancel,
                    const std::function<bool(const std::string&)>& onLine) {
    HttpOutcome outcome;
    const std::wstring wideUrl = Wide(url);
    URL_COMPONENTS parts = {};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    wchar_t extra[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = ARRAYSIZE(host);
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = ARRAYSIZE(path);
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = ARRAYSIZE(extra);
    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts)) {
        outcome.error = "URL が正しくありません: " + url;
        return outcome;
    }
    const std::string hostName = Utf32ToUtf8(Utf16ToUtf32(host));
    auto handles = std::make_shared<Handles>();
    const auto failWith = [&](DWORD code) {
        outcome.cancelled = cancel && cancel->Cancelled();
        outcome.error = outcome.cancelled ? "取り消しました" : ErrorText(code, timeoutSeconds, hostName);
        if (cancel) cancel->SetCanceller(nullptr);
        handles->CloseAll();
        return outcome;
    };
    handles->session = WinHttpOpen(L"iroha", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0);
    if (!handles->session) return failWith(GetLastError());
    // 名前解決・接続・送信は 10〜30 秒、受信は「何も届かない時間」の上限（ストリームが進む限りは切れない）
    WinHttpSetTimeouts(handles->session, 10000, 10000, 30000, timeoutSeconds * 1000);
    handles->connection = WinHttpConnect(handles->session, host, parts.nPort, 0);
    if (!handles->connection) return failWith(GetLastError());
    const std::wstring object = std::wstring(path) + extra;
    handles->request =
        WinHttpOpenRequest(handles->connection, method, object.c_str(), nullptr, WINHTTP_NO_REFERER,
                           WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!handles->request) return failWith(GetLastError());
    if (cancel && !cancel->SetCanceller([handles] { handles->CloseAll(); })) return failWith(ERROR_WINHTTP_OPERATION_CANCELLED);

    std::wstring headerText;
    for (const std::wstring& header : headers) headerText += header + L"\r\n";
    BOOL sent = WinHttpSendRequest(handles->request, headerText.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerText.c_str(),
                                   headerText.empty() ? 0 : static_cast<DWORD>(-1),
                                   body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                                   static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    if (!sent || !WinHttpReceiveResponse(handles->request, nullptr)) return failWith(GetLastError());
    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(handles->request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    outcome.status = status;

    std::string pending;
    char buffer[8192];
    bool reading = true;
    while (reading) {
        // 届いている量だけを読む（いっぺんに大きく読むと、受け取り口がいっぱいになるまで待ち、途中経過が遅れる）
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(handles->request, &available)) return failWith(GetLastError());
        if (available == 0) break;
        DWORD got = 0;
        if (!WinHttpReadData(handles->request, buffer, std::min<DWORD>(available, sizeof(buffer)), &got)) {
            return failWith(GetLastError());
        }
        if (got == 0) break;
        if (status != 200) {
            if (outcome.errorBody.size() < 4000) outcome.errorBody.append(buffer, got);
            continue;
        }
        pending.append(buffer, got);
        for (size_t newline = pending.find('\n'); newline != std::string::npos; newline = pending.find('\n')) {
            std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!onLine(line)) {
                reading = false;
                break;
            }
        }
    }
    if (reading && status == 200 && !pending.empty()) onLine(pending);
    if (cancel) cancel->SetCanceller(nullptr);
    outcome.cancelled = cancel && cancel->Cancelled();
    handles->CloseAll();
    return outcome;
}

std::string HttpFailure(const HttpOutcome& outcome) {
    if (!outcome.error.empty()) return outcome.error;
    const auto message = AIErrorMessageFromBody(outcome.errorBody);
    return "HTTP " + std::to_string(outcome.status) + (message ? ": " + *message : std::string());
}

std::vector<std::wstring> ClaudeHeaders(const std::string& apiKey) {
    return {L"x-api-key: " + Wide(apiKey), L"anthropic-version: " + Wide(kClaudeApiVersion)};
}

// モデルごとの「考える量（effort）を指定できるか」。指定できないモデルに送ると 400 になるので、
// モデルの情報（capabilities）で確かめてから送る。プロセスの間だけ覚える
std::mutex g_effortMutex;
std::map<std::string, bool> g_effortSupport;

void RememberEffortSupport(const std::string& id, const json::Value& model) {
    bool supported = false;
    if (const json::Value* capabilities = model.Find("capabilities")) {
        if (const json::Value* effort = capabilities->Find("effort")) {
            if (const json::Value* low = effort->Find("low")) {
                if (const json::Value* flag = low->Find("supported"); flag && flag->type == json::Value::Type::Bool) {
                    supported = flag->boolean;
                }
            }
        }
    }
    std::lock_guard<std::mutex> lock(g_effortMutex);
    g_effortSupport[id] = supported;
}

bool ClaudeSupportsEffort(const std::string& model, const std::string& apiKey) {
    {
        std::lock_guard<std::mutex> lock(g_effortMutex);
        const auto found = g_effortSupport.find(model);
        if (found != g_effortSupport.end()) return found->second;
    }
    std::string text;
    const HttpOutcome outcome = Request(L"GET", std::string(kClaudeEndpoint) + "/models/" + model, ClaudeHeaders(apiKey), "",
                                        5, nullptr, [&text](const std::string& line) {
                                            text += line + "\n";
                                            return true;
                                        });
    json::Value root;
    if (!outcome.error.empty() || outcome.status != 200 || !json::Parse(text, &root)) return false;
    RememberEffortSupport(model, root);
    std::lock_guard<std::mutex> lock(g_effortMutex);
    return g_effortSupport[model];
}

// 断られた依頼を、サーバ側で別のモデルに回してやり直させられるモデル（macOS版と同じ）
bool ClaudeHasServerFallback(const std::string& model) {
    for (const char* name : {"claude-fable-5-1", "claude-opus-5-5", "claude-opus-5", "claude-sonnet-5-5"}) {
        if (model == name) return true;
    }
    return false;
}

std::string ChatMessages(const AIRequest& request) {
    return "[{\"role\":\"system\",\"content\":" + Quote(request.instructions) + "},{\"role\":\"user\",\"content\":" +
           Quote(request.userMessage) + "}]";
}

} // namespace

std::string AIClient::NormalizedV1Base(const std::string& endpoint) {
    std::string base = Trim(endpoint);
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (base.size() >= 3 && base.compare(base.size() - 3, 3, "/v1") == 0) base.resize(base.size() - 3);
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base + "/v1";
}

std::string AIClient::DefaultEndpoint(AIService service) {
    switch (service) {
    case AIService::Ollama:
        return "http://localhost:11434";
    case AIService::LMStudio:
        return "http://localhost:1234";
    case AIService::OpenAI:
        return "https://api.openai.com/v1";
    case AIService::Claude:
        return kClaudeEndpoint;
    default:
        return "";
    }
}

bool AIClient::IsConfigured(const AIConnection& connection) {
    switch (connection.service) {
    case AIService::Ollama:
    case AIService::LMStudio:
    case AIService::OpenAI:
        return !Trim(connection.model).empty();
    case AIService::Claude:
        return !connection.apiKey.empty();
    default:
        return false;
    }
}

AIResult AIClient::Run(const AIConnection& connection, const AIRequest& request,
                       const std::function<void(const std::u32string&)>& onPartial, CancelToken* cancel,
                       int stallSeconds) {
    AIResult result;
    if (!IsConfigured(connection)) {
        result.error = connection.service == AIService::Claude ? "APIキーが設定されていません" : "モデルが選ばれていません";
        return result;
    }
    const std::string endpoint = Trim(connection.endpoint).empty() ? DefaultEndpoint(connection.service) : Trim(connection.endpoint);
    std::string accumulated;
    std::string streamError;
    ClaudeStreamReader claude;
    std::string url;
    std::string body;
    std::vector<std::wstring> headers = {L"Content-Type: application/json"};
    std::function<bool(const std::string&)> onLine;
    const auto emit = [&onPartial](const std::string& text) {
        if (onPartial) onPartial(Utf8ToUtf32(text));
    };

    switch (connection.service) {
    case AIService::Ollama:
        // /api/chat（JSONL）。"think": false で思考の出力を止める
        url = endpoint + "/api/chat";
        body = "{\"model\":" + json::QuoteString(connection.model) + ",\"messages\":" + ChatMessages(request) +
               ",\"stream\":true,\"think\":false,\"options\":{\"temperature\":0.3}}";
        onLine = [&](const std::string& line) {
            const OllamaStreamLine parsed = ParseOllamaStreamLine(line);
            if (parsed.error) {
                streamError = *parsed.error;
                return false;
            }
            if (!parsed.piece.empty()) {
                accumulated += parsed.piece;
                emit(StripThinking(accumulated));
            }
            return !parsed.done;
        };
        break;
    case AIService::LMStudio:
    case AIService::OpenAI:
        // /v1/chat/completions（SSE）。LM Studio では reasoning_effort "none" だけが思考を止める。
        // OpenAI 本家は reasoning_effort "none" と temperature を受け付けないことがある（400）ので送らない
        url = NormalizedV1Base(endpoint) + "/chat/completions";
        body = "{\"model\":" + json::QuoteString(connection.model) + ",\"messages\":" + ChatMessages(request) +
               ",\"stream\":true" +
               (connection.service == AIService::LMStudio ? ",\"reasoning_effort\":\"none\",\"temperature\":0.3" : "") +
               "}";
        if (!connection.apiKey.empty()) headers.push_back(L"Authorization: Bearer " + Wide(connection.apiKey));
        onLine = [&](const std::string& line) {
            const OpenAIStreamLine parsed = ParseOpenAIStreamLine(line);
            if (!parsed.piece.empty()) {
                accumulated += parsed.piece;
                emit(StripThinking(accumulated));
            }
            return !parsed.done;
        };
        break;
    case AIService::Claude: {
        // Messages API（SSE）。指示は system に入れる。考える量は low（指定できるモデルだけ）
        const std::string model = Trim(connection.model).empty() ? kClaudeDefaultModel : Trim(connection.model);
        url = std::string(kClaudeEndpoint) + "/messages";
        body = "{\"model\":" + json::QuoteString(model) + ",\"max_tokens\":16000,\"system\":" + Quote(request.instructions) +
               ",\"messages\":[{\"role\":\"user\",\"content\":" + Quote(request.userMessage) + "}],\"stream\":true";
        if (ClaudeSupportsEffort(model, connection.apiKey)) body += ",\"output_config\":{\"effort\":\"low\"}";
        if (ClaudeHasServerFallback(model)) {
            body += ",\"fallbacks\":\"default\"";
            headers.push_back(L"anthropic-beta: server-side-fallback-2026-07-01");
        }
        body += "}";
        for (const std::wstring& header : ClaudeHeaders(connection.apiKey)) headers.push_back(header);
        onLine = [&](const std::string& line) {
            if (claude.Consume(line)) emit(Trim(claude.Text()));
            return !claude.IsFinished() && !claude.Failed();
        };
        break;
    }
    default:
        result.error = "AI の接続先が設定されていません";
        return result;
    }

    const HttpOutcome outcome = Request(L"POST", url, headers, body, stallSeconds, cancel, onLine);
    if (outcome.cancelled) {
        result.error = "取り消しました";
        return result;
    }
    if (!outcome.error.empty() || outcome.status != 200) {
        result.error = HttpFailure(outcome);
        return result;
    }
    if (!streamError.empty()) {
        result.error = streamError;
        return result;
    }
    std::string text;
    if (connection.service == AIService::Claude) {
        const auto answer = claude.Result();
        if (!answer) {
            result.error = claude.FailureMessage();
            return result;
        }
        text = *answer;
    } else {
        text = StripThinking(accumulated);
    }
    if (text.empty()) {
        result.error = "空の応答が返りました";
        return result;
    }
    result.text = Utf8ToUtf32(text);
    return result;
}

bool AIClient::ListModels(const AIConnection& connection, std::vector<std::string>* models, std::string* error) {
    const std::string endpoint = Trim(connection.endpoint).empty() ? DefaultEndpoint(connection.service) : Trim(connection.endpoint);
    std::string url;
    std::vector<std::wstring> headers;
    switch (connection.service) {
    case AIService::Ollama:
        url = endpoint + "/api/tags";
        break;
    case AIService::LMStudio:
    case AIService::OpenAI:
        url = NormalizedV1Base(endpoint) + "/models";
        if (connection.service == AIService::OpenAI && !connection.apiKey.empty()) {
            headers.push_back(L"Authorization: Bearer " + Wide(connection.apiKey));
        }
        break;
    case AIService::Claude:
        if (connection.apiKey.empty()) {
            if (error) *error = "APIキーを入力してください";
            return false;
        }
        url = std::string(kClaudeEndpoint) + "/models?limit=1000";
        headers = ClaudeHeaders(connection.apiKey);
        break;
    default:
        if (error) *error = "AI の接続先を選んでください";
        return false;
    }
    std::string text;
    // 起動していないサーバを素早く見分けるため、受信の上限は短くする
    const HttpOutcome outcome = Request(L"GET", url, headers, "", connection.service == AIService::Claude ? 10 : 3,
                                        nullptr, [&text](const std::string& line) {
                                            text += line + "\n";
                                            return true;
                                        });
    if (!outcome.error.empty() || outcome.status != 200) {
        if (error) *error = HttpFailure(outcome);
        return false;
    }
    json::Value root;
    if (!json::Parse(text, &root)) {
        if (error) *error = "モデルの一覧を読めません";
        return false;
    }
    models->clear();
    if (connection.service == AIService::Ollama) {
        if (const json::Value* list = root.Find("models"); list && list->type == json::Value::Type::Array) {
            for (const json::Value& item : list->array) {
                if (item.Find("name")) models->push_back(item.Find("name")->AsString());
            }
        }
        std::sort(models->begin(), models->end());
    } else if (const json::Value* list = root.Find("data"); list && list->type == json::Value::Type::Array) {
        for (const json::Value& item : list->array) {
            const std::string id = item.Find("id") ? item.Find("id")->AsString() : "";
            if (id.empty()) continue;
            models->push_back(id);
            if (connection.service == AIService::Claude) RememberEffortSupport(id, item);
        }
        // Claude は API の並びのまま（新しいモデルが先）
        if (connection.service != AIService::Claude) std::sort(models->begin(), models->end());
    }
    return true;
}

} // namespace iroha
