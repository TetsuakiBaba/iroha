#include "engine_host.h"

#include <cctype>
#include <chrono>
#include <cstdio>

#include "iroha/platform.h"
#include "key_shortcut.h"
#include "iroha/typo_normalizer_catalog.h"
#include "iroha/unicode.h"

namespace iroha {

namespace {

long long MillisecondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
        .count();
}

} // namespace

EngineHost::EngineHost(Options options) : options_(std::move(options)) {}

EngineHost::~EngineHost() = default;

void EngineHost::Log(const std::string& message) const {
    if (options_.log) {
        options_.log(message);
    } else {
        std::fprintf(stderr, "[iroha] %s\n", message.c_str());
    }
}

std::filesystem::path EngineHost::ConfigPath() const { return platform::DataDirectory() / "config.json"; }

std::filesystem::path EngineHost::ResourceDirectory() const {
    return options_.resourceDirectory.empty() ? platform::ExecutableDirectory() : options_.resourceDirectory;
}

std::shared_ptr<TypoNormalizer> EngineHost::Typo() {
    std::lock_guard<std::mutex> lock(typoMutex_);
    return typo_;
}

void EngineHost::LoadTypoNormalizer(bool enabled) {
    std::filesystem::path directory = TypoNormalizerInstall::Directory(platform::DataDirectory());
    if (const auto env = platform::GetEnv("IROHA_TYPO_MODEL")) directory = PathFromUtf8(*env);
    std::shared_ptr<TypoNormalizer> next;
    std::string signature;
    if (enabled && TypoNormalizer::IsModelDirectory(directory)) {
        // 入れ直されたかは weights.bin の大きさと更新日時で見る
        std::error_code ignored;
        const auto weights = directory / "weights.bin";
        signature = PathToUtf8(directory) + "|" + std::to_string(std::filesystem::file_size(weights, ignored)) +
                    "|" +
                    std::to_string(std::filesystem::last_write_time(weights, ignored).time_since_epoch().count());
        {
            std::lock_guard<std::mutex> lock(typoMutex_);
            if (typo_ && signature == typoSignature_) return;
        }
        next = std::make_shared<TypoNormalizer>(directory);
        std::string error;
        const auto start = std::chrono::steady_clock::now();
        if (next->Load(&error)) {
            Log("typo normalizer loaded (" + std::to_string(MillisecondsSince(start)) + "ms, " +
                TypoNormalizer::KernelName() + ")");
        } else {
            Log("typo normalizer failed: " + error);
            next = nullptr;
        }
    } else if (enabled) {
        Log("typo normalizer not installed");
    }
    std::lock_guard<std::mutex> lock(typoMutex_);
    typo_ = std::move(next);
    typoSignature_ = signature;
}

void EngineHost::LoadTermDictionaries() {
    const std::filesystem::path data = platform::DataDirectory();
    const auto records = TermDictionaryInstall::InstalledRecords(data);
    const std::string signature = TermDictionaryInstall::Signature(records);
    if (termsLoaded_ && signature == termSignature_) return;
    termsLoaded_ = true;
    termSignature_ = signature;
    termIndex_ = nullptr;
    termLookup_ = nullptr;
    if (records.empty()) return;
    const auto start = std::chrono::steady_clock::now();
    if (!dictionaryPath_.empty()) {
        termIndex_ = LatticeConverter::BuildTermIndex(dictionaryPath_, TermDictionaryInstall::LoadPacks(data, records));
    }
    termLookup_ = std::make_shared<TermDictionaryLookup>(TermDictionaryLookup::Load(data, records));
    Log("term dictionaries loaded: " + std::to_string(records.size()) + " dictionaries, " +
        std::to_string(termIndex_ ? LatticeConverter::TermIndexEntryCount(*termIndex_) : size_t(0)) + " words (" +
        std::to_string(MillisecondsSince(start)) + "ms)");
}

void EngineHost::LoadAI(const Config& config) {
    AIConnection connection = AIClient::FromConfig(config, /*withKey=*/false);
    std::array<AICommitPreset, AICommitSettings::kCount> presets;
    sessionSettings_.aiPresetNames.clear();
    sessionSettings_.aiShortcuts.clear();
    for (size_t i = 0; i < presets.size(); ++i) {
        const auto& stored = config.aiPresets[i];
        const auto& fallback = AICommitSettings::Defaults()[i];
        presets[i].name = stored.name.value_or(fallback.name);
        presets[i].prompt = stored.prompt.value_or(fallback.prompt);
        presets[i].shortcut = stored.hotkey.value_or(fallback.shortcut);
        sessionSettings_.aiPresetNames.push_back(presets[i].DisplayName(i));
        const auto shortcut = KeyShortcut::Parse(presets[i].shortcut);
        sessionSettings_.aiShortcuts.push_back(shortcut ? shortcut->Encode() : 0);
    }
    std::lock_guard<std::mutex> lock(aiMutex_);
    aiConnection_ = std::move(connection);
    aiPresets_ = std::move(presets);
    aiKeyLoaded_ = false;
    sessionSettings_.aiAvailable = aiConnection_.service == AIService::Claude ||
                                   AIClient::IsConfigured(aiConnection_);
}

AIConnection EngineHost::ConnectionForRequest() {
    std::lock_guard<std::mutex> lock(aiMutex_);
    if (!aiKeyLoaded_) {
        aiKeyLoaded_ = true;
        AIClient::LoadKey(&aiConnection_);
    }
    return aiConnection_;
}

bool EngineHost::AIConfigured() {
    std::lock_guard<std::mutex> lock(aiMutex_);
    return aiConnection_.service == AIService::Claude || AIClient::IsConfigured(aiConnection_);
}

Config EngineHost::CurrentConfig() {
    std::lock_guard<std::mutex> lock(aiMutex_);
    return config_;
}

AIResult EngineHost::RunAIWith(const AIRequest& request, const std::function<void(const std::u32string&)>& onPartial,
                               CancelToken* cancel, int stallSeconds) {
    const AIConnection connection = ConnectionForRequest();
    if (connection.service == AIService::Claude && connection.apiKey.empty()) {
        return AIResult{std::nullopt, "Claude の API キーがありません（設定画面で入れてください）"};
    }
    const auto start = std::chrono::steady_clock::now();
    AIResult result = AIClient::Run(connection, request, onPartial, cancel, stallSeconds);
    Log(std::string("ai ") + AIServiceKey(connection.service) + " (" + std::to_string(MillisecondsSince(start)) +
        "ms)" + (result.text ? "" : ": " + result.error));
    return result;
}

EngineHost::ScoredText EngineHost::ScoreConversion(const std::u32string& reading, const std::u32string& context) {
    // AIで直す: 学習・ユーザ辞書・辞書ラティスを通さない、モデル単体の貪欲変換（macOS版と同じ）
    ScoredText scored;
    if (!zenz_) return scored;
    ZenzEngine::ScoredConversion result;
    std::string error;
    if (!zenz_->ConvertScored(reading, context, &result, &error)) return scored;
    scored.text = result.text;
    for (const auto& confidence : result.confidences) scored.margins.push_back(confidence.margin);
    return scored;
}

AIOutcome EngineHost::RunAIRequest(const AIRequest& request, int stallSeconds, CancelToken* cancel) {
    const AIResult result = RunAIWith(request, nullptr, cancel, stallSeconds);
    return AIOutcome{result.text, result.error};
}

std::optional<std::u32string> EngineHost::RunAI(size_t preset, const std::u32string& text,
                                                const std::function<void(const std::u32string&)>& onPartial,
                                                CancelToken* cancel) {
    AICommitPreset settings;
    {
        std::lock_guard<std::mutex> lock(aiMutex_);
        if (preset >= aiPresets_.size()) return std::nullopt;
        settings = aiPresets_[preset];
    }
    return RunAIWith(settings.Request(preset, text), onPartial, cancel).text;
}

void EngineHost::Build() {
    const Config config = LoadConfig(ConfigPath());
    {
        std::lock_guard<std::mutex> lock(aiMutex_);
        config_ = config;
    }
    LoadAI(config);
    // 区切り記号の英字入力。記号は英字・数字以外の ASCII の記号 1 文字（使えない値なら「_」。macOS版と同じ）
    const auto symbol = [](const std::string& text) -> char32_t {
        const bool valid = text.size() == 1 && text[0] >= 0x21 && text[0] <= 0x7E &&
                           !std::isalnum(static_cast<unsigned char>(text[0]));
        return valid ? static_cast<char32_t>(text[0]) : U'_';
    };
    sessionSettings_.aiFix = config.aiFix;
    sessionSettings_.aiFixThreshold = config.aiFixThreshold;
    const auto fixShortcut = KeyShortcut::Parse(config.aiFixShortcut);
    sessionSettings_.aiFixShortcut = fixShortcut ? fixShortcut->Encode() : 0;
    sessionSettings_.aiFixShortcutLabel = fixShortcut ? Utf8ToUtf32(config.aiFixShortcut) : U"";
    sessionSettings_.delimitedAlphabet = config.delimitedAlphabet;
    sessionSettings_.delimitedStart = symbol(config.delimitedAlphabetStart);
    sessionSettings_.delimitedEnd = symbol(config.delimitedAlphabetEnd);
    sessionSettings_.liveConversion = config.liveConversion;
    sessionSettings_.punctuation = config.punctuation;
    sessionSettings_.fullWidthSpace = config.fullWidthSpace;
    sessionSettings_.candidateCount = config.candidateCount;
    sessionSettings_.predictive = config.predictive;
    sessionSettings_.privateMode = config.privateMode;
    sessionSettings_.documentContext = config.documentContext;
    const auto privateShortcut = KeyShortcut::Parse(config.privateModeHotkey);
    sessionSettings_.privateModeShortcut = privateShortcut ? privateShortcut->Encode() : 0;
    learningEnabled_ = config.learningEnabled;
    sessionSettings_.conversionLog = config.conversionLog;
    sessionSettings_.typoCorrection = config.typoCorrection;
    sessionSettings_.typoThreshold = config.typoThreshold;
    sessionSettings_.typoDelayMs = config.typoDelayMs;
    sessionSettings_.typoMinimumLength = config.typoMinimumLength;
    LoadTypoNormalizer(config.typoCorrection);
    sessionSettings_.conversionLogScope =
        config.conversionLogCorrectionsOnly ? ConversionLogScope::Corrections : ConversionLogScope::All;
    privateMode_ = config.privateMode;
    const std::string host = platform::HostName();
    if (!conversionLog_) {
        conversionLog_ = std::make_unique<ConversionLog>(
            platform::StorePath("IROHA_CONVERSION_LOG", std::filesystem::path("logs") / "conversions"), host);
    }
    if (!inputHistory_) {
        inputHistory_ =
            std::make_unique<InputHistoryStore>(platform::StorePath("IROHA_INPUT_HISTORY", "input-history"), host);
    } else {
        inputHistory_->Reload();
    }
    if (config.symbolCandidates && !symbolsLoaded_) {
        symbolsLoaded_ = true;
        if (const auto directory = SymbolCandidateTable::DefaultDirectory(ResourceDirectory())) {
            const auto start = std::chrono::steady_clock::now();
            if (auto table = SymbolCandidateTable::Load(*directory, options_.isRenderableEmoji)) {
                loadedSymbols_ = std::make_shared<SymbolCandidateTable>(std::move(*table));
                const auto counts = loadedSymbols_->GetCounts();
                Log("symbols loaded: emoji " + std::to_string(counts.emoji) + ", emoticons " +
                    std::to_string(counts.emoticons) + ", symbols " + std::to_string(counts.symbols) + " (" +
                    std::to_string(MillisecondsSince(start)) + "ms)");
            }
        } else {
            Log("symbols not found (no symbol candidates)");
        }
    }
    symbols_ = config.symbolCandidates ? loadedSymbols_ : nullptr;
    const std::string newModelPath = platform::ResolveModelPath();
    // 追加学習した LoRA アダプタ（ファイルが無ければ使わない）
    std::string newAdapterPath = config.modelAdapterPath;
    if (!newAdapterPath.empty() && !std::filesystem::exists(PathFromUtf8(newAdapterPath))) {
        Log("adapter not found: " + newAdapterPath);
        newAdapterPath.clear();
    }
    if (!zenz_ || newModelPath != modelPath_ || newAdapterPath != adapterPath_) {
        ZenzEngine::Options zenzOptions;
        zenzOptions.adapterPath = newAdapterPath;
        zenz_ = std::make_unique<ZenzEngine>(newModelPath, zenzOptions);
        modelPath_ = newModelPath;
        adapterPath_ = newAdapterPath;
        if (!adapterPath_.empty()) Log("adapter: " + adapterPath_);
        std::string error;
        if (zenz_->Prewarm(&error)) {
            Log("model loaded: " + newModelPath);
        } else {
            // モデル未取得でも動き続ける（変換の要求にエラーを返す）
            Log("prewarm failed: " + error);
        }
    }
    if (!lattice_) {
        if (const auto dictionary = LatticeConverter::DefaultDictionaryPath(ResourceDirectory())) {
            dictionaryPath_ = *dictionary;
            lattice_ = std::make_unique<LatticeConverter>(*dictionary);
            // 辞書の読み込みを済ませておく（最初の候補ウィンドウのもたつき防止）
            lattice_->Candidates(U"うぉーむあっぷ", 1);
            Log("dictionary loaded: " + PathToUtf8(*dictionary));
        } else {
            Log("dictionary not found (candidates come from the model only)");
        }
    }
    LoadTermDictionaries();
    if (lattice_) lattice_->SetTermDictionary(termIndex_);
    ConversionEngine* core = zenz_.get();
    if (lattice_) {
        rescoring_ = std::make_unique<LatticeRescoringEngine>(zenz_.get(), zenz_.get(), lattice_.get());
        core = rescoring_.get();
    }
    // 異体字を候補の末尾に補い、80文字を超える読みは区切って順に変換する
    variants_ = std::make_unique<VariantKanjiEngine>(core);
    chunked_ = std::make_unique<ChunkedConversionEngine>(variants_.get());
    core = chunked_.get();
    userDictionary_ =
        std::make_unique<UserDictionaryStore>(platform::StorePath("IROHA_USER_DICT", "user-dictionary.json"));
    learning_ = std::make_unique<LearningStore>(platform::StorePath("IROHA_LEARNING", "learning.json"));
    if (!rewriteRules_) {
        rewriteRules_ = std::make_unique<UserRewriteRuleStore>(
            platform::StorePath("IROHA_REWRITE_RULES", "user-rewrite-rules.json"), /*seedsDefaults=*/true);
    } else {
        rewriteRules_->Reload();
    }
    rewriteRuleSet_ = std::make_shared<UserRewriteRuleSet>(rewriteRules_->Current());
    // プライベートモードの間はユーザ辞書と学習を使わない（変換を iroha 本来の結果にする。macOS版と同じ）
    dictionaryEngine_ = std::make_unique<UserDictionaryEngine>(
        core, [this] { return privateMode_ ? UserDictionary() : userDictionary_->Current(); }, lattice_.get());
    engine_ = std::make_unique<LearningEngine>(
        dictionaryEngine_.get(), [this] { return privateMode_ || !learningEnabled_ ? LearningDictionary() : learning_->Current(); });
}

void EngineHost::ApplyTo(SessionManager& sessions) {
    sessions.SetSettings(sessionSettings_);
    sessions.SetSymbolTable(symbols_);
    sessions.SetTermLookup(termLookup_);
    sessions.SetRewriteRules(rewriteRuleSet_);
}

void EngineHost::RecordCorrection(const std::u32string& reading, const std::optional<std::u32string>& engineBaseline,
                                  const std::u32string& baseline, const std::u32string& committed) {
    if (committed == baseline || committed.empty() || !engine_ || !learningEnabled_) return;
    const std::u32string engineResult =
        engineBaseline ? *engineBaseline : engine_->EngineResult(reading, baseline).value_or(baseline);
    if (committed == engineResult) {
        learning_->Forget(reading, engineResult, baseline);
        Log("learning forgotten");
    } else {
        learning_->Record(reading, engineResult, committed);
        Log("learned");
    }
}

bool EngineHost::Convert(const std::u32string& reading, const std::u32string& context, int candidateCount,
                         std::vector<std::u32string>* results, std::string* error) {
    std::string localError;
    if (!error) error = &localError;
    if (!engine_) {
        *error = "エンジン未初期化";
        return false;
    }
    const bool ok = engine_->Convert(reading, context, candidateCount, results, error);
    if (!ok && *error != ZenzEngine::kCancelledError) Log("convert failed: " + *error);
    return ok;
}

void EngineHost::SetCancelCheck(std::function<bool()> check) {
    if (zenz_) zenz_->SetCancelCheck(std::move(check));
}

void EngineHost::RecordCommit(const std::u32string& reading, const std::u32string& engineBaseline,
                              const std::u32string& baseline, const std::u32string& committed) {
    RecordCorrection(reading, engineBaseline, baseline, committed);
}

std::optional<std::u32string> EngineHost::EngineResult(const std::u32string& reading, const std::u32string& shown) {
    if (!engine_) return std::nullopt;
    return engine_->EngineResult(reading, shown);
}

void EngineHost::ForgetLearning(const std::u32string& reading, const std::u32string& engineResult,
                                const std::u32string& shown) {
    if (!learning_) return;
    learning_->Forget(reading, engineResult, shown);
    Log("learning forgotten (display form chosen)");
}

// Ctrl+. で切り替えた句読点を設定に保存する（次に起動したときも同じにする。macOS版と同じ）
void EngineHost::PunctuationChanged(const std::u32string& punctuation) {
    const std::filesystem::path path = ConfigPath();
    Config config = LoadConfig(path);
    config.punctuation = punctuation;
    SaveConfig(path, config);
    sessionSettings_.punctuation = punctuation;
}

std::vector<std::u32string> EngineHost::CandidateOnlyWords(const std::u32string& reading) {
    if (!userDictionary_ || privateMode_) return {};
    return userDictionary_->Current().CandidateOnlyWords(reading);
}

std::vector<InputHistoryEntry> EngineHost::PredictionCandidates(const std::u32string& prefix) {
    if (!inputHistory_) return {};
    return inputHistory_->Candidates(prefix);
}

void EngineHost::RecordInputHistory(const std::u32string& reading, const std::u32string& text) {
    if (inputHistory_) inputHistory_->Record(reading, text, InputHistoryStore::NowSeconds());
}

// プライベートモードの切り替えを設定に保存する（次に起動したときも同じにする。macOS版と同じ）
void EngineHost::PrivateModeChanged(bool enabled) {
    const std::filesystem::path path = ConfigPath();
    Config config = LoadConfig(path);
    config.privateMode = enabled;
    SaveConfig(path, config);
    privateMode_ = enabled;
    sessionSettings_.privateMode = enabled;
    Log(enabled ? "private mode on" : "private mode off");
}

// 確定を変換記録に書く（モデルの名前はファイル名から拡張子を除いたもの。macOS版と同じ）
void EngineHost::RecordConversion(const ConversionLogEntry& entry) {
    if (!conversionLog_) return;
    ConversionLogEntry named = entry;
    named.model = PathToUtf8(PathFromUtf8(modelPath_).stem());
    conversionLog_->Record(named);
}

bool EngineHost::TypoCorrectionAvailable() { return Typo() != nullptr; }

std::optional<TypoCorrection> EngineHost::CorrectTypo(const std::u32string& reading, double threshold,
                                                      size_t minimumLength) {
    const auto typo = Typo();
    if (!typo) return std::nullopt;
    std::string error;
    auto correction = typo->Correction(reading, threshold, minimumLength, &error);
    if (!error.empty()) Log("typo correction failed: " + error);
    return correction;
}

} // namespace iroha
