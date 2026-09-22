"""Sudachi の読みと文節チャンク。Sudachi が無い環境では skip する。"""
import pytest

from iroha_dataset.config import load_config
from iroha_dataset.preprocess.chunking import Chunker
from iroha_dataset.preprocess.reading import Morpheme, ReadingAnalyzer
from tests.conftest import needs_sudachi


@pytest.fixture(scope="module")
def analyzer():
    return ReadingAnalyzer(load_config())


@needs_sudachi
@pytest.mark.parametrize("text,reading", [
    ("今日は東京都立大学で講義を行います", "きょうはとうきょうとりつだいがくでこうぎをおこないます"),
    ("本研究では新しい入力手法を提案する", "ほんけんきゅうではあたらしいにゅうりょくしゅほうをていあんする"),
    ("視覚障害者の情報アクセスについて検討した", "しかくしょうがいしゃのじょうほうあくせすについてけんとうした"),
])
def test_reading(analyzer, text, reading):
    result = analyzer.analyze(text)
    assert result.ok
    assert result.reading == reading


@needs_sudachi
def test_reading_is_hiragana_only(analyzer):
    result = analyzer.analyze("メカニズムを検討した")
    assert result.ok
    assert all("ぁ" <= c <= "ゖ" or c in "ー、。・「」〜！？" for c in result.reading)


@needs_sudachi
def test_latin_reading_is_rejected(analyzer):
    """読みがひらがなに落ちない文は捨てる（教師にできない）。"""
    result = analyzer.analyze("これはKAKENのデータ")
    assert not result.ok or "ひらがな" not in result.reading
    if not result.ok:
        assert result.failure.startswith("non_kana_reading") or result.failure == "oov"


@needs_sudachi
def test_proper_noun_is_low_confidence(analyzer):
    result = analyzer.analyze("田中さんは大阪に住んでいる")
    assert result.ok
    assert result.confidence == "low"
    assert any(r.startswith("proper_noun") for r in result.reasons)


@needs_sudachi
def test_plain_sentence_is_high_confidence(analyzer):
    result = analyzer.analyze("本研究では新しい入力手法を提案する")
    assert result.confidence == "high"
    assert result.reasons == []


@needs_sudachi
def test_morphemes_are_recorded(analyzer):
    result = analyzer.analyze("本研究では新しい入力手法を提案する")
    assert result.morphemes
    m = result.morphemes[0]
    for key in ("surface", "reading", "normalized_form", "pos", "oov"):
        assert key in m.as_dict()


@needs_sudachi
def test_mode_second_opinion_is_registered(analyzer):
    names = [o.name for o in analyzer.second_opinions]
    assert "sudachi_mode" in names


@needs_sudachi
def test_oov_rejection_can_be_turned_off():
    strict = ReadingAnalyzer(load_config())
    lenient = ReadingAnalyzer(load_config(overrides=["reading.reject_oov=false"]))
    assert strict.reject_oov
    assert not lenient.reject_oov


# ---------------------------------------------------------------- chunking
def _m(surface, reading, pos):
    return Morpheme(surface=surface, reading=reading, normalized_form=surface,
                    pos=pos.split("/"), is_oov=False)


def test_chunker_on_handmade_morphemes():
    """Sudachi 無しでもチャンク規則そのものは試せる。"""
    morphemes = [
        _m("本", "ほん", "接頭辞/*"),
        _m("研究", "けんきゅう", "名詞/普通名詞"),
        _m("で", "で", "助詞/格助詞"),
        _m("は", "は", "助詞/係助詞"),
        _m("新しい", "あたらしい", "形容詞/一般"),
        _m("入力", "にゅうりょく", "名詞/普通名詞"),
        _m("手法", "しゅほう", "名詞/普通名詞"),
        _m("を", "を", "助詞/格助詞"),
        _m("提案", "ていあん", "名詞/普通名詞"),
        _m("する", "する", "動詞/非自立可能"),
        _m("。", "。", "補助記号/句点"),
    ]
    chunks = Chunker(load_config()).chunk(morphemes)
    assert [c.target for c in chunks] == ["本研究では", "新しい入力手法を", "提案する"]
    assert [c.reading for c in chunks] == [
        "ほんけんきゅうでは", "あたらしいにゅうりょくしゅほうを", "ていあんする"]


@needs_sudachi
@pytest.mark.parametrize("text,expected", [
    ("本研究では新しい入力手法を提案する。", ["本研究では", "新しい入力手法を", "提案する"]),
    ("今日は東京都立大学で講義を行います。",
     ["今日は", "東京都立大学で", "講義を", "行います"]),
    ("視覚障害者の情報アクセスについて検討した。",
     ["視覚障害者の", "情報アクセスについて", "検討した"]),
])
def test_chunker_with_sudachi(analyzer, text, expected):
    result = analyzer.analyze(text)
    chunks = Chunker(load_config()).chunk(result.morphemes)
    assert [c.target for c in chunks] == expected


@needs_sudachi
def test_chunks_reconstruct_the_sentence(analyzer):
    """チャンクを繋ぐと（文末の句点を除いて）原文と読みに戻る。"""
    for text in ["本研究では新しい入力手法を提案する", "彼は毎朝早く起きて、犬と一緒に公園を走る"]:
        result = analyzer.analyze(text)
        chunks = Chunker(load_config()).chunk(result.morphemes)
        assert "".join(c.target for c in chunks) == text
        assert "".join(c.reading for c in chunks) == result.reading


@needs_sudachi
def test_chunk_max_chars_is_respected(analyzer):
    cfg = load_config(overrides=["chunk.max_chars=8"])
    result = analyzer.analyze("本研究では新しい入力手法を提案し、その有効性を実験によって確認した")
    for chunk in Chunker(cfg).chunk(result.morphemes):
        assert len(chunk.target) <= 8, chunk.target


def test_chunker_on_empty_input():
    assert Chunker(load_config()).chunk([]) == []
