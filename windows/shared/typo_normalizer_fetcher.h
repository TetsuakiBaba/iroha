#pragma once
// 打ち間違い訂正モデルの取得・照合・設置（Windows 専用。設定画面と iroha-cli が使う）。
// 移植元: macos/Sources/IrohaCore/TypoNormalizer/TypoNormalizerFetcher.swift
//
// 壊れたモデルを置かないため、データフォルダの中の一時フォルダへ 2 つのファイルを落として大きさと SHA-256 を
// 照合してから、設置先のフォルダと入れ替える（途中で失敗しても入っているものは壊れない）

#include <filesystem>
#include <string>

#include "iroha/typo_normalizer_catalog.h"

namespace iroha {

class TypoNormalizerFetcher {
public:
    // モデルの一覧を取りにいく（キャッシュを使わない）
    static bool FetchCatalog(const std::string& url, TypoNormalizerCatalog* catalog, std::string* error);

    // モデルを取得して設置する（入っていれば置き換える）。license は表示するライセンス（カタログの既定を補ったもの）
    static bool Install(const TypoNormalizerCatalog::Model& model, const std::u32string& license,
                        const std::filesystem::path& dataDirectory, TypoNormalizerInstall::Record* record,
                        std::string* error);
};

} // namespace iroha
