import Foundation

/// 文書のフォルダを、切り替えて使う単位（以下「原稿のフォルダ」）に分ける（2026-11-07）。
///
/// - `documents/` の直下にフォルダがあれば、その 1 つ 1 つが原稿のフォルダになる（`documents/論文/`、
///   `documents/太宰治/`）。フォルダの中は下位フォルダも含めて読む。このとき直下に置いたファイルは使わない
///   （`ignoredFiles` で知らせる。どれを選んでも足す共通の分にすると、点数の算数が 1 段増えるため）
/// - 直下にフォルダが無ければ（対応する文書を含むフォルダが無ければ）、直下全体が 1 つの原稿のフォルダになる
///   （名前は空文字列。フォルダに分ける前と同じ動き）
///
/// 選ぶのは 1 つだけ（`DocumentProfileSelection`）。表記の回数はそのフォルダの文書を合わせて数え、
/// 用語は文書ごとに話題で切り替える
public enum DocumentProfiles {

    public struct Folder: Sendable, Equatable {
        /// フォルダ名。直下全体を使うときは空文字列
        public let name: String
        public let url: URL
    }

    /// 原稿のフォルダの一覧（名前の順）と、使わない直下のファイル（名前の順）
    public static func folders(in root: URL) -> (folders: [Folder], ignoredFiles: [String]) {
        let root = root.standardizedFileURL
        let items = (try? FileManager.default.contentsOfDirectory(
            at: root, includingPropertiesForKeys: [.isDirectoryKey, .isPackageKey],
            options: [.skipsHiddenFiles])) ?? []
        var folders: [Folder] = []
        var topLevelFiles: [String] = []
        for url in items {
            let values = try? url.resourceValues(forKeys: [.isDirectoryKey, .isPackageKey])
            if values?.isDirectory == true, values?.isPackage != true {
                if !DocumentCollection.documentURLs(in: url).isEmpty {
                    folders.append(Folder(name: url.lastPathComponent, url: url.standardizedFileURL))
                }
            } else if DocumentTextReader.isSupported(url) {
                topLevelFiles.append(url.lastPathComponent)
            }
        }
        guard !folders.isEmpty else {
            return (topLevelFiles.isEmpty ? [] : [Folder(name: "", url: root)], [])
        }
        return (folders.sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending },
                topLevelFiles.sorted())
    }
}

/// どの原稿のフォルダを使うか（設定に文字列で持つ）。
///
/// - 保存値が `off`（"/"。フォルダ名に使えない文字）なら使わない
/// - 保存値と同じ名前のフォルダがあればそれを使う
/// - 保存値が無い・そのフォルダが無くなったときは、最初のフォルダを使う
///   （フォルダに分ける前に ON にしていた人は、そのまま直下全体を使い続ける）
public enum DocumentProfileSelection {

    public static let off = "/"

    /// 使うフォルダの名前。使わないとき・フォルダが 1 つも無いときは nil
    public static func resolve(stored: String?, names: [String]) -> String? {
        if stored == off { return nil }
        if let stored, names.contains(stored) { return stored }
        return names.first
    }

    /// 切り替えの次の選択（使わない → 1 つ目 → 2 つ目 → … → 使わない）。保存する値を返す
    public static func next(stored: String?, names: [String]) -> String {
        guard let current = resolve(stored: stored, names: names) else { return names.first ?? off }
        guard let index = names.firstIndex(of: current), index + 1 < names.count else { return off }
        return names[index + 1]
    }
}
