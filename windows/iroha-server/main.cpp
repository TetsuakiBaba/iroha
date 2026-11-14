// iroha変換サーバ。
// 名前付きパイプでTIP（各アプリのプロセス内）からの変換要求を受け、
// ZenzEngine（llama.cpp + GGUFモデル）で変換して返す。
//
// - 単一インスタンス（セッション別の名前付きミューテックスでガード）
// - パイプは1つのスレッドで1接続ずつ処理する
// - 入力の状態（読み・ライブ変換・文節・候補）は TIP ごとのセッションとしてここに持つ
//   （iroha::SessionManager）。ライブ変換だけは裏のスレッドで行い、TIP が結果を問い合わせる
// - モデルは起動時にプリロードする（初回変換のもたつき防止）

#include "emoji_renderability.h"
#include "ipc_protocol.h"

#include <sddl.h>

#include <cstdio>
#include <atomic>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <memory>
#include <array>
#include <chrono>
#include <mutex>
#include <thread>

#include "iroha/chunked_conversion_engine.h"
#include "iroha/config.h"
#include "ai_client.h"
#include "iroha/ai_conversion.h"
#include "iroha/conversion_log.h"
#include "key_shortcut.h"
#include "secret_store.h"
#include "selection_assistant.h"
#include "voice_recognizer.h"
#include "iroha/typo_normalizer.h"
#include "iroha/typo_normalizer_catalog.h"
#include "iroha/conversion_engine.h"
#include "iroha/lattice_converter.h"
#include "iroha/lattice_rescoring_engine.h"
#include "iroha/learning_engine.h"
#include "iroha/learning_store.h"
#include "iroha/reading_aligner.h"
#include "iroha/session_manager.h"
#include "iroha/symbol_candidates.h"
#include "iroha/term_dictionary.h"
#include "iroha/input_history.h"
#include "iroha/update_check.h"
#include "iroha/user_rewrite_rule.h"
#include "iroha/unicode.h"
#include "iroha/user_dictionary_engine.h"
#include "iroha/user_dictionary_store.h"
#include "iroha/variant_kanji_engine.h"
#include "iroha/zenz_engine.h"

namespace {

void Log(const char* message) {
    std::printf("%s\n", message);
    std::fflush(stdout);
    OutputDebugStringA((std::string("[iroha-server] ") + message + "\n").c_str());
}

std::wstring DataDir() {
    const wchar_t* localAppData = _wgetenv(L"LOCALAPPDATA");
    return (localAppData ? std::wstring(localAppData) : L".") + L"\\iroha";
}

// モデルパスの決定: IROHA_MODEL環境変数 > config.jsonの"model" > 既定。
// config側はファイル名（modelsディレクトリ相対）と絶対パスの両方を受ける
std::string ResolveModelPath() {
    if (const wchar_t* env = _wgetenv(L"IROHA_MODEL")) {
        return iroha::Utf32ToUtf8(iroha::Utf16ToUtf32(env));
    }
    const iroha::Config config =
        iroha::LoadConfig(std::filesystem::path(DataDir() + L"\\config.json"));
    if (!config.model.empty()) {
        const std::wstring model = iroha::Utf32ToUtf16(config.model);
        const std::filesystem::path path(model);
        if (path.is_absolute()) return iroha::Utf32ToUtf8(config.model);
        return iroha::Utf32ToUtf8(iroha::Utf16ToUtf32(DataDir() + L"\\models\\" + model));
    }
    return iroha::Utf32ToUtf8(iroha::Utf16ToUtf32(
        iroha::DefaultModelPath(std::filesystem::path(DataDir() + L"\\models")).wstring()));
}

std::filesystem::path ExecutableDirectory() {
    wchar_t modulePath[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    return std::filesystem::path(modulePath).parent_path();
}

// このコンピュータの名前（入力履歴のファイル名。macOS版のホスト名にあたる）
std::string HostName() {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD length = ARRAYSIZE(name);
    if (!GetComputerNameW(name, &length)) return "windows";
    return iroha::Utf32ToUtf8(iroha::Utf16ToUtf32(std::wstring(name, length)));
}

std::filesystem::path StorePath(const wchar_t* envName, const wchar_t* fileName) {
    if (const wchar_t* env = _wgetenv(envName)) return std::filesystem::path(env);
    return std::filesystem::path(DataDir() + L"\\" + fileName);
}

// ストアアプリ（AppContainer・低整合性）内のTIPからも接続できるパイプの
// セキュリティ記述子を作る:
//   - 実行ユーザにフルアクセス
//   - ALL APPLICATION PACKAGES / ALL RESTRICTED APPLICATION PACKAGES に読み書き
//   - 低整合性レベルからのアクセスを許可（UWPはLow ILで動く）
PSECURITY_DESCRIPTOR CreatePipeSecurityDescriptor() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return nullptr;
    BYTE buffer[256] = {};
    DWORD length = 0;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &length)) {
        LPWSTR userSid = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid,
                                   &userSid)) {
            wchar_t sddl[512];
            _snwprintf_s(sddl, _TRUNCATE,
                         L"D:(A;;GA;;;%s)"
                         L"(A;;GRGW;;;S-1-15-2-1)"
                         L"(A;;GRGW;;;S-1-15-2-2)"
                         L"S:(ML;;NW;;;LW)",
                         userSid);
            LocalFree(userSid);
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                    sddl, SDDL_REVISION_1, &descriptor, nullptr)) {
                descriptor = nullptr;
            }
        }
    }
    CloseHandle(token);
    return descriptor;
}

// IME本体と同じ構成のエンジン一式:
// 学習 → ユーザ辞書 → 長い読みの区切り → 異体字 → 辞書ラティスの候補をモデルで再採点 → モデル。
// 候補を複数求められたとき（候補ウィンドウ）は辞書ラティスで候補を作ってモデルで並べ、
// 1つだけのとき（全体の変換）はモデルの生成に任せる。辞書が無ければモデル単体で動く。
// 設定GUIからのReload要求で設定・辞書を読み直す（モデルは変わったときだけロード）
struct ServerState {
    std::string modelPath;
    std::string adapterPath;
    std::unique_ptr<iroha::ZenzEngine> zenz;
    std::unique_ptr<iroha::LatticeConverter> lattice;
    std::unique_ptr<iroha::LatticeRescoringEngine> rescoring;
    std::unique_ptr<iroha::VariantKanjiEngine> variants;
    std::unique_ptr<iroha::ChunkedConversionEngine> chunked;
    std::unique_ptr<iroha::UserDictionaryStore> userDictionary;
    std::unique_ptr<iroha::LearningStore> learning;
    // 入力履歴（予測変換。<データフォルダ>/input-history/<コンピュータ名>.json）
    std::unique_ptr<iroha::InputHistoryStore> inputHistory;
    // 変換記録（学習用データ。<データフォルダ>/logs/conversions/。設定で ON のときだけ書く）
    std::unique_ptr<iroha::ConversionLog> conversionLog;
    // 打ち間違いの訂正（設定で ON で、モデルが入っているときだけ。訂正の裏のスレッドからも使うので鍵で守る。
    // 入れ直されたら（weights の SHA-256 が変わったら）作り直す）
    std::mutex typoMutex;
    std::shared_ptr<iroha::TypoNormalizer> typo;
    std::string typoSignature;

    std::shared_ptr<iroha::TypoNormalizer> Typo() {
        std::lock_guard<std::mutex> lock(typoMutex);
        return typo;
    }

    // 「こえ」モードの録音と認識（main で作る。sessions より先に片付ける）
    iroha::VoiceRecognizer* voice = nullptr;

    // 選択テキストのAI編集（main で作って、設定を読み直すたびに渡す）
    iroha::SelectionAssistant* selection = nullptr;
    iroha::SelectionAssistantSettings selectionSettings;

    void LoadSelection(const iroha::Config& config) {
        iroha::SelectionAssistantSettings settings;
        settings.enabled = config.selectionEnabled;
        for (size_t i = 0; i < config.selectionPresets.size(); ++i) {
            const auto& stored = config.selectionPresets[i];
            const auto& fallback = iroha::SelectionSettingsDefaults::Presets()[i];
            settings.presets.push_back({stored.name.value_or(fallback.name), stored.prompt.value_or(fallback.prompt),
                                        stored.hotkey.value_or(fallback.hotkey), stored.enabled.value_or(fallback.enabled)});
        }
        settings.onDemandHotkey = config.selectionOnDemandHotkey.value_or(iroha::SelectionSettingsDefaults::kOnDemandHotkey);
        using Trigger = iroha::SelectionAssistantSettings::Trigger;
        settings.trigger = config.selectionTriggerMode == "off"             ? Trigger::Off
                           : config.selectionTriggerMode == "immediateMenu" ? Trigger::ImmediateMenu
                           : config.selectionTriggerMode == "optionMenu"    ? Trigger::AltMenu
                                                                            : Trigger::Bubble;
        settings.characterCount = config.selectionCharacterCount;
        // 使わないアプリ: カンマか改行で区切った実行ファイルの名前（小文字で比べる）
        std::wstring current;
        const std::wstring apps = iroha::Utf32ToUtf16(iroha::Utf8ToUtf32(config.selectionExcludedApps)) + L",";
        for (wchar_t c : apps) {
            if (c == L',' || c == L'\n' || c == L'\r') {
                const std::u32string trimmed = iroha::TrimWhitespace(iroha::Utf16ToUtf32(current));
                std::wstring name = iroha::Utf32ToUtf16(trimmed);
                std::transform(name.begin(), name.end(), name.begin(),
                               [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
                if (!name.empty()) settings.excludedApps.push_back(name);
                current.clear();
            } else {
                current += c;
            }
        }
        selectionSettings = std::move(settings);
        if (selection) selection->Configure(selectionSettings);
    }

    // AI変換の接続先とプリセット（裏のスレッドから読むので鍵で守る）
    std::mutex aiMutex;
    iroha::AIConnection aiConnection;
    std::array<iroha::AICommitPreset, iroha::AICommitSettings::kCount> aiPresets;

    void LoadAI(const iroha::Config& config) {
        iroha::AIConnection connection;
        connection.service = iroha::ParseAIService(config.aiService);
        switch (connection.service) {
        case iroha::AIService::Ollama:
            connection.endpoint = config.ollamaEndpoint;
            connection.model = config.ollamaModel;
            break;
        case iroha::AIService::LMStudio:
            connection.endpoint = config.lmStudioEndpoint;
            connection.model = config.lmStudioModel;
            break;
        case iroha::AIService::OpenAI:
            connection.endpoint = config.openAIEndpoint;
            connection.model = config.openAIModel;
            iroha::SecretStore::Get(iroha::AIClient::kOpenAIKeyAccount, &connection.apiKey);
            break;
        case iroha::AIService::Claude:
            connection.model = config.claudeModel;
            iroha::SecretStore::Get(iroha::AIClient::kClaudeKeyAccount, &connection.apiKey);
            break;
        default:
            break;
        }
        std::array<iroha::AICommitPreset, iroha::AICommitSettings::kCount> presets;
        sessionSettings.aiPresetNames.clear();
        sessionSettings.aiShortcuts.clear();
        for (size_t i = 0; i < presets.size(); ++i) {
            const auto& stored = config.aiPresets[i];
            const auto& fallback = iroha::AICommitSettings::Defaults()[i];
            presets[i].name = stored.name.value_or(fallback.name);
            presets[i].prompt = stored.prompt.value_or(fallback.prompt);
            presets[i].shortcut = stored.hotkey.value_or(fallback.shortcut);
            sessionSettings.aiPresetNames.push_back(presets[i].DisplayName(i));
            const auto shortcut = iroha::KeyShortcut::Parse(presets[i].shortcut);
            sessionSettings.aiShortcuts.push_back(shortcut ? shortcut->Encode() : 0);
        }
        sessionSettings.aiAvailable = iroha::AIClient::IsConfigured(connection);
        std::lock_guard<std::mutex> lock(aiMutex);
        aiConnection = std::move(connection);
        aiPresets = std::move(presets);
    }

    void LoadTypoNormalizer(bool enabled) {
        std::filesystem::path directory = iroha::TypoNormalizerInstall::Directory(std::filesystem::path(DataDir()));
        if (const wchar_t* env = _wgetenv(L"IROHA_TYPO_MODEL")) {
            if (env[0] != 0) directory = std::filesystem::path(env);
        }
        std::shared_ptr<iroha::TypoNormalizer> next;
        std::string signature;
        if (enabled && iroha::TypoNormalizer::IsModelDirectory(directory)) {
            // 入れ直されたかは weights.bin の大きさと更新日時で見る
            std::error_code ignored;
            const auto weights = directory / "weights.bin";
            signature = directory.u8string() + "|" + std::to_string(std::filesystem::file_size(weights, ignored)) + "|" +
                        std::to_string(std::filesystem::last_write_time(weights, ignored).time_since_epoch().count());
            {
                std::lock_guard<std::mutex> lock(typoMutex);
                if (typo && signature == typoSignature) return;
            }
            next = std::make_shared<iroha::TypoNormalizer>(directory);
            std::string error;
            const auto start = GetTickCount64();
            if (next->Load(&error)) {
                char message[160];
                std::snprintf(message, sizeof(message), "typo normalizer loaded (%llums, %s)", GetTickCount64() - start,
                              iroha::TypoNormalizer::KernelName());
                Log(message);
            } else {
                Log(("typo normalizer failed: " + error).c_str());
                next = nullptr;
            }
        } else if (enabled) {
            Log("typo normalizer not installed");
        }
        std::lock_guard<std::mutex> lock(typoMutex);
        typo = std::move(next);
        typoSignature = signature;
    }
    // プライベートモードの間は、学習の差し替えとユーザ辞書を止める（変換のスレッドからも読む）
    std::atomic<bool> privateMode{false};
    // 変換の学習を使うか（設定。OFF なら記録も適用もしない）
    std::atomic<bool> learningEnabled{true};
    // 変換ルール（設定画面が書き換えたら Reload で読み直す。初回は既定のルールを足す。macOS版と同じ）
    std::unique_ptr<iroha::UserRewriteRuleStore> rewriteRules;
    std::shared_ptr<const iroha::UserRewriteRuleSet> rewriteRuleSet;
    std::unique_ptr<iroha::UserDictionaryEngine> dictionaryEngine;
    std::unique_ptr<iroha::LearningEngine> engine;
    // セッションの設定（ライブ変換・句読点・全角スペース・候補数）
    iroha::SessionSettings sessionSettings;
    // 記号・顔文字・絵文字の表（設定で切ってあれば nullptr。読み込みは初めて要るときに 1 回だけ）
    std::shared_ptr<const iroha::SymbolCandidateTable> symbols;
    std::shared_ptr<const iroha::SymbolCandidateTable> loadedSymbols;
    bool symbolsLoaded = false;
    // 専門辞書（設定画面から入れたもの。入れ外しされたら Reload で読み直す）
    std::filesystem::path dictionaryPath;
    std::string termSignature;
    bool termsLoaded = false;
    std::shared_ptr<const iroha::TermLatticeIndex> termIndex;
    std::shared_ptr<const iroha::TermDictionaryLookup> termLookup;

    // 入れている専門辞書が変わっていれば読み直す（数十万語で 1 秒前後）
    void LoadTermDictionaries() {
        const std::filesystem::path data(DataDir());
        const auto records = iroha::TermDictionaryInstall::InstalledRecords(data);
        const std::string signature = iroha::TermDictionaryInstall::Signature(records);
        if (termsLoaded && signature == termSignature) return;
        termsLoaded = true;
        termSignature = signature;
        termIndex = nullptr;
        termLookup = nullptr;
        if (records.empty()) return;
        const auto start = GetTickCount64();
        if (!dictionaryPath.empty()) {
            termIndex = iroha::LatticeConverter::BuildTermIndex(
                dictionaryPath, iroha::TermDictionaryInstall::LoadPacks(data, records));
        }
        termLookup = std::make_shared<iroha::TermDictionaryLookup>(
            iroha::TermDictionaryLookup::Load(data, records));
        char message[160];
        std::snprintf(message, sizeof(message), "term dictionaries loaded: %zu dictionaries, %zu words (%llums)",
                      records.size(),
                      termIndex ? iroha::LatticeConverter::TermIndexEntryCount(*termIndex) : size_t(0),
                      GetTickCount64() - start);
        Log(message);
    }

    void Build() {
        const iroha::Config config =
            iroha::LoadConfig(std::filesystem::path(DataDir() + L"\\config.json"));
        sessionSettings.liveConversion = config.liveConversion;
        sessionSettings.punctuation = config.punctuation;
        sessionSettings.fullWidthSpace = config.fullWidthSpace;
        sessionSettings.candidateCount = config.candidateCount;
        sessionSettings.predictive = config.predictive;
        sessionSettings.privateMode = config.privateMode;
        sessionSettings.conversionLog = config.conversionLog;
        sessionSettings.typoCorrection = config.typoCorrection;
        sessionSettings.typoThreshold = config.typoThreshold;
        sessionSettings.typoDelayMs = config.typoDelayMs;
        sessionSettings.typoMinimumLength = config.typoMinimumLength;
        LoadTypoNormalizer(config.typoCorrection);
        LoadAI(config);
        LoadSelection(config);
        // 区切り記号の英字入力。記号は英字・数字以外の ASCII の記号 1 文字（使えない値なら「_」。macOS版と同じ）
        const auto symbol = [](const std::string& text) -> char32_t {
            const bool valid = text.size() == 1 && text[0] >= 0x21 && text[0] <= 0x7E &&
                               !std::isalnum(static_cast<unsigned char>(text[0]));
            return valid ? static_cast<char32_t>(text[0]) : U'_';
        };
        sessionSettings.aiFix = config.aiFix;
        sessionSettings.aiFixThreshold = config.aiFixThreshold;
        const auto fixShortcut = iroha::KeyShortcut::Parse(config.aiFixShortcut);
        sessionSettings.aiFixShortcut = fixShortcut ? fixShortcut->Encode() : 0;
        sessionSettings.aiFixShortcutLabel = fixShortcut ? iroha::Utf8ToUtf32(config.aiFixShortcut) : U"";
        // 「こえ」モード: 押している間録音するキー（TIP に渡す仮想キーコード。右 Ctrl か右 Shift）
        sessionSettings.voiceInput = config.voiceInput;
        sessionSettings.documentContext = config.documentContext;
        const auto privateShortcut = iroha::KeyShortcut::Parse(config.privateModeHotkey);
        sessionSettings.privateModeShortcut = privateShortcut ? privateShortcut->Encode() : 0;
        // デバッグ表示は開発者モードが ON のときだけ（macOS版と同じ）
        sessionSettings.developerOverlay = config.developerMode && config.developerOverlay;
        learningEnabled = config.learningEnabled;
        sessionSettings.voiceKey = config.voiceInputHoldKey == "rightShift" ? VK_RSHIFT : VK_RCONTROL;
        sessionSettings.delimitedAlphabet = config.delimitedAlphabet;
        sessionSettings.delimitedStart = symbol(config.delimitedAlphabetStart);
        sessionSettings.delimitedEnd = symbol(config.delimitedAlphabetEnd);
        sessionSettings.conversionLogScope = config.conversionLogCorrectionsOnly
                                                 ? iroha::ConversionLogScope::Corrections
                                                 : iroha::ConversionLogScope::All;
        privateMode = config.privateMode;
        if (!conversionLog) {
            conversionLog = std::make_unique<iroha::ConversionLog>(
                StorePath(L"IROHA_CONVERSION_LOG", L"logs\\conversions"), HostName());
        }
        if (!inputHistory) {
            inputHistory = std::make_unique<iroha::InputHistoryStore>(
                StorePath(L"IROHA_INPUT_HISTORY", L"input-history"), HostName());
        } else {
            inputHistory->Reload();
        }
        if (config.symbolCandidates && !symbolsLoaded) {
            symbolsLoaded = true;
            if (const auto directory =
                    iroha::SymbolCandidateTable::DefaultDirectory(ExecutableDirectory())) {
                // Windows が描けない絵文字（新しい版の絵文字・国旗）は表に入れない
                const auto start = GetTickCount64();
                if (auto table = iroha::SymbolCandidateTable::Load(
                        *directory, iroha::EmojiRenderability::MakePredicate())) {
                    loadedSymbols = std::make_shared<iroha::SymbolCandidateTable>(std::move(*table));
                    const auto counts = loadedSymbols->GetCounts();
                    char message[160];
                    std::snprintf(message, sizeof(message),
                                  "symbols loaded: emoji %zu, emoticons %zu, symbols %zu (%llums)",
                                  counts.emoji, counts.emoticons, counts.symbols,
                                  GetTickCount64() - start);
                    Log(message);
                }
            } else {
                Log("symbols not found (no symbol candidates)");
            }
        }
        symbols = config.symbolCandidates ? loadedSymbols : nullptr;
        const std::string newModelPath = ResolveModelPath();
        // 追加学習した LoRA アダプタ（ファイルが無ければ使わない）
        std::string newAdapterPath = config.modelAdapterPath;
        if (!newAdapterPath.empty() &&
            !std::filesystem::exists(std::filesystem::path(iroha::Utf32ToUtf16(iroha::Utf8ToUtf32(newAdapterPath))))) {
            Log(("adapter not found: " + newAdapterPath).c_str());
            newAdapterPath.clear();
        }
        if (!zenz || newModelPath != modelPath || newAdapterPath != adapterPath) {
            iroha::ZenzEngine::Options options;
            options.adapterPath = newAdapterPath;
            zenz = std::make_unique<iroha::ZenzEngine>(newModelPath, options);
            modelPath = newModelPath;
            adapterPath = newAdapterPath;
            if (!adapterPath.empty()) Log(("adapter: " + adapterPath).c_str());
            std::string error;
            if (zenz->Prewarm(&error)) {
                Log(("model loaded: " + newModelPath).c_str());
            } else {
                // モデル未取得でも起動は継続する（変換要求時にエラーを返す）
                Log(("prewarm failed: " + error).c_str());
            }
        }
        if (!lattice) {
            if (const auto dictionary =
                    iroha::LatticeConverter::DefaultDictionaryPath(ExecutableDirectory())) {
                dictionaryPath = *dictionary;
                lattice = std::make_unique<iroha::LatticeConverter>(*dictionary);
                // 辞書の読み込みを済ませておく（最初の候補ウィンドウのもたつき防止）
                lattice->Candidates(U"うぉーむあっぷ", 1);
                Log("dictionary loaded");
            } else {
                Log("dictionary not found (candidates come from the model only)");
            }
        }
        LoadTermDictionaries();
        if (lattice) lattice->SetTermDictionary(termIndex);
        iroha::ConversionEngine* core = zenz.get();
        if (lattice) {
            rescoring = std::make_unique<iroha::LatticeRescoringEngine>(
                zenz.get(), zenz.get(), lattice.get());
            core = rescoring.get();
        }
        // 異体字を候補の末尾に補い、80文字を超える読みは区切って順に変換する
        variants = std::make_unique<iroha::VariantKanjiEngine>(core);
        chunked = std::make_unique<iroha::ChunkedConversionEngine>(variants.get());
        core = chunked.get();
        userDictionary = std::make_unique<iroha::UserDictionaryStore>(
            StorePath(L"IROHA_USER_DICT", L"user-dictionary.json"));
        learning = std::make_unique<iroha::LearningStore>(
            StorePath(L"IROHA_LEARNING", L"learning.json"));
        if (!rewriteRules) {
            rewriteRules = std::make_unique<iroha::UserRewriteRuleStore>(
                StorePath(L"IROHA_REWRITE_RULES", L"user-rewrite-rules.json"), /*seedsDefaults=*/true);
        } else {
            rewriteRules->Reload();
        }
        rewriteRuleSet = std::make_shared<iroha::UserRewriteRuleSet>(rewriteRules->Current());
        // プライベートモードの間はユーザ辞書と学習を使わない（変換を iroha 本来の結果にする。macOS版と同じ）
        dictionaryEngine = std::make_unique<iroha::UserDictionaryEngine>(
            core,
            [this] { return privateMode ? iroha::UserDictionary() : userDictionary->Current(); },
            lattice.get());
        engine = std::make_unique<iroha::LearningEngine>(
            dictionaryEngine.get(),
            [this] { return privateMode || !learningEnabled ? iroha::LearningDictionary() : learning->Current(); });
    }
};

// 文節変換の結果が表示していた変換（baseline）と違っていたら、ユーザによる修正として学習する
// （macOS版 IrohaInputController.learnIfCorrected と同じ規則）。
// 覚えるのは入力の読み全体 → 確定文字列と、直す前にエンジンが出していた結果。
// 学習が差し替えた結果を、ユーザがエンジンの結果に戻して確定したら、その学習を消す
// engineBaseline が分からない（前の版の TIP からの Record）なら、エンジンに聞く
void RecordCorrection(ServerState& state, const std::u32string& reading,
                      const std::optional<std::u32string>& engineBaseline,
                      const std::u32string& baseline, const std::u32string& committed) {
    if (committed == baseline || committed.empty() || !state.engine) return;
    const std::u32string engineResult =
        engineBaseline ? *engineBaseline : state.engine->EngineResult(reading, baseline).value_or(baseline);
    if (committed == engineResult) {
        state.learning->Forget(reading, engineResult, baseline);
        Log("learning forgotten");
    } else {
        state.learning->Record(reading, engineResult, committed);
        Log("learned");
    }
}

// セッション（iroha::SessionManager）から見た変換エンジンと学習
class ServerBackend : public iroha::SessionManager::Backend {
public:
    explicit ServerBackend(ServerState* state) : state_(state) {}

    bool Convert(const std::u32string& reading, const std::u32string& context, int candidateCount,
                 std::vector<std::u32string>* results, std::string* error) override {
        std::string localError;
        if (!error) error = &localError;
        if (!state_->engine) {
            *error = "エンジン未初期化";
            return false;
        }
        const bool ok = state_->engine->Convert(reading, context, candidateCount, results, error);
        if (!ok && *error != iroha::ZenzEngine::kCancelledError) {
            Log(("convert failed: " + *error).c_str());
        }
        return ok;
    }

    void SetCancelCheck(std::function<bool()> check) override {
        if (state_->zenz) state_->zenz->SetCancelCheck(std::move(check));
    }

    void RecordCommit(const std::u32string& reading, const std::u32string& engineBaseline,
                      const std::u32string& baseline, const std::u32string& committed) override {
        if (!state_->learningEnabled) return;
        RecordCorrection(*state_, reading, engineBaseline, baseline, committed);
    }

    std::optional<std::u32string> EngineResult(const std::u32string& reading,
                                               const std::u32string& shown) override {
        if (!state_->engine) return std::nullopt;
        return state_->engine->EngineResult(reading, shown);
    }

    std::vector<std::u32string> CandidateOnlyWords(const std::u32string& reading) override {
        if (!state_->userDictionary || state_->privateMode) return {};
        return state_->userDictionary->Current().CandidateOnlyWords(reading);
    }

    std::vector<iroha::InputHistoryEntry> PredictionCandidates(const std::u32string& prefix) override {
        if (!state_->inputHistory) return {};
        return state_->inputHistory->Candidates(prefix);
    }

    void RecordInputHistory(const std::u32string& reading, const std::u32string& text) override {
        if (state_->inputHistory) state_->inputHistory->Record(reading, text, iroha::InputHistoryStore::NowSeconds());
    }

    bool TypoCorrectionAvailable() override { return state_->Typo() != nullptr; }

    // AIで直す: 学習・ユーザ辞書・辞書ラティスを通さない、モデル単体の貪欲変換（macOS版と同じ）
    ScoredText ScoreConversion(const std::u32string& reading, const std::u32string& context) override {
        ScoredText scored;
        if (!state_->zenz) return scored;
        iroha::ZenzEngine::ScoredConversion result;
        std::string error;
        if (!state_->zenz->ConvertScored(reading, context, &result, &error)) return scored;
        scored.text = result.text;
        for (const auto& confidence : result.confidences) scored.margins.push_back(confidence.margin);
        return scored;
    }

    iroha::AIOutcome RunAIRequest(const iroha::AIRequest& request, int stallSeconds, iroha::CancelToken* cancel) override {
        iroha::AIConnection connection;
        {
            std::lock_guard<std::mutex> lock(state_->aiMutex);
            connection = state_->aiConnection;
        }
        const auto start = GetTickCount64();
        const iroha::AIResult result = iroha::AIClient::Run(connection, request, nullptr, cancel, stallSeconds);
        char message[512];
        std::snprintf(message, sizeof(message), "ai fix %s (%llums)%s%s", iroha::AIServiceKey(connection.service),
                      GetTickCount64() - start, result.text ? "" : ": ", result.text ? "" : result.error.c_str());
        Log(message);
        return iroha::AIOutcome{result.text, result.error};
    }

    std::optional<std::u32string> RunAI(size_t preset, const std::u32string& text,
                                        const std::function<void(const std::u32string&)>& onPartial,
                                        iroha::CancelToken* cancel) override {
        iroha::AIConnection connection;
        iroha::AICommitPreset settings;
        {
            std::lock_guard<std::mutex> lock(state_->aiMutex);
            if (preset >= state_->aiPresets.size()) return std::nullopt;
            connection = state_->aiConnection;
            settings = state_->aiPresets[preset];
        }
        const auto start = GetTickCount64();
        const iroha::AIResult result =
            iroha::AIClient::Run(connection, settings.Request(preset, text), onPartial, cancel);
        char message[512];
        std::snprintf(message, sizeof(message), "ai %s (%llums)%s%s", iroha::AIServiceKey(connection.service),
                      GetTickCount64() - start, result.text ? "" : ": ", result.text ? "" : result.error.c_str());
        Log(message);
        return result.text;
    }

    std::optional<iroha::TypoCorrection> CorrectTypo(const std::u32string& reading, double threshold,
                                                     size_t minimumLength) override {
        const auto typo = state_->Typo();
        if (!typo) return std::nullopt;
        std::string error;
        auto correction = typo->Correction(reading, threshold, minimumLength, &error);
        if (!error.empty()) Log(("typo correction failed: " + error).c_str());
        return correction;
    }

    // 確定を変換記録に書く（モデルの名前はファイル名から拡張子を除いたもの。macOS版と同じ）
    void RecordConversion(const iroha::ConversionLogEntry& entry) override {
        if (!state_->conversionLog) return;
        iroha::ConversionLogEntry named = entry;
        named.model = iroha::Utf32ToUtf8(iroha::Utf16ToUtf32(
            std::filesystem::path(iroha::Utf32ToUtf16(iroha::Utf8ToUtf32(state_->modelPath))).stem().wstring()));
        state_->conversionLog->Record(named);
    }

    // Ctrl+Alt+P で切り替えたプライベートモードを設定に保存する（次に起動したときも同じにする。macOS版と同じ）
    void PrivateModeChanged(bool enabled) override {
        const std::filesystem::path path(DataDir() + L"\\config.json");
        iroha::Config config = iroha::LoadConfig(path);
        config.privateMode = enabled;
        iroha::SaveConfig(path, config);
        state_->privateMode = enabled;
        state_->sessionSettings.privateMode = enabled;
        Log(enabled ? "private mode on" : "private mode off");
    }

    void ForgetLearning(const std::u32string& reading, const std::u32string& engineResult,
                        const std::u32string& shown) override {
        if (!state_->learning) return;
        state_->learning->Forget(reading, engineResult, shown);
        Log("learning forgotten (display form chosen)");
    }

    // 「こえ」モード。知らせはそのまま渡し、ログには種類だけを書く（話した内容は書かない）
    std::u32string StartVoice(std::function<void(const iroha::InputSession::VoiceEvent&)> onEvent) override {
        if (!state_->voice) return U"音声認識を使えません";
        const auto start = GetTickCount64();
        return state_->voice->Start([onEvent, start](const iroha::InputSession::VoiceEvent& event) {
            using Kind = iroha::InputSession::VoiceEvent::Kind;
            if (event.kind == Kind::Finished || event.kind == Kind::Failed || event.kind == Kind::Discarded) {
                char message[160];
                std::snprintf(message, sizeof(message), "voice %s (%llums, %zu words)%s%s",
                              event.kind == Kind::Finished ? "finished" : event.kind == Kind::Failed ? "failed" : "discarded",
                              GetTickCount64() - start, event.tokens.size(), event.kind == Kind::Failed ? ": " : "",
                              event.kind == Kind::Failed ? iroha::Utf32ToUtf8(event.text).c_str() : "");
                Log(message);
            }
            onEvent(event);
        });
    }
    void StopVoice() override {
        if (state_->voice) state_->voice->Stop();
    }
    void CancelVoice() override {
        if (state_->voice) state_->voice->Cancel();
    }

    // 言語バーのメニューで切り替えたライブ変換・予測変換を設定に保存する（設定画面にも同じ値が出る）
    void ConversionOptionsChanged(bool liveConversion, bool predictive) override {
        const std::filesystem::path path(DataDir() + L"\\config.json");
        iroha::Config config = iroha::LoadConfig(path);
        config.liveConversion = liveConversion;
        config.predictive = predictive;
        iroha::SaveConfig(path, config);
        state_->sessionSettings.liveConversion = liveConversion;
        state_->sessionSettings.predictive = predictive;
    }

    // Ctrl+. で切り替えた句読点を設定に保存する（次に起動したときも同じにする。macOS版と同じ）
    void PunctuationChanged(const std::u32string& punctuation) override {
        const std::filesystem::path path(DataDir() + L"\\config.json");
        iroha::Config config = iroha::LoadConfig(path);
        config.punctuation = punctuation;
        iroha::SaveConfig(path, config);
        state_->sessionSettings.punctuation = punctuation;
    }

private:
    ServerState* state_;
};

// 1リクエストを処理してレスポンスのバイト列を返す。shutdownならfalseを返す
// 前の版の TIP を読み込んだままのアプリを知らせる（アプリは起動したときの TIP を使い続ける。インストールし直しても
// 開き直すまで新しい機能が効かない）。プロセスごとに 1 回だけ
void NoticeStaleTip(ServerState& state, DWORD processId) {
    static std::vector<DWORD> noticed;
    if (processId == 0 || !state.selection ||
        std::find(noticed.begin(), noticed.end(), processId) != noticed.end()) {
        return;
    }
    noticed.push_back(processId);
    std::wstring name = L"このアプリ";
    if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId)) {
        wchar_t path[MAX_PATH];
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(process, 0, path, &size)) {
            name = std::filesystem::path(path).stem().wstring();
        }
        CloseHandle(process);
    }
    Log(("stale tip: pid " + std::to_string(processId)).c_str());
    state.selection->ShowNotice(U"「" + iroha::Utf16ToUtf32(name) +
                                    U"」は前の版の iroha を読み込んだままです。新しい機能を使うには開き直してください",
                                8000);
}

bool HandleRequest(ServerState& state, iroha::SessionManager& sessions,
                   const std::vector<char>& request, std::vector<char>* response, DWORD clientProcessId) {
    iroha::ipc::Reader reader{request.data(), request.size()};
    uint32_t version = 0;
    uint32_t type = 0;
    if (!reader.U32(&version) || version != iroha::ipc::kProtocolVersion ||
        !reader.U32(&type)) {
        *response = iroha::ipc::BuildErrorResponse("プロトコルが不正です");
        return true;
    }
    switch (static_cast<iroha::ipc::MessageType>(type)) {
        case iroha::ipc::MessageType::Ping:
            *response = iroha::ipc::BuildOkResponse({});
            return true;
        case iroha::ipc::MessageType::Shutdown:
            *response = iroha::ipc::BuildOkResponse({});
            return false;
        case iroha::ipc::MessageType::Convert: {
            uint32_t candidateCount = 0;
            std::string readingUtf8;
            std::string contextUtf8;
            if (!reader.U32(&candidateCount) || !reader.String(&readingUtf8) ||
                !reader.String(&contextUtf8)) {
                *response = iroha::ipc::BuildErrorResponse("プロトコルが不正です");
                return true;
            }
            candidateCount = candidateCount == 0 ? 1 : candidateCount;
            candidateCount = candidateCount > 32 ? 32 : candidateCount;
            std::vector<std::u32string> candidates;
            std::string error;
            if (!state.engine) {
                *response = iroha::ipc::BuildErrorResponse("エンジン未初期化");
                return true;
            }
            bool converted = false;
            sessions.WithEngine([&] {
                converted = state.engine->Convert(iroha::Utf8ToUtf32(readingUtf8),
                                                  iroha::Utf8ToUtf32(contextUtf8),
                                                  static_cast<int>(candidateCount), &candidates,
                                                  &error);
            });
            if (!converted) {
                Log(("convert failed: " + error).c_str());
                *response = iroha::ipc::BuildErrorResponse(error);
                return true;
            }
            std::vector<std::string> utf8Candidates;
            utf8Candidates.reserve(candidates.size());
            for (const auto& candidate : candidates) {
                utf8Candidates.push_back(iroha::Utf32ToUtf8(candidate));
            }
            *response = iroha::ipc::BuildOkResponse(utf8Candidates);
            return true;
        }
        case iroha::ipc::MessageType::Reload: {
            Log("reload requested");
            sessions.WithEngine([&] { state.Build(); });
            sessions.SetSettings(state.sessionSettings);
            sessions.SetSymbolTable(state.symbols);
            sessions.SetTermLookup(state.termLookup);
            sessions.SetRewriteRules(state.rewriteRuleSet);
            *response = iroha::ipc::BuildOkResponse({});
            return true;
        }
        case iroha::ipc::MessageType::SessionKey: {
            uint64_t sessionId = 0;
            uint32_t key = 0;
            uint32_t character = 0;
            uint32_t flags = 0;
            std::string leftContextUtf8;
            if (!reader.U64(&sessionId) || !reader.U32(&key) || !reader.U32(&character) ||
                !reader.U32(&flags) || !reader.String(&leftContextUtf8)) {
                *response = iroha::ipc::BuildErrorResponse("プロトコルが不正です");
                return true;
            }
            if (key < static_cast<uint32_t>(iroha::SessionKey::Character) || key > iroha::kMaxSessionKey) {
                *response = iroha::ipc::BuildErrorResponse("未知のキーです");
                return true;
            }
            // TIP の版（後から足した。無ければ前の版の TIP）
            uint32_t tipRevision = 0;
            reader.U32(&tipRevision);
            if (tipRevision < iroha::ipc::kTipRevision) NoticeStaleTip(state, clientProcessId);
            const std::u32string leftContext = iroha::Utf8ToUtf32(leftContextUtf8);
            const iroha::SessionOutput output = sessions.HandleKey(
                sessionId, static_cast<iroha::SessionKey>(key), static_cast<char32_t>(character),
                (flags & iroha::ipc::kKeyFlagShift) != 0,
                (flags & iroha::ipc::kKeyFlagHasLeftContext) != 0 ? &leftContext : nullptr);
            *response = iroha::ipc::BuildSessionResponse(&output);
            return true;
        }
        case iroha::ipc::MessageType::SessionPoll: {
            uint64_t sessionId = 0;
            uint64_t revision = 0;
            if (!reader.U64(&sessionId) || !reader.U64(&revision)) {
                *response = iroha::ipc::BuildErrorResponse("プロトコルが不正です");
                return true;
            }
            iroha::SessionOutput output;
            const bool changed = sessions.Poll(sessionId, revision, &output);
            *response = iroha::ipc::BuildSessionResponse(changed ? &output : nullptr);
            return true;
        }
        case iroha::ipc::MessageType::SessionEnd: {
            uint64_t sessionId = 0;
            if (!reader.U64(&sessionId)) {
                *response = iroha::ipc::BuildErrorResponse("プロトコルが不正です");
                return true;
            }
            sessions.End(sessionId);
            *response = iroha::ipc::BuildOkResponse({});
            return true;
        }
        case iroha::ipc::MessageType::Record: {
            std::string readingUtf8;
            std::string committedUtf8;
            std::string baselineUtf8;
            if (!reader.String(&readingUtf8) || !reader.String(&committedUtf8) ||
                !reader.String(&baselineUtf8)) {
                *response = iroha::ipc::BuildErrorResponse("プロトコルが不正です");
                return true;
            }
            sessions.WithEngine([&] {
                RecordCorrection(state, iroha::Utf8ToUtf32(readingUtf8), std::nullopt,
                                 iroha::Utf8ToUtf32(baselineUtf8),
                                 iroha::Utf8ToUtf32(committedUtf8));
            });
            *response = iroha::ipc::BuildOkResponse({});
            return true;
        }
        default:
            *response = iroha::ipc::BuildErrorResponse("未知のメッセージ種別です");
            return true;
    }
}

// アップデートの自動の確認（macOS版 UpdateChecker.autoCheckIfDue）。1 時間ごとに確認の時期かを見て、
// 時期なら設定画面を --check-update で起動する（GitHub に問い合わせ、新しい版があれば知らせる窓を出すのは設定画面）。
// 設定画面は確認した時刻を update.json に書くので、ここは読むだけ
void StartUpdateChecks() {
    // 開発用の変換サーバ（IROHA_PIPE_SUFFIX）では確かめない
    if (const wchar_t* suffix = _wgetenv(L"IROHA_PIPE_SUFFIX"); suffix && *suffix) return;
    std::thread([] {
        // ログオンの直後は避ける
        std::this_thread::sleep_for(std::chrono::minutes(2));
        int64_t launched = 0;
        for (;;) {
            const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
            const iroha::Config config = iroha::LoadConfig(std::filesystem::path(DataDir() + L"\\config.json"));
            const iroha::Update::State state =
                iroha::Update::LoadState(iroha::Update::StatePath(std::filesystem::path(DataDir())));
            // 設定画面が記録を書けなかったときに起動し続けないよう、起動は 1 時間に 1 回まで
            if (config.autoUpdateCheck && iroha::Update::IsDue(state, now) && now - launched >= 60 * 60) {
                launched = now;
                const std::wstring exe = (ExecutableDirectory() / L"iroha-settings.exe").wstring();
                std::wstring commandLine = L"\"" + exe + L"\" --check-update";
                STARTUPINFOW startupInfo = {sizeof(startupInfo)};
                PROCESS_INFORMATION process = {};
                if (CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                                   &startupInfo, &process)) {
                    CloseHandle(process.hThread);
                    CloseHandle(process.hProcess);
                    Log("update check started");
                } else {
                    Log(("update check failed to start: " + std::to_string(GetLastError())).c_str());
                }
            }
            std::this_thread::sleep_for(std::chrono::hours(1));
        }
    }).detach();
}

} // namespace

int wmain() {
    SetConsoleOutputCP(CP_UTF8);

    // 単一インスタンスガード（セッション別）
    const std::wstring mutexName = L"Local\\iroha-server-" + iroha::ipc::InstanceSuffix();
    HANDLE mutex = CreateMutexW(nullptr, TRUE, mutexName.c_str());
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        Log("already running");
        return 0;
    }

    // IME本体と同じ構成でエンジンを組み立てる: 学習 → ユーザ辞書 → zenz
    // （macOS版 iroha-cli の makeEngine と同じ。環境変数・config.jsonで差し替え可能）
    ServerState state;
    state.Build();
    // セッションの表とライブ変換のスレッド（state より先に片付ける）
    ServerBackend backend(&state);
    iroha::SessionManager sessions(&backend);
    sessions.SetSettings(state.sessionSettings);
    sessions.SetSymbolTable(state.symbols);
    sessions.SetTermLookup(state.termLookup);
    sessions.SetRewriteRules(state.rewriteRuleSet);
    // 選択テキストのAI編集（専用のスレッドと窓。sessions より先に片付ける）
    iroha::SelectionAssistant selection(
        [&state](const iroha::AIRequest& request, const std::function<void(const std::u32string&)>& onPartial,
                 iroha::CancelToken* cancel) {
            iroha::AIConnection connection;
            {
                std::lock_guard<std::mutex> lock(state.aiMutex);
                connection = state.aiConnection;
            }
            const iroha::AIResult result = iroha::AIClient::Run(connection, request, onPartial, cancel);
            if (!result.text) Log(("selection ai failed: " + result.error).c_str());
            return result;
        },
        [&state] {
            std::lock_guard<std::mutex> lock(state.aiMutex);
            return iroha::AIClient::IsConfigured(state.aiConnection);
        },
        [&sessions] { return sessions.AnyComposing(); }, [](const std::string& message) { Log(message.c_str()); });
    state.selection = &selection;
    selection.Configure(state.selectionSettings);
    StartUpdateChecks();
    // 「こえ」モードの録音と認識（裏のスレッドから sessions に知らせるので、sessions より先に片付ける）
    iroha::VoiceRecognizer voice;
    state.voice = &voice;
    // 確かめる用: マイクの代わりに音声ファイルを流す
    if (const wchar_t* file = _wgetenv(L"IROHA_VOICE_FILE"); file && *file) voice.SetFileSource(file);
    {
        std::u32string engineName;
        Log(iroha::VoiceRecognizer::Available(&engineName)
                ? ("voice recognizer: " + iroha::Utf32ToUtf8(engineName)).c_str()
                : "voice recognizer: no Japanese engine");
    }

    const std::wstring pipeName = iroha::ipc::PipeName();
    PSECURITY_DESCRIPTOR pipeSecurity = CreatePipeSecurityDescriptor();
    if (!pipeSecurity) Log("pipe SD unavailable (falling back to default ACL)");
    SECURITY_ATTRIBUTES securityAttributes = {};
    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.lpSecurityDescriptor = pipeSecurity;
    Log("serving");

    const auto createPipe = [&] {
        return CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_DUPLEX,
                                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                PIPE_UNLIMITED_INSTANCES, iroha::ipc::kMaxMessageBytes,
                                iroha::ipc::kMaxMessageBytes, 0,
                                pipeSecurity ? &securityAttributes : nullptr);
    };

    bool running = true;
    HANDLE pipe = createPipe();
    while (running) {
        if (pipe == INVALID_HANDLE_VALUE) {
            Log("CreateNamedPipe failed");
            return 1;
        }
        const BOOL connected = ConnectNamedPipe(pipe, nullptr)
                                   ? TRUE
                                   : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (connected) {
            std::vector<char> request(iroha::ipc::kMaxMessageBytes);
            DWORD bytesRead = 0;
            if (ReadFile(pipe, request.data(), static_cast<DWORD>(request.size()),
                         &bytesRead, nullptr)) {
                request.resize(bytesRead);
                std::vector<char> response;
                ULONG clientProcessId = 0;
                GetNamedPipeClientProcessId(pipe, &clientProcessId);
                running = HandleRequest(state, sessions, request, &response, clientProcessId);
                DWORD written = 0;
                WriteFile(pipe, response.data(), static_cast<DWORD>(response.size()),
                          &written, nullptr);
                FlushFileBuffers(pipe);
            }
        }
        // 次の接続を受けるパイプを先に作ってから今のものを閉じる（パイプの名前が無くなる隙間を作らない。
        // 隙間に当たったクライアントは「サーバがいない」と見て待ち直すことになる）
        const HANDLE next = running ? createPipe() : INVALID_HANDLE_VALUE;
        if (connected) DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        pipe = next;
    }
    if (pipeSecurity) LocalFree(pipeSecurity);
    Log("shutdown");
    return 0;
}
