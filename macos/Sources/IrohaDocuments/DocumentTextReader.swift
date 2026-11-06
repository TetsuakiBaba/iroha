import AppKit
import Foundation
import PDFKit

/// 文書ファイルから文字列を取り出す（macOS の機能を使う。IrohaCore には置かない）。
///
/// - PDF: PDFKit
/// - Word（.docx・.doc）・リッチテキスト（.rtf・.rtfd）・HTML・OpenDocument（.odt）: `NSAttributedString`
/// - テキスト（.txt・.md・.tex など）: UTF-8 のまま読む
///
/// 取り出した文字列は組版の改行・空白を含むので、使う側で `DocumentText.normalize` を通す
public enum DocumentTextReader {

    public enum ReadError: Error, CustomStringConvertible {
        case unsupported(String)
        case unreadable(String)

        public var description: String {
            switch self {
            case .unsupported(let name): return "対応していない形式です: \(name)"
            case .unreadable(let name): return "読めませんでした: \(name)"
            }
        }
    }

    public static let pdfExtensions: Set<String> = ["pdf"]
    public static let attributedExtensions: Set<String> = ["docx", "doc", "rtf", "rtfd", "html", "htm", "odt"]
    public static let plainExtensions: Set<String> = ["txt", "md", "markdown", "tex", "text"]

    public static var supportedExtensions: Set<String> {
        pdfExtensions.union(attributedExtensions).union(plainExtensions)
    }

    public static func isSupported(_ url: URL) -> Bool {
        supportedExtensions.contains(url.pathExtension.lowercased())
    }

    public static func text(of url: URL) throws -> String {
        let ext = url.pathExtension.lowercased()
        let name = url.lastPathComponent
        if pdfExtensions.contains(ext) {
            guard let document = PDFDocument(url: url), let text = document.string else {
                throw ReadError.unreadable(name)
            }
            return text
        }
        if attributedExtensions.contains(ext) {
            guard let attributed = try? NSAttributedString(url: url, options: [:], documentAttributes: nil) else {
                throw ReadError.unreadable(name)
            }
            return attributed.string
        }
        if plainExtensions.contains(ext) {
            guard let text = try? String(contentsOf: url, encoding: .utf8) else { throw ReadError.unreadable(name) }
            return text
        }
        throw ReadError.unsupported(name)
    }
}
