#pragma once
// 設定画面の部品（Windows 11 の設定アプリの見た目に合わせる）。
//
// - Theme: 色と文字（DPI ごとに作り直す）
// - スイッチ（トグル）: チェックボックスと同じく BM_GETCHECK / BM_SETCHECK を受け、押されたら親に
//   WM_COMMAND（BN_CLICKED）を送る
// - 「?」ボタン: 押すと説明の小窓を出す（macOS版の HelpButton。項目の細かな説明は画面に並べずここに入れる）
// - Page: 節の見出しと、項目のカード（題・灰色の短い説明・右側の操作部品）を縦に並べて描き、はみ出したら
//   縦にスクロールする。開発者モードの項目には紫の「開発者」の印を付ける（macOS版の DeveloperBadge）
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ui {

struct Theme {
    COLORREF background = RGB(0xF3, 0xF3, 0xF3); // 窓の地
    COLORREF card = RGB(0xFB, 0xFB, 0xFB);       // 項目のカード
    COLORREF cardBorder = RGB(0xE5, 0xE5, 0xE5);
    COLORREF text = RGB(0x1A, 0x1A, 0x1A);
    COLORREF secondaryText = RGB(0x5F, 0x5F, 0x5F);
    COLORREF disabledText = RGB(0xA0, 0xA0, 0xA0);
    COLORREF accent = RGB(0x00, 0x5F, 0xB8);
    COLORREF developer = RGB(0x7A, 0x3E, 0xC8);     // 「開発者」の印の文字
    COLORREF developerFill = RGB(0xEE, 0xE4, 0xFA); // 印の地（紫を薄く）
    COLORREF warning = RGB(0xA3, 0x4A, 0x00);
    COLORREF navSelected = RGB(0xEA, 0xEA, 0xEA);
    COLORREF navHover = RGB(0xEE, 0xEE, 0xEE);

    UINT dpi = 96;
    HFONT pageTitle = nullptr; // ページの題
    HFONT header = nullptr;    // 節の見出し
    HFONT body = nullptr;      // 項目の題・操作部品
    HFONT caption = nullptr;   // 灰色の説明
    HFONT badge = nullptr;     // 「開発者」の印
    HFONT icon = nullptr;      // 左の一覧のアイコン（Segoe Fluent Icons）
    HBRUSH backgroundBrush = nullptr;
    HBRUSH cardBrush = nullptr;

    int S(int value) const { return MulDiv(value, static_cast<int>(dpi), 96); }
    void Rebuild(UINT newDpi);
    void Release();
};

extern Theme g_theme;

void RegisterClasses(HINSTANCE instance);

// スイッチ（id は WM_COMMAND の番号）
HWND CreateToggle(HWND parent, int id);
// 「?」ボタン
HWND CreateHelpButton(HWND parent, const std::wstring& text);
// 説明の小窓を anchor の下に出す
void ShowFlyout(HWND anchor, const std::wstring& text);

// 子の部品を作る（文字は g_theme.body）
HWND CreateControl(HWND parent, const wchar_t* className, const wchar_t* text, DWORD style, int id, DWORD exStyle = 0);

// 縦に並べた項目のページ
class Page {
public:
    struct Control {
        HWND hwnd = nullptr;
        int width = 0;  // DIP。-1 なら行の幅いっぱい
        int height = 32; // DIP
        // コンボボックスの一覧を開いたときの高さ（DIP。並べる高さには使わない）
        int dropHeight = 0;
    };
    struct Row {
        enum class Kind { Header, Item, Note, Fill };
        Kind kind = Kind::Item;
        std::wstring title;
        std::wstring description;
        std::wstring help;
        bool developer = false;
        bool warning = false; // 説明を注意の色で描く
        // 右側の操作部品（右から詰めずに、左から並べて右に寄せる）
        std::vector<Control> right;
        // 題と説明の下に並べる部品（1 要素が 1 行）
        std::vector<std::vector<Control>> below;
        // 表示するか・操作できるか（無ければ常に）
        std::function<bool()> visible;
        std::function<bool()> enabled;
        // Fill: 残りの高さをこの行に使う（一覧など）。最低の高さ（DIP）
        int minHeight = 160;
        // 内部: 「?」ボタン・描く位置
        HWND helpButton = nullptr;
        RECT rect = {};
        bool shown = true;
    };

    Page(HWND parent, const std::wstring& title);
    ~Page();
    Page(const Page&) = delete;
    Page& operator=(const Page&) = delete;

    HWND hwnd() const { return hwnd_; }

    Row* AddHeader(const std::wstring& title, const std::wstring& help = L"", bool developer = false);
    Row* AddItem(const std::wstring& title, const std::wstring& description, const std::wstring& help = L"",
                 bool developer = false);
    Row* AddNote(const std::wstring& text);
    Row* AddFill(HWND control, int minHeight);

    // 説明を変える（並べ直して描き直す）
    void SetDescription(Row* row, const std::wstring& text, bool warning = false);
    // 表示・操作できるかを見直して並べ直す
    void Relayout();
    void Show(bool visible);
    void ScrollTo(int position);

    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

private:
    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam);
    void Paint(HDC dc);
    int MeasureText(HFONT font, const std::wstring& text, int width) const;

    HWND hwnd_ = nullptr;
    std::wstring title_;
    std::vector<std::unique_ptr<Row>> rows_;
    int scroll_ = 0;
    int contentHeight_ = 0;
};

} // namespace ui
