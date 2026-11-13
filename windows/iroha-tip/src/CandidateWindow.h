#pragma once
#include <string>
#include <vector>

#include <d2d1.h>
#include <dwrite.h>

#include "Globals.h"

// 変換候補のリストを表示するowned popup。
// - フォーカスを奪わない（WS_EX_NOACTIVATE、SWP_NOACTIVATE）
// - TIP自身はDPI awarenessを宣言しない（ホストに追従）ため、座標は
//   GetTextExtが返すスクリーン座標をそのまま使い、フォントだけDPIに合わせる
// - 表示/非表示/変更時に EVENT_OBJECT_IME_* を発火する（ライトディスミス協調）
// - 文字は Direct2D・DirectWrite で描く。GDI では絵文字が白黒になり、肌の色などの組み合わせも
//   1 つの絵にならないため。絵文字の候補は Segoe UI Emoji で描く（Meiryo UI にも ❤ などの字形が
//   あり、そのままでは白黒の字形が使われる）。Direct2D を使えなければ GDI で描く
class CandidateWindow {
public:
    CandidateWindow() = default;
    CandidateWindow(const CandidateWindow&) = delete;
    CandidateWindow& operator=(const CandidateWindow&) = delete;
    ~CandidateWindow();

    // 1ページに出す候補数（数字キー 1〜9 で選ぶ）
    static constexpr size_t kPageSize = 9;

    // anchor: コンポジションのスクリーン座標矩形。直下（入らなければ直上）に出す。
    // candidates は全候補。selectedIndex のあるページだけを表示する。
    // annotations は候補の右に小さく出す印（専門辞書の名前。候補と同じ数か空）
    void Show(HWND owner, const RECT& anchor, const std::vector<std::u32string>& candidates,
              const std::vector<std::u32string>& annotations, size_t selectedIndex);
    void Hide();

    // 絵文字として描く候補か（絵文字のフォントで描く）
    static bool IsEmojiText(const std::wstring& text);

private:
    struct Row {
        std::wstring label; // 番号（「1  」）
        std::wstring text;  // 候補
        std::wstring annotation; // 候補の右に出す印（無ければ空）
        IDWriteTextLayout* layout = nullptr;
        IDWriteTextLayout* annotationLayout = nullptr;
    };

    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam,
                                         LPARAM lParam);
    void EnsureWindow(HWND owner);
    void UpdateFont();
    bool EnsureDirectWrite();
    void ReleaseLayouts();
    void BuildLayouts();
    // 大きさを測る（印の列の位置 annotationX_ も決める）
    SIZE MeasureContent();
    void Paint(HDC dc, const RECT& client);
    void PaintWithGdi(HDC dc, const RECT& client) const;

    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    UINT dpi_ = 96;
    float fontSize_ = 13.3f; // DirectWrite の文字の大きさ（ピクセル）
    // 表示中のページの候補（番号つき）と、その中で選んでいる位置
    std::vector<Row> rows_;
    size_t selected_ = 0;
    // 複数ページあるときのページ表示（「2/7」。1ページなら空）
    std::wstring footer_;
    IDWriteTextLayout* footerLayout_ = nullptr;
    int rowHeight_ = 0;
    int padding_ = 0;
    // 印の列の左端（候補の列の幅 + 間）。印が無ければ 0
    int annotationX_ = 0;

    // Direct2D・DirectWrite（初めて表示するときに作る。作れなければ GDI で描く）
    bool directWriteTried_ = false;
    ID2D1Factory* d2dFactory_ = nullptr;
    IDWriteFactory* dwriteFactory_ = nullptr;
    ID2D1DCRenderTarget* renderTarget_ = nullptr;
    IDWriteTextFormat* textFormat_ = nullptr;
    IDWriteTextFormat* footerFormat_ = nullptr;
    IDWriteTextFormat* annotationFormat_ = nullptr;
};
