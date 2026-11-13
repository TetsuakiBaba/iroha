// ライブ変換・打ち間違いの訂正の結果待ちの間、変換サーバに「表示が変わったか」を問い合わせる。
//
// キーを送ると、変換サーバは今出せる表示（前の変換結果 + 増えたかな）をすぐ返し、
// 変換そのものは裏で進める。届いたかどうかは、アプリのメッセージループで動くタイマーで確かめる
// （TIP はアプリのスレッドの中で動くので、待つためのスレッドを自分では持たない）。
// タイマーは結果待ちの間しか動かさない
#include "ConvertClient.h"
#include "TextService.h"

namespace {

// 問い合わせの間隔。Windows のタイマーの刻み（約 15.6ms）と同じにしてある
constexpr UINT kPollIntervalMs = 15;
// 結果が届かないまま問い合わせ続ける上限（変換サーバが止まったときに回り続けないように）。
// AI変換は、変換サーバが 30 秒応答の無い依頼を打ち切るので、それより長く待つ
constexpr DWORD kPollLimitMs = 10000;
constexpr DWORD kAIPollLimitMs = 120000;
// 続けてこの回数つながらなければやめる
constexpr int kPollFailureLimit = 5;

} // namespace

void CALLBACK TextService::PollTimerProc(HWND, UINT, UINT_PTR id, DWORD) {
    // タイマーの番号に TextService のポインタを入れてある（止めてからでないと破棄しない）
    reinterpret_cast<TextService*>(id)->OnPollTimer();
}

bool TextService::EnsurePollWindow() {
    if (pollWindow_) return true;
    // タイマーを受けるだけのメッセージ専用ウィンドウ（システムのクラスを使い、クラス登録を持たない）
    pollWindow_ = CreateWindowExW(0, L"Static", nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_hInst,
                                  nullptr);
    if (!pollWindow_) IrohaLog(L"poll window creation failed: %u", GetLastError());
    return pollWindow_ != nullptr;
}

void TextService::StartPolling() {
    pollStartedTick_ = GetTickCount();
    pollFailures_ = 0;
    if (polling_ || !EnsurePollWindow()) return;
    polling_ = SetTimer(pollWindow_, reinterpret_cast<UINT_PTR>(this), kPollIntervalMs,
                        &TextService::PollTimerProc) != 0;
}

// 知らせの小窓を閉じるタイマー（番号は TextService のポインタ + 1。ポインタは偶数なので問い合わせのタイマーと重ならない）
void CALLBACK TextService::NoticeTimerProc(HWND hwnd, UINT, UINT_PTR id, DWORD) {
    KillTimer(hwnd, id);
    reinterpret_cast<TextService*>(id - 1)->noticeWindow_.Hide();
}

void TextService::StopPolling() {
    if (!polling_) return;
    KillTimer(pollWindow_, reinterpret_cast<UINT_PTR>(this));
    polling_ = false;
}

void TextService::DestroyPollWindow() {
    StopPolling();
    if (pollWindow_) {
        KillTimer(pollWindow_, reinterpret_cast<UINT_PTR>(this) + 1);
        noticeWindow_.Hide();
        DestroyWindow(pollWindow_);
        pollWindow_ = nullptr;
    }
}

void TextService::OnPollTimer() {
    if (!polling_) return;
    if (!composition_ || !compositionContext_ || (!shown_.livePending && !shown_.typoPending && !shown_.aiPending) ||
        GetTickCount() - pollStartedTick_ > (shown_.aiPending ? kAIPollLimitMs : kPollLimitMs)) {
        StopPolling();
        return;
    }
    bool changed = false;
    iroha::SessionOutput output;
    if (!ConvertClient::Poll(sessionId_, shown_.revision, &changed, &output)) {
        if (++pollFailures_ >= kPollFailureLimit) StopPolling();
        return;
    }
    pollFailures_ = 0;
    if (!changed) return;
    // ApplyOutput の中で合成が終わることがあるので、文書の参照を持っておく
    ITfContext* context = compositionContext_;
    context->AddRef();
    ApplyOutput(context, output, /*synchronous=*/false);
    context->Release();
}
