#include "voice_recognizer.h"

#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <sapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <thread>

#include "iroha/unicode.h"
#include "iroha/voice_transcript.h"

namespace iroha {

namespace {

using Clock = std::chrono::steady_clock;

int ElapsedMs(Clock::time_point since) {
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since).count());
}

std::u32string HResultText(const char* what, HRESULT hr) {
    char buffer[96];
    snprintf(buffer, sizeof(buffer), "%s（0x%08lX）", what, static_cast<unsigned long>(hr));
    return Utf8ToUtf32(buffer);
}

// 書き足されるまで読み取りを待たせるストリーム（マイクの音声を SAPI に実時間で流す）。
// SAPI は要求した長さに足りない読み取りを終わりとみなすので、要求の長さがそろうか、閉じるまで待たせる
class LiveAudioStream : public IStream {
public:
    void Append(const BYTE* data, size_t size) {
        if (size == 0) return;
        std::lock_guard<std::mutex> lock(mutex_);
        buffer_.insert(buffer_.end(), data, data + size);
        condition_.notify_all();
    }
    // これ以上書き足さない（読み取りは残りを返して終わる）
    void Finish() {
        std::lock_guard<std::mutex> lock(mutex_);
        finished_ = true;
        condition_.notify_all();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IStream || riid == IID_ISequentialStream) {
            *object = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) delete this;
        return remaining;
    }
    STDMETHODIMP Read(void* data, ULONG size, ULONG* read) override {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [&] { return buffer_.size() - position_ >= size || finished_; });
        const size_t count = std::min<size_t>(size, buffer_.size() - position_);
        if (count > 0) memcpy(data, buffer_.data() + position_, count);
        position_ += count;
        if (read) *read = static_cast<ULONG>(count);
        return count == size ? S_OK : S_FALSE;
    }
    STDMETHODIMP Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }
    STDMETHODIMP Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* newPosition) override {
        std::lock_guard<std::mutex> lock(mutex_);
        long long target = 0;
        if (origin == STREAM_SEEK_SET) {
            target = move.QuadPart;
        } else if (origin == STREAM_SEEK_CUR) {
            target = static_cast<long long>(position_) + move.QuadPart;
        } else {
            return E_NOTIMPL;
        }
        if (target < 0 || target > static_cast<long long>(buffer_.size())) return STG_E_INVALIDFUNCTION;
        position_ = static_cast<size_t>(target);
        if (newPosition) newPosition->QuadPart = position_;
        return S_OK;
    }
    STDMETHODIMP SetSize(ULARGE_INTEGER) override { return E_NOTIMPL; }
    STDMETHODIMP CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override { return E_NOTIMPL; }
    STDMETHODIMP Commit(DWORD) override { return S_OK; }
    STDMETHODIMP Revert() override { return E_NOTIMPL; }
    STDMETHODIMP LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP Stat(STATSTG* stat, DWORD) override {
        if (!stat) return E_POINTER;
        ZeroMemory(stat, sizeof(*stat));
        stat->type = STGTY_STREAM;
        return S_OK;
    }
    STDMETHODIMP Clone(IStream**) override { return E_NOTIMPL; }

private:
    std::atomic<ULONG> references_{1};
    std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<BYTE> buffer_;
    size_t position_ = 0;
    bool finished_ = false;
};

template <typename T>
void SafeRelease(T** object) {
    if (*object) {
        (*object)->Release();
        *object = nullptr;
    }
}

// 日本語の認識エンジン（言語 0x411）のトークン。呼び出し側で COM を初期化しておく
ISpObjectToken* FindJapaneseRecognizer() {
    ISpObjectTokenCategory* category = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&category)))) {
        return nullptr;
    }
    ISpObjectToken* token = nullptr;
    IEnumSpObjectTokens* tokens = nullptr;
    if (SUCCEEDED(category->SetId(SPCAT_RECOGNIZERS, FALSE)) &&
        SUCCEEDED(category->EnumTokens(L"Language=411", nullptr, &tokens)) && tokens) {
        ULONG fetched = 0;
        tokens->Next(1, &token, &fetched);
        tokens->Release();
    }
    category->Release();
    return token;
}

std::u32string TokenName(ISpObjectToken* token) {
    wchar_t* name = nullptr;
    std::u32string result;
    if (SUCCEEDED(token->GetStringValue(nullptr, &name)) && name) {
        result = Utf16ToUtf32(name);
        CoTaskMemFree(name);
    }
    return result;
}

std::u32string ResultText(ISpRecoResult* result) {
    wchar_t* text = nullptr;
    std::u32string value;
    if (SUCCEEDED(result->GetText(static_cast<ULONG>(SP_GETWHOLEPHRASE), static_cast<ULONG>(SP_GETWHOLEPHRASE), TRUE,
                                    &text, nullptr)) && text) {
        value = Utf16ToUtf32(text);
        CoTaskMemFree(text);
    }
    return value;
}

// 結果の語ごとの表示形（数字への置き換えの前）と語彙形から、表記と読みの組を作る
void AppendTokens(ISpRecoResult* result, std::vector<ReadingToken>* tokens) {
    SPPHRASE* phrase = nullptr;
    if (FAILED(result->GetPhrase(&phrase)) || !phrase) return;
    for (ULONG i = 0; i < phrase->Rule.ulCountOfElements; ++i) {
        const SPPHRASEELEMENT& element = phrase->pElements[i];
        const std::u32string display = Utf16ToUtf32(element.pszDisplayText ? element.pszDisplayText : L"");
        const std::u32string lexical = Utf16ToUtf32(element.pszLexicalForm ? element.pszLexicalForm : L"");
        if (display.empty()) continue;
        tokens->push_back(VoiceTranscript::TokenFromRecognition(display, lexical));
        if (element.bDisplayAttributes & (SPAF_ONE_TRAILING_SPACE | SPAF_TWO_TRAILING_SPACES)) {
            tokens->push_back({U" ", std::nullopt});
        }
    }
    CoTaskMemFree(phrase);
}

} // namespace

bool LoadWave(const std::wstring& path, WAVEFORMATEX* format, std::vector<unsigned char>* pcm, std::string* error);

struct VoiceRecognizer::Shared {
    std::mutex mutex;
    std::condition_variable condition;
    int active = 0;
};

// 1 回ぶん（押してから結果を渡すまで）。裏のスレッド 1 本で、マイク・ストリーム・SAPI の知らせを順に見る
struct VoiceRecognizer::Utterance {
    EventHandler handler;
    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE cancelEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Clock::time_point pressedAt = Clock::now();
    // ファイルから流すとき（iroha-cli・確かめる用）。live ならキーの扱いをマイクと同じにする
    bool fromFile = false;
    bool realtime = false;
    bool live = true;
    WAVEFORMATEX fileFormat = {};
    std::vector<BYTE> filePcm;

    ~Utterance() {
        CloseHandle(stopEvent);
        CloseHandle(cancelEvent);
    }

    void Send(InputSession::VoiceEvent::Kind kind, const std::u32string& text = U"",
              std::vector<ReadingToken> tokens = {}) {
        InputSession::VoiceEvent event;
        event.kind = kind;
        event.text = text;
        event.tokens = std::move(tokens);
        handler(event);
    }

    bool Signaled(HANDLE event) const { return WaitForSingleObject(event, 0) == WAIT_OBJECT_0; }

    void Run() {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        RunInitialized();
        if (SUCCEEDED(initialized)) CoUninitialize();
    }

    void RunInitialized() {
        using Kind = InputSession::VoiceEvent::Kind;
        if (live) {
            // 少し待ってからマイクを開く（その前に離した・ほかのキーが来たなら何もしない）
            HANDLE events[] = {stopEvent, cancelEvent};
            if (WaitForMultipleObjects(2, events, FALSE, kStartDelayMs) != WAIT_TIMEOUT) {
                Send(Kind::Discarded);
                return;
            }
        }
        ISpObjectToken* engine = FindJapaneseRecognizer();
        if (!engine) {
            Send(Kind::Failed, U"日本語の音声認識エンジンが見つかりません");
            return;
        }

        LiveAudioStream* stream = new LiveAudioStream();
        WAVEFORMATEX format = {WAVE_FORMAT_PCM, 1, 16000, 32000, 2, 16, 0};
        if (fromFile) format = fileFormat;
        HWAVEIN wave = nullptr;
        HANDLE waveEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        constexpr int kBufferCount = 4;
        std::vector<std::vector<char>> buffers(kBufferCount, std::vector<char>(format.nAvgBytesPerSec / 10));
        WAVEHDR headers[kBufferCount] = {};
        bool capturing = false;
        ISpRecognizer* recognizer = nullptr;
        ISpStream* input = nullptr;
        ISpRecoContext* context = nullptr;
        ISpRecoGrammar* grammar = nullptr;

        // 受け取り済みの録音をストリームに足し、バッファを返す（requeue が偽なら返さない）
        const auto drain = [&](bool requeue) {
            for (WAVEHDR& header : headers) {
                if (!(header.dwFlags & WHDR_DONE)) continue;
                stream->Append(reinterpret_cast<const BYTE*>(header.lpData), header.dwBytesRecorded);
                header.dwFlags &= ~WHDR_DONE;
                header.dwBytesRecorded = 0;
                if (requeue) waveInAddBuffer(wave, &header, sizeof(header));
            }
        };
        const auto stopCapture = [&] {
            if (!capturing) return;
            capturing = false;
            waveInStop(wave);
            waveInReset(wave); // 録音中のバッファも返ってくる
            drain(/*requeue=*/false);
        };
        const auto cleanup = [&] {
            stopCapture();
            if (wave) {
                for (WAVEHDR& header : headers) {
                    if (header.dwFlags & WHDR_PREPARED) waveInUnprepareHeader(wave, &header, sizeof(header));
                }
                waveInClose(wave);
                wave = nullptr;
            }
            stream->Finish(); // SAPI の読み取りの待ちを解く
            if (grammar) grammar->SetDictationState(SPRS_INACTIVE);
            SafeRelease(&grammar);
            SafeRelease(&context);
            SafeRelease(&recognizer);
            SafeRelease(&input);
            stream->Release();
            engine->Release();
            CloseHandle(waveEvent);
        };

        // マイクは認識エンジンより先に開く（エンジンの立ち上げの間の音声もストリームに貯まる）
        if (!fromFile && waveInGetNumDevs() == 0) {
            // 録音の機器が 1 つも無い（リモートデスクトップでマイクを転送していないときもこうなる。2026-11-14）
            cleanup();
            Send(Kind::Failed, U"マイクが見つかりません（リモートデスクトップでは、接続の設定でマイクを転送してください）");
            return;
        }
        if (!fromFile) {
            const MMRESULT opened = waveInOpen(&wave, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(waveEvent), 0,
                                               CALLBACK_EVENT);
            if (opened != MMSYSERR_NOERROR) {
                wave = nullptr;
                cleanup();
                Send(Kind::Failed, U"マイクを開けません（" + Utf8ToUtf32(std::to_string(opened)) + U"）");
                return;
            }
            for (int i = 0; i < kBufferCount; ++i) {
                headers[i].lpData = buffers[i].data();
                headers[i].dwBufferLength = static_cast<DWORD>(buffers[i].size());
                waveInPrepareHeader(wave, &headers[i], sizeof(headers[i]));
                waveInAddBuffer(wave, &headers[i], sizeof(headers[i]));
            }
            capturing = waveInStart(wave) == MMSYSERR_NOERROR;
            if (!capturing) {
                cleanup();
                Send(Kind::Failed, U"マイクを開けません");
                return;
            }
        }
        const Clock::time_point recordingStarted = Clock::now();
        Send(Kind::Started);

        HRESULT hr = CoCreateInstance(CLSID_SpInprocRecognizer, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&recognizer));
        if (SUCCEEDED(hr)) hr = recognizer->SetRecognizer(engine);
        if (SUCCEEDED(hr)) hr = CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&input));
        if (SUCCEEDED(hr)) hr = input->SetBaseStream(stream, SPDFID_WaveFormatEx, &format);
        if (SUCCEEDED(hr)) hr = recognizer->SetInput(input, FALSE);
        if (SUCCEEDED(hr)) hr = recognizer->CreateRecoContext(&context);
        if (SUCCEEDED(hr)) hr = context->SetNotifyWin32Event();
        const ULONGLONG interest =
            SPFEI(SPEI_RECOGNITION) | SPFEI(SPEI_HYPOTHESIS) | SPFEI(SPEI_END_SR_STREAM);
        if (SUCCEEDED(hr)) hr = context->SetInterest(interest, interest);
        if (SUCCEEDED(hr)) hr = context->CreateGrammar(0, &grammar);
        if (SUCCEEDED(hr)) hr = grammar->LoadDictation(nullptr, SPLO_STATIC);
        if (SUCCEEDED(hr)) hr = grammar->SetDictationState(SPRS_ACTIVE);
        if (FAILED(hr)) {
            cleanup();
            Send(Kind::Failed, HResultText("音声認識を始められません", hr));
            return;
        }
        const HANDLE recognitionEvent = context->GetNotifyEventHandle();

        std::vector<ReadingToken> tokens;
        std::u32string committed; // 確定した結果の表記（フレーズごとに足す）
        bool closed = false;      // ストリームを閉じた（あとは結果を待つだけ）
        bool ended = false;
        size_t fileOffset = 0;
        Clock::time_point closedAt;
        const auto close = [&] {
            stopCapture();
            stream->Finish();
            closed = true;
            closedAt = Clock::now();
        };
        while (!ended) {
            HANDLE events[] = {recognitionEvent, cancelEvent, stopEvent, waveEvent};
            const DWORD count = closed ? 2 : (capturing ? 4 : 3);
            WaitForMultipleObjects(count, events, FALSE, 50);
            if (Signaled(cancelEvent)) {
                cleanup();
                Send(Kind::Discarded);
                return;
            }
            if (capturing) drain(/*requeue=*/true);
            if (fromFile && !closed) {
                // 実時間の速さなら、経過した時間ぶんだけ足す
                size_t due = filePcm.size();
                if (realtime) {
                    due = std::min(filePcm.size(), static_cast<size_t>(static_cast<double>(ElapsedMs(recordingStarted)) *
                                                                       format.nAvgBytesPerSec / 1000.0));
                    due -= due % std::max<WORD>(1, format.nBlockAlign);
                }
                if (due > fileOffset) {
                    stream->Append(filePcm.data() + fileOffset, due - fileOffset);
                    fileOffset = due;
                }
                if (fileOffset >= filePcm.size()) close();
            }
            if (!closed && live && (Signaled(stopEvent) || ElapsedMs(recordingStarted) >= kMaximumHoldMs)) {
                if (ElapsedMs(pressedAt) < kMinimumHoldMs) {
                    cleanup();
                    Send(Kind::Discarded);
                    return;
                }
                close();
            }
            SPEVENT event = {};
            ULONG fetched = 0;
            while (context->GetEvents(1, &event, &fetched) == S_OK && fetched == 1) {
                if (event.eEventId == SPEI_HYPOTHESIS && event.elParamType == SPET_LPARAM_IS_OBJECT) {
                    const std::u32string text = ResultText(reinterpret_cast<ISpRecoResult*>(event.lParam));
                    if (!text.empty()) Send(Kind::Partial, committed + text);
                } else if (event.eEventId == SPEI_RECOGNITION && event.elParamType == SPET_LPARAM_IS_OBJECT) {
                    auto* result = reinterpret_cast<ISpRecoResult*>(event.lParam);
                    AppendTokens(result, &tokens);
                    committed += ResultText(result);
                    Send(Kind::Partial, committed);
                } else if (event.eEventId == SPEI_END_SR_STREAM) {
                    ended = true;
                }
                if (event.elParamType == SPET_LPARAM_IS_OBJECT && event.lParam) {
                    reinterpret_cast<IUnknown*>(event.lParam)->Release();
                } else if (event.elParamType == SPET_LPARAM_IS_POINTER || event.elParamType == SPET_LPARAM_IS_STRING) {
                    CoTaskMemFree(reinterpret_cast<void*>(event.lParam));
                }
                event = {};
            }
            if (!ended && closed && ElapsedMs(closedAt) > kFinishTimeoutMs) {
                cleanup();
                Send(Kind::Failed, U"音声認識が終わりませんでした");
                return;
            }
        }
        cleanup();
        Send(Kind::Finished, committed, std::move(tokens));
    }
};

VoiceRecognizer::VoiceRecognizer() : shared_(std::make_shared<Shared>()) {}

VoiceRecognizer::~VoiceRecognizer() {
    Cancel();
    std::unique_lock<std::mutex> lock(shared_->mutex);
    shared_->condition.wait_for(lock, std::chrono::seconds(5), [this] { return shared_->active == 0; });
}

bool VoiceRecognizer::Available(std::u32string* name) {
    // 呼び出し側のスレッドの COM の状態に触らないよう、別のスレッドで調べる
    bool found = false;
    std::u32string engineName;
    std::thread([&found, &engineName] {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (ISpObjectToken* token = FindJapaneseRecognizer()) {
            found = true;
            engineName = TokenName(token);
            token->Release();
        }
        if (SUCCEEDED(initialized)) CoUninitialize();
    }).join();
    if (name) *name = engineName;
    return found;
}

std::u32string VoiceRecognizer::Start(EventHandler handler) {
    auto utterance = std::make_shared<Utterance>();
    utterance->handler = std::move(handler);
    if (!fileSource_.empty()) {
        std::string error;
        if (!LoadWave(fileSource_, &utterance->fileFormat, &utterance->filePcm, &error)) return Utf8ToUtf32(error);
        utterance->fromFile = true;
        utterance->realtime = true;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (current_) SetEvent(current_->cancelEvent);
        current_ = utterance;
    }
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        ++shared_->active;
    }
    std::thread([utterance, shared = shared_] {
        utterance->Run();
        std::lock_guard<std::mutex> lock(shared->mutex);
        --shared->active;
        shared->condition.notify_all();
    }).detach();
    return U"";
}

void VoiceRecognizer::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_) SetEvent(current_->stopEvent);
}

void VoiceRecognizer::Cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_) SetEvent(current_->cancelEvent);
    current_.reset();
}

void VoiceRecognizer::Wait() {
    std::unique_lock<std::mutex> lock(shared_->mutex);
    shared_->condition.wait(lock, [this] { return shared_->active == 0; });
}

// WAV（PCM）を読む
bool LoadWave(const std::wstring& path, WAVEFORMATEX* format, std::vector<unsigned char>* pcm,
                               std::string* error) {
    std::ifstream file(path, std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.size() < 12 || memcmp(bytes.data(), "RIFF", 4) != 0 || memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        if (error) *error = "WAV ファイルではありません";
        return false;
    }
    *format = {};
    bool hasFormat = false;
    bool hasData = false;
    for (size_t offset = 12; offset + 8 <= bytes.size();) {
        uint32_t size = 0;
        memcpy(&size, bytes.data() + offset + 4, 4);
        const size_t body = offset + 8;
        const size_t available = std::min<size_t>(size, bytes.size() - body);
        if (memcmp(bytes.data() + offset, "fmt ", 4) == 0 && available >= 16) {
            memcpy(format, bytes.data() + body, std::min<size_t>(available, sizeof(WAVEFORMATEX)));
            hasFormat = true;
        } else if (memcmp(bytes.data() + offset, "data", 4) == 0) {
            pcm->assign(bytes.begin() + static_cast<std::ptrdiff_t>(body),
                        bytes.begin() + static_cast<std::ptrdiff_t>(body + available));
            hasData = true;
        }
        offset = body + size + (size & 1);
    }
    if (!hasFormat || !hasData || format->wFormatTag != WAVE_FORMAT_PCM) {
        if (error) *error = "PCM の WAV ファイルではありません";
        return false;
    }
    format->cbSize = 0;
    return true;
}

bool VoiceRecognizer::RecognizeFile(const std::wstring& path, bool realtime, const EventHandler& handler,
                                    std::string* error) {
    Utterance utterance;
    utterance.handler = handler;
    utterance.fromFile = true;
    utterance.realtime = realtime;
    utterance.live = false;
    if (!LoadWave(path, &utterance.fileFormat, &utterance.filePcm, error)) return false;
    std::thread([&utterance] { utterance.Run(); }).join();
    return true;
}

} // namespace iroha
