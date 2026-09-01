#pragma once
#include <windows.h>
#include <msctf.h>

// TIP本体のCLSID {F28F7C00-A045-448E-A73E-FA35EADC161A}
extern const CLSID CLSID_IROHA_TIP;
// 日本語キーボードプロファイルのGUID {A8EDAF76-70D1-4B0D-B963-68696CD3AE8C}
extern const GUID GUID_IROHA_PROFILE;
// 未確定文字列の表示属性（下線）のGUID {1E7A5F7B-D326-4E2A-A4F3-883BDD6DCCDF}
extern const GUID GUID_IROHA_DISPLAY_ATTRIBUTE;
// 選択中の文節の表示属性（太下線）のGUID {AD52BACE-4DBB-462E-9C4A-91F5E4F1822B}
extern const GUID GUID_IROHA_DISPLAY_ATTRIBUTE_CURRENT;

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
