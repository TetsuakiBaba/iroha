#pragma once
#include <windows.h>
#include <msctf.h>

// TIP本体のCLSID {F28F7C00-A045-448E-A73E-FA35EADC161A}
extern const CLSID CLSID_IROHA_TIP;
// 日本語キーボードプロファイルのGUID {A8EDAF76-70D1-4B0D-B963-68696CD3AE8C}
extern const GUID GUID_IROHA_PROFILE;

inline constexpr LANGID IROHA_LANGID = 0x0411; // 日本語

extern HINSTANCE g_hInst;
extern LONG g_dllRefCount;

void DllAddRef();
void DllRelease();

HRESULT IrohaRegisterServer();
void IrohaUnregisterServer();

// DebugView / VSの出力ウィンドウで見るログ。
// TIPはホストアプリ内で動くため、ログはOutputDebugStringに集約する。
void IrohaLog(const wchar_t* fmt, ...);
