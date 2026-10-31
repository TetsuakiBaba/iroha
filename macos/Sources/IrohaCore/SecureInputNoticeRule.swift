import Foundation

/// 別のアプリが Secure Input（パスワード入力の保護）を有効にしたままのとき、入力メニューで iroha を選べなくなったと
/// 知らせるかの判定（iroha 側は `SecureInputNotice`。2026-10-31）。
///
/// 知らせるのは、Secure Input が有効で、持っているアプリが前面のアプリでも iroha 自身でもないときだけ。
/// パスワード欄に入力している間は持っているアプリが前面なので知らせない。iroha 自身が持つのは設定画面の API キー欄。
/// 同じアプリが持ち続けている間は 1 回だけ知らせ、Secure Input が解除されるか持っているアプリが変わったら、また知らせる
public struct SecureInputNoticeRule: Sendable {

    /// 前面のアプリが切り替わったあとに確かめた状態
    public struct Observation: Sendable, Equatable {
        public var isSecureInputEnabled: Bool
        /// Secure Input を持っているプロセス（読めなければ nil）
        public var holder: Int32?
        public var frontmost: Int32?
        /// iroha 自身
        public var own: Int32

        public init(isSecureInputEnabled: Bool, holder: Int32?, frontmost: Int32?, own: Int32) {
            self.isSecureInputEnabled = isSecureInputEnabled
            self.holder = holder
            self.frontmost = frontmost
            self.own = own
        }
    }

    /// 知らせ済みの持ち主
    private var notifiedHolder: Int32?

    public init() {}

    /// 知らせるなら true。知らせたことを覚える
    public mutating func shouldNotify(_ observation: Observation) -> Bool {
        guard observation.isSecureInputEnabled else {
            notifiedHolder = nil
            return false
        }
        // 持ち主が読めなければ、パスワード欄に入力しているだけなのかを見分けられない
        guard let holder = observation.holder else { return false }
        if holder != notifiedHolder { notifiedHolder = nil }
        guard holder != observation.frontmost, holder != observation.own, notifiedHolder == nil else {
            return false
        }
        notifiedHolder = holder
        return true
    }

    /// 小窓の文面。アプリの名前が引けなければ「別のアプリ」
    public static func message(holderName: String?) -> String {
        guard let name = holderName, !name.isEmpty else {
            return "別のアプリが Secure Input（パスワード入力の保護）を有効にしたままのため、iroha を選べません。\n"
                + "パスワードを入力したアプリを終了し、別のアプリに一度切り替えると戻ります。"
        }
        return "\(name) が Secure Input（パスワード入力の保護）を有効にしたままのため、iroha を選べません。\n"
            + "\(name) を終了し、別のアプリに一度切り替えると戻ります。"
    }
}
