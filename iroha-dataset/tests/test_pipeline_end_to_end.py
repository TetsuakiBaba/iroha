"""ダミーソースで全 pipeline を通す。

* canonical → kkc → typo → stats → samples が生成されること
* **同じ document から出た example が train と test に混ざらないこと**
* context の作り方（文内の確定文字列 + 文をまたぐ前文）が仕様どおりであること
"""
import json

import pytest

from iroha_dataset.config import load_config
from iroha_dataset.jsonlio import read_jsonl
from iroha_dataset.kkc.build import KkcBuilder
from iroha_dataset.paths import Paths, SPLITS
from iroha_dataset.preprocess.pipeline import run_preprocess
from iroha_dataset.samples import run_samples
from iroha_dataset.sources.base import Document, SourceAdapter, SourceInfo
from iroha_dataset.stats import run_stats
from iroha_dataset.typo.build import TypoBuilder
from tests.conftest import needs_sudachi

PARAGRAPHS = [
    "視覚障害者の情報アクセスについて検討した。本研究では新しい入力手法を提案する。"
    "提案手法の有効性を実験によって確認した。",
    "今日は東京都立大学で講義を行います。学生は熱心に話を聞いていました。",
]


class FakeAdapter(SourceAdapter):
    name = "fake"
    n_documents = 40

    @classmethod
    def info(cls) -> SourceInfo:
        return SourceInfo(name="fake", title="テスト用", homepage="-",
                          license="CC0 1.0", license_url="-", attribution="テスト")

    @property
    def enabled(self) -> bool:
        return True

    def download(self, *, force: bool = False) -> dict:
        return {}

    def documents(self):
        for i in range(self.n_documents):
            yield Document(
                document_id=f"fake_{i:03d}",
                source=self.name,
                paragraphs=list(PARAGRAPHS),
                license="CC0 1.0",
                url=None,
                attribution="テスト",
                meta={},
            )


@pytest.fixture
def built(tmp_path):
    cfg = load_config(overrides=[
        "split.train=0.5", "split.validation=0.25", "split.test=0.25",
        "typo.variants_per_clean_sample=3", "samples.per_file=20",
    ])
    paths = Paths(tmp_path / "out", tmp_path / "raw").ensure()
    adapters = [FakeAdapter(cfg, paths)]
    pre = run_preprocess(cfg, paths, adapters)
    kkc = KkcBuilder(cfg, paths).run(adapters)
    typo = TypoBuilder(cfg, paths).run(adapters)
    stats = run_stats(cfg, paths)
    samples = run_samples(cfg, paths)
    return cfg, paths, pre, kkc, typo, stats, samples


@needs_sudachi
def test_canonical_records_have_the_expected_shape(built):
    _cfg, paths, pre, *_ = built
    assert pre["totals"]["records"] > 0
    records = list(read_jsonl(paths.canonical_for("fake")))
    assert records
    for r in records:
        for key in ("id", "source", "document_id", "sentence_index", "text", "reading",
                    "previous_text", "license", "has_oov", "reading_confidence", "split"):
            assert key in r, key
        assert r["reading"]
        assert r["chunks"]
        assert r["split"] in SPLITS


@needs_sudachi
def test_morphemes_are_written(built):
    _cfg, paths, *_ = built
    rows = list(read_jsonl(paths.morphemes_for("fake")))
    assert rows
    m = rows[0]["morphemes"][0]
    assert set(m) == {"surface", "reading", "normalized_form", "pos", "oov"}


@needs_sudachi
def test_previous_text_crosses_sentences_within_a_paragraph(built):
    _cfg, paths, *_ = built
    records = list(read_jsonl(paths.canonical_for("fake")))
    by_index = {r["sentence_index"]: r for r in records if r["document_id"] == "fake_000"}
    # 段落の 1 文目は前文なし、2 文目には前文が入る
    assert by_index[0]["previous_text"] == ""
    assert "視覚障害者" in by_index[1]["previous_text"]


@needs_sudachi
def test_paragraph_boundary_resets_previous_text(built):
    _cfg, paths, *_ = built
    records = [r for r in read_jsonl(paths.canonical_for("fake"))
               if r["document_id"] == "fake_000"]
    # 2 つ目の段落の先頭（「今日は…」）には前の段落の文が入らない
    heads = [r for r in records if r["text"].startswith("今日は")]
    assert heads and heads[0]["previous_text"] == ""


@needs_sudachi
def test_kkc_examples_match_the_spec(built):
    _cfg, paths, _pre, kkc, *_ = built
    assert kkc["total"] > 0
    examples = []
    for split in SPLITS:
        examples.extend(read_jsonl(paths.kkc / f"{split}.jsonl"))
    wanted = [e for e in examples
              if e["target"] == "新しい入力手法を" and e["context"].endswith("本研究では")]
    assert wanted, "仕様の例（context=…本研究では / target=新しい入力手法を）が作られていない"
    assert wanted[0]["input"] == "あたらしいにゅうりょくしゅほうを"


@needs_sudachi
def test_kkc_context_grows_with_committed_text(built):
    _cfg, paths, *_ = built
    examples = [e for e in read_jsonl(paths.kkc / "train.jsonl")]
    by_doc = {}
    for e in examples:
        by_doc.setdefault(e["id"].rsplit("_c", 1)[0], []).append(e)
    for group in by_doc.values():
        group.sort(key=lambda e: e["id"])
        for prev, nxt in zip(group, group[1:]):
            # 1 つ前の target が context の末尾に足されている
            assert nxt["context"].endswith(prev["context"][-10:] + prev["target"]) or \
                   nxt["context"].endswith(prev["target"])


@needs_sudachi
def test_no_document_leaks_across_splits(built):
    """同じ document から出た example が 2 つ以上の split に現れない。"""
    _cfg, paths, *_ = built
    for directory in (paths.kkc, paths.typo):
        owner = {}
        for split in SPLITS:
            path = directory / f"{split}.jsonl"
            if not path.exists():
                continue
            for e in read_jsonl(path):
                doc = e["document_id"]
                assert owner.setdefault(doc, split) == split, \
                    f"{doc} が {owner[doc]} と {split} の両方に出ている（{directory.name}）"
        assert owner


@needs_sudachi
def test_typo_examples_match_the_spec(built):
    _cfg, paths, _pre, _kkc, typo, *_ = built
    assert typo["total"] > 0
    assert typo["clean"] > 0 and typo["noisy"] > 0
    examples = []
    for split in SPLITS:
        examples.extend(read_jsonl(paths.typo / f"{split}.jsonl"))
    for e in examples:
        for key in ("id", "source", "input", "target", "error_type"):
            assert key in e
        if e["error_type"] == "clean":
            assert e["input"] == e["target"]
        else:
            assert e["input"] != e["target"], e


@needs_sudachi
def test_stats_and_report_are_written(built):
    _cfg, paths, _pre, _kkc, _typo, stats, _samples = built
    assert paths.stats_json.exists()
    assert paths.report_md.exists()
    saved = json.loads(paths.stats_json.read_text(encoding="utf-8"))
    assert saved["canonical"]["total_sentences"] == stats["canonical"]["total_sentences"]
    report = paths.report_md.read_text(encoding="utf-8")
    for heading in ("原文（canonical）", "かな漢字変換データ", "typo normalizer データ",
                    "ソースとライセンス", "フィルタで捨てた文", "読み生成の失敗",
                    "重複除去"):
        assert heading in report, heading


@needs_sudachi
def test_samples_are_written_and_reproducible(built):
    cfg, paths, *_ = built
    for name in ("canonical_samples.txt", "kkc_samples.txt", "typo_samples.txt"):
        path = paths.samples / name
        assert path.exists() and path.read_text(encoding="utf-8").strip()
    before = (paths.samples / "typo_samples.txt").read_text(encoding="utf-8")
    run_samples(cfg, paths)
    assert (paths.samples / "typo_samples.txt").read_text(encoding="utf-8") == before


@needs_sudachi
def test_rebuild_is_reproducible(tmp_path):
    """同じ設定・同じ seed で 2 回作ると同じファイルになる。"""
    cfg = load_config(overrides=["typo.variants_per_clean_sample=3"])
    outputs = []
    for run in range(2):
        paths = Paths(tmp_path / f"out{run}", tmp_path / "raw").ensure()
        adapters = [FakeAdapter(cfg, paths)]
        run_preprocess(cfg, paths, adapters)
        KkcBuilder(cfg, paths).run(adapters)
        TypoBuilder(cfg, paths).run(adapters)
        outputs.append((
            (paths.kkc / "train.jsonl").read_text(encoding="utf-8"),
            (paths.typo / "train.jsonl").read_text(encoding="utf-8"),
        ))
    assert outputs[0] == outputs[1]
