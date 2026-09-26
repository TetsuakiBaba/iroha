import Foundation
import IrohaCore

/// 打ち間違い訂正モデルの取得状況を設定画面へ流す薄い層。
///
/// 実際の取得・検証・設置は `TypoNormalizerFetcher`（IrohaCore）が行う。
/// zenz の `ModelDownloader` と違い、**ユーザが明示的にONにしたときだけ**動く（勝手に通信しない）
@MainActor
final class TypoNormalizerDownloader: ObservableObject {

    static let shared = TypoNormalizerDownloader()

    enum State: Equatable {
        case idle
        case loadingCatalog
        case downloading(progress: Double)   // 0.0-1.0
        case verifying
        case failed(String)
    }

    @Published private(set) var state: State = .idle
    /// 取得できたモデル一覧（未取得なら nil）
    @Published private(set) var catalog: TypoNormalizerCatalog?
    /// 今入っているモデル
    @Published private(set) var installed: TypoNormalizerInstall.Record?

    private var task: Task<Void, Never>?

    private init() {
        installed = TypoNormalizerInstall.installedRecord()
    }

    var isBusy: Bool {
        switch state {
        case .idle, .failed: return false
        case .loadingCatalog, .downloading, .verifying: return true
        }
    }

    /// まだ入れていない人に出す説明。大きさもライセンスもカタログから取る
    /// （学習元コーパスで条件が変わるので、アプリ側に文言を焼き込まない）
    var offerDescription: String {
        guard let catalog, let model = catalog.models.first else {
            return "このモデルはアプリに含まれていません。有効にすると1回だけダウンロードします。"
        }
        let megabytes = Double(model.totalBytes) / 1_000_000
        var text = String(
            format: "このモデルはアプリに含まれていません。有効にすると約%.0fMBを1回だけダウンロードします",
            megabytes)
        if let license = catalog.license(for: model) {
            text += "（\(license)"
            if let attribution = catalog.attribution(for: model) {
                text += "、学習元: \(attribution)"
            }
            text += "）"
        }
        text += "。保存先はデータの保存場所の中なので、共有フォルダにしていれば1回で済みます。"
        return text
    }

    /// 入れているモデルの説明（学習元・ライセンス）。カタログに同じ ID があるときだけ出す
    /// （設置記録には ID と名前しか残していないので、条件はカタログから取る）
    var installedDescription: String? {
        guard let installed, let catalog else { return nil }
        guard let model = catalog.models.first(where: { $0.id == installed.id }) else {
            // カタログから外れた古いモデル（small-v1 など）。自動では入れ替えないので、やり方を示す
            guard let latest = catalog.models.first else { return nil }
            return "カタログには新しいモデル（\(latest.id)）が載っています。"
                + "削除してからダウンロードすると入れ替わります。"
        }
        var parts: [String] = []
        if let attribution = catalog.attribution(for: model) { parts.append("学習元: \(attribution)") }
        if let license = catalog.license(for: model) { parts.append("モデルのライセンス: \(license)") }
        return parts.isEmpty ? nil : parts.joined(separator: "。") + "。"
    }

    /// 設定画面を開いたときにモデル一覧を取りにいく。
    ///
    /// 一覧が要るのは「まだ入れていない」「入れ替える」ときだけなので、**既に入っているなら
    /// 取得に失敗しても黙って諦める**（オフラインで設定を開いただけで赤い字を出さない）
    func refreshCatalogIfNeeded() {
        guard catalog == nil, !isBusy else { return }
        task = Task { [weak self] in
            guard let self else { return }
            self.state = .loadingCatalog
            do {
                self.catalog = try await TypoNormalizerFetcher.fetchCatalog()
                self.state = .idle
            } catch {
                if self.installed == nil {
                    self.state = .failed(Self.message(for: error))
                } else {
                    NSLog("iroha: 訂正モデルの一覧を取得できませんでした: \(error)")
                    self.state = .idle
                }
            }
        }
    }

    /// モデルを取得して設置する。`model` を省略するとカタログの先頭（推奨モデル）
    func install(_ model: TypoNormalizerCatalog.Model? = nil) {
        guard !isBusy else { return }
        task = Task { [weak self] in
            guard let self else { return }
            do {
                let catalog: TypoNormalizerCatalog
                if let existing = self.catalog {
                    catalog = existing
                } else {
                    self.state = .loadingCatalog
                    catalog = try await TypoNormalizerFetcher.fetchCatalog()
                    self.catalog = catalog
                }
                guard let target = model ?? catalog.models.first else {
                    throw TypoNormalizerFetcher.FetchError.noModels
                }
                self.state = .downloading(progress: 0)
                let record = try await TypoNormalizerFetcher.install(target) { progress in
                    Task { @MainActor [weak self] in
                        guard let self, case .downloading = self.state else { return }
                        self.state = progress >= 1 ? .verifying : .downloading(progress: progress)
                    }
                }
                self.installed = record
                // 置き場所が変わったので次の利用で調べ直させる（再起動なしで効かせる）
                IrohaInputController.invalidateTypoNormalizer()
                self.state = .idle
            } catch is CancellationError {
                self.state = .idle
            } catch {
                self.state = .failed(Self.message(for: error))
            }
        }
    }

    func cancel() {
        task?.cancel()
        task = nil
        state = .idle
    }

    /// 入れたモデルを消す
    func remove() {
        cancel()
        do {
            try TypoNormalizerInstall.remove()
            installed = nil
            IrohaInputController.invalidateTypoNormalizer()
            state = .idle
        } catch {
            state = .failed("削除できませんでした: \(error.localizedDescription)")
        }
    }

    private static func message(for error: any Error) -> String {
        if let described = (error as? LocalizedError)?.errorDescription { return described }
        let nsError = error as NSError
        if nsError.domain == NSURLErrorDomain {
            return "ネットワークに接続できませんでした（\(nsError.localizedDescription)）"
        }
        return nsError.localizedDescription
    }
}
