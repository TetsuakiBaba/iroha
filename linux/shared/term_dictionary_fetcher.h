#pragma once
// 専門辞書の取得・照合・設置（設定画面と iroha-cli が使う）。
// 移植元: macos/Sources/IrohaCore/TermDictionary/TermDictionaryFetcher.swift
//
// 壊れた辞書を置かないため、同じフォルダの一時ファイルへ落として大きさと SHA-256 を照合してから
// 設置先へ移す。取得は curl（無ければ wget）のコマンドを引数の配列で起動して行う
// （https と file:// の両方を扱える。libcurl の開発用ファイルが入っていない環境でもビルドできるように）

#include <filesystem>
#include <functional>
#include <string>

#include "iroha/term_dictionary.h"

namespace iroha {

class TermDictionaryFetcher {
public:
    // 辞書の一覧を取りにいく（キャッシュを使わない）
    static bool FetchCatalog(const std::string& url, TermDictionaryCatalog* catalog,
                             std::string* error);

    // 辞書を取得して設置する（入っていれば置き換える）
    static bool Install(const TermDictionaryCatalog::Dictionary& dictionary,
                        const std::filesystem::path& dataDirectory,
                        TermDictionaryInstall::Record* record, std::string* error);

    // url を path に落とす（キャッシュを使わない。打ち間違い訂正のモデルの取得も使う）
    static bool Download(const std::string& url, const std::filesystem::path& path, std::string* error);
    // ファイルの SHA-256（16進小文字）。読めなければ空
    static std::string Sha256Hex(const std::filesystem::path& path);
    // 今の時刻（UTC、ISO 8601。例 2026-11-13T10:00:00Z）
    static std::string NowIso8601();
};

} // namespace iroha
