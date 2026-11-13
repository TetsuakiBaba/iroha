#include "selection_assistant.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>

#include "iroha/unicode.h"
#include "key_shortcut.h"

namespace iroha {

namespace {

constexpr UINT WM_APP_CONFIGURE = WM_APP + 1;
constexpr UINT WM_APP_PARTIAL = WM_APP + 2; // wParam: 番号、lParam: std::u32string*
constexpr UINT WM_APP_DONE = WM_APP + 3;    // wParam: 番号、lParam: AIResult*
constexpr UINT WM_APP_QUIT = WM_APP + 4;
constexpr int kOnDemandHotkeyId = 100;
constexpr const wchar_t* kHiddenClass = L"IrohaSelectionHotkeys";
constexpr const wchar_t* kPanelClass = L"IrohaSelectionPanel";

enum ControlId {
    kIdTitle = 200,
    kIdSource,
    kIdPrompt,
    kIdOutput,
    kIdNotice,
    kIdCopy,
};

std::wstring Wide(const std::u32string& text) { return Utf32ToUtf16(text); }

// ---- クリップボード ----

bool OpenClipboardRetry(HWND owner) {
    for (int i = 0; i < 20; ++i) {
        if (OpenClipboard(owner)) return true;
        Sleep(15);
    }
    return false;
}

// クリップボードの中身の控え（メモリで持てる形式だけ。ビットマップなどの GDI の取っ手は控えられない）
struct ClipboardSnapshot {
    std::vector<std::pair<UINT, std::vector<char>>> items;
};

// GDI の取っ手で渡される形式（メモリとして写せないので控えない）
bool IsHandleFormat(UINT format) {
    switch (format) {
    case CF_BITMAP:
    case CF_DSPBITMAP:
    case CF_PALETTE:
    case CF_METAFILEPICT:
    case CF_DSPMETAFILEPICT:
    case CF_ENHMETAFILE:
    case CF_DSPENHMETAFILE:
    case CF_OWNERDISPLAY:
        return true;
    default:
        return false;
    }
}

ClipboardSnapshot SnapshotClipboard(HWND owner) {
    ClipboardSnapshot snapshot;
    if (!OpenClipboardRetry(owner)) return snapshot;
    for (UINT format = EnumClipboardFormats(0); format != 0; format = EnumClipboardFormats(format)) {
        if (IsHandleFormat(format)) continue;
        HANDLE handle = GetClipboardData(format);
        if (!handle) continue;
        const SIZE_T size = GlobalSize(handle);
        if (size == 0) continue;
        const void* data = GlobalLock(handle);
        if (!data) continue;
        snapshot.items.push_back({format, std::vector<char>(static_cast<const char*>(data), static_cast<const char*>(data) + size)});
        GlobalUnlock(handle);
    }
    CloseClipboard();
    return snapshot;
}

bool PutClipboardData(UINT format, const void* data, size_t size) {
    HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, size == 0 ? 1 : size);
    if (!global) return false;
    void* target = GlobalLock(global);
    if (!target) {
        GlobalFree(global);
        return false;
    }
    if (size > 0) std::memcpy(target, data, size);
    GlobalUnlock(global);
    if (!SetClipboardData(format, global)) {
        GlobalFree(global);
        return false;
    }
    return true;
}

// クリップボードの履歴（Win+V）とクラウドの同期に残さない印（書き込む一時の中身に付ける）
void PutExcludeFromHistory() {
    static const UINT exclude = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    static const UINT history = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    static const UINT cloud = RegisterClipboardFormatW(L"CanUploadToCloudClipboard");
    const DWORD zero = 0;
    PutClipboardData(exclude, &zero, sizeof(zero));
    PutClipboardData(history, &zero, sizeof(zero));
    PutClipboardData(cloud, &zero, sizeof(zero));
}

void RestoreClipboard(HWND owner, const ClipboardSnapshot& snapshot) {
    if (!OpenClipboardRetry(owner)) return;
    EmptyClipboard();
    for (const auto& item : snapshot.items) PutClipboardData(item.first, item.second.data(), item.second.size());
    CloseClipboard();
}

bool SetClipboardText(HWND owner, const std::wstring& text, bool temporary) {
    if (!OpenClipboardRetry(owner)) return false;
    EmptyClipboard();
    const bool ok = PutClipboardData(CF_UNICODETEXT, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    if (temporary) PutExcludeFromHistory();
    CloseClipboard();
    return ok;
}

std::optional<std::wstring> ReadClipboardText(HWND owner) {
    if (!OpenClipboardRetry(owner)) return std::nullopt;
    std::optional<std::wstring> text;
    if (HANDLE handle = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* data = static_cast<const wchar_t*>(GlobalLock(handle))) {
            text = std::wstring(data);
            GlobalUnlock(handle);
        }
    }
    CloseClipboard();
    return text;
}

// ---- キーを送る ----

// ショートカットの修飾キーを離すのを待つ（押したまま Ctrl+C を送ると Ctrl+Alt+C などになる）
void WaitForModifiersReleased() {
    const int keys[] = {VK_MENU, VK_SHIFT, VK_LWIN, VK_RWIN, VK_CONTROL};
    for (int i = 0; i < 100; ++i) {
        bool held = false;
        for (int key : keys) held = held || (GetAsyncKeyState(key) & 0x8000) != 0;
        if (!held) return;
        Sleep(15);
    }
}

// Ctrl+<キー> を送る
void SendControlChord(WORD key) {
    INPUT inputs[4] = {};
    for (INPUT& input : inputs) input.type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;
    inputs[1].ki.wVk = key;
    inputs[2].ki.wVk = key;
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3].ki.wVk = VK_CONTROL;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, inputs, sizeof(INPUT));
}

// 窓を前面に出す。ほかのアプリが前面にいる間は SetForegroundWindow が断られることがあるので、
// 前面のスレッドの入力の状態をいったんつないでから頼む
void BringToForeground(HWND window) {
    HWND foreground = GetForegroundWindow();
    const DWORD foregroundThread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const DWORD currentThread = GetCurrentThreadId();
    const bool attached = foregroundThread != 0 && foregroundThread != currentThread &&
                          AttachThreadInput(foregroundThread, currentThread, TRUE);
    SetForegroundWindow(window);
    SetActiveWindow(window);
    if (attached) AttachThreadInput(foregroundThread, currentThread, FALSE);
}

std::wstring ProcessName(HWND window) {
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return L"";
    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    std::wstring name;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        name = path;
        const size_t slash = name.find_last_of(L"\\/");
        if (slash != std::wstring::npos) name = name.substr(slash + 1);
        std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    }
    CloseHandle(process);
    return name;
}

} // namespace

struct SelectionAssistant::Impl {
    enum class Phase { Hidden, PromptInput, Processing, Done };

    Runner runner;
    Check available;
    Check composing;
    Logger log;

    std::mutex settingsMutex;
    SelectionAssistantSettings settings;
    std::thread thread;
    std::atomic<HWND> hidden{nullptr};

    // 以下は窓のスレッドだけで触る
    SelectionAssistantSettings active;
    HWND panel = nullptr;
    HFONT font = nullptr;
    HWND title = nullptr, source = nullptr, prompt = nullptr, output = nullptr, notice = nullptr;
    HWND primary = nullptr, copy = nullptr, close = nullptr;
    Phase phase = Phase::Hidden;
    HWND target = nullptr;       // 選択を取ったアプリの窓（結果を貼る先）
    std::u32string sourceText;   // 選択していた文章（無ければ空）
    std::u32string resultText;
    bool generation = false;     // 選択が無く、文章を作る
    uint64_t requestNumber = 0;
    std::shared_ptr<CancelToken> cancel;
    std::thread worker;

    void Run();
    void RegisterHotkeys();
    void OnHotkey(int id);
    std::optional<std::u32string> CaptureSelection(HWND window);
    void ShowPanel(const std::u32string& heading);
    void Layout();
    void SetPhase(Phase next);
    void StartRequest(const std::u32string& heading, const AIRequest& request);
    void SubmitPrompt();
    void Apply();
    void CopyResult();
    void HidePanel();
    void StopWorker();
    int Scale(int value) const { return MulDiv(value, panel ? static_cast<int>(GetDpiForWindow(panel)) : 96, 96); }

    static LRESULT CALLBACK HiddenProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK PanelProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
};

SelectionAssistant::SelectionAssistant(Runner runner, Check available, Check composing, Logger log)
    : impl_(std::make_unique<Impl>()) {
    impl_->runner = std::move(runner);
    impl_->available = std::move(available);
    impl_->composing = std::move(composing);
    impl_->log = std::move(log);
    impl_->thread = std::thread([impl = impl_.get()] { impl->Run(); });
}

SelectionAssistant::~SelectionAssistant() {
    // 窓のスレッドを止める（窓ができる前なら、できるのを待ってから知らせる）
    for (int i = 0; i < 200 && !impl_->hidden.load(); ++i) Sleep(10);
    if (HWND hidden = impl_->hidden.load()) PostMessageW(hidden, WM_APP_QUIT, 0, 0);
    if (impl_->thread.joinable()) impl_->thread.join();
}

void SelectionAssistant::Configure(SelectionAssistantSettings settings) {
    {
        std::lock_guard<std::mutex> lock(impl_->settingsMutex);
        impl_->settings = std::move(settings);
    }
    if (HWND hidden = impl_->hidden.load()) PostMessageW(hidden, WM_APP_CONFIGURE, 0, 0);
}

void SelectionAssistant::Impl::Run() {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW hiddenClass = {sizeof(hiddenClass)};
    hiddenClass.lpfnWndProc = HiddenProc;
    hiddenClass.hInstance = instance;
    hiddenClass.lpszClassName = kHiddenClass;
    RegisterClassExW(&hiddenClass);
    WNDCLASSEXW panelClass = {sizeof(panelClass)};
    panelClass.lpfnWndProc = PanelProc;
    panelClass.hInstance = instance;
    panelClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    panelClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    panelClass.lpszClassName = kPanelClass;
    RegisterClassExW(&panelClass);

    HWND window = CreateWindowExW(0, kHiddenClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
    if (!window) {
        log("selection: window creation failed");
        return;
    }
    hidden = window;
    RegisterHotkeys();

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (panel && IsDialogMessageW(panel, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    StopWorker();
    if (panel) DestroyWindow(panel);
    if (font) DeleteObject(font);
}

void SelectionAssistant::Impl::RegisterHotkeys() {
    HWND window = hidden.load();
    for (int id = 0; id <= kOnDemandHotkeyId; ++id) UnregisterHotKey(window, id);
    {
        std::lock_guard<std::mutex> lock(settingsMutex);
        active = settings;
    }
    if (!active.enabled) return;
    std::vector<uint32_t> registered;
    const auto add = [&](int id, const std::string& text) {
        const auto shortcut = KeyShortcut::Parse(text);
        if (!shortcut) return;
        // 同じキーが複数の枠に入っていたら先勝ち（二重に動かない）
        if (std::find(registered.begin(), registered.end(), shortcut->Encode()) != registered.end()) return;
        const UINT modifiers = (shortcut->control ? MOD_CONTROL : 0) | (shortcut->alt ? MOD_ALT : 0) |
                               (shortcut->shift ? MOD_SHIFT : 0) | MOD_NOREPEAT;
        if (!RegisterHotKey(window, id, modifiers, shortcut->vk)) {
            log("selection: hotkey " + text + " is already in use (" + std::to_string(GetLastError()) + ")");
            return;
        }
        registered.push_back(shortcut->Encode());
    };
    for (size_t i = 0; i < active.presets.size(); ++i) {
        if (active.presets[i].enabled && !active.presets[i].hotkey.empty()) add(static_cast<int>(i), active.presets[i].hotkey);
    }
    if (!active.onDemandHotkey.empty()) add(kOnDemandHotkeyId, active.onDemandHotkey);
    log("selection: " + std::to_string(registered.size()) + " hotkeys registered");
}

// 選択している文章を取る（クリップボードの中身を控えて Ctrl+C を送り、控えた中身を戻す）。選択が無ければ無し
std::optional<std::u32string> SelectionAssistant::Impl::CaptureSelection(HWND window) {
    HWND owner = hidden.load();
    WaitForModifiersReleased();
    const ClipboardSnapshot snapshot = SnapshotClipboard(owner);
    const std::wstring marker = L"__IROHA_SELECTION_MARKER__";
    SetClipboardText(owner, marker, /*temporary=*/true);
    const DWORD sequence = GetClipboardSequenceNumber();
    SetForegroundWindow(window);
    SendControlChord('C');
    // コピーが終わるのを待つ（クリップボードの番号が進む。進まなければ選択が無いか、コピーできないアプリ）
    for (int i = 0; i < 40 && GetClipboardSequenceNumber() == sequence; ++i) Sleep(15);
    Sleep(30);
    const auto text = ReadClipboardText(owner);
    RestoreClipboard(owner, snapshot);
    if (!text || *text == marker || TrimWhitespace(Utf16ToUtf32(*text)).empty()) return std::nullopt;
    return Utf16ToUtf32(*text);
}

void SelectionAssistant::Impl::OnHotkey(int id) {
    if (!active.enabled || phase == Phase::Processing || (panel && IsWindowVisible(panel))) return;
    // 入力中（未確定文字列がある）は使わない（macOS版と同じ）
    if (composing && composing()) return;
    HWND window = GetForegroundWindow();
    if (!window) return;
    const std::wstring process = ProcessName(window);
    if (process.rfind(L"iroha", 0) == 0 ||
        std::find(active.excludedApps.begin(), active.excludedApps.end(), process) != active.excludedApps.end()) {
        return;
    }
    target = window;
    const auto selected = CaptureSelection(window);
    sourceText = selected.value_or(U"");
    generation = !selected;
    if (id == kOnDemandHotkeyId || !selected) {
        // その場でプロンプトを入れる（選択が無ければ文章を作る）
        ShowPanel(generation ? U"テキストを生成" : U"AIに指示");
        SetPhase(Phase::PromptInput);
        SetFocus(prompt);
        return;
    }
    if (id < 0 || static_cast<size_t>(id) >= active.presets.size()) return;
    const auto& preset = active.presets[static_cast<size_t>(id)];
    const std::u32string presetPrompt = TrimWhitespace(preset.prompt).empty() && static_cast<size_t>(id) <
                                                                              SelectionSettingsDefaults::Presets().size()
                                      ? std::u32string(SelectionSettingsDefaults::Presets()[static_cast<size_t>(id)].prompt)
                                      : preset.prompt;
    const std::u32string heading =
        TrimWhitespace(preset.name).empty() ? U"プリセット" + Utf8ToUtf32(std::to_string(id + 1)) : preset.name;
    ShowPanel(heading);
    StartRequest(heading, AIRequest::Build(presetPrompt, sourceText));
}

void SelectionAssistant::Impl::ShowPanel(const std::u32string& heading) {
    if (!panel) {
        panel = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kPanelClass, L"iroha",
                                WS_POPUP | WS_CAPTION | WS_SYSMENU, 0, 0, 100, 100, nullptr, nullptr,
                                GetModuleHandleW(nullptr), this);
        if (!panel) return;
        font = CreateFontW(-Scale(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Meiryo UI");
        const auto child = [this](const wchar_t* className, const wchar_t* text, DWORD style, int id, DWORD exStyle = 0) {
            HWND control = CreateWindowExW(exStyle, className, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, panel,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr),
                                           nullptr);
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            return control;
        };
        title = child(L"STATIC", L"", SS_LEFT, kIdTitle);
        source = child(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, kIdSource);
        prompt = child(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, kIdPrompt, WS_EX_CLIENTEDGE);
        output = child(L"EDIT", L"", WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, kIdOutput,
                       WS_EX_CLIENTEDGE);
        notice = child(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, kIdNotice);
        primary = child(L"BUTTON", L"置き換える", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK);
        copy = child(L"BUTTON", L"コピー", WS_TABSTOP, kIdCopy);
        close = child(L"BUTTON", L"閉じる", WS_TABSTOP, IDCANCEL);
    }
    SetWindowTextW(title, Wide(heading).c_str());
    std::u32string shown = sourceText;
    std::replace(shown.begin(), shown.end(), U'\n', U' ');
    std::replace(shown.begin(), shown.end(), U'\r', U' ');
    SetWindowTextW(source, generation ? L"（選択なし: 指示から文章を作り、カーソルの位置に入れます）"
                                      : (L"選択: " + Wide(shown)).c_str());
    SetWindowTextW(output, L"");
    SetWindowTextW(prompt, L"");
    SetWindowTextW(notice, L"");
    // マウスの近く（画面からはみ出さないように）
    POINT cursor = {};
    GetCursorPos(&cursor);
    MONITORINFO monitor = {sizeof(monitor)};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor);
    const UINT dpi = GetDpiForWindow(panel);
    RECT frame = {0, 0, MulDiv(480, static_cast<int>(dpi), 96), MulDiv(330, static_cast<int>(dpi), 96)};
    AdjustWindowRectExForDpi(&frame, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_TOPMOST | WS_EX_TOOLWINDOW, dpi);
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;
    const int x = std::min(std::max<int>(cursor.x + 12, monitor.rcWork.left), static_cast<int>(monitor.rcWork.right) - width);
    const int y = std::min(std::max<int>(cursor.y + 16, monitor.rcWork.top), static_cast<int>(monitor.rcWork.bottom) - height);
    SetWindowPos(panel, HWND_TOPMOST, x, y, width, height, SWP_SHOWWINDOW);
    Layout();
    BringToForeground(panel);
}

void SelectionAssistant::Impl::Layout() {
    RECT client = {};
    GetClientRect(panel, &client);
    const int margin = Scale(12);
    const int width = client.right - margin * 2;
    MoveWindow(title, margin, Scale(10), width, Scale(22), TRUE);
    MoveWindow(source, margin, Scale(34), width, Scale(20), TRUE);
    const bool prompting = phase == Phase::PromptInput;
    ShowWindow(prompt, prompting ? SW_SHOW : SW_HIDE);
    int y = Scale(60);
    if (prompting) {
        MoveWindow(prompt, margin, y, width, Scale(26), TRUE);
        y += Scale(34);
    }
    const int buttons = client.bottom - Scale(40);
    MoveWindow(output, margin, y, width, buttons - Scale(28) - y, TRUE);
    MoveWindow(notice, margin, buttons - Scale(24), width, Scale(20), TRUE);
    MoveWindow(primary, client.right - margin - Scale(300), buttons, Scale(110), Scale(28), TRUE);
    MoveWindow(copy, client.right - margin - Scale(182), buttons, Scale(84), Scale(28), TRUE);
    MoveWindow(close, client.right - margin - Scale(90), buttons, Scale(90), Scale(28), TRUE);
}

void SelectionAssistant::Impl::SetPhase(Phase next) {
    phase = next;
    const bool done = phase == Phase::Done && !resultText.empty();
    SetWindowTextW(primary, phase == Phase::PromptInput ? L"実行" : generation ? L"入れる" : L"置き換える");
    EnableWindow(primary, phase == Phase::PromptInput || done);
    EnableWindow(copy, done);
    Layout();
}

void SelectionAssistant::Impl::StartRequest(const std::u32string& heading, const AIRequest& request) {
    SetWindowTextW(title, Wide(heading).c_str());
    resultText.clear();
    SetWindowTextW(output, L"");
    if (available && !available()) {
        SetWindowTextW(notice, L"AIサービスが設定されていません（設定 > AI）");
        SetPhase(Phase::Done);
        return;
    }
    SetWindowTextW(notice, L"AIで処理しています…（Esc で閉じる）");
    SetPhase(Phase::Processing);
    StopWorker();
    const uint64_t number = ++requestNumber;
    cancel = std::make_shared<CancelToken>();
    HWND window = panel;
    worker = std::thread([this, number, window, request, token = cancel] {
        const AIResult result = runner(
            request,
            [window, number](const std::u32string& partial) {
                PostMessageW(window, WM_APP_PARTIAL, static_cast<WPARAM>(number),
                             reinterpret_cast<LPARAM>(new std::u32string(partial)));
            },
            token.get());
        if (!PostMessageW(window, WM_APP_DONE, static_cast<WPARAM>(number), reinterpret_cast<LPARAM>(new AIResult(result)))) {
            // 窓が無くなっていれば、結果は捨てる
        }
    });
}

void SelectionAssistant::Impl::SubmitPrompt() {
    wchar_t buffer[2048];
    GetWindowTextW(prompt, buffer, ARRAYSIZE(buffer));
    const std::u32string instruction = TrimWhitespace(Utf16ToUtf32(buffer));
    if (instruction.empty()) return;
    AIRequest request;
    if (generation) {
        request.instructions = std::u32string(SelectionSettingsDefaults::kGenerationInstructions) + AICommitSettings::kOutputRule;
        request.userMessage = instruction;
    } else {
        request = AIRequest::Build(instruction, sourceText);
    }
    StartRequest(generation ? U"テキストを生成" : U"AIに指示", request);
}

// 結果を元のアプリに戻って貼る（選択していれば置き換わる。選択が無ければカーソルの位置に入る）
void SelectionAssistant::Impl::Apply() {
    if (phase != Phase::Done || resultText.empty()) return;
    const std::wstring text = Wide(resultText);
    HWND owner = hidden.load();
    HWND destination = target;
    HidePanel();
    if (!destination || !IsWindow(destination)) return;
    const ClipboardSnapshot snapshot = SnapshotClipboard(owner);
    SetClipboardText(owner, text, /*temporary=*/true);
    SetForegroundWindow(destination);
    Sleep(150);
    WaitForModifiersReleased();
    SendControlChord('V');
    Sleep(300);
    RestoreClipboard(owner, snapshot);
}

void SelectionAssistant::Impl::CopyResult() {
    if (phase != Phase::Done || resultText.empty()) return;
    SetClipboardText(hidden.load(), Wide(resultText), /*temporary=*/false);
    SetWindowTextW(notice, L"コピーしました");
}

void SelectionAssistant::Impl::HidePanel() {
    if (cancel) cancel->Cancel();
    ++requestNumber; // 遅れて届いた途中経過・結果を捨てる
    phase = Phase::Hidden;
    if (panel) ShowWindow(panel, SW_HIDE);
}

void SelectionAssistant::Impl::StopWorker() {
    if (cancel) cancel->Cancel();
    if (worker.joinable()) worker.join();
}

LRESULT CALLBACK SelectionAssistant::Impl::HiddenProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
    }
    auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_HOTKEY:
        if (self) self->OnHotkey(static_cast<int>(wParam));
        return 0;
    case WM_APP_CONFIGURE:
        if (self) self->RegisterHotkeys();
        return 0;
    case WM_APP_QUIT:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK SelectionAssistant::Impl::PanelProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
    }
    auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);
    switch (message) {
    case WM_APP_PARTIAL: {
        std::unique_ptr<std::u32string> partial(reinterpret_cast<std::u32string*>(lParam));
        if (wParam == self->requestNumber && self->phase == Phase::Processing) {
            std::u32string text = *partial;
            // 複数行の入力欄は改行を CRLF で受ける
            std::wstring wide;
            for (char32_t c : text) {
                if (c == U'\n') wide += L'\r';
                wide += Utf32ToUtf16(std::u32string(1, c));
            }
            SetWindowTextW(self->output, wide.c_str());
        }
        return 0;
    }
    case WM_APP_DONE: {
        std::unique_ptr<AIResult> result(reinterpret_cast<AIResult*>(lParam));
        if (wParam != self->requestNumber) return 0;
        if (result->text) {
            self->resultText = CleanAIOutput(*result->text);
            std::wstring wide;
            for (char32_t c : self->resultText) {
                if (c == U'\n') wide += L'\r';
                wide += Utf32ToUtf16(std::u32string(1, c));
            }
            SetWindowTextW(self->output, wide.c_str());
            SetWindowTextW(self->notice, self->generation ? L"Enter で入れる・Esc で閉じる" : L"Enter で置き換える・Esc で閉じる");
        } else {
            SetWindowTextW(self->notice, (L"AIの処理に失敗しました（" + Wide(Utf8ToUtf32(result->error)) + L"）").c_str());
        }
        self->SetPhase(Phase::Done);
        SetFocus(self->resultText.empty() ? self->close : self->primary);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK:
            if (self->phase == Phase::PromptInput) {
                self->SubmitPrompt();
            } else {
                self->Apply();
            }
            return 0;
        case kIdCopy:
            self->CopyResult();
            return 0;
        case IDCANCEL:
            self->HidePanel();
            return 0;
        default:
            break;
        }
        break;
    case WM_CLOSE:
        self->HidePanel();
        return 0;
    case WM_DPICHANGED: {
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        self->Layout();
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace iroha
