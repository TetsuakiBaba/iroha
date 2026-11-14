#pragma once
// 評価セットの取得・照合・設置（設定画面と iroha-cli が使う）。
// 移植元: macos/Sources/IrohaCore/Benchmark/BenchmarkCatalog.swift の BenchmarkFetcher（Windows版 windows/shared/benchmark_fetcher）
//
// 専門辞書（TermDictionaryFetcher）と同じ手順: 同じフォルダの一時ファイルへ落として大きさと SHA-256 を照合してから
// 設置先へ移す。取得は libcurl（https と file:// の両方を扱える）

#include <filesystem>
#include <string>

#include "iroha/benchmark_catalog.h"

namespace iroha {

class BenchmarkFetcher {
public:
    // 評価セットの一覧を取りにいく（キャッシュを使わない）
    static bool FetchCatalog(const std::string& url, BenchmarkCatalog* catalog, std::string* error);

    // 評価セットを取得して設置する（入っていれば置き換える）
    static bool Install(const BenchmarkCatalog::Entry& entry, const std::filesystem::path& dataDirectory,
                        BenchmarkInstall::Record* record, std::string* error);
};

} // namespace iroha
