"""typo normalizer 用コーパス（config/typo-corpus.yaml）のソースと、そのための仕組み。

LLM-jp Corpus v4 の 4 サブコーパス・zenz-v2.5 の Wikipedia、文書の間引き、評価セットの読みの除外。
"""
import gzip
import json
from collections import Counter

from iroha.config import PROJECT_ROOT, load_config
from iroha.paths import Paths
from iroha.preprocess.pipeline import sampled
from iroha.sources import get_adapter
from iroha.sources.llmjp import pick_evenly
from iroha.split import Splitter
from iroha.typo.build import TypoBuilder, load_readings

from tests.conftest import needs_sudachi


def _adapter(tmp_path, name, overrides=()):
    cfg = load_config(overrides=[f"sources.{name}.enabled=true", *overrides])
    return get_adapter(name, cfg, Paths(tmp_path / "out", tmp_path / "raw").ensure())


def _write_gz(path, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with gzip.open(path, "wt", encoding="utf-8") as fh:
        for r in rows:
            fh.write(json.dumps(r, ensure_ascii=False) + "\n")


def test_pick_evenly_spreads_over_the_files():
    names = [f"{i:04d}.jsonl.gz" for i in range(621)]
    picked = pick_evenly(names, 8)
    assert picked[0] == "0000.jsonl.gz" and len(picked) == 8
    gaps = {int(b[:4]) - int(a[:4]) for a, b in zip(picked, picked[1:])}
    assert gaps <= {77, 78}
    assert pick_evenly(names, 0) == names


def test_patent_drops_headings_figures_and_reference_numbers(tmp_path):
    a = _adapter(tmp_path, "llmjp_patent")
    text = ("(57)【要約】  本発明は、金属シート（110，700）を加熱する。【請求項１】\n"
            "ａ．基部（120）と、支持スタッド(130)と;\n【図１】\n0006770505.tif 000002")
    assert a.paragraphs(text) == ["本発明は、金属シートを加熱する。", "基部と、支持スタッドと;"]


def test_egov_drops_katakana_bungo_paragraphs(tmp_path):
    a = _adapter(tmp_path, "llmjp_egov")
    text = "第一条\n決闘ヲ挑ミタル者ハ六月以上二年以下ノ拘禁刑ニ処ス\nこの政令は、公布の日から施行する。"
    assert a.paragraphs(text) == ["この政令は、公布の日から施行する。"]


def test_aozora_keeps_only_new_kana_public_domain_works(tmp_path):
    a = _adapter(tmp_path, "llmjp_aozora")
    assert a.keep({"文字遣い種別": "新字新仮名", "作品著作権フラグ": "なし"})
    assert not a.keep({"文字遣い種別": "新字旧仮名", "作品著作権フラグ": "なし"})
    assert not a.keep({"文字遣い種別": "新字新仮名", "作品著作権フラグ": "あり"})


def test_llmjp_documents_read_gz_and_respect_max_documents(tmp_path):
    a = _adapter(tmp_path, "llmjp_kaken", ["sources.llmjp_kaken.max_documents=2"])
    _write_gz(a.raw_dir / "0000.jsonl.gz",
              [{"text": f"本研究では課題{i}を扱う。\n次に検討した。", "meta": {}} for i in range(5)])
    docs = list(a.documents())
    assert [d.document_id for d in docs] == ["llmjp_kaken_0000_000000", "llmjp_kaken_0000_000001"]
    assert docs[0].paragraphs == ["本研究では課題0を扱う。", "次に検討した。"]
    assert docs[0].license == "CC BY 4.0" and "LLM-jp Corpus v4" in docs[0].attribution


def test_zenz_wiki_groups_rows_and_drops_bench_overlaps(tmp_path):
    bench = tmp_path / "bench.jsonl"
    bench.write_text(json.dumps({"surface_noisy": "ために頑張ってると思う」と",
                                 "surface_clean": "ために頑張っていると思う」と"}, ensure_ascii=False) + "\n",
                     encoding="utf-8")
    a = _adapter(tmp_path, "zenz_wiki", ["sources.zenz_wiki.block_rows=2",
                                         f"sources.zenz_wiki.exclude_overlap_with=[{bench}]"])
    rows = [{"input": "ア", "output": "新宿駅周辺には", "left_context": None},
            {"input": "イ", "output": "彼はために頑張っていると思うと述べた", "left_context": None},
            {"input": "ウ", "output": "計画としていた", "left_context": "前に"}]
    a.raw_dir.mkdir(parents=True, exist_ok=True)
    (a.raw_dir / "train_wikipedia.jsonl").write_text(
        "".join(json.dumps(r, ensure_ascii=False) + "\n" for r in rows), encoding="utf-8")
    docs = list(a.documents())
    assert [(d.document_id, d.paragraphs) for d in docs] == [
        ("zenz_wiki_000000000", ["新宿駅周辺には"]), ("zenz_wiki_000000002", ["計画としていた"])]
    assert a.excluded_overlap == 1
    assert docs[0].license == "CC BY-SA 4.0"


def test_document_sampling_is_independent_of_the_split():
    """間引きと split は別のハッシュ。同じだと選んだ文書が train に偏る。"""
    ids = [f"doc_{i}" for i in range(20000)]
    picked = [d for d in ids if sampled(d, 0.3)]
    assert 0.28 < len(picked) / len(ids) < 0.32
    assert all(sampled(d, 1.0) for d in ids[:100]) and not any(sampled(d, 0.0) for d in ids[:100])
    splits = Counter(Splitter(load_config()).split_for(d) for d in picked)
    assert splits["validation"] > 0 and splits["test"] > 0


def test_load_readings_reads_clean_or_target(tmp_path):
    a = tmp_path / "a.jsonl"
    a.write_text('{"clean": "がっこう", "noisy": "gあっこう"}\n{"target": "しんぶん"}\n', encoding="utf-8")
    readings, counts = load_readings([str(a), str(tmp_path / "missing.jsonl")])
    assert readings == {"がっこう", "しんぶん"}
    assert counts == {str(a): 2, str(tmp_path / "missing.jsonl"): -1}


def test_build_typo_skips_eval_readings(tmp_path):
    held = tmp_path / "held.jsonl"
    held.write_text('{"clean": "しんぶんをよむ"}\n', encoding="utf-8")
    cfg = load_config(overrides=["typo.units=[sentence]", "typo.variants_per_clean_sample=1",
                                 f"typo.exclude_readings_from=[{held}]"])
    paths = Paths(tmp_path / "out", tmp_path / "raw").ensure()
    rows = [{"id": f"r{i}", "source": "tatoeba", "document_id": f"d{i}", "reading": r, "chunks": [],
             "split": "train"} for i, r in enumerate(["しんぶんをよむ", "がっこうにいく"])]
    paths.canonical_for("tatoeba").write_text(
        "".join(json.dumps(r, ensure_ascii=False) + "\n" for r in rows), encoding="utf-8")
    stats = TypoBuilder(cfg, paths).run([get_adapter("tatoeba", cfg, paths)])
    targets = {json.loads(l)["target"] for l in (paths.typo / "train.jsonl").open(encoding="utf-8")}
    assert targets == {"がっこうにいく"}
    assert stats["skipped"]["eval_reading"] == 1


def test_typo_corpus_config_loads():
    cfg = load_config(PROJECT_ROOT / "config" / "typo-corpus.yaml")
    enabled = {n for n in ("zenz_wiki", "llmjp_kaken", "llmjp_egov", "llmjp_patent", "llmjp_aozora",
                           "tatoeba", "kaken", "aozora") if cfg.get(f"sources.{n}.enabled")}
    assert enabled == {"zenz_wiki", "llmjp_kaken", "llmjp_egov", "llmjp_patent", "llmjp_aozora"}
    assert cfg.get("typo.variants_per_clean_sample") == 1


@needs_sudachi
def test_llmjp_source_goes_through_preprocess(tmp_path):
    from iroha.preprocess.pipeline import run_preprocess
    a = _adapter(tmp_path, "llmjp_egov")
    _write_gz(a.raw_dir / "0000.jsonl.gz", [{"text": "第一条\n決闘ヲ挑ミタル者ハ処ス\nこの政令は、公布の日から施行する。",
                                              "meta": {"LawNum": "令和六年政令第六号"}}])
    stats = run_preprocess(a.cfg, a.paths, [a])
    rec = json.loads(a.paths.canonical_for("llmjp_egov").read_text(encoding="utf-8").splitlines()[0])
    assert rec["reading"] == "このせいれいは、こうふのひからしこうする"
    assert "令和六年政令第六号" in rec["attribution"]
    assert stats["sources"]["llmjp_egov"]["records"] == 1


def test_build_readings_excludes_eval_and_dedups_across_splits(tmp_path):
    from iroha.typo.readings import ReadingsBuilder
    held = tmp_path / "held.jsonl"
    held.write_text('{"clean": "しんぶんをよむ"}\n', encoding="utf-8")
    cfg = load_config(overrides=["typo.units=[sentence, chunk]", "typo.min_chars=4",
                                 f"typo.exclude_readings_from=[{held}]"])
    paths = Paths(tmp_path / "out", tmp_path / "raw").ensure()
    rows = [
        {"id": "a", "source": "tatoeba", "document_id": "d1", "split": "train",
         "reading": "がっこうにいく", "chunks": [{"reading": "がっこうに"}, {"reading": "いく"}]},
        {"id": "b", "source": "tatoeba", "document_id": "d2", "split": "test",
         "reading": "しんぶんをよむ", "chunks": [{"reading": "がっこうに"}]},
    ]
    paths.canonical_for("tatoeba").write_text(
        "".join(json.dumps(r, ensure_ascii=False) + "\n" for r in rows), encoding="utf-8")
    b = ReadingsBuilder(cfg, paths)
    stats = b.run([get_adapter("tatoeba", cfg, paths)])
    train = [json.loads(l) for l in (b.out / "train.jsonl").open(encoding="utf-8")]
    test = [json.loads(l) for l in (b.out / "test.jsonl").open(encoding="utf-8")]
    assert [(r["reading"], r["unit"]) for r in train] == [("がっこうにいく", "sentence"), ("がっこうに", "chunk")]
    assert test == []          # しんぶんをよむ は評価セット、がっこうに は train に既出
    assert stats["skipped"] == {"length": 1, "eval_reading": 1, "not_romanizable": 0, "duplicate": 1}
