import Testing
@testable import IrohaCore

private let own: Int32 = 100
private let chrome: Int32 = 200
private let finder: Int32 = 300
private let safari: Int32 = 400

private func observe(_ enabled: Bool, holder: Int32?, front: Int32?) -> SecureInputNoticeRule.Observation {
    .init(isSecureInputEnabled: enabled, holder: holder, frontmost: front, own: own)
}

/// 前面のアプリが切り替わるたびに確かめた状態を順に渡し、知らせたかを並べる
private func notices(_ observations: [SecureInputNoticeRule.Observation]) -> [Bool] {
    var rule = SecureInputNoticeRule()
    return observations.map { rule.shouldNotify($0) }
}

struct SecureInputNoticeRuleTests {
    @Test func notifiesWhenBackgroundAppHolds() {
        #expect(notices([observe(true, holder: chrome, front: finder)]) == [true])
    }

    @Test func ignoresPasswordFieldInFrontApp() {
        #expect(notices([observe(true, holder: chrome, front: chrome)]) == [false])
    }

    @Test func ignoresOwnSettingsWindow() {
        #expect(notices([observe(true, holder: own, front: finder)]) == [false])
    }

    @Test func ignoresWhenOffOrHolderUnknown() {
        #expect(notices([
            observe(false, holder: nil, front: finder),
            observe(true, holder: nil, front: finder),
        ]) == [false, false])
    }

    @Test func notifiesOnceWhileSameAppHolds() {
        // 持ち主のアプリを前面にしても、持ち続けている間は数え直さない
        #expect(notices([
            observe(true, holder: chrome, front: finder),
            observe(true, holder: chrome, front: safari),
            observe(true, holder: chrome, front: chrome),
            observe(true, holder: chrome, front: finder),
        ]) == [true, false, false, false])
    }

    @Test func notifiesAgainAfterRelease() {
        #expect(notices([
            observe(true, holder: chrome, front: finder),
            observe(false, holder: nil, front: finder),
            observe(true, holder: chrome, front: finder),
        ]) == [true, false, true])
    }

    @Test func notifiesAgainWhenHolderChanges() {
        #expect(notices([
            observe(true, holder: chrome, front: finder),
            observe(true, holder: safari, front: finder),
        ]) == [true, true])
    }

    @Test func messageNamesHolder() {
        #expect(SecureInputNoticeRule.message(holderName: "Google Chrome").hasPrefix("Google Chrome が Secure Input"))
        #expect(SecureInputNoticeRule.message(holderName: nil).hasPrefix("別のアプリが"))
        #expect(SecureInputNoticeRule.message(holderName: "").hasPrefix("別のアプリが"))
    }
}
