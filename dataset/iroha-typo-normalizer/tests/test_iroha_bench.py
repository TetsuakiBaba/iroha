import json
import random

from iroha.config import load_config
from iroha.kkc.iroha_bench import IrohaBenchBuilder, to_katakana
from iroha.paths import paths_from_config


def _record(**overrides) -> dict:
    record = {
        "id": "kaken_1_0000",
        "source": "llmjp_kaken",
        "document_id": "kaken_1",
        "text": "本研究では新しい入力手法を提案する。",
        "previous_text": "視覚障害者の情報アクセスについて検討した。",
        "reading_confidence": "high",
        "confidence_reasons": [],
        "chunks": [
            {"target": "本研究では", "reading": "ほんけんきゅうでは"},
            {"target": "新しい入力手法を", "reading": "あたらしいにゅうりょくしゅほうを"},
            {"target": "提案する", "reading": "ていあんする"},
        ],
    }
    record.update(overrides)
    return record


def _builder(tmp_path, *overrides: str) -> IrohaBenchBuilder:
    cfg = load_config("config/iroha-bench-v1.yaml",
                      [f"data_dir={tmp_path}", "iroha_bench.exclude_overlap_with=null", *overrides])
    return IrohaBenchBuilder(cfg, paths_from_config(cfg))


def test_to_katakana_keeps_symbols():
    assert to_katakana("きょうは、いい「てんき」ー") == "キョウハ、イイ「テンキ」ー"


def test_item_is_a_contiguous_span_within_the_length_range(tmp_path):
    builder = _builder(tmp_path, "iroha_bench.min_input_chars=8", "iroha_bench.max_input_chars=20")
    for seed in range(20):
        item = builder.item_for(_record(), random.Random(seed))
        assert 8 <= len(item["input"]) <= 20
        # 正解はチャンクの連続した並びで、読みと対応している
        assert item["expected_output"][0] in "本研究では新しい入力手法を提案する"
        assert item["input"] == item["input"].upper()


def test_context_is_previous_text_and_preceding_chunks(tmp_path):
    builder = _builder(tmp_path, "iroha_bench.context_ratio=1.0", "iroha_bench.max_context_chars=40",
                       "iroha_bench.min_input_chars=6", "iroha_bench.max_input_chars=6")
    item = builder.item_for(_record(), random.Random(0))
    # 6 字に収まるのは「ていあんする」だけ
    assert item["input"] == "テイアンスル"
    assert item["context_text"] == ("視覚障害者の情報アクセスについて検討した。本研究では新しい入力手法を")[-40:]


def test_no_span_fits_returns_none(tmp_path):
    builder = _builder(tmp_path, "iroha_bench.min_input_chars=50")
    assert builder.item_for(_record(), random.Random(0)) is None


def test_low_confidence_only_for_allowed_reasons(tmp_path):
    builder = _builder(tmp_path)
    assert builder._usable(_record(reading_confidence="low", confidence_reasons=["proper_noun:東京"]))
    assert not builder._usable(_record(reading_confidence="low",
                                       confidence_reasons=["proper_noun:東京", "surface_fallback:鬱"]))


def test_run_writes_ajimee_format_with_source_prefixed_index(tmp_path):
    builder = _builder(tmp_path, "iroha_bench.items_per_document=1")
    canonical = builder.paths.canonical_for("llmjp_kaken")
    canonical.parent.mkdir(parents=True)
    with open(canonical, "w", encoding="utf-8") as f:
        for i in range(3):
            # 文書ごとに 1 件。同じ正解は 1 回だけ
            f.write(json.dumps(_record(id=f"r{i}", document_id=f"d{i}"), ensure_ascii=False) + "\n")

    class Adapter:
        name = "llmjp_kaken"

    report = builder.run([Adapter()])
    items = json.loads((tmp_path / "iroha-bench-v1" / "evaluation_items.json").read_text(encoding="utf-8"))
    assert report["total"] == len(items) >= 1
    assert items[0]["index"] == "llmjp_kaken-0001"
    assert set(items[0]) >= {"index", "context_text", "input", "expected_output"}
    assert len({i["expected_output"][0] for i in items}) == len(items)
