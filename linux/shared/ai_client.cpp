#include "ai_client.h"

#include <curl/curl.h>

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>

#include "iroha/json.h"
#include "secret_store.h"
#include "iroha/unicode.h"

namespace iroha {

namespace {

constexpr char kClaudeEndpoint[] = "https://api.anthropic.com/v1";
constexpr char kClaudeApiVersion[] = "2023-06-01";

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

struct HttpOutcome {
    long status = 0;
    std::string errorBody; // 200 以外のときの本文（先頭 4000 バイトまで）
    std::string error;     // 通信できなかった理由（空なら status が有効）
    bool cancelled = false;
};

std::string HostOf(const std::string& url) {
    const size_t scheme = url.find("://");
    const size_t start = scheme == std::string::npos ? 0 : scheme + 3;
    const size_t end = url.find_first_of(":/?", start);
    return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

std::string ErrorText(CURLcode code, int timeoutSeconds, const std::string& host) {
    switch (code) {
    case CURLE_OPERATION_TIMEDOUT:
        return std::to_string(timeoutSeconds) + "秒間応答がありませんでした";
    case CURLE_COULDNT_CONNECT:
        return "接続できません（" + host + "）";
    case CURLE_COULDNT_RESOLVE_HOST:
        return "名前を引けません（" + host + "）";
    case CURLE_ABORTED_BY_CALLBACK:
        return "取り消しました";
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_PEER_FAILED_VERIFICATION:
        return "安全な接続を確立できません（" + host + "）";
    case CURLE_URL_MALFORMAT:
    case CURLE_UNSUPPORTED_PROTOCOL:
        return "URL が正しくありません";
    default:
        return std::string("通信エラー（") + curl_easy_strerror(code) + "）";
    }
}

void EnsureCurl() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

// 受け取りの途中の状態（libcurl の呼び戻しに渡す）
struct Transfer {
    CURL* curl = nullptr;
    CancelToken* cancel = nullptr;
    const std::function<bool(const std::string&)>* onLine = nullptr;
    HttpOutcome* outcome = nullptr;
    std::string pending;
    bool stoppedByReader = false;
};

size_t OnData(char* data, size_t size, size_t count, void* user) {
    Transfer& transfer = *static_cast<Transfer*>(user);
    const size_t length = size * count;
    long status = 0;
    curl_easy_getinfo(transfer.curl, CURLINFO_RESPONSE_CODE, &status);
    transfer.outcome->status = status;
    if (status != 200) {
        if (transfer.outcome->errorBody.size() < 4000) transfer.outcome->errorBody.append(data, length);
        return length;
    }
    transfer.pending.append(data, length);
    for (size_t newline = transfer.pending.find('\n'); newline != std::string::npos;
         newline = transfer.pending.find('\n')) {
        std::string line = transfer.pending.substr(0, newline);
        transfer.pending.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!(*transfer.onLine)(line)) {
            // 読むのをやめる（0 を返すと libcurl が受け取りを打ち切る）
            transfer.stoppedByReader = true;
            return 0;
        }
    }
    return length;
}

int OnProgress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const Transfer& transfer = *static_cast<const Transfer*>(user);
    return transfer.cancel && transfer.cancel->Cancelled() ? 1 : 0;
}

// 1 回の HTTP 要求。応答の本文を 1 行ずつ onLine に渡す（偽を返すと読むのをやめる）。
// timeoutSeconds は「何も届かない時間」の上限（ストリームが進む限りは切れない）
HttpOutcome Request(const char* method, const std::string& url, const std::vector<std::string>& headers,
                    const std::string& body, int timeoutSeconds, CancelToken* cancel,
                    const std::function<bool(const std::string&)>& onLine) {
    EnsureCurl();
    HttpOutcome outcome;
    if (cancel && cancel->Cancelled()) {
        outcome.cancelled = true;
        outcome.error = "取り消しました";
        return outcome;
    }
    CURL* curl = curl_easy_init();
    if (!curl) {
        outcome.error = "通信の準備ができません";
        return outcome;
    }
    Transfer transfer;
    transfer.curl = curl;
    transfer.cancel = cancel;
    transfer.onLine = &onLine;
    transfer.outcome = &outcome;
    curl_slist* headerList = nullptr;
    for (const std::string& header : headers) headerList = curl_slist_append(headerList, header.c_str());
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    if (std::string(method) == "POST") {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "iroha");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    // 何も届かない時間の上限（1 バイト/秒を下回る状態が timeoutSeconds 続いたら打ち切る）
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, static_cast<long>(timeoutSeconds));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, OnData);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, OnProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);
    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    outcome.status = status;
    outcome.cancelled = cancel && cancel->Cancelled();
    if (outcome.cancelled) {
        outcome.error = "取り消しました";
    } else if (code == CURLE_WRITE_ERROR && transfer.stoppedByReader) {
        // 読む側がやめた（必要なものを読み終えた）
    } else if (code != CURLE_OK) {
        outcome.error = ErrorText(code, timeoutSeconds, HostOf(url));
    } else if (status == 200 && !transfer.pending.empty()) {
        onLine(transfer.pending);
    }
    curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);
    return outcome;
}

std::string HttpFailure(const HttpOutcome& outcome) {
    if (!outcome.error.empty()) return outcome.error;
    const auto message = AIErrorMessageFromBody(outcome.errorBody);
    return "HTTP " + std::to_string(outcome.status) + (message ? ": " + *message : std::string());
}

std::vector<std::string> ClaudeHeaders(const std::string& apiKey) {
    return {"x-api-key: " + apiKey, std::string("anthropic-version: ") + kClaudeApiVersion};
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
    const HttpOutcome outcome = Request("GET", std::string(kClaudeEndpoint) + "/models/" + model, ClaudeHeaders(apiKey), "",
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
    std::vector<std::string> headers = {"Content-Type: application/json"};
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
        if (!connection.apiKey.empty()) headers.push_back("Authorization: Bearer " + connection.apiKey);
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
            headers.push_back("anthropic-beta: server-side-fallback-2026-07-01");
        }
        body += "}";
        for (const std::string& header : ClaudeHeaders(connection.apiKey)) headers.push_back(header);
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

    const HttpOutcome outcome = Request("POST", url, headers, body, stallSeconds, cancel, onLine);
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
    std::vector<std::string> headers;
    switch (connection.service) {
    case AIService::Ollama:
        url = endpoint + "/api/tags";
        break;
    case AIService::LMStudio:
    case AIService::OpenAI:
        url = NormalizedV1Base(endpoint) + "/models";
        if (connection.service == AIService::OpenAI && !connection.apiKey.empty()) {
            headers.push_back("Authorization: Bearer " + connection.apiKey);
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
    const HttpOutcome outcome = Request("GET", url, headers, "", connection.service == AIService::Claude ? 10 : 3,
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

AIConnection AIClient::FromConfig(const Config& config, bool withKey) {
    AIConnection connection;
    connection.service = ParseAIService(config.aiService);
    switch (connection.service) {
    case AIService::Ollama:
        connection.endpoint = config.ollamaEndpoint;
        connection.model = config.ollamaModel;
        break;
    case AIService::LMStudio:
        connection.endpoint = config.lmStudioEndpoint;
        connection.model = config.lmStudioModel;
        break;
    case AIService::OpenAI:
        connection.endpoint = config.openAIEndpoint;
        connection.model = config.openAIModel;
        break;
    case AIService::Claude:
        connection.model = config.claudeModel;
        break;
    default:
        break;
    }
    if (withKey) LoadKey(&connection);
    return connection;
}

void AIClient::LoadKey(AIConnection* connection) {
    std::string key;
    if (connection->service == AIService::OpenAI && SecretStore::Get(kOpenAIKeyAccount, &key)) {
        connection->apiKey = key;
    } else if (connection->service == AIService::Claude && SecretStore::Get(kClaudeKeyAccount, &key)) {
        connection->apiKey = key;
    }
}

} // namespace iroha
