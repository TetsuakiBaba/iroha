"""JWTD → typo normalizer データ。純粋関数は常に、Sudachi を使うものは入っていれば検査する。"""
import json

import pytest

from iroha.config import load_config
from iroha.paths import Paths
from iroha.wild.jwtd import (
    EDITING, KEYSTROKE, JwtdBuilder, WindowReader, classify, extract, locate_edit,
    osa_distance, surface_diff,
)
from tests.conftest import needs_sudachi


def test_surface_diff_puts_duplicate_on_the_right():
    pre, post = "基本協定をを締結した", "基本協定を締結した"
    p, a, b = surface_diff(pre, post)
    assert (pre[:p], a, b) == ("基本協定を", "を", "")


def test_locate_edit():
    assert locate_edit("ぐんぞくする", "ぐんにぞくする", "", "に") == 2
    # 誤入力側で別の場所の読みまで変わっていたら一致しない
    assert locate_edit("にっぽんぞくする", "にほんにぞくする", "", "に") is None


def test_osa_distance_counts_transposition_as_one():
    assert osa_distance("airgatou", "arigatou") == 1
    assert osa_distance("ni", "") == 2


@pytest.mark.parametrize("left,a,b,right,tier,etype", [
    ("じょうきょうを", "を", "", "かいぜん", KEYSTROKE, "mora_duplication"),
    ("ふるくから", "から", "", "ぞうせん", EDITING, "word_duplication"),
    ("やむ", "お", "を", "えない", KEYSTROKE, "key_missing"),        # o / wo
    ("なぞ", "に", "の", "じんぶつ", KEYSTROKE, "key_adjacent"),      # i / o は隣
    ("", "た", "だ", "し", EDITING, "key_far"),                        # t / d は離れている
    ("ぐん", "", "に", "ぞくする", EDITING, "mora_missing"),
    ("しようする", "の", "", "ばあいは", EDITING, "mora_extra"),
    ("と", "う", "いう", "ちょうさ", KEYSTROKE, "key_missing"),        # tou / toiu
    ("めい", "", "ん", "ふれーむ", KEYSTROKE, "key_missing"),          # 子音の前の n は 1 打鍵
])
def test_classify(left, a, b, right, tier, etype):
    got_tier, got_type, _ = classify(left, a, b, right)
    assert (got_tier, got_type) == (tier, etype)


@pytest.fixture(scope="module")
def reader():
    return WindowReader(load_config())


@needs_sudachi
def test_extract_window_contains_diff(reader):
    pre = "愛南町は、愛媛県南部に位置し、南宇和郡属する町である。"
    post = "愛南町は、愛媛県南部に位置し、南宇和郡に属する町である。"
    s, why = extract(reader, pre, post, window=1, max_chars=48, max_dup=3)
    assert s is not None, why
    assert s.noisy.replace("ぐんぞく", "ぐんにぞく") == s.clean
    assert len(s.clean) <= 48 and "。" not in s.clean
    assert s.tier == EDITING


@needs_sudachi
def test_extract_kana_surface_is_typed_as_written(reader):
    # あるいは の Sudachi の読みは あるいわ だが、打つのは あるいは
    s, why = extract(reader, "これはあるいはのの例だ", "これはあるいはの例だ", 1, 48, 3)
    assert s is not None, why
    assert "あるいは" in s.clean


@needs_sudachi
def test_extract_drops_latin(reader):
    s, why = extract(reader, "TEAM dreamのピヴォして活躍", "TEAM dreamのピヴォとして活躍", 1, 48, 3)
    assert s is None and why == "latin_or_digit"


def _write(path, rows):
    with open(path, "w", encoding="utf-8") as f:
        for r in rows:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")


def _row(page, pre, post, pre_str, post_str, category="insertion_a", rev="1"):
    return {"page": page, "title": "t", "pre_rev": rev, "post_rev": rev + "0",
            "pre_text": pre, "post_text": post,
            "diffs": [{"pre_str": pre_str, "post_str": post_str, "category": category}]}


@needs_sudachi
def test_builder_end_to_end(tmp_path):
    src = tmp_path / "src"
    src.mkdir()
    train = [
        _row("1", "考えたことががなかったという。", "考えたことがなかったという。", "が", ""),
        _row("1", "固体発生の研究。", "個体発生の研究。", "固体", "個体", "kanji-conversion_a", "2"),
        # ベンチ（gold）と同じページ → train から除かれる
        _row("9", "巨大な鉄橋にに挟まれた。", "巨大な鉄橋に挟まれた。", "に", "", rev="3"),
        # 推敲系 → train には入らない
        _row("2", "南宇和郡属する町。", "南宇和郡に属する町。", "", "に", "deletion", "4"),
    ]
    test = [_row("5", "両者は明確にには区別されない。", "両者は明確には区別されない。", "に", "")]
    gold = [{"page": "9", "title": "g", "pre_rev": "7", "post_rev": "8",
             "pre_text": "予備校であったのをを改修した。", "post_text": "予備校であったのを改修した。"}]
    _write(src / "train.jsonl", train)
    _write(src / "test.jsonl", test)
    _write(src / "gold.jsonl", gold)
    cfg = load_config(overrides=[f"jwtd.src={src}", "jwtd.workers=1", "jwtd.clean_ratio=0.5"])
    paths = Paths(tmp_path / "out", tmp_path / "raw")
    stats = JwtdBuilder(cfg, paths).run()

    rows = [json.loads(l) for l in open(tmp_path / "out/jwtd/train.jsonl", encoding="utf-8")]
    assert {r["page"] for r in rows} == {"1"}
    assert all(r["tier"] == KEYSTROKE for r in rows)
    typo = [r for r in rows if r["n_errors"]]
    assert [(r["input"], r["target"]) for r in typo] == [
        ("かんがえたことががなかったという", "かんがえたことがなかったという")]
    assert stats["dropped"]["train"]["category:kanji-conversion_a"] == 1
    assert stats["train"]["excluded"]["bench_page"] == 1
    assert stats["train"]["excluded"]["editing"] == 1

    bench = [json.loads(l) for l in open(tmp_path / "out/jwtd/bench/keystroke.jsonl",
                                          encoding="utf-8")]
    assert {r["subset"] for r in bench} == {"test", "gold"}
    # typo 行ごとに、同じ窓の clean 行が対になっている
    for r in bench:
        if r["error_type"] != "none":
            assert any(c["error_type"] == "none" and c["noisy"] == r["clean"] for c in bench)

    # 同じ入力・同じ seed なら同じ出力
    before = (tmp_path / "out/jwtd/train.jsonl").read_text(encoding="utf-8")
    JwtdBuilder(cfg, paths).run()
    assert (tmp_path / "out/jwtd/train.jsonl").read_text(encoding="utf-8") == before
