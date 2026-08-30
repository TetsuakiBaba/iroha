import Cocoa
import Carbon
import Security

/// GitHub Releasesを利用した軽量アップデータ。
/// 手動（メニュー）と自動（1日1回、activateServer時）で最新版を確認し、
/// 新版があればダウンロード→署名検証→入れ替え→再起動する。
final class UpdateChecker {
    static let shared = UpdateChecker()
    private init() {}

    /// 署名検証で要求するTeam ID（リリース署名証明書と一致させること）
    private static let expectedTeamID = "ZE8M4T49DP"
    private static let defaultAPIURL = "https://api.github.com/repos/TetsuakiBaba/iroha/releases/latest"

    private static let autoCheckKey = "autoUpdateCheck"
    private static let lastCheckKey = "lastUpdateCheckDate"
    private static let skippedVersionKey = "skippedVersion"
    private static let lastNotifiedVersionKey = "lastNotifiedVersion"
    /// テスト用: 確認先APIのURLを上書き（プレリリースでのQA用）
    private static let checkURLOverrideKey = "updateCheckURL"

    private var isWorking = false

    struct Release: Decodable {
        let tag_name: String
        let html_url: String
        let body: String?
        let assets: [Asset]
        struct Asset: Decodable {
            let name: String
            let browser_download_url: String
        }
    }

    static var currentVersion: String {
        Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "0"
    }

    // MARK: - チェック

    /// 自動チェック（24時間に1回まで。新版があればアラート、最新なら何もしない）
    func autoCheckIfDue() async {
        let defaults = UserDefaults.standard
        guard defaults.object(forKey: Self.autoCheckKey) as? Bool ?? true else { return }
        if let last = defaults.object(forKey: Self.lastCheckKey) as? Date,
           Date().timeIntervalSince(last) < 86_400 {
            return
        }
        defaults.set(Date(), forKey: Self.lastCheckKey)
        guard let release = await fetchLatestRelease() else { return }
        guard Self.isNewer(release.tag_name, than: Self.currentVersion) else { return }
        // 同じバージョンを何度も通知しない（スキップ指定・通知済み）
        if release.tag_name == defaults.string(forKey: Self.skippedVersionKey) { return }
        if release.tag_name == defaults.string(forKey: Self.lastNotifiedVersionKey) { return }
        defaults.set(release.tag_name, forKey: Self.lastNotifiedVersionKey)
        await presentUpdateAlert(release: release, manual: false)
    }

    /// 手動チェック（結果を必ずアラートで表示）
    func checkAndPresent() async {
        UserDefaults.standard.set(Date(), forKey: Self.lastCheckKey)
        guard let release = await fetchLatestRelease() else {
            await Self.showInfoAlert(
                message: "アップデートを確認できませんでした",
                informative: "ネットワーク接続を確認してください。")
            return
        }
        if Self.isNewer(release.tag_name, than: Self.currentVersion) {
            await presentUpdateAlert(release: release, manual: true)
        } else {
            await Self.showInfoAlert(
                message: "irohaは最新です",
                informative: "バージョン \(Self.currentVersion)")
        }
    }

    private func fetchLatestRelease() async -> Release? {
        let urlString = UserDefaults.standard.string(forKey: Self.checkURLOverrideKey)
            ?? Self.defaultAPIURL
        guard let url = URL(string: urlString) else { return nil }
        var request = URLRequest(url: url)
        request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        do {
            let (data, response) = try await URLSession.shared.data(for: request)
            guard (response as? HTTPURLResponse)?.statusCode == 200 else { return nil }
            return try JSONDecoder().decode(Release.self, from: data)
        } catch {
            NSLog("iroha: アップデート確認エラー: \(error)")
            return nil
        }
    }

    /// セマンティックバージョン比較（"v"接頭辞は無視、プレリリース識別子も比較する:
    /// 0.4.0-beta.1 < 0.4.0-beta.2 < 0.4.0）
    static func isNewer(_ remote: String, than local: String) -> Bool {
        func parse(_ version: String) -> (core: [Int], pre: [Substring]) {
            let trimmed = version.trimmingCharacters(in: CharacterSet(charactersIn: "vV"))
            let parts = trimmed.split(separator: "-", maxSplits: 1)
            let core = parts[0].split(separator: ".").map { Int($0) ?? 0 }
            let pre = parts.count > 1 ? parts[1].split(separator: ".") : []
            return (core, pre)
        }
        let r = parse(remote), l = parse(local)
        for i in 0..<max(r.core.count, l.core.count) {
            let rv = i < r.core.count ? r.core[i] : 0
            let lv = i < l.core.count ? l.core[i] : 0
            if rv != lv { return rv > lv }
        }
        // コアが同一: 正式版（プレリリース無し）はプレリリースより新しい
        if r.pre.isEmpty != l.pre.isEmpty { return r.pre.isEmpty }
        // 両方プレリリース: 識別子を順に比較（数値同士は数値として）
        for i in 0..<max(r.pre.count, l.pre.count) {
            if i >= r.pre.count { return false }  // remoteの識別子が少ない → 古い
            if i >= l.pre.count { return true }
            let rp = r.pre[i], lp = l.pre[i]
            if let ri = Int(rp), let li = Int(lp) {
                if ri != li { return ri > li }
            } else if rp != lp {
                return rp > lp
            }
        }
        return false
    }

    // MARK: - アラート

    @MainActor
    private func presentUpdateAlert(release: Release, manual: Bool) async {
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        let version = release.tag_name.trimmingCharacters(in: CharacterSet(charactersIn: "vV"))
        alert.messageText = "iroha \(version) が利用可能です"
        var informative = "現在のバージョン: \(Self.currentVersion)"
        if let body = release.body, !body.isEmpty {
            informative += "\n\n" + String(body.prefix(600))
        }
        alert.informativeText = informative
        alert.addButton(withTitle: "アップデート")
        alert.addButton(withTitle: "リリースノート")
        alert.addButton(withTitle: "後で")
        if !manual {
            alert.showsSuppressionButton = true
            alert.suppressionButton?.title = "このバージョンをスキップ"
        }
        alert.window.level = .modalPanel  // LSBackgroundOnlyでも前面に出す
        let response = alert.runModal()
        if alert.suppressionButton?.state == .on {
            UserDefaults.standard.set(release.tag_name, forKey: Self.skippedVersionKey)
        }
        switch response {
        case .alertFirstButtonReturn:
            await downloadAndInstall(release: release)
        case .alertSecondButtonReturn:
            if let url = URL(string: release.html_url) {
                NSWorkspace.shared.open(url)
            }
        default:
            break
        }
    }

    @MainActor
    private static func showInfoAlert(message: String, informative: String) {
        NSApp.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = message
        alert.informativeText = informative
        alert.window.level = .modalPanel
        alert.runModal()
    }

    // MARK: - ダウンロードとインストール

    private func downloadAndInstall(release: Release) async {
        guard !isWorking else { return }
        isWorking = true
        defer { isWorking = false }

        guard let asset = release.assets.first(where: {
            $0.name.hasPrefix("iroha-") && $0.name.hasSuffix(".zip")
        }), let assetURL = URL(string: asset.browser_download_url) else {
            await Self.showInfoAlert(
                message: "アップデートに失敗しました",
                informative: "リリースに配布ファイルが見つかりません。")
            return
        }

        do {
            let (zipURL, _) = try await URLSession.shared.download(from: assetURL)
            let workDir = FileManager.default.temporaryDirectory
                .appendingPathComponent("iroha-update-\(UUID().uuidString)")
            try FileManager.default.createDirectory(at: workDir, withIntermediateDirectories: true)
            defer { try? FileManager.default.removeItem(at: workDir) }

            try Self.runCommand("/usr/bin/ditto", ["-xk", zipURL.path, workDir.path])
            let newApp = workDir.appendingPathComponent("iroha.app")
            guard FileManager.default.fileExists(atPath: newApp.path) else {
                throw UpdateError.appNotFoundInArchive
            }

            // 署名検証: Developer ID(Apple anchor) + Team ID一致 + Gatekeeper受理を確認してから入れ替える
            try Self.verifySignature(of: newApp)

            let installedURL = SelfInstaller.installedURL
            // 実行中バンドルの置換はinstall.shと同じ手順で安全（プロセスはマップ済みバイナリを保持する）
            if FileManager.default.fileExists(atPath: installedURL.path) {
                try FileManager.default.removeItem(at: installedURL)
            }
            try FileManager.default.copyItem(at: newApp, to: installedURL)
            TISRegisterInputSource(installedURL as CFURL)

            // 次の入力時にシステムが新バージョンを起動する（SettingsViewの再起動ボタンと同じ手法）
            UserDefaults.standard.synchronize()
            _exit(0)
        } catch {
            NSLog("iroha: アップデートエラー: \(error)")
            await Self.showInfoAlert(
                message: "アップデートに失敗しました",
                informative: error.localizedDescription)
        }
    }

    enum UpdateError: LocalizedError {
        case appNotFoundInArchive
        case signatureVerificationFailed(String)

        var errorDescription: String? {
            switch self {
            case .appNotFoundInArchive:
                return "アーカイブにiroha.appが含まれていません。"
            case .signatureVerificationFailed(let detail):
                return "署名の検証に失敗しました（\(detail)）。安全のためアップデートを中止しました。"
            }
        }
    }

    /// ダウンロードしたバンドルの署名検証。改竄・すり替え対策として
    /// Developer ID証明書のTeam ID一致とGatekeeper（公証）受理を要求する
    private static func verifySignature(of appURL: URL) throws {
        var staticCode: SecStaticCode?
        guard SecStaticCodeCreateWithPath(appURL as CFURL, [], &staticCode) == errSecSuccess,
              let code = staticCode else {
            throw UpdateError.signatureVerificationFailed("コード情報を取得できません")
        }
        var requirement: SecRequirement?
        let requirementString =
            "anchor apple generic and certificate leaf[subject.OU] = \"\(expectedTeamID)\"" as CFString
        guard SecRequirementCreateWithString(requirementString, [], &requirement) == errSecSuccess,
              let req = requirement else {
            throw UpdateError.signatureVerificationFailed("検証要件を構築できません")
        }
        let status = SecStaticCodeCheckValidity(code, SecCSFlags(rawValue: kSecCSCheckAllArchitectures), req)
        guard status == errSecSuccess else {
            throw UpdateError.signatureVerificationFailed("Team ID不一致または署名破損 (\(status))")
        }
        // 公証の確認（Gatekeeper評価）
        do {
            try runCommand("/usr/sbin/spctl", ["--assess", "--type", "execute", appURL.path])
        } catch {
            throw UpdateError.signatureVerificationFailed("Gatekeeper評価に失敗")
        }
    }

    private static func runCommand(_ path: String, _ arguments: [String]) throws {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: path)
        process.arguments = arguments
        try process.run()
        process.waitUntilExit()
        guard process.terminationStatus == 0 else {
            throw NSError(domain: "iroha.update", code: Int(process.terminationStatus),
                          userInfo: [NSLocalizedDescriptionKey: "\(path) failed (\(process.terminationStatus))"])
        }
    }
}
