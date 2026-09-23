"""typo 生成。打鍵列の上で崩し、かなに戻したときに実際に変わっていること。"""
import random

import pytest

from iroha_dataset.config import load_config
from iroha_dataset.typo.build import CLEAN, TypoGenerator
from iroha_dataset.typo.errors import APPLICABLE, ERROR_TYPES, ErrorContext, KeyStream
from iroha_dataset.typo.romanize import romanize_units, to_kana

READINGS = ["きょうはとうきょうとりつだいがくにいきます", "きって", "ありがとう",
            "ほんけんきゅうではあたらしいにゅうりょくしゅほうをていあんする",
            "がっこうにいった", "しんぶんをよむ"]


def _stream(kana: str, style: str = "nn") -> KeyStream:
    keys, units, unit_keys = romanize_units(kana, style)
    return KeyStream(keys=keys, units=units, unit_keys=unit_keys)


def test_every_error_type_is_registered_in_applicable():
    assert set(ERROR_TYPES) == set(APPLICABLE)


def test_config_error_types_all_exist():
    cfg = load_config()
    for name in (cfg.get("typo.error_types") or {}):
        assert name in ERROR_TYPES, f"config の {name} が errors.py に無い"


@pytest.mark.parametrize("name", sorted(ERROR_TYPES))
def test_error_changes_kana_when_applicable(name):
    """適用できる読みに当てたら、かな（または出力）が必ず変わる。"""
    ctx = ErrorContext(rng=random.Random(0), key_weights={})
    changed = 0
    for kana in READINGS:
        stream = _stream(kana)
        if not APPLICABLE[name](stream):
            continue
        for _ in range(20):
            typo = ERROR_TYPES[name](stream, ctx)
            if typo is None:
                continue
            out = typo.raw_output if typo.raw_output is not None else to_kana(typo.keys)
            if out != kana:
                changed += 1
                break
    assert changed > 0, f"{name} がどの読みでも変化を作れなかった"


def test_missing_double_consonant_removes_sokuon():
    ctx = ErrorContext(rng=random.Random(1), key_weights={})
    stream = _stream("きって")
    typo = ERROR_TYPES["missing_double_consonant"](stream, ctx)
    assert typo is not None
    assert to_kana(typo.keys) == "きて"


def test_excessive_double_consonant_adds_consonant():
    ctx = ErrorContext(rng=random.Random(1), key_weights={})
    stream = _stream("きって")
    typo = ERROR_TYPES["excessive_double_consonant"](stream, ctx)
    assert typo is not None
    assert typo.keys == "kittte"


def test_sokuon_types_need_a_double_consonant():
    stream = _stream("ありがとう")
    for name in ("missing_double_consonant", "excessive_double_consonant"):
        assert not APPLICABLE[name](stream)


def test_mixed_input_leaves_raw_keys():
    ctx = ErrorContext(rng=random.Random(3), key_weights={})
    stream = _stream("きょうはだいがく")
    typo = ERROR_TYPES["mixed_input"](stream, ctx)
    assert typo is not None and typo.raw_output is not None
    # ローマ字がそのまま残っている
    assert any("a" <= c <= "z" for c in typo.raw_output)


def test_weak_finger_omission_only_drops_weak_keys():
    ctx = ErrorContext(rng=random.Random(4), key_weights={})
    stream = _stream("ありがとう")     # arigatou: a が小指
    for _ in range(20):
        typo = ERROR_TYPES["weak_finger_omission"](stream, ctx)
        if typo is None:
            continue
        dropped = stream.keys[typo.position]
        from iroha_dataset.typo import keyboard
        assert keyboard.is_weak_finger(dropped)


def test_generator_target_is_always_the_clean_reading():
    cfg = load_config()
    gen = TypoGenerator(cfg)
    rng = random.Random(7)
    for kana in READINGS:
        for _ in range(20):
            sample = gen.generate(kana, rng)
            if sample is None:
                continue
            assert sample["target"] == kana
            assert sample["input"] != kana or sample["error_type"] == CLEAN


def test_generator_clean_returns_identity():
    gen = TypoGenerator(load_config())
    sample = gen.generate("きょうはいいてんきです", random.Random(0), force_clean=True)
    assert sample is not None
    assert sample["input"] == sample["target"] == "きょうはいいてんきです"
    assert sample["error_type"] == CLEAN
    assert sample["n_errors"] == 0


def test_generator_is_deterministic_for_a_seed():
    cfg = load_config()
    first = [TypoGenerator(cfg).generate("がっこうにいった", random.Random(99))
             for _ in range(3)]
    second = [TypoGenerator(cfg).generate("がっこうにいった", random.Random(99))
              for _ in range(3)]
    assert first == second


def test_max_errors_is_respected():
    cfg = load_config()
    cfg.set("typo.max_errors_per_sample", 2)
    cfg.set("typo.second_error_ratio", 1.0)
    gen = TypoGenerator(cfg)
    rng = random.Random(5)
    for _ in range(30):
        sample = gen.generate("ほんけんきゅうではあたらしいにゅうりょくしゅほうをていあんする", rng)
        if sample is None:
            continue
        assert sample["n_errors"] <= 2


# ---------------------------------------------------------------- 仮名単位の誤りと推定した分布（typo.dist）
KANA_TYPES = ("mora_missing", "mora_extra", "mora_substitution", "mora_duplication", "word_duplication")


def _apply(name: str, kana: str, dist: dict | None = None, seed: int = 0, n: int = 200) -> list[str]:
    ctx = ErrorContext(rng=random.Random(seed), key_weights={}, dist=dist or {})
    out = []
    for _ in range(n):
        typo = ERROR_TYPES[name](_stream(kana), ctx)
        if typo is not None:
            out.append(to_kana(typo.keys))
    return out


@pytest.mark.parametrize("name", KANA_TYPES)
def test_kana_types_rebuild_consistent_keys(name):
    """仮名を編集したあとの打鍵列は、かなに戻すと編集後の読みそのものになる（ローマ字が残らない）"""
    for kana in READINGS:
        for out in _apply(name, kana, n=30):
            assert all("ぁ" <= c <= "ゖ" or c == "ー" for c in out), (name, kana, out)


def test_mora_missing_never_drops_the_last_kana():
    """末尾の脱落は入力途中と区別できない（本体も末尾に足すだけの訂正は捨てる）ので作らない"""
    for kana in READINGS:
        for out in _apply("mora_missing", kana, n=100):
            assert out[-1] == kana[-1] and len(out) == len(kana) - 1, (kana, out)


def test_mora_missing_follows_the_table():
    dist = {"mora_missing": {"を": 1.0}}
    outs = _apply("mora_missing", "しんぶんをよむ", dist)
    assert outs and all(o == "しんぶんよむ" for o in outs)


def test_mora_extra_uses_the_previous_kana():
    dist = {"mora_extra": {"ん": {"の": 1.0}}}
    outs = set(_apply("mora_extra", "しんぶんをよむ", dist))
    assert outs == {"しんのぶんをよむ", "しんぶんのをよむ"}


def test_mora_substitution_follows_the_table():
    dist = {"mora_substitution": {"を": {"が": 1.0}}}
    assert set(_apply("mora_substitution", "しんぶんをよむ", dist)) == {"しんぶんがよむ"}


def test_duplications():
    assert set(_apply("mora_duplication", "しんぶんをよむ", {"mora_duplication": {"を": 1.0}})) == {"しんぶんををよむ"}
    assert set(_apply("word_duplication", "してからいく", {"word_duplication": {"から": 1.0}})) == {"してからからいく"}


def test_key_far_uses_confusion_and_is_never_adjacent():
    from iroha_dataset.typo import keyboard
    dist = {"key_rates": {"key_far": {"w": 1.0}}, "confusion": {"key_far": {"w": {"n": 1.0}}}}
    assert set(_apply("key_far", "しんぶんをよむ", dist)) == {"しんぶんのよむ"}
    ctx = ErrorContext(rng=random.Random(1), key_weights={})
    for _ in range(200):
        t = ERROR_TYPES["key_far"](_stream("ありがとう"), ctx)
        i = t.position
        assert t.keys[i] not in keyboard.neighbors(_stream("ありがとう").keys[i])


def test_substitution_confusion_is_limited_to_neighbors():
    dist = {"key_rates": {"substitution": {"r": 1.0}}, "confusion": {"substitution": {"r": {"t": 1.0, "n": 5.0}}}}
    outs = set(_apply("substitution", "される", dist))
    # sareru の r を t に置き換える。n は r の隣接キーでないので、表で重くても使わない
    assert outs == {to_kana("sateru"), to_kana("saretu")}


def test_learned_key_rates_replace_key_weights():
    dist = {"key_rates": {"deletion": {"k": 1.0}}}
    outs = set(_apply("deletion", "きって", dist))
    assert outs <= {to_kana("itte"), to_kana("kite")}


def test_generator_reads_dist_from_config():
    cfg = load_config(overrides=[
        "typo.error_types={deletion: 0, substitution: 0, insertion: 0, transposition: 0, repeated_key: 0, "
        "missing_double_consonant: 0, excessive_double_consonant: 0, mixed_input: 0, weak_finger_omission: 0, "
        "mora_substitution: 1}",
        "typo.dist={mora_substitution: {を: {が: 1.0}}}",
        "typo.second_error_ratio=0",
    ])
    gen = TypoGenerator(cfg)
    s = gen.generate("しんぶんをよむ", random.Random(0))
    assert s["input"] == "しんぶんがよむ" and s["error_type"] == "mora_substitution"
