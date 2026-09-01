#include "Globals.h"

#include <string>

namespace {

std::wstring ModulePath() {
    wchar_t path[MAX_PATH];
    const DWORD len = GetModuleFileNameW(g_hInst, path, ARRAYSIZE(path));
    return std::wstring(path, len);
}

std::wstring ClsidKeyPath() {
    wchar_t clsidStr[64];
    StringFromGUID2(CLSID_IROHA_TIP, clsidStr, ARRAYSIZE(clsidStr));
    return std::wstring(L"CLSID\\") + clsidStr;
}

HRESULT SetKeyValue(HKEY key, const wchar_t* name, const std::wstring& value) {
    const LONG r = RegSetValueExW(
        key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    return HRESULT_FROM_WIN32(r);
}

constexpr wchar_t kDescription[] = L"iroha";

// TIPの対応機能の宣言。ストアアプリ（AppContainer）対応のIMMERSIVESUPPORTを
// 含め、後のマイルストーンで必要になるものまでM1で登録しておく。
const GUID* kCategories[] = {
    &GUID_TFCAT_TIP_KEYBOARD,
    &GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER,
    &GUID_TFCAT_TIPCAP_UIELEMENTENABLED,
    &GUID_TFCAT_TIPCAP_SECUREMODE,
    &GUID_TFCAT_TIPCAP_COMLESS,
    &GUID_TFCAT_TIPCAP_INPUTMODECOMPARTMENT,
    &GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT,
    &GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT,
};

HRESULT RegisterComServer() {
    // 管理者のregsvr32経由ならHKCRへの書き込みはHKLM\Software\Classesに入る
    HKEY clsidKey = nullptr;
    LONG r = RegCreateKeyExW(HKEY_CLASSES_ROOT, ClsidKeyPath().c_str(), 0, nullptr,
                             REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &clsidKey,
                             nullptr);
    if (r != ERROR_SUCCESS) return HRESULT_FROM_WIN32(r);

    HRESULT hr = SetKeyValue(clsidKey, nullptr, kDescription);
    HKEY inprocKey = nullptr;
    if (SUCCEEDED(hr)) {
        r = RegCreateKeyExW(clsidKey, L"InProcServer32", 0, nullptr,
                            REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &inprocKey,
                            nullptr);
        hr = HRESULT_FROM_WIN32(r);
    }
    if (SUCCEEDED(hr)) hr = SetKeyValue(inprocKey, nullptr, ModulePath());
    if (SUCCEEDED(hr)) hr = SetKeyValue(inprocKey, L"ThreadingModel", L"Apartment");

    if (inprocKey) RegCloseKey(inprocKey);
    RegCloseKey(clsidKey);
    return hr;
}

void UnregisterComServer() {
    RegDeleteTreeW(HKEY_CLASSES_ROOT, ClsidKeyPath().c_str());
}

HRESULT RegisterProfile() {
    ITfInputProcessorProfileMgr* mgr = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mgr));
    if (FAILED(hr)) return hr;
    // アイコンはこのDLLのリソース1番（言語切替UI・入力インジケーターに出る）
    const std::wstring iconPath = ModulePath();
    hr = mgr->RegisterProfile(CLSID_IROHA_TIP, IROHA_LANGID, GUID_IROHA_PROFILE,
                              kDescription,
                              static_cast<ULONG>(wcslen(kDescription)),
                              iconPath.c_str(),
                              static_cast<ULONG>(iconPath.size()), 0,
                              nullptr, 0, TRUE, 0);
    mgr->Release();
    return hr;
}

void UnregisterProfile() {
    ITfInputProcessorProfileMgr* mgr = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                   CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mgr)))) {
        mgr->UnregisterProfile(CLSID_IROHA_TIP, IROHA_LANGID, GUID_IROHA_PROFILE, 0);
        mgr->Release();
    }
}

HRESULT RegisterCategories() {
    ITfCategoryMgr* mgr = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&mgr));
    if (FAILED(hr)) return hr;
    for (const GUID* category : kCategories) {
        hr = mgr->RegisterCategory(CLSID_IROHA_TIP, *category, CLSID_IROHA_TIP);
        if (FAILED(hr)) break;
    }
    mgr->Release();
    return hr;
}

void UnregisterCategories() {
    ITfCategoryMgr* mgr = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&mgr)))) {
        for (const GUID* category : kCategories) {
            mgr->UnregisterCategory(CLSID_IROHA_TIP, *category, CLSID_IROHA_TIP);
        }
        mgr->Release();
    }
}

// regsvr32はCOM初期化済みで呼ぶが、他経路からの呼び出しに備えて保証する
class ComInit {
public:
    ComInit() { hr_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }
    ~ComInit() {
        if (SUCCEEDED(hr_)) CoUninitialize();
    }

private:
    HRESULT hr_;
};

} // namespace

HRESULT IrohaRegisterServer() {
    ComInit com;
    HRESULT hr = RegisterComServer();
    if (SUCCEEDED(hr)) hr = RegisterProfile();
    if (SUCCEEDED(hr)) hr = RegisterCategories();
    if (FAILED(hr)) IrohaUnregisterServer();
    return hr;
}

void IrohaUnregisterServer() {
    ComInit com;
    UnregisterCategories();
    UnregisterProfile();
    UnregisterComServer();
}
