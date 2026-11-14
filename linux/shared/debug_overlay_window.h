#pragma once
// デバッグ表示の窓（開発者モードの「推論にかかった時間と左文脈をカーソルの右下に表示する」）。
// 移植元: macos/Sources/iroha/DeveloperOverlay.swift の表示の部分。行の文面は iroha-core の debug_overlay が作る。
//
// X11 の override-redirect の窓に Pango で描く（GTK を使わない。IBus のエンジンも fcitx5 も GTK の main loop を
// 持たないため）。キーボードのフォーカスは取らず、マウスも受けない（入力の形を空にする）。
// 置き場所はカーソルの右下（行の下。上の行を隠さないように）で、画面の下に収まらないときと、候補ウィンドウが
// 出ているとき（IBus・fcitx5 の候補ウィンドウはカーソルの下に出るので重なる）は行の上にする。
// Wayland のセッションでは開かない（ほかのアプリのカーソル位置に窓を置けない）。
// 呼び出し側の main loop で Fd() を見張り、読めるようになったら Dispatch() を呼ぶ（再描画のため）。
// 同じ窓を、かな・英数を切り替えたときの「あ」「A」の表示にも使う（OverlayStyle で文字の大きさと濃さを変える）
#include <string>

namespace iroha {

// 窓の見た目（既定はデバッグ表示）
struct OverlayStyle {
    const char* font = "Sans 9"; // Pango の書き方（ポイント）
    double paddingX = 6;         // 余白（論理ピクセル）
    double paddingY = 3;
    bool strongText = false;     // 文字を濃くする（偽なら少し薄い灰色。macOS版の secondaryLabelColor）
    const char* name = "iroha debug"; // 窓の名前（調べるとき用）
};

class DebugOverlayWindow {
public:
    struct Rect {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
    };

    explicit DebugOverlayWindow(OverlayStyle style = OverlayStyle());
    ~DebugOverlayWindow();
    DebugOverlayWindow(const DebugOverlayWindow&) = delete;
    DebugOverlayWindow& operator=(const DebugOverlayWindow&) = delete;

    // X の接続を開く（開けなければ偽。Wayland のセッションでも偽）。2 回目からは前の結果を返す
    bool Open();
    // X の接続のファイル記述子（開いていなければ -1）
    int Fd() const;
    // 届いたイベントを処理する（窓が隠れていたところが見えたら描き直す）
    void Dispatch();

    // text（UTF-8。改行で行を分ける）をカーソル（ルートの窓の座標）の近くに出す。aboveLine なら行の上に出す
    void Show(const std::string& text, const Rect& caret, bool aboveLine);
    void Hide();
    bool Visible() const { return visible_; }
    // マウスポインタの位置（大きさ 0 の Rect。カーソルの位置を知らせないアプリで代わりに使う）。取れなければ偽
    bool PointerLocation(Rect* location);

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool opened_ = false;
    bool openFailed_ = false;
    bool visible_ = false;
};

// 窓の置き場所（ルートの窓の座標。テストする）。screen は画面全体の大きさ
DebugOverlayWindow::Rect PlaceDebugOverlay(const DebugOverlayWindow::Rect& caret, int width, int height,
                                           int screenWidth, int screenHeight, bool aboveLine);

} // namespace iroha
