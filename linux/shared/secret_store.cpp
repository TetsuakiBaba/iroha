#include "secret_store.h"

#include <libsecret/secret.h>

namespace iroha {
namespace SecretStore {

namespace {

// 保管庫の中の項目の形（"account" で引く）
const SecretSchema* Schema() {
    static const SecretSchema schema = {
        "jp.iroha.Secret",
        SECRET_SCHEMA_NONE,
        {
            {"account", SECRET_SCHEMA_ATTRIBUTE_STRING},
            {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING},
        },
        0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    };
    return &schema;
}

} // namespace

bool Get(const std::string& account, std::string* value) {
    GError* error = nullptr;
    gchar* secret = secret_password_lookup_sync(Schema(), nullptr, &error, "account", account.c_str(), nullptr);
    if (error) g_error_free(error);
    if (!secret) return false;
    value->assign(secret);
    secret_password_free(secret);
    return !value->empty();
}

bool Set(const std::string& account, const std::string& value) {
    GError* error = nullptr;
    gboolean ok = FALSE;
    if (value.empty()) {
        secret_password_clear_sync(Schema(), nullptr, &error, "account", account.c_str(), nullptr);
        ok = error == nullptr;
    } else {
        const std::string label = "iroha: " + account;
        ok = secret_password_store_sync(Schema(), SECRET_COLLECTION_DEFAULT, label.c_str(), value.c_str(), nullptr,
                                        &error, "account", account.c_str(), nullptr);
    }
    if (error) g_error_free(error);
    return ok;
}

} // namespace SecretStore
} // namespace iroha
