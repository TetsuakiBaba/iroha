"""ローマ字 ⇄ かな。Swift の RomajiComposer と同じ挙動であることが前提。"""
import pytest

from iroha_dataset.typo.romanize import (KANA_TO_KEYS, RomanizeError, romanize,
                                         romanize_units, roundtrip_ok, to_kana)


@pytest.mark.parametrize("keys,expected", [
    ("konnnichiha", "こんにちは"),
    ("konichiha", "こにちは"),
    ("kitte", "きって"),
    ("kite", "きて"),
    ("kittte", "きっって"),
    ("arigatou", "ありがとう"),
    ("arigattou", "ありがっとう"),
    ("matcha", "まっちゃ"),
    ("sinbun", "しんぶん"),
    ("kyouhaiitenkidesune", "きょうはいいてんきですね"),
])
def test_to_kana(keys, expected):
    assert to_kana(keys) == expected


def test_unresolved_keys_pass_through():
    """解決できない打鍵はそのまま残る（iroha の合成中の見え方と同じ）。"""
    assert to_kana("airgatou") == "あいrがとう"


@pytest.mark.parametrize("kana", [
    "こんにちは", "きって", "ありがとう", "きょうはいいてんきですね", "しんぶん",
    "まっちゃ", "とうきょうとりつだいがく", "がっこう", "しゃしん", "ふぁいる",
    "でぃすく", "しかし、そのけっか", "「かっこ」〜です",
])
@pytest.mark.parametrize("style", ["nn", "contextual"])
def test_roundtrip(kana, style):
    assert roundtrip_ok(kana, style), (kana, style, romanize(kana, style))


@pytest.mark.parametrize("style", ["nn", "contextual"])
def test_romanize_units_is_consistent(style):
    """units / unit_keys を繋ぎ直すと元のかな・打鍵列に戻る（mixed_input が依存する）。"""
    for kana in ["きって", "こんにちは", "きょうはとうきょうとにいきます", "まっちゃ", "しんぶん"]:
        keys, units, unit_keys = romanize_units(kana, style)
        assert "".join(units) == kana
        assert "".join(unit_keys) == keys
        assert keys == romanize(kana, style)
        assert to_kana(keys) == kana


def test_every_table_kana_roundtrips():
    """KANA_TO_KEYS の全エントリが打鍵列 → かな で元に戻る。"""
    broken = [k for k in KANA_TO_KEYS if to_kana(KANA_TO_KEYS[k]) != k]
    assert broken == []


def test_unromanizable_raises():
    with pytest.raises(RomanizeError):
        romanize("あAう")
