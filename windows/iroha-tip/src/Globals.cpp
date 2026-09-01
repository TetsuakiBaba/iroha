// initguid.h を先頭で取り込むと、このTUに限り以降のヘッダの
// DEFINE_GUID（GUID_TFCAT_* など）が実体定義になる。
#include <initguid.h>
#include "Globals.h"

#include <cstdarg>
#include <cstdio>

const CLSID CLSID_IROHA_TIP = {
    0xF28F7C00, 0xA045, 0x448E, {0xA7, 0x3E, 0xFA, 0x35, 0xEA, 0xDC, 0x16, 0x1A}};
const GUID GUID_IROHA_PROFILE = {
    0xA8EDAF76, 0x70D1, 0x4B0D, {0xB9, 0x63, 0x68, 0x69, 0x6C, 0xD3, 0xAE, 0x8C}};
const GUID GUID_IROHA_DISPLAY_ATTRIBUTE = {
    0x1E7A5F7B, 0xD326, 0x4E2A, {0xA4, 0xF3, 0x88, 0x3B, 0xDD, 0x6D, 0xCC, 0xDF}};

HINSTANCE g_hInst = nullptr;
LONG g_dllRefCount = 0;

void DllAddRef() { InterlockedIncrement(&g_dllRefCount); }
void DllRelease() { InterlockedDecrement(&g_dllRefCount); }

void IrohaLog(const wchar_t* fmt, ...) {
    wchar_t buf[512];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, args);
    va_end(args);
    wchar_t line[560];
    _snwprintf_s(line, _TRUNCATE, L"[iroha-tip] %s\n", buf);
    OutputDebugStringW(line);
}
