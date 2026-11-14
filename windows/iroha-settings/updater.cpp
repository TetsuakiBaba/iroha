#include "updater.h"

#include <commctrl.h>
#include <shellapi.h>
#include <urlmon.h>
#include <wininet.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

#include "iroha/unicode.h"
#include "iroha/update_check.h"
#include "term_dictionary_fetcher.h"

namespace updater {

namespace {

constexpr int kButtonUpdate = 100;
constexpr int kButtonNotes = 101;

std::wstring Wide(const std::string& utf8) { return iroha::Utf32ToUtf16(iroha::Utf8ToUtf32(utf8)); }

int64_t Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void ShowInfo(HWND owner, const std::wstring& title, const std::wstring& text, PCWSTR icon) {
    TASKDIALOGCONFIG dialog = {sizeof(dialog)};
    dialog.hwndParent = owner;
    dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    dialog.pszWindowTitle = L"iroha";
    dialog.pszMainIcon = icon;
    dialog.pszMainInstruction = title.c_str();
    dialog.pszContent = text.c_str();
    dialog.dwCommonButtons = TDCBF_OK_BUTTON;
    TaskDialogIndirect(&dialog, nullptr, nullptr, nullptr);
}

// 一時フォルダの iroha-update（無ければ作る）
std::filesystem::path WorkDirectory() {
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetTempPathW(MAX_PATH, buffer);
    std::filesystem::path directory = std::filesystem::path(std::wstring(buffer, length)) / L"iroha-update";
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    return directory;
}

bool FetchLatest(const iroha::Config& config, iroha::ReleaseInfo* release) {
    const std::string url = config.updateCheckURL.empty() ? iroha::Update::kLatestReleaseUrl : config.updateCheckURL;
    const std::filesystem::path path = WorkDirectory() / (L"latest-" + std::to_wstring(GetCurrentProcessId()) + L".json");
    std::string error;
    bool ok = iroha::TermDictionaryFetcher::Download(url, path, &error);
    std::string text;
    if (ok) {
        std::ifstream file(path, std::ios::binary);
        std::stringstream buffer;
        buffer << file.rdbuf();
        text = buffer.str();
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    return ok && iroha::Update::ParseRelease(text, release);
}

// URLDownloadToFile の進み具合を受け取る（取り消されたら E_ABORT を返して止める）
class Progress : public IBindStatusCallback {
public:
    std::atomic<uint64_t> received{0};
    std::atomic<uint64_t> total{0};
    std::atomic<bool> cancel{false};

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (riid == IID_IUnknown || riid == IID_IBindStatusCallback) {
            *object = static_cast<IBindStatusCallback*>(this);
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE OnStartBinding(DWORD, IBinding*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPriority(LONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnLowResource(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnProgress(ULONG progress, ULONG progressMax, ULONG, LPCWSTR) override {
        received = progress;
        if (progressMax != 0) total = progressMax;
        return cancel ? E_ABORT : S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnStopBinding(HRESULT, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBindInfo(DWORD*, BINDINFO*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDataAvailable(DWORD, DWORD, FORMATETC*, STGMEDIUM*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnObjectAvailable(REFIID, IUnknown*) override { return S_OK; }
};

struct DownloadJob {
    Progress progress;
    std::atomic<bool> done{false};
    HRESULT result = E_FAIL;
    uint64_t expectedSize = 0;
};

HRESULT CALLBACK DownloadDialogProc(HWND hwnd, UINT notification, WPARAM, LPARAM, LONG_PTR data) {
    auto* job = reinterpret_cast<DownloadJob*>(data);
    if (notification == TDN_CREATED) {
        SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_RANGE, 0, MAKELPARAM(0, 1000));
    } else if (notification == TDN_TIMER) {
        const uint64_t total = job->progress.total != 0 ? job->progress.total.load() : job->expectedSize;
        if (total != 0) {
            const uint64_t position = std::min<uint64_t>(1000, job->progress.received * 1000 / total);
            SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_POS, static_cast<WPARAM>(position), 0);
        }
        // 終わったら閉じる（Cancel を押したのと同じ形で閉じ、終わったかは done で見分ける）
        if (job->done) PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
    return S_OK;
}

// インストーラーを落とす（進み具合の窓つき）。取り消し・失敗なら偽
bool DownloadInstaller(HWND owner, const iroha::ReleaseAsset& asset, const std::filesystem::path& path,
                       std::wstring* error) {
    const std::wstring url = Wide(asset.url);
    DeleteUrlCacheEntryW(url.c_str());
    DownloadJob job;
    job.expectedSize = asset.size;
    std::thread worker([&job, url, path] {
        job.result = URLDownloadToFileW(nullptr, url.c_str(), path.c_str(), 0, &job.progress);
        job.done = true;
    });
    const std::wstring content = Wide(asset.name);
    TASKDIALOGCONFIG dialog = {sizeof(dialog)};
    dialog.hwndParent = owner;
    dialog.dwFlags = TDF_SHOW_PROGRESS_BAR | TDF_CALLBACK_TIMER | TDF_ALLOW_DIALOG_CANCELLATION |
                     TDF_POSITION_RELATIVE_TO_WINDOW;
    dialog.pszWindowTitle = L"iroha";
    dialog.pszMainInstruction = L"アップデートをダウンロードしています";
    dialog.pszContent = content.c_str();
    dialog.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    dialog.pfCallback = DownloadDialogProc;
    dialog.lpCallbackData = reinterpret_cast<LONG_PTR>(&job);
    TaskDialogIndirect(&dialog, nullptr, nullptr, nullptr);
    if (!job.done) job.progress.cancel = true;
    worker.join();
    std::error_code ignored;
    if (job.progress.cancel) {
        std::filesystem::remove(path, ignored);
        return false;
    }
    if (FAILED(job.result)) {
        wchar_t code[32];
        swprintf_s(code, L"0x%08X", static_cast<unsigned>(job.result));
        *error = std::wstring(L"ダウンロードできませんでした（") + code + L"）。ネットワーク接続を確認してください。";
        std::filesystem::remove(path, ignored);
        return false;
    }
    // 途中で切れた・差し替わったファイルを起動しない
    const uintmax_t size = std::filesystem::file_size(path, ignored);
    if (asset.size != 0 && size != asset.size) {
        *error = L"ダウンロードしたファイルの大きさが違います。もう一度お試しください。";
        std::filesystem::remove(path, ignored);
        return false;
    }
    if (!asset.sha256.empty() && iroha::TermDictionaryFetcher::Sha256Hex(path) != asset.sha256) {
        *error = L"ダウンロードしたファイルが壊れています（SHA-256 が一致しません）。安全のためアップデートを中止しました。";
        std::filesystem::remove(path, ignored);
        return false;
    }
    return true;
}

} // namespace

Result CheckAndOffer(HWND owner, const iroha::Config& config, const std::filesystem::path& dataDirectory,
                     const std::string& currentVersion, bool manual) {
    const std::filesystem::path statePath = iroha::Update::StatePath(dataDirectory);
    iroha::Update::State state = iroha::Update::LoadState(statePath);
    if (!manual && (!config.autoUpdateCheck || !iroha::Update::IsDue(state, Now()))) return Result::None;
    // 先に記録する（確認に失敗しても、次は 24 時間後）
    state.lastCheck = Now();
    iroha::Update::SaveState(statePath, state);

    iroha::ReleaseInfo release;
    if (!FetchLatest(config, &release)) {
        if (manual) ShowInfo(owner, L"アップデートを確認できませんでした", L"ネットワーク接続を確認してください。", TD_WARNING_ICON);
        return Result::None;
    }
    const std::string local = iroha::Update::ComparableVersion(currentVersion);
    const std::wstring currentText = L"現在の版: " + Wide(currentVersion);
    if (!iroha::Update::IsNewer(release.tag, local)) {
        if (manual) ShowInfo(owner, L"iroha は最新です", currentText, TD_INFORMATION_ICON);
        return Result::None;
    }
    const iroha::ReleaseAsset* installer = iroha::Update::WindowsInstaller(release);
    if (!installer) {
        if (manual) {
            ShowInfo(owner, L"新しい版に Windows のインストーラーがありません",
                     L"iroha " + Wide(release.tag) + L" のリリースに Windows 版が含まれていません。", TD_WARNING_ICON);
        }
        return Result::None;
    }
    // 同じ版を何度も知らせない（スキップした版・自動で知らせた版）
    if (!manual) {
        if (release.tag == state.skipped || release.tag == state.lastNotified) return Result::None;
        state.lastNotified = release.tag;
        iroha::Update::SaveState(statePath, state);
    }

    std::wstring version = Wide(release.tag);
    if (!version.empty() && (version[0] == L'v' || version[0] == L'V')) version.erase(0, 1);
    const std::wstring title = L"iroha " + version + L" が利用可能です";
    std::u32string notes = release.notes;
    if (notes.size() > 600) notes = notes.substr(0, 600) + U"…";
    const std::wstring content = currentText + (notes.empty() ? L"" : L"\n\n" + iroha::Utf32ToUtf16(notes));
    const TASKDIALOG_BUTTON buttons[] = {{kButtonUpdate, L"アップデート"}, {kButtonNotes, L"リリースノート"}};
    TASKDIALOGCONFIG dialog = {sizeof(dialog)};
    dialog.hwndParent = owner;
    dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    dialog.pszWindowTitle = L"iroha";
    dialog.pszMainIcon = TD_INFORMATION_ICON;
    dialog.pszMainInstruction = title.c_str();
    dialog.pszContent = content.c_str();
    dialog.cButtons = ARRAYSIZE(buttons);
    dialog.pButtons = buttons;
    dialog.nDefaultButton = kButtonUpdate;
    dialog.dwCommonButtons = TDCBF_CLOSE_BUTTON; // 「閉じる」= 後で
    if (!manual) dialog.pszVerificationText = L"この版をスキップ";
    int pressed = 0;
    BOOL skip = FALSE;
    if (FAILED(TaskDialogIndirect(&dialog, &pressed, nullptr, &skip))) return Result::None;
    if (skip) {
        state.skipped = release.tag;
        iroha::Update::SaveState(statePath, state);
    }
    if (pressed == kButtonNotes) {
        ShellExecuteW(owner, L"open", Wide(release.page).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return Result::None;
    }
    if (pressed != kButtonUpdate) return Result::None;

    const std::filesystem::path path = WorkDirectory() / Wide(installer->name);
    std::wstring error;
    if (!DownloadInstaller(owner, *installer, path, &error)) {
        if (!error.empty()) ShowInfo(owner, L"アップデートに失敗しました", error, TD_ERROR_ICON);
        return Result::None;
    }
    // インストーラーは自分で管理者の許可を求める（ここで昇格させると、終わったあとに変換サーバを普段のユーザで
    // 起動し直せない）。/SILENT は進み具合だけを出す
    SHELLEXECUTEINFOW execute = {sizeof(execute)};
    execute.fMask = SEE_MASK_NOASYNC;
    execute.hwnd = owner;
    execute.lpVerb = L"open";
    execute.lpFile = path.c_str();
    execute.lpParameters = L"/SILENT /SUPPRESSMSGBOXES /NORESTART";
    execute.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&execute)) {
        ShowInfo(owner, L"アップデートに失敗しました", L"インストーラーを起動できませんでした。", TD_ERROR_ICON);
        return Result::None;
    }
    return Result::Launched;
}

} // namespace updater
