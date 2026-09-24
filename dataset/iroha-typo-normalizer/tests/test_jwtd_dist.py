"""JWTD の実誤りからの分布推定（wild/jwtd_dist.py）。"""
import json
import random

from iroha.config import load_config
from iroha.typo.build import TypoGenerator
from iroha.wild.jwtd_dist import Estimator, estimate


def _pair(clean: str, k: int, typed: str, intended: str, et: str, tier: str = "keystroke") -> dict:
    noisy = clean[:k] + typed + clean[k + len(intended):]
    return {"noisy": noisy, "clean": clean, "tier": tier, "error_type": et, "detail": "",
            "edit_at": k, "typed": typed, "intended": intended,
            "at_end": k + len(intended) == len(clean), "category": "", "page": "1"}


def test_i_nuki_and_trailing_missing_are_excluded():
    est = Estimator()
    est.add(_pair("していたので", 2, "", "い", "key_missing"))            # い抜き（していた → してた）
    est.add(_pair("してたので", 2, "い", "", "key_extra"))                 # い足し（してた → していた）
    est.add(_pair("そちらは", 3, "", "は", "mora_missing", "editing"))     # 末尾の脱落
    assert est.excluded["i_nuki"] == 2
    assert est.excluded["mora_missing_at_end"] == 1
    assert sum(est.types.values()) == 0


def test_keystroke_pairs_map_to_generator_types():
    est = Estimator()
    est.add(_pair("きってをかう", 1, "", "っ", "key_missing"))              # kitte → kite
    est.add(_pair("ありがとう", 2, "ぎ", "が", "key_adjacent"))             # a → i
    est.add(_pair("ほんをよむ", 2, "の", "を", "key_far", "editing"))       # w → n
    est.add(_pair("しんぶんをよむ", 4, "をを", "を", "mora_duplication"))
    assert est.types["missing_double_consonant"] == 1
    assert est.types["substitution"] == 1 and est.confusion["substitution"]["a"]["i"] == 1
    assert est.types["key_far"] == 1 and est.confusion["key_far"]["w"]["n"] == 1
    assert est.types["mora_duplication"] == 1


def test_estimate_writes_a_config_the_generator_can_use(tmp_path):
    rows = []
    for _ in range(20):
        rows.append(_pair("ぐんにぞくする", 2, "", "に", "mora_missing", "editing"))
        rows.append(_pair("ほんをよむ", 2, "の", "を", "key_far", "editing"))
        rows.append(_pair("しんぶんをよむ", 4, "をを", "を", "mora_duplication"))
    pairs = tmp_path / "pairs.jsonl"
    pairs.write_text("".join(json.dumps(r, ensure_ascii=False) + "\n" for r in rows), encoding="utf-8")
    res = estimate(pairs, None, tmp_path / "out", mixed_input=0.05)
    assert abs(sum(res["error_types"].values()) - 1.0) < 1e-6
    assert res["error_types"]["mixed_input"] == 0.05
    cfg = load_config(tmp_path / "out" / "jwtd.yaml")
    assert cfg.get("typo.dist")["mora_missing"]["に"] > 0
    gen = TypoGenerator(cfg)
    rng = random.Random(0)
    kinds = {k for _ in range(60) for k in gen.generate("ぐんにぞくするほんをよむ", rng)["error_type"].split("+")}
    assert kinds <= {"mora_missing", "key_far", "mora_duplication", "mixed_input"}
    assert (tmp_path / "out" / "REPORT.md").exists()
