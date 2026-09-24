from iroha.config import load_config
from iroha.dedup import Deduplicator, KeyDeduplicator, normalize_key


def test_exact_duplicate():
    d = Deduplicator(load_config())
    assert not d.is_duplicate("本研究では新しい入力手法を提案する。")
    assert d.is_duplicate("本研究では新しい入力手法を提案する。")
    assert d.stats.exact == 1


def test_normalized_duplicate():
    d = Deduplicator(load_config())
    assert not d.is_duplicate("本研究では、新しい入力手法を提案する。")
    assert d.is_duplicate("本研究では新しい入力手法を提案する")
    assert d.stats.normalized == 1


def test_normalize_key_drops_punctuation():
    assert normalize_key("あ、い。う（え）") == normalize_key("あいうえ")


def test_near_duplicate_off_by_default():
    d = Deduplicator(load_config())
    assert not d.use_near


def test_near_duplicate_catches_small_edits():
    cfg = load_config(overrides=["dedup.near_duplicate=true", "dedup.exact=false",
                                 "dedup.normalized=false"])
    d = Deduplicator(cfg)
    base = "本研究では新しい日本語入力手法を提案し、その有効性を実験によって確認した"
    assert not d.is_duplicate(base)
    assert d.is_duplicate(base.replace("確認した", "確認します"))
    assert d.stats.near == 1


def test_near_duplicate_keeps_different_sentences():
    cfg = load_config(overrides=["dedup.near_duplicate=true", "dedup.exact=false",
                                 "dedup.normalized=false"])
    d = Deduplicator(cfg)
    assert not d.is_duplicate("本研究では新しい日本語入力手法を提案した")
    assert not d.is_duplicate("視覚障害者の情報アクセスについて広く検討を行った")


def test_key_deduplicator():
    k = KeyDeduplicator()
    assert not k.is_duplicate("a", "b", "c")
    assert k.is_duplicate("a", "b", "c")
    assert not k.is_duplicate("a", "b", "d")
    assert k.dropped == 1
