"""split は document_id 単位。train と test に同じ原文由来の example が混ざらないこと。"""
from iroha_dataset.config import load_config
from iroha_dataset.paths import SPLITS
from iroha_dataset.split import Splitter


def test_same_document_always_gets_the_same_split():
    s = Splitter(load_config())
    for i in range(500):
        doc = f"kaken_{i:06d}"
        assert s.split_for(doc) == s.split_for(doc)


def test_split_is_stable_across_instances():
    cfg = load_config()
    a, b = Splitter(cfg), Splitter(cfg)
    for i in range(200):
        doc = f"tatoeba_{i}"
        assert a.split_for(doc) == b.split_for(doc)


def test_seed_changes_the_assignment():
    cfg_a = load_config(overrides=["seed=1"])
    cfg_b = load_config(overrides=["seed=2"])
    a, b = Splitter(cfg_a), Splitter(cfg_b)
    docs = [f"doc_{i}" for i in range(2000)]
    assert [a.split_for(d) for d in docs] != [b.split_for(d) for d in docs]


def test_ratios_are_roughly_respected():
    cfg = load_config(overrides=["split.train=0.8", "split.validation=0.1", "split.test=0.1"])
    s = Splitter(cfg)
    counts = {k: 0 for k in SPLITS}
    n = 20000
    for i in range(n):
        counts[s.split_for(f"doc_{i}")] += 1
    assert abs(counts["train"] / n - 0.8) < 0.02
    assert abs(counts["validation"] / n - 0.1) < 0.02
    assert abs(counts["test"] / n - 0.1) < 0.02


def test_ratios_are_normalized():
    cfg = load_config(overrides=["split.train=98", "split.validation=1", "split.test=1"])
    s = Splitter(cfg)
    assert abs(s.ratios["train"] - 0.98) < 1e-9


def test_all_splits_are_reachable():
    s = Splitter(load_config())
    seen = {s.split_for(f"doc_{i}") for i in range(5000)}
    assert seen == set(SPLITS)
