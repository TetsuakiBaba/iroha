#pragma once
#include <string>
#include <vector>

#include "Globals.h"

// 変換候補のリストを表示するowned popup。
// - フォーカスを奪わない（WS_EX_NOACTIVATE、SWP_NOACTIVATE）
// - TIP自身はDPI awarenessを宣言しない（ホストに追従）ため、座標は
//   GetTextExtが返すスクリーン座標をそのまま使い、フォントだけDPIに合わせる
// - 表示/非表示/変更時に EVENT_OBJECT_IME_* を発火する（ライトディスミス協調）
class CandidateWindow {
public:
    CandidateWindow() = default;
    CandidateWindow(const CandidateWindow&) = delete;
    CandidateWindow& operator=(const CandidateWindow&) = delete;
    ~CandidateWindow();

    // 1ページに出す候補数（数字キー 1〜9 で選ぶ）
    static constexpr size_t kPageSize = 9;

    // anchor: コンポジションのスクリーン座標矩形。直下（入らなければ直上）に出す。
    // candidates は全候補。selectedIndex のあるページだけを表示する
    void Show(HWND owner, const RECT& anchor,
              const std::vector<std::u32string>& candidates, size_t selectedIndex);
    void Hide();

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam,
                                         LPARAM lParam);
    void EnsureWindow(HWND owner);
    void UpdateFont();
    SIZE MeasureContent() const;
    void Paint(HDC dc, const RECT& client) const;

    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    UINT dpi_ = 96;
    // 表示中のページの候補（番号つき）と、その中で選んでいる位置
    std::vector<std::wstring> items_;
    size_t selected_ = 0;
    // 複数ページあるときのページ表示（「2/7」。1ページなら空）
    std::wstring footer_;
    int rowHeight_ = 0;
    int padding_ = 0;
};
