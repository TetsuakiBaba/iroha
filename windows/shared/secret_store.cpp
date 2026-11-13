#include "secret_store.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <wincred.h>

#include "iroha/unicode.h"

#pragma comment(lib, "advapi32.lib")

namespace iroha {
namespace SecretStore {

namespace {
std::wstring Target(const std::string& account) { return L"iroha/" + Utf32ToUtf16(Utf8ToUtf32(account)); }
} // namespace

bool Get(const std::string& account, std::string* value) {
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(Target(account).c_str(), CRED_TYPE_GENERIC, 0, &credential)) return false;
    value->assign(reinterpret_cast<const char*>(credential->CredentialBlob), credential->CredentialBlobSize);
    CredFree(credential);
    return true;
}

bool Set(const std::string& account, const std::string& value) {
    const std::wstring target = Target(account);
    if (value.empty()) {
        return CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0) || GetLastError() == ERROR_NOT_FOUND;
    }
    CREDENTIALW credential = {};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<wchar_t*>(target.c_str());
    credential.CredentialBlobSize = static_cast<DWORD>(value.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(value.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = const_cast<wchar_t*>(L"iroha");
    return CredWriteW(&credential, 0) != FALSE;
}

} // namespace SecretStore
} // namespace iroha
