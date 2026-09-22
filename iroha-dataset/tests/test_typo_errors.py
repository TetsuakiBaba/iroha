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
