#include "iroha/zenz_engine.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <optional>

#include "iroha/kana.h"
#include "iroha/reading_constraint.h"
#include "iroha/unicode.h"

#include "llama.h"

namespace iroha {

namespace {

// 出力末尾の空白・改行類を落とす（Swiftの.whitespacesAndNewlines相当の主要部分）
bool IsTrimmable(char32_t c) {
    switch (c) {
        case U' ':
        case U'\t':
        case U'\n':
        case U'\r':
        case 0x0B:
        case 0x0C:
        case 0x85:   // NEL
        case 0xA0:   // NBSP
        case 0x1680:
        case 0x2028:
        case 0x2029:
        case 0x202F:
        case 0x205F:
        case 0x3000: // 全角空白
            return true;
        default:
            return c >= 0x2000 && c <= 0x200A;
    }
}

std::u32string Trimmed(const std::u32string& s) {
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && IsTrimmable(s[begin])) ++begin;
    while (end > begin && IsTrimmable(s[end - 1])) --end;
    return s.substr(begin, end - begin);
}

// zenzの特殊トークン（私用領域 U+EE00-U+EE0F）のUTF-8は EE B8 80-8F の3バイト
bool EndsWithZenzSpecial(const std::string& bytes) {
    const size_t n = bytes.size();
    if (n < 3) return false;
    return static_cast<unsigned char>(bytes[n - 3]) == 0xEE &&
           static_cast<unsigned char>(bytes[n - 2]) == 0xB8 &&
           static_cast<unsigned char>(bytes[n - 1]) >= 0x80 &&
           static_cast<unsigned char>(bytes[n - 1]) <= 0x8F;
}

} // namespace

// llama.cppのリソース一式。デストラクタで確実に解放する
struct ZenzEngine::Runtime {
    llama_model* model = nullptr;
    llama_context* context = nullptr;
    const llama_vocab* vocab = nullptr;
    // トークンID → 出力文字列（UTF-8として不正なバイト断片は無効=空のoptional）
    std::vector<std::optional<std::u32string>> tokenTexts;
    // トークンID → 生成終了とみなすトークンか（EOG・zenzの特殊トークン）
    std::vector<char> tokenIsTerminator;

    ~Runtime() {
        if (context) llama_free(context);
        if (model) llama_model_free(model);
    }
};

ZenzEngine::ZenzEngine(std::string modelPath) : modelPath_(std::move(modelPath)) {}
ZenzEngine::~ZenzEngine() = default;

bool ZenzEngine::Prewarm(std::string* error) {
    return EnsureLoaded(error);
}

std::u32string ZenzEngine::BuildPrompt(const std::u32string& reading,
                                       const std::u32string& leftContext,
                                       size_t maxContextLength) {
    // zenz-v3の特殊トークン（私用領域）
    constexpr char32_t kContextMarker = 0xEE02; // 左文脈の開始
    constexpr char32_t kReadingMarker = 0xEE00; // 読みの開始
    constexpr char32_t kOutputMarker = 0xEE01;  // 変換結果の開始
    std::u32string prompt;
    if (!leftContext.empty()) {
        prompt += kContextMarker;
        const size_t start = leftContext.size() > maxContextLength
                                 ? leftContext.size() - maxContextLength
                                 : 0;
        prompt += leftContext.substr(start);
    }
    prompt += kReadingMarker;
    prompt += HiraganaToKatakana(reading);
    prompt += kOutputMarker;
    return prompt;
}

bool ZenzEngine::EnsureLoaded(std::string* error) {
    if (runtime_) return true;

    const std::wstring widePath = Utf32ToUtf16(Utf8ToUtf32(modelPath_));
    if (!std::filesystem::exists(std::filesystem::path(widePath))) {
        if (error) *error = "モデルファイルが見つかりません: " + modelPath_;
        return false;
    }

    llama_backend_init();
    llama_model_params modelParams = llama_model_default_params();
    // CPUビルドでは無視されるが、GPU対応ビルドに切り替えたときのためmacOS版に合わせる。
    // IROHA_GPU_LAYERS=0 でCPU推論に切替可能（計測用）
    if (const char* env = std::getenv("IROHA_GPU_LAYERS")) {
        modelParams.n_gpu_layers = std::atoi(env);
    } else {
        modelParams.n_gpu_layers = 99;
    }

    llama_model* model = llama_model_load_from_file(modelPath_.c_str(), modelParams);
    if (!model) {
        if (error) *error = "モデルの読み込みに失敗: " + modelPath_;
        return false;
    }

    llama_context_params contextParams = llama_context_default_params();
    contextParams.n_ctx = 512;
    contextParams.n_batch = 512;

    llama_context* context = llama_init_from_model(model, contextParams);
    if (!context) {
        llama_model_free(model);
        if (error) *error = "モデルの読み込みに失敗: コンテキストの作成に失敗";
        return false;
    }

    const llama_vocab* vocab = llama_model_get_vocab(model);
    if (!vocab) {
        llama_free(context);
        llama_model_free(model);
        if (error) *error = "モデルの読み込みに失敗: vocabの取得に失敗";
        return false;
    }

    auto runtime = std::make_unique<Runtime>();
    runtime->model = model;
    runtime->context = context;
    runtime->vocab = vocab;

    // 語彙全体の出力文字列を1度だけ取り出しておく（制約判定を毎トークン安く行うため）
    const int vocabSize = llama_vocab_n_tokens(vocab);
    runtime->tokenTexts.resize(vocabSize);
    runtime->tokenIsTerminator.assign(vocabSize, 0);
    std::vector<char> buffer(128);
    for (int index = 0; index < vocabSize; ++index) {
        const llama_token token = static_cast<llama_token>(index);
        if (llama_vocab_is_eog(vocab, token)) {
            runtime->tokenIsTerminator[index] = 1;
            continue;
        }
        const int written = llama_token_to_piece(
            vocab, token, buffer.data(), static_cast<int32_t>(buffer.size()), 0, true);
        if (written <= 0) continue;
        auto text = Utf8ToUtf32Strict(std::string_view(buffer.data(), written));
        if (!text || text->empty()) continue;
        // zenzの特殊トークン（私用領域 U+EE00-U+EE0F）は生成終了の印
        const bool isSpecial =
            std::any_of(text->begin(), text->end(),
                        [](char32_t c) { return c >= 0xEE00 && c <= 0xEE0F; });
        if (isSpecial) {
            runtime->tokenIsTerminator[index] = 1;
            continue;
        }
        runtime->tokenTexts[index] = std::move(*text);
    }

    runtime_ = std::move(runtime);
    return true;
}

namespace {

bool Tokenize(const llama_vocab* vocab, const std::u32string& text, bool addSpecial,
              std::vector<llama_token>* tokens, std::string* error) {
    const std::string utf8 = Utf32ToUtf8(text);
    tokens->assign(utf8.size() + 8, 0);
    const int count = llama_tokenize(vocab, utf8.data(),
                                     static_cast<int32_t>(utf8.size()), tokens->data(),
                                     static_cast<int32_t>(tokens->size()), addSpecial,
                                     /*parse_special=*/true);
    if (count < 0) {
        if (error) *error = "推論に失敗: トークン化に失敗";
        return false;
    }
    tokens->resize(count);
    return true;
}

bool Decode(llama_context* ctx, llama_token* tokens, int count, std::string* error) {
    const int result = llama_decode(ctx, llama_batch_get_one(tokens, count));
    if (result != 0) {
        if (error) *error = "推論に失敗: llama_decode=" + std::to_string(result);
        return false;
    }
    return true;
}

} // namespace

bool ZenzEngine::Convert(const std::u32string& reading,
                         const std::u32string& leftContext, int candidateCount,
                         std::vector<std::u32string>* results, std::string* error) {
    if (!results) return false;
    results->clear();
    if (!EnsureLoaded(error)) return false;
    Runtime& rt = *runtime_;

    const std::u32string prompt = BuildPrompt(reading, leftContext, kMaxContextLength);
    std::vector<llama_token> promptTokens;
    if (!Tokenize(rt.vocab, prompt, /*addSpecial=*/true, &promptTokens, error)) {
        return false;
    }

    // 現在のlogitsから、読みの制約を満たすもっとも尤度の高いトークンを選ぶ。
    // 制約を満たすトークンが1つもなければ制約を諦めて素の最尤トークンを返す（relaxed）
    struct Picked {
        llama_token token;
        uint64_t mask;
        bool relaxed;
    };
    auto selectToken = [&rt](const ReadingConstraint* constraint,
                             uint64_t mask) -> std::optional<Picked> {
        const float* logits = llama_get_logits_ith(rt.context, -1);
        if (!logits) return std::nullopt;
        const int vocabSize = llama_vocab_n_tokens(rt.vocab);
        int bestIndex = -1;
        float bestLogit = 0;
        uint64_t bestMask = 0;
        int fallbackIndex = -1;
        float fallbackLogit = 0;

        for (int index = 0; index < vocabSize; ++index) {
            const float logit = logits[index];
            if (fallbackIndex < 0 || logit > fallbackLogit) {
                fallbackIndex = index;
                fallbackLogit = logit;
            }
            if (!constraint) continue;
            // 現在の最良より低いトークンは制約を調べるまでもない
            if (bestIndex >= 0 && logit <= bestLogit) continue;
            if (rt.tokenIsTerminator[index]) {
                // 読みを使い切っていなければ終端は許さない（食い残し防止）
                if (!constraint->IsComplete(mask)) continue;
                bestIndex = index;
                bestLogit = logit;
                bestMask = mask;
            } else {
                const auto& text = rt.tokenTexts[index];
                if (!text) continue;
                const uint64_t next = constraint->Advance(mask, *text);
                if (next == 0) continue;
                bestIndex = index;
                bestLogit = logit;
                bestMask = next;
            }
        }

        if (constraint && bestIndex >= 0) {
            return Picked{static_cast<llama_token>(bestIndex), bestMask, false};
        }
        if (fallbackIndex < 0) return std::nullopt;
        return Picked{static_cast<llama_token>(fallbackIndex), 0,
                      constraint != nullptr};
    };

    // 貪欲法で1候補を生成する。forcedFirstTokenがあれば先頭をそのトークンに固定する。
    // 各ステップでは読みと辻褄の合うトークンだけを選ぶ（constrained decoding）
    auto generate = [&](const llama_token* forcedFirstToken, std::u32string* out,
                        std::string* generateError) -> bool {
        std::vector<llama_token> tokens = promptTokens;
        if (forcedFirstToken) tokens.push_back(*forcedFirstToken);

        // KVキャッシュを破棄してプロンプトを評価（TODO: プレフィックス再利用で増分デコード）
        llama_memory_clear(llama_get_memory(rt.context), true);
        if (!Decode(rt.context, tokens.data(), static_cast<int>(tokens.size()),
                    generateError)) {
            return false;
        }

        std::string outputBytes;
        std::vector<char> pieceBuffer(128);
        auto appendPiece = [&](llama_token token) {
            const int written = llama_token_to_piece(
                rt.vocab, token, pieceBuffer.data(),
                static_cast<int32_t>(pieceBuffer.size()), 0, true);
            if (written > 0) outputBytes.append(pieceBuffer.data(), written);
        };
        if (forcedFirstToken) appendPiece(*forcedFirstToken);

        // 読みの消費状況（constrained decoding用）。追跡できない読みや
        // 制約を満たすトークンが尽きた場合は制約なしの貪欲生成に戻す
        std::optional<ReadingConstraint> constraintStorage =
            ReadingConstraint::Create(reading);
        const ReadingConstraint* constraint =
            constraintStorage ? &*constraintStorage : nullptr;
        uint64_t mask = constraint ? constraint->InitialMask() : 0;
        if (forcedFirstToken && constraint) {
            const auto& text = rt.tokenTexts[*forcedFirstToken];
            mask = constraint->Advance(mask, text ? *text : std::u32string());
            if (mask == 0) constraint = nullptr;
        }

        const int maxNewTokens = static_cast<int>(reading.size()) * 3 + 8;
        for (int step = 0; step < maxNewTokens; ++step) {
            auto picked = selectToken(constraint, mask);
            if (!picked) break;
            llama_token token = picked->token;
            if (picked->relaxed) constraint = nullptr;
            mask = picked->mask;
            if (llama_vocab_is_eog(rt.vocab, token)) break;

            appendPiece(token);
            // zenzの特殊トークン（私用領域 U+EE00-U+EE0F）が出たら終了
            if (EndsWithZenzSpecial(outputBytes)) {
                outputBytes.resize(outputBytes.size() - 3);
                break;
            }

            if (!Decode(rt.context, &token, 1, generateError)) return false;
        }

        auto output = Utf8ToUtf32Strict(outputBytes);
        if (!output) {
            if (generateError) *generateError = "推論に失敗: 出力がUTF-8として不正です";
            return false;
        }
        *out = Trimmed(*output);
        return true;
    };

    if (candidateCount <= 1) {
        std::u32string result;
        if (!generate(nullptr, &result, error)) return false;
        results->push_back(result.empty() ? reading : result);
        return true;
    }

    // n-best: 先頭トークンを上位候補に分岐し、それぞれ貪欲に補完する。
    // かな漢字変換では先頭の文字が同音異義語をほぼ決めるため、これで多様な候補が得られる
    {
        std::vector<llama_token> tokens = promptTokens;
        llama_memory_clear(llama_get_memory(rt.context), true);
        if (!Decode(rt.context, tokens.data(), static_cast<int>(tokens.size()),
                    error)) {
            return false;
        }
    }

    const auto constraint = ReadingConstraint::Create(reading);

    // プロンプト評価直後のlogitsから上位トークンを取る
    std::vector<llama_token> firstTokens;
    {
        const float* logits = llama_get_logits_ith(rt.context, -1);
        if (logits) {
            const int vocabSize = llama_vocab_n_tokens(rt.vocab);
            std::vector<int> indexed(vocabSize);
            for (int i = 0; i < vocabSize; ++i) indexed[i] = i;
            const size_t top = std::min<size_t>(
                static_cast<size_t>(candidateCount) * 8, indexed.size());
            std::partial_sort(indexed.begin(), indexed.begin() + top, indexed.end(),
                              [logits](int a, int b) { return logits[a] > logits[b]; });
            indexed.resize(top);
            for (int i : indexed) firstTokens.push_back(static_cast<llama_token>(i));
        }
    }

    for (llama_token token : firstTokens) {
        if (llama_vocab_is_eog(rt.vocab, token)) continue;
        // 読みと辻褄の合わない先頭トークンは候補にしない
        if (constraint) {
            const auto& text = rt.tokenTexts[token];
            if (!text) continue;
            if (constraint->Advance(constraint->InitialMask(), *text) == 0) continue;
        }
        std::u32string text;
        if (!generate(&token, &text, error)) return false;
        if (!text.empty() &&
            std::find(results->begin(), results->end(), text) == results->end()) {
            results->push_back(text);
        }
        if (static_cast<int>(results->size()) >= candidateCount) break;
    }
    if (results->empty()) results->push_back(reading);
    return true;
}

} // namespace iroha
