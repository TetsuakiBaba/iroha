#pragma once
// TIP ⇔ 変換サーバ間の名前付きパイプIPCプロトコル（ヘッダオンリー）。
//
// - パイプ名はセッション別（マルチユーザ環境での衝突を避ける）
// - メッセージモードのパイプで1リクエスト=1接続（CallNamedPipe）。
//   サーバはパイプを1つのスレッドで順に処理する（ライブ変換だけは裏のスレッドで行う）
// - ワイヤ形式はリトルエンディアンのu32・u64と長さ付きUTF-8文字列のみ
//   （JSONパーサ等の依存を持ち込まない）
//
//   リクエスト:  u32 version, u32 type,
//                type==Convert のとき u32 n, string reading, string context
//                type==Record  のとき string reading, string committed, string baseline
//                type==SessionKey  のとき u64 session, u32 key, u32 character, u32 flags,
//                                         string leftContext, u32 tipRevision（後から足した。無ければ前の版の TIP）
//                type==SessionPoll のとき u64 session, u64 revision
//                type==SessionEnd  のとき u64 session
//   レスポンス:  u32 version, u32 status(0=ok),
//                ok: u32 count, string×count / エラー: string message
//   SessionKey / SessionPoll のレスポンス:
//                u32 version, u32 status(0=ok),
//                ok: u32 changed（0 なら以降なし）, u64 revision, string commit, u32 flags,
//                    u32 spanCount, (u32 style, string text)×spanCount,
//                    u32 candidateIndex, u32 candidateCount, string×candidateCount,
//                    u32 annotationCount, string×annotationCount（候補の右に出す印）,
//                    string undoableCommit（取り消せる確定）,
//                    u32 predictionCount, string×predictionCount, u32 predictionIndex, string notice,
//                    u32 typoFeedbackCount（0 か 4）, string×typoFeedbackCount（打ち間違いの訂正の小窓。
//                    前の部分・打った読みの変わった部分・直した読みの変わった部分・後の部分）,
//                    u32 aiShortcutCount, u32×aiShortcutCount（AI変換のプリセットのキー）,
//                    u32 aiFixShortcut, string aiFixNotice, u32 aiFixFeedbackCount（0 か 4）, string×aiFixFeedbackCount,
//                    u32 voiceKey, string voiceNotice（「こえ」モード）, u32 privateModeShortcut,
//                    string debugOverlay（デバッグ表示の行。改行区切り）, u32 inputModeShortcut（かな・英数を切り替えるキー）
//                    （印と取り消せる確定から後は後から足したので、無い返事も読めるようにしてある）
//                flags: 1 文節変換中 / 2 変換の結果待ち / 4 候補ウィンドウ / 8 入力していないときの Space は全角 /
//                       16 確定した文字列を未確定文字列に置き換える（UndoCommit への返事） / 32 プライベートモード /
//                       64 打ち間違いの訂正の結果待ち / 128 AI変換の結果待ち / 256 候補ウィンドウを表に広げる /
//                       512 AIで直すの判定・依頼の結果待ち / 1024 「こえ」モードの録音・認識の最中 /
//                       2048 ライブ変換が ON / 4096 予測変換が ON / 8192 句読点が「，．」（言語バーのメニューの印）
//                エラー: string message
//
// セッションのメッセージは、入力の状態（読み・ライブ変換・文節・候補）をサーバ側に持つためのもの
// （iroha/input_session.h）。TIP はキーを送り、返ってきた表示を画面に反映する。
//
// メッセージの種類は足すだけにして version は変えない（アプリに読み込まれたままの古い TIP が、
// 新しいサーバとも Convert / Record で話せるように）

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "iroha/input_session.h"
#include "iroha/unicode.h"

namespace iroha::ipc {

constexpr uint32_t kProtocolVersion = 1;
// TIP の版。変換サーバは、これより古い（送ってこない）TIP を読み込んだままのアプリを知らせる。
// TIP と変換サーバの間で、新しい機能のために TIP を入れ替える必要が出たら上げる
constexpr uint32_t kTipRevision = 3;
constexpr DWORD kMaxMessageBytes = 64 * 1024;

enum class MessageType : uint32_t {
    Ping = 1,
    Convert = 2,
    Shutdown = 3,
    Record = 4, // 確定の通知（学習用）
    Reload = 5, // 設定・辞書・モデルの再読み込み（設定GUIから）
    SessionKey = 6,  // キー 1 つをセッションに渡し、表示を受け取る
    SessionPoll = 7, // ライブ変換が届いたか（表示が変わったか）を確かめる
    SessionEnd = 8,  // セッションを捨てる（TIP が無効になった）
};

// 環境変数 IROHA_PIPE_SUFFIX があれば名前の末尾に足す（開発用。インストール済みの変換サーバを
// 止めずに、ビルドしたばかりの変換サーバを別の名前で動かして確かめるのに使う）
inline std::wstring InstanceSuffix() {
    DWORD sessionId = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sessionId);
    std::wstring suffix = std::to_wstring(sessionId);
    wchar_t extra[64];
    const DWORD length = GetEnvironmentVariableW(L"IROHA_PIPE_SUFFIX", extra, ARRAYSIZE(extra));
    if (length > 0 && length < ARRAYSIZE(extra)) suffix += L"-" + std::wstring(extra, length);
    return suffix;
}

inline std::wstring PipeName() {
    return L"\\\\.\\pipe\\iroha-server-" + InstanceSuffix();
}

// ---- シリアライズ ----

inline void PutU32(std::vector<char>* buf, uint32_t value) {
    buf->push_back(static_cast<char>(value & 0xFF));
    buf->push_back(static_cast<char>((value >> 8) & 0xFF));
    buf->push_back(static_cast<char>((value >> 16) & 0xFF));
    buf->push_back(static_cast<char>((value >> 24) & 0xFF));
}

inline void PutU64(std::vector<char>* buf, uint64_t value) {
    PutU32(buf, static_cast<uint32_t>(value & 0xFFFFFFFFu));
    PutU32(buf, static_cast<uint32_t>(value >> 32));
}

inline void PutString(std::vector<char>* buf, const std::string& s) {
    PutU32(buf, static_cast<uint32_t>(s.size()));
    buf->insert(buf->end(), s.begin(), s.end());
}

struct Reader {
    const char* data;
    size_t size;
    size_t pos = 0;

    bool U32(uint32_t* value) {
        if (pos + 4 > size) return false;
        *value = static_cast<uint32_t>(static_cast<unsigned char>(data[pos])) |
                 (static_cast<uint32_t>(static_cast<unsigned char>(data[pos + 1])) << 8) |
                 (static_cast<uint32_t>(static_cast<unsigned char>(data[pos + 2])) << 16) |
                 (static_cast<uint32_t>(static_cast<unsigned char>(data[pos + 3])) << 24);
        pos += 4;
        return true;
    }

    bool U64(uint64_t* value) {
        uint32_t low = 0;
        uint32_t high = 0;
        if (!U32(&low) || !U32(&high)) return false;
        *value = (static_cast<uint64_t>(high) << 32) | low;
        return true;
    }

    bool String(std::string* s) {
        uint32_t length = 0;
        if (!U32(&length)) return false;
        if (length > size - pos) return false;
        s->assign(data + pos, length);
        pos += length;
        return true;
    }
};

inline std::vector<char> BuildPingRequest() {
    std::vector<char> buf;
    PutU32(&buf, kProtocolVersion);
    PutU32(&buf, static_cast<uint32_t>(MessageType::Ping));
    return buf;
}

inline std::vector<char> BuildShutdownRequest() {
    std::vector<char> buf;
    PutU32(&buf, kProtocolVersion);
    PutU32(&buf, static_cast<uint32_t>(MessageType::Shutdown));
    return buf;
}

inline std::vector<char> BuildReloadRequest() {
    std::vector<char> buf;
    PutU32(&buf, kProtocolVersion);
    PutU32(&buf, static_cast<uint32_t>(MessageType::Reload));
    return buf;
}

inline std::vector<char> BuildConvertRequest(const std::string& readingUtf8,
                                             const std::string& contextUtf8,
                                             uint32_t candidateCount) {
    std::vector<char> buf;
    PutU32(&buf, kProtocolVersion);
    PutU32(&buf, static_cast<uint32_t>(MessageType::Convert));
    PutU32(&buf, candidateCount);
    PutString(&buf, readingUtf8);
    PutString(&buf, contextUtf8);
    return buf;
}

// 確定の通知。baselineは変換直後の第一候補（これと違う確定だけが学習される）
inline std::vector<char> BuildRecordRequest(const std::string& readingUtf8,
                                            const std::string& committedUtf8,
                                            const std::string& baselineUtf8) {
    std::vector<char> buf;
    PutU32(&buf, kProtocolVersion);
    PutU32(&buf, static_cast<uint32_t>(MessageType::Record));
    PutString(&buf, readingUtf8);
    PutString(&buf, committedUtf8);
    PutString(&buf, baselineUtf8);
    return buf;
}

inline std::vector<char> BuildOkResponse(const std::vector<std::string>& candidates) {
    std::vector<char> buf;
    PutU32(&buf, kProtocolVersion);
    PutU32(&buf, 0);
    PutU32(&buf, static_cast<uint32_t>(candidates.size()));
    for (const auto& candidate : candidates) PutString(&buf, candidate);
    return buf;
}

inline std::vector<char> BuildErrorResponse(const std::string& message) {
    std::vector<char> buf;
    PutU32(&buf, kProtocolVersion);
    PutU32(&buf, 1);
    PutString(&buf, message);
    return buf;
}

// ---- セッション ----

// SessionKey の flags
constexpr uint32_t kKeyFlagShift = 1;
// leftContext を読めた（空文字列でも、読めて空だったことを表す）
constexpr uint32_t kKeyFlagHasLeftContext = 2;

// セッションのレスポンスの flags
constexpr uint32_t kOutputFlagSegmenting = 1;
constexpr uint32_t kOutputFlagLivePending = 2;
constexpr uint32_t kOutputFlagCandidatesVisible = 4;
constexpr uint32_t kOutputFlagFullWidthSpace = 8;
constexpr uint32_t kOutputFlagReplacesCommitted = 16;
constexpr uint32_t kOutputFlagPrivateMode = 32;
constexpr uint32_t kOutputFlagTypoPending = 64;
constexpr uint32_t kOutputFlagAIPending = 128;
constexpr uint32_t kOutputFlagCandidatesExpanded = 256;
constexpr uint32_t kOutputFlagAIFixPending = 512;
constexpr uint32_t kOutputFlagVoicePending = 1024;
constexpr uint32_t kOutputFlagLiveConversion = 2048;
constexpr uint32_t kOutputFlagPredictive = 4096;
constexpr uint32_t kOutputFlagCommaPeriod = 8192;

// leftContext は、合成していないときにカーソルより前から読んだ文書のテキスト（読めなければ nullptr）
inline std::vector<char> BuildSessionKeyRequest(uint64_t sessionId, SessionKey key,
                                                char32_t character, bool shift,
                                                const std::u32string* leftContext) {
    std::vector<char> buf;
    PutU32(&buf, kProtocolVersion);
    PutU32(&buf, static_cast<uint32_t>(MessageType::SessionKey));
    PutU64(&buf, sessionId);
    PutU32(&buf, static_cast<uint32_t>(key));
    PutU32(&buf, static_cast<uint32_t>(character));
    PutU32(&buf, (shift ? kKeyFlagShift : 0) | (leftContext ? kKeyFlagHasLeftContext : 0));
    PutString(&buf, leftContext ? Utf32ToUtf8(*leftContext) : std::string());
    PutU32(&buf, kTipRevision);
    return buf;
}

inline std::vector<char> BuildSessionPollRequest(uint64_t sessionId, uint64_t revision) {
    std::vector<char> buf;
    PutU32(&buf, kProtocolVersion);
    PutU32(&buf, static_cast<uint32_t>(MessageType::SessionPoll));
    PutU64(&buf, sessionId);
    PutU64(&buf, revision);
    return buf;
}

inline std::vector<char> BuildSessionEndRequest(uint64_t sessionId) {
    std::vector<char> buf;
    PutU32(&buf, kProtocolVersion);
    PutU32(&buf, static_cast<uint32_t>(MessageType::SessionEnd));
    PutU64(&buf, sessionId);
    return buf;
}

// output が nullptr なら「変わっていない」（SessionPoll への返事）
inline std::vector<char> BuildSessionResponse(const SessionOutput* output) {
    std::vector<char> buf;
    PutU32(&buf, kProtocolVersion);
    PutU32(&buf, 0);
    PutU32(&buf, output ? 1 : 0);
    if (!output) return buf;
    PutU64(&buf, output->revision);
    PutString(&buf, Utf32ToUtf8(output->commit));
    PutU32(&buf, (output->segmenting ? kOutputFlagSegmenting : 0) |
                     (output->livePending ? kOutputFlagLivePending : 0) |
                     (output->candidatesVisible ? kOutputFlagCandidatesVisible : 0) |
                     (output->fullWidthSpace ? kOutputFlagFullWidthSpace : 0) |
                     (output->replacesCommitted ? kOutputFlagReplacesCommitted : 0) |
                     (output->privateMode ? kOutputFlagPrivateMode : 0) |
                     (output->typoPending ? kOutputFlagTypoPending : 0) |
                     (output->aiPending ? kOutputFlagAIPending : 0) |
                     (output->candidatesExpanded ? kOutputFlagCandidatesExpanded : 0) |
                     (output->aiFixPending ? kOutputFlagAIFixPending : 0) |
                     (output->voicePending ? kOutputFlagVoicePending : 0) |
                     (output->liveConversion ? kOutputFlagLiveConversion : 0) |
                     (output->predictive ? kOutputFlagPredictive : 0) |
                     (output->punctuation == U"，．" ? kOutputFlagCommaPeriod : 0));
    PutU32(&buf, static_cast<uint32_t>(output->preedit.size()));
    for (const SessionOutput::Span& span : output->preedit) {
        PutU32(&buf, span.style);
        PutString(&buf, Utf32ToUtf8(span.text));
    }
    PutU32(&buf, output->candidateIndex);
    // 候補は1メッセージに収まる数だけ入れる（選んでいる候補より前で切れることは実際には無い）
    std::vector<std::string> candidates;
    size_t bytes = buf.size() + 4;
    for (const std::u32string& candidate : output->candidates) {
        std::string utf8 = Utf32ToUtf8(candidate);
        if (bytes + 4 + utf8.size() > kMaxMessageBytes) break;
        bytes += 4 + utf8.size();
        candidates.push_back(std::move(utf8));
    }
    PutU32(&buf, static_cast<uint32_t>(candidates.size()));
    for (const std::string& candidate : candidates) PutString(&buf, candidate);
    // 印は候補と同じ数だけ（収まらなかった候補のぶんは送らない）
    const size_t annotations = output->candidateAnnotations.size() == output->candidates.size()
                                   ? candidates.size()
                                   : 0;
    std::vector<std::string> annotationTexts;
    size_t annotationBytes = buf.size() + 4;
    for (size_t i = 0; i < annotations; ++i) {
        std::string utf8 = Utf32ToUtf8(output->candidateAnnotations[i]);
        if (annotationBytes + 4 + utf8.size() > kMaxMessageBytes) {
            annotationTexts.clear();
            break;
        }
        annotationBytes += 4 + utf8.size();
        annotationTexts.push_back(std::move(utf8));
    }
    PutU32(&buf, static_cast<uint32_t>(annotationTexts.size()));
    for (const std::string& annotation : annotationTexts) PutString(&buf, annotation);
    // 取り消せる確定（後から足した。無い返事も読める）
    PutString(&buf, Utf32ToUtf8(output->undoableCommit));
    // 予測変換の候補・選んでいる位置（0xFFFFFFFF なら選んでいない）・知らせ（後から足した。無い返事も読める）
    PutU32(&buf, static_cast<uint32_t>(output->predictions.size()));
    for (const std::u32string& prediction : output->predictions) PutString(&buf, Utf32ToUtf8(prediction));
    PutU32(&buf, output->predictionIndex < 0 ? 0xFFFFFFFFu : static_cast<uint32_t>(output->predictionIndex));
    PutString(&buf, Utf32ToUtf8(output->notice));
    // 打ち間違いの訂正の小窓（後から足した。無い返事も読める）
    if (output->typoFeedback) {
        PutU32(&buf, 4);
        for (const std::u32string* part : {&output->typoFeedback->prefix, &output->typoFeedback->originalChanged,
                                           &output->typoFeedback->correctedChanged, &output->typoFeedback->suffix}) {
            PutString(&buf, Utf32ToUtf8(*part));
        }
    } else {
        PutU32(&buf, 0);
    }
    // AI変換のキー（後から足した。無い返事も読める）
    PutU32(&buf, static_cast<uint32_t>(output->aiShortcuts.size()));
    for (uint32_t shortcut : output->aiShortcuts) PutU32(&buf, shortcut);
    // AIで直す（後から足した。無い返事も読める）
    PutU32(&buf, output->aiFixShortcut);
    PutString(&buf, Utf32ToUtf8(output->aiFixNotice));
    if (output->aiFixFeedback) {
        PutU32(&buf, 4);
        for (const std::u32string* part : {&output->aiFixFeedback->prefix, &output->aiFixFeedback->originalChanged,
                                           &output->aiFixFeedback->correctedChanged, &output->aiFixFeedback->suffix}) {
            PutString(&buf, Utf32ToUtf8(*part));
        }
    } else {
        PutU32(&buf, 0);
    }
    // 「こえ」モード（後から足した。無い返事も読める）
    PutU32(&buf, output->voiceKey);
    PutString(&buf, Utf32ToUtf8(output->voiceNotice));
    PutU32(&buf, output->privateModeShortcut);
    PutString(&buf, Utf32ToUtf8(output->debugOverlay));
    PutU32(&buf, output->inputModeShortcut);
    return buf;
}

struct SessionResponse {
    bool ok = false;
    std::string error;
    // 偽なら表示は変わっていない（output は空）
    bool changed = false;
    SessionOutput output;
};

inline SessionResponse ParseSessionResponse(const std::vector<char>& bytes) {
    SessionResponse result;
    Reader reader{bytes.data(), bytes.size()};
    uint32_t version = 0;
    uint32_t status = 0;
    if (!reader.U32(&version) || version != kProtocolVersion || !reader.U32(&status)) {
        result.error = "protocol error";
        return result;
    }
    if (status != 0) {
        reader.String(&result.error);
        return result;
    }
    uint32_t changed = 0;
    if (!reader.U32(&changed)) {
        result.error = "protocol error";
        return result;
    }
    if (changed == 0) {
        result.ok = true;
        return result;
    }
    SessionOutput& output = result.output;
    std::string commit;
    uint32_t flags = 0;
    uint32_t spanCount = 0;
    if (!reader.U64(&output.revision) || !reader.String(&commit) || !reader.U32(&flags) ||
        !reader.U32(&spanCount)) {
        result.error = "protocol error";
        return result;
    }
    output.commit = Utf8ToUtf32(commit);
    output.segmenting = (flags & kOutputFlagSegmenting) != 0;
    output.livePending = (flags & kOutputFlagLivePending) != 0;
    output.candidatesVisible = (flags & kOutputFlagCandidatesVisible) != 0;
    output.fullWidthSpace = (flags & kOutputFlagFullWidthSpace) != 0;
    output.replacesCommitted = (flags & kOutputFlagReplacesCommitted) != 0;
    output.privateMode = (flags & kOutputFlagPrivateMode) != 0;
    output.typoPending = (flags & kOutputFlagTypoPending) != 0;
    output.aiPending = (flags & kOutputFlagAIPending) != 0;
    output.candidatesExpanded = (flags & kOutputFlagCandidatesExpanded) != 0;
    output.aiFixPending = (flags & kOutputFlagAIFixPending) != 0;
    output.voicePending = (flags & kOutputFlagVoicePending) != 0;
    output.liveConversion = (flags & kOutputFlagLiveConversion) != 0;
    output.predictive = (flags & kOutputFlagPredictive) != 0;
    output.punctuation = (flags & kOutputFlagCommaPeriod) != 0 ? U"，．" : U"、。";
    for (uint32_t i = 0; i < spanCount; ++i) {
        uint32_t style = 0;
        std::string text;
        if (!reader.U32(&style) || !reader.String(&text)) {
            result.error = "protocol error";
            return result;
        }
        output.preedit.push_back({Utf8ToUtf32(text), style});
    }
    uint32_t candidateCount = 0;
    if (!reader.U32(&output.candidateIndex) || !reader.U32(&candidateCount)) {
        result.error = "protocol error";
        return result;
    }
    for (uint32_t i = 0; i < candidateCount; ++i) {
        std::string candidate;
        if (!reader.String(&candidate)) {
            result.error = "protocol error";
            return result;
        }
        output.candidates.push_back(Utf8ToUtf32(candidate));
    }
    // 候補の印（印を送らない古い変換サーバの返事には無い）
    uint32_t annotationCount = 0;
    if (reader.U32(&annotationCount)) {
        for (uint32_t i = 0; i < annotationCount; ++i) {
            std::string annotation;
            if (!reader.String(&annotation)) {
                output.candidateAnnotations.clear();
                break;
            }
            output.candidateAnnotations.push_back(Utf8ToUtf32(annotation));
        }
        if (output.candidateAnnotations.size() != output.candidates.size()) {
            output.candidateAnnotations.clear();
        }
        std::string undoable;
        if (reader.String(&undoable)) output.undoableCommit = Utf8ToUtf32(undoable);
        uint32_t predictionCount = 0;
        if (reader.U32(&predictionCount)) {
            for (uint32_t i = 0; i < predictionCount && i < 16; ++i) {
                std::string prediction;
                if (!reader.String(&prediction)) break;
                output.predictions.push_back(Utf8ToUtf32(prediction));
            }
            uint32_t predictionIndex = 0xFFFFFFFFu;
            if (reader.U32(&predictionIndex) && predictionIndex < output.predictions.size()) {
                output.predictionIndex = static_cast<int32_t>(predictionIndex);
            }
            std::string notice;
            if (reader.String(&notice)) output.notice = Utf8ToUtf32(notice);
            uint32_t typoCount = 0;
            if (reader.U32(&typoCount) && typoCount == 4) {
                std::string parts[4];
                if (reader.String(&parts[0]) && reader.String(&parts[1]) && reader.String(&parts[2]) &&
                    reader.String(&parts[3])) {
                    output.typoFeedback = TypoCorrectionFeedback{Utf8ToUtf32(parts[0]), Utf8ToUtf32(parts[1]),
                                                                 Utf8ToUtf32(parts[2]), Utf8ToUtf32(parts[3])};
                }
            }
            uint32_t shortcutCount = 0;
            if (reader.U32(&shortcutCount)) {
                for (uint32_t i = 0; i < shortcutCount && i < 16; ++i) {
                    uint32_t shortcut = 0;
                    if (!reader.U32(&shortcut)) break;
                    output.aiShortcuts.push_back(shortcut);
                }
                std::string fixNotice;
                if (reader.U32(&output.aiFixShortcut) && reader.String(&fixNotice)) {
                    output.aiFixNotice = Utf8ToUtf32(fixNotice);
                    uint32_t fixCount = 0;
                    if (reader.U32(&fixCount) && fixCount == 4) {
                        std::string parts[4];
                        if (reader.String(&parts[0]) && reader.String(&parts[1]) && reader.String(&parts[2]) &&
                            reader.String(&parts[3])) {
                            output.aiFixFeedback = TypoCorrectionFeedback{Utf8ToUtf32(parts[0]), Utf8ToUtf32(parts[1]),
                                                                          Utf8ToUtf32(parts[2]), Utf8ToUtf32(parts[3])};
                        }
                    }
                    std::string voiceNotice;
                    if (reader.U32(&output.voiceKey) && reader.String(&voiceNotice)) {
                        output.voiceNotice = Utf8ToUtf32(voiceNotice);
                        std::string debugOverlay;
                        if (reader.U32(&output.privateModeShortcut) && reader.String(&debugOverlay)) {
                            output.debugOverlay = Utf8ToUtf32(debugOverlay);
                            reader.U32(&output.inputModeShortcut);
                        }
                    }
                }
            }
        }
    }
    if (output.candidateIndex >= output.candidates.size()) output.candidateIndex = 0;
    result.changed = true;
    result.ok = true;
    return result;
}

// ---- クライアント ----

// 1リクエストを送って応答を受け取る。サーバが別クライアントを処理中の場合は
// CallNamedPipeが内部で待つ（timeoutMs まで）。パイプが無いとき（サーバが起動の途中）は
// 少し待って試し直す（attempts 回まで。待たせたくない呼び出しは 1 にする）
inline bool Call(const std::vector<char>& request, std::vector<char>* response,
                 DWORD timeoutMs, int attempts = 3) {
    const std::wstring name = PipeName();
    response->assign(kMaxMessageBytes, 0);
    for (int attempt = 0; attempt < attempts; ++attempt) {
        DWORD bytesRead = 0;
        if (CallNamedPipeW(name.c_str(), const_cast<char*>(request.data()),
                           static_cast<DWORD>(request.size()), response->data(),
                           static_cast<DWORD>(response->size()), &bytesRead,
                           timeoutMs)) {
            response->resize(bytesRead);
            return true;
        }
        if (GetLastError() == ERROR_FILE_NOT_FOUND && attempt + 1 < attempts) {
            Sleep(30);
            continue;
        }
        break;
    }
    response->clear();
    return false;
}

struct ConvertResponse {
    bool ok = false;
    std::vector<std::string> candidates; // UTF-8
    std::string error;
};

inline ConvertResponse ParseConvertResponse(const std::vector<char>& bytes) {
    ConvertResponse result;
    Reader reader{bytes.data(), bytes.size()};
    uint32_t version = 0;
    uint32_t status = 0;
    if (!reader.U32(&version) || version != kProtocolVersion ||
        !reader.U32(&status)) {
        result.error = "protocol error";
        return result;
    }
    if (status != 0) {
        reader.String(&result.error);
        return result;
    }
    uint32_t count = 0;
    if (!reader.U32(&count)) {
        result.error = "protocol error";
        return result;
    }
    for (uint32_t i = 0; i < count; ++i) {
        std::string candidate;
        if (!reader.String(&candidate)) {
            result.error = "protocol error";
            return result;
        }
        result.candidates.push_back(std::move(candidate));
    }
    result.ok = true;
    return result;
}

} // namespace iroha::ipc
