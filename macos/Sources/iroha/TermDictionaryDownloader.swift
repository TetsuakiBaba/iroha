import Foundation
import IrohaCore

/// 専門辞書の一覧・取得・削除の状況を設定画面へ流す薄い層。
///
/// 取得・照合・設置は `TermDictionaryFetcher`（IrohaCore）、読み込みは `TermDictionaryStore`。
/// 通信するのは設定画面の専門辞書の欄を開いたとき（一覧の取得）と、ユーザが「追加」を押したときだけ
@MainActor
final class TermDictionaryDownloader: ObservableObject {

    static let shared = TermDictionaryDownloader()

    /// 1つの辞書の取得状況
    enum Progress: Equatable {
        case downloading(Double)   // 0.0-1.0
        case failed(String)
    }

    enum CatalogState: Equatable {
        case idle
        case loading
        case failed(String)
    }

    @Published private(set) var catalog: TermDictionaryCatalog?
    @Published private(set) var catalogState: CatalogState = .idle
    /// 入れている辞書（ID → 素性）
    @Published private(set) var installed: [String: TermDictionaryInstall.Record] = [:]
    /// 取得中・失敗した辞書（ID → 状況）
    @Published private(set) var progress: [String: Progress] = [:]

    private var tasks: [String: Task<Void, Never>] = [:]
    private var observer: NSObjectProtocol?

    private init() {
        reloadInstalled()
        // 他のMacで入れ外しした辞書が同期で届いたときにも表示を合わせる
        observer = NotificationCenter.default.addObserver(
            forName: TermDictionaryStore.didChangeNotification, object: nil, queue: .main
        ) { [weak self] _ in
            MainActor.assumeIsolated { self?.reloadInstalled() }
        }
    }

    func isBusy(_ id: String) -> Bool {
        if case .downloading = progress[id] { return true }
        return false
    }

    /// 設定画面を開いたときに一覧を取りにいく。作り直された辞書を入れていれば入れ直す
    /// （JMdict のライセンスは配る版を新しく保つことを求めるので、入れている側も追随させる）
    func refreshCatalog() {
        guard catalogState != .loading else { return }
        catalogState = .loading
        Task { [weak self] in
            do {
                let catalog = try await TermDictionaryFetcher.fetchCatalog()
                guard let self else { return }
                self.catalog = catalog
                self.catalogState = .idle
                self.updateOutdated()
            } catch {
                self?.catalogState = .failed(Self.message(for: error))
            }
        }
    }

    /// 入れている辞書のうち、カタログで作り直されたものを入れ直す
    private func updateOutdated() {
        guard let catalog else { return }
        for record in installed.values {
            guard let dictionary = catalog.dictionary(id: record.id),
                  TermDictionaryInstall.needsUpdate(record, to: dictionary)
            else { continue }
            install(dictionary)
        }
    }

    func install(_ dictionary: TermDictionaryCatalog.Dictionary) {
        guard !isBusy(dictionary.id) else { return }
        progress[dictionary.id] = .downloading(0)
        let id = dictionary.id
        let report: @Sendable (Double) -> Void = { [weak self] value in
            Task { @MainActor in
                guard let self, case .downloading = self.progress[id] else { return }
                self.progress[id] = .downloading(value)
            }
        }
        tasks[id] = Task { [weak self] in
            do {
                try await TermDictionaryFetcher.install(dictionary, onProgress: report)
                guard let self else { return }
                self.progress[id] = nil
                self.tasks[id] = nil
                self.reloadStore()
            } catch is CancellationError {
                self?.progress[id] = nil
                self?.tasks[id] = nil
            } catch {
                self?.progress[id] = .failed(Self.message(for: error))
                self?.tasks[id] = nil
            }
        }
    }

    func remove(id: String) {
        tasks[id]?.cancel()
        tasks[id] = nil
        do {
            try TermDictionaryInstall.remove(id: id)
            progress[id] = nil
        } catch {
            progress[id] = .failed("削除できませんでした: \(error.localizedDescription)")
        }
        reloadStore()
    }

    /// 入れている辞書のライセンス一覧（設定 > 情報）に出す出典。同じ出典は1行にまとめる
    var installedSources: [(source: TermDictionaryCatalog.Source, dictionaries: [String])] {
        var order: [TermDictionaryCatalog.Source] = []
        var names: [TermDictionaryCatalog.Source: [String]] = [:]
        for record in installed.values.sorted(by: { $0.id < $1.id }) {
            for source in record.sources {
                if names[source] == nil { order.append(source) }
                names[source, default: []].append(record.name)
            }
        }
        return order.map { ($0, names[$0] ?? []) }
    }

    /// 入れ外しを反映する: 表示はすぐ、変換に使う索引は裏で作り直す
    private func reloadStore() {
        reloadInstalled()
        TermDictionaryStore.shared.reloadInBackground()
    }

    private func reloadInstalled() {
        installed = Dictionary(
            TermDictionaryInstall.installedRecords().map { ($0.id, $0) }, uniquingKeysWith: { a, _ in a })
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
