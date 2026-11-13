#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace iroha {

// 配布している打ち間違い訂正モデルの一覧（models/typo-normalizer-format2.json）。
// 移植元: macos/Sources/IrohaCore/TypoNormalizer/TypoNormalizerCatalog.swift（形式を合わせること）。
// アプリに焼き込むのはカタログの URL だけで、モデルの追加・差し替えはカタログを更新するだけで済ませる。
// 重みは本体コード（MIT）と別ライセンスなので、ライセンスと学習元もカタログに持たせる（アプリに焼き込まない）
struct TypoNormalizerCatalog {
    struct File {
        std::string url;
        uint64_t bytes = 0;
        std::string sha256; // 16進小文字。落としたあと必ず照合する
    };
    // 学習元の 1 件
    struct Source {
        std::u32string name;
        std::u32string holder; // 作成者・著作権者
        std::u32string license;
        std::string url;
    };
    struct Model {
        std::string id;
        std::u32string name;    // 設定画面に出す名前
        std::u32string summary; // 補足（パラメータ数・速度など。無ければ空）
        File manifest;
        File weights;
        std::u32string license;     // 無ければ空（カタログの既定を使う）
        std::u32string attribution; // 無ければ空
        std::vector<Source> sources;
        std::string page; // 配布ページ
        uint64_t TotalBytes() const { return manifest.bytes + weights.bytes; }
    };

    int formatVersion = 0;
    std::vector<Model> models;
    std::u32string license;     // モデル側に無いときの既定
    std::u32string attribution; // 同上

    static constexpr int kSupportedFormatVersion = 2;
    // カタログの場所。環境変数 IROHA_TYPO_CATALOG で差し替えられる（開発・検証用。file:/// も可）
    static std::string DefaultUrl();

    // JSON を読む。読めなければ偽（error に理由）。形式が新しすぎる・モデルが無いときも偽
    static bool Parse(const std::string& json, TypoNormalizerCatalog* catalog, std::string* error);

    const Model* Find(const std::string& id) const;
    std::u32string LicenseFor(const Model& model) const { return model.license.empty() ? license : model.license; }
    std::u32string AttributionFor(const Model& model) const {
        return model.attribution.empty() ? attribution : model.attribution;
    }
};

// 取得したモデルの置き場所（<データフォルダ>/models/typo-normalizer/）と、入っているかどうかの判定。
// installed.json の形は macOS版と同じ（データフォルダを共有したときに互いに読める）
class TypoNormalizerInstall {
public:
    struct Record {
        std::string id;
        std::u32string name;
        std::string sha256; // weights.bin の SHA-256。カタログと違っていれば入れ直す
        std::string installedAt; // ISO 8601
        // 入れた時点のカタログにあったライセンス・学習元・配布ページ（オフラインでも出せるように残す）
        std::u32string license;
        std::vector<TypoNormalizerCatalog::Source> sources;
        std::string page;
    };

    static std::filesystem::path Directory(const std::filesystem::path& dataDirectory);
    static std::filesystem::path RecordPath(const std::filesystem::path& dataDirectory);
    // 使える状態で入っているか（manifest.json と weights.bin が揃っているか）
    static bool IsInstalled(const std::filesystem::path& dataDirectory);
    static std::optional<Record> InstalledRecord(const std::filesystem::path& dataDirectory);
    static bool WriteRecord(const std::filesystem::path& dataDirectory, const Record& record);
    // 入れたモデルを消す
    static bool Remove(const std::filesystem::path& dataDirectory);
    // カタログの中身と入っているものを見比べて、入れ直しが要るか
    static bool NeedsUpdate(const std::filesystem::path& dataDirectory, const TypoNormalizerCatalog::Model& model);

    static std::string EncodeRecord(const Record& record);
    static std::optional<Record> DecodeRecord(const std::string& json);
};

} // namespace iroha
