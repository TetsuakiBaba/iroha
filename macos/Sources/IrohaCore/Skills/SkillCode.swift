import Foundation

/// スキルのコードの作り方と照合（2026-11-11。v1.0.0 から使う）。
///
/// 青空写経（Web ページ）で 1 作品を書き上げるたびにコードが出て、iroha に入れるとスキル（機能）を 1 つ解除できる
/// （`SkillState`）。目的はユーザが進み具合を楽しむことで、厳密に制限することではない。iroha はソースコードを
/// 公開しているので、ここの計算は誰でも読める。照合はアプリの中だけで行い、サーバは使わない。
///
/// 形（青空写経の JavaScript と同じ計算にする。変えるときは両方を合わせ、`SkillCodeTests` の値も更新する）:
/// - 文字は数字と英大文字 32 種（`alphabet`。見間違えやすい I・L・O・U を除く）
/// - iroha の ID: 8 文字（iroha が 1 回作る。表示は `7QF2-MX8K`）
/// - 作品の番号: 2 文字（青空写経が作品ごとに決める）
/// - コード: 作品の番号 2 文字 + 照合用 6 文字（表示は `03-K9T4WB`）
/// - 照合用 6 文字: HMAC-SHA256(鍵 = `key` の UTF-8, 本文 = "<ID>:<作品の番号>" の UTF-8) の先頭 30 ビットを
///   上位から 5 ビットずつ `alphabet` の文字にしたもの
///
/// コードに作品の番号が入っているので、iroha は作品の一覧を持たない（青空写経に作品を足しても iroha の更新は要らない）
public enum SkillCode {

    public static let alphabet = Array("0123456789ABCDEFGHJKMNPQRSTVWXYZ")
    public static let idLength = 8
    public static let workLength = 2
    public static let checkLength = 6
    public static var codeLength: Int { workLength + checkLength }

    /// 照合用の計算に使う固定の鍵。変えると発行済みのコードがすべて通らなくなる
    static let key = "iroha-skill-code-v1"

    private static let values: [Character: Int] = Dictionary(
        uniqueKeysWithValues: alphabet.enumerated().map { ($1, $0) })

    /// 入力された ID・コードを照合用の形にそろえる: 全角を半角に、小文字を大文字に、空白とハイフンを除き、
    /// 見間違えやすい文字を数字に寄せる（I・L → 1、O → 0）。使えない文字が残ったら nil
    public static func normalize(_ text: String) -> String? {
        let halfWidth = text.applyingTransform(.fullwidthToHalfwidth, reverse: false) ?? text
        var result = ""
        for character in halfWidth.uppercased() {
            switch character {
            case " ", "\t", "\n", "-", "‐", "‑", "–", "—", "ー", "－": continue
            case "I", "L": result.append("1")
            case "O": result.append("0")
            default:
                guard values[character] != nil else { return nil }
                result.append(character)
            }
        }
        return result
    }

    public static func isValidID(_ id: String) -> Bool {
        id.count == idLength && id.allSatisfy { values[$0] != nil }
    }

    public static func isValidWork(_ work: String) -> Bool {
        work.count == workLength && work.allSatisfy { values[$0] != nil }
    }

    /// 新しい ID（乱数 40 ビット）
    public static func newID() -> String {
        String((0..<idLength).map { _ in alphabet[Int.random(in: 0..<alphabet.count)] })
    }

    /// ID の表示（4 文字ずつハイフンで区切る）
    public static func formatID(_ id: String) -> String {
        guard id.count == idLength else { return id }
        return "\(id.prefix(4))-\(id.suffix(4))"
    }

    /// コードの表示（作品の番号と照合用の間をハイフンで区切る）
    public static func format(_ code: String) -> String {
        guard code.count == codeLength else { return code }
        return "\(code.prefix(workLength))-\(code.suffix(checkLength))"
    }

    /// その ID・作品のコード（照合用の形。8 文字）。ID か作品の番号が形に合わなければ nil
    public static func code(id: String, work: String) -> String? {
        guard isValidID(id), isValidWork(work) else { return nil }
        let mac = hmacSHA256(key: Array(key.utf8), message: Array("\(id):\(work)".utf8))
        // 先頭 30 ビットを 5 ビットずつ取り出す
        let bits = mac.prefix(4).reduce(UInt32(0)) { $0 << 8 | UInt32($1) } >> 2
        let check = (0..<checkLength).map { index in
            alphabet[Int(bits >> UInt32(5 * (checkLength - 1 - index)) & 0x1F)]
        }
        return work + String(check)
    }

    /// 入力されたコードをその ID で照合する。合っていれば作品の番号を返す
    public static func work(ofCode text: String, id: String) -> String? {
        guard let normalized = normalize(text), normalized.count == codeLength else { return nil }
        let work = String(normalized.prefix(workLength))
        return code(id: id, work: work) == normalized ? work : nil
    }

    /// HMAC-SHA256（RFC 2104）。IrohaCore は Foundation だけに依存するので、自前の `SHA256` の上に組む
    static func hmacSHA256(key: [UInt8], message: [UInt8]) -> [UInt8] {
        let blockSize = 64
        var block = key.count > blockSize ? SHA256.digest(Data(key)) : key
        block += [UInt8](repeating: 0, count: blockSize - block.count)
        let inner = SHA256.digest(Data(block.map { $0 ^ 0x36 } + message))
        return SHA256.digest(Data(block.map { $0 ^ 0x5C } + inner))
    }
}
