import pytest

from iroha_dataset.config import load_config
from iroha_dataset.preprocess.filtering import SentenceFilter
from iroha_dataset.preprocess.normalize import (has_broken_unicode, japanese_ratio,
                                                katakana_to_hiragana, normalize_text)
from iroha_dataset.preprocess.sentence import split_sentences, strip_terminator


def test_normalize_widths_and_spaces():
    assert normalize_text("　ＡＢＣ１２３　") == "ABC123"
    assert normalize_text("あ​い\tう") == "あい う"


def test_normalize_restores_japanese_punctuation():
    # NFKC が半角にした ！？ を全角に戻す（文分割が全角で見ているため）
    assert normalize_text("本当？") == "本当？"
    assert normalize_text("本当!") == "本当！"


def test_normalize_removes_control_chars():
    assert "\x07" not in normalize_text("あ\x07い")


def test_broken_unicode_detection():
    assert has_broken_unicode("あ�い")
    assert not has_broken_unicode("ふつうの文です")


def test_katakana_to_hiragana():
    assert katakana_to_hiragana("トウキョウ") == "とうきょう"
    assert katakana_to_hiragana("アクセスー") == "あくせすー"


def test_japanese_ratio():
    assert japanese_ratio("日本語の文") == 1.0
    assert japanese_ratio("abcde") == 0.0


def test_split_sentences_keeps_quotes_together():
    assert split_sentences("「これは一文ですか。はい。」と言った。次の文。") == [
        "「これは一文ですか。はい。」と言った。", "次の文。"]


def test_strip_terminator():
    assert strip_terminator("提案する。") == "提案する"
    assert strip_terminator("よ！？") == "よ"
    assert strip_terminator("と言った。") == "と言った"
    assert strip_terminator("句点なし") == "句点なし"


@pytest.mark.parametrize("text,reason", [
    ("本研究では新しい入力手法を提案する。", None),
    ("今日は東京都立大学で講義を行います。", None),
    ("短い。", "too_short"),
    ("https://example.com/page", "url"),
    ("<p>だめ</p>", "html_fragment"),
    ("研究概要&amp;まとめ", "html_fragment"),
    ("x = y + z", "latin_run"),
    ("平成3年に行った。", "digits"),
    ("あ" * 200, "too_long"),
    ("+-*/=<>±×÷", "symbols_only"),
    ("", "empty"),
    ("あ�いうえおかき", "broken_unicode"),
])
def test_filter_reasons(text, reason):
    f = SentenceFilter(load_config())
    assert f.reject_reason(text) == reason


def test_filter_counts_reasons():
    f = SentenceFilter(load_config())
    f.accept("本研究では新しい入力手法を提案する。")
    f.accept("短い。")
    assert f.stats.kept == 1
    assert f.stats.seen == 2
    assert f.stats.counts["too_short"] == 1


def test_digits_can_be_allowed():
    f = SentenceFilter(load_config(overrides=["filter.allow_digits=true"]))
    assert f.reject_reason("平成3年に行った。") is None
