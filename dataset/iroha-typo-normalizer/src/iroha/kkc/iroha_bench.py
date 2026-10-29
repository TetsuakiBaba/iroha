"""かな漢字変換モデルの評価セット（``build-iroha-bench``）。

canonical record から AJIMEE-Bench（evaluation_items.json）と同じ形式の項目を作る:

    {"index": "llmjp_kaken-0001", "context_text": "…", "input": "カタカナノヨミ",
     "expected_output": ["正解"], "source": "llmjp_kaken", …}

``iroha-cli ajimee <この JSON>`` でそのまま測れる。index の先頭がソース名なので、
誤答の行（``✗ [index]``）からソースごとの正解率を数えられる。

1 文から 1 項目だけ作る。文節チャンクの連続した範囲を、読みが ``min_input_chars`` 〜
``max_input_chars`` 字に収まるように選ぶ（AJIMEE-Bench と同じく文の途中から始まることがある）。
左文脈は ``context_ratio`` の割合で付け、「前の文 + この文で範囲より前の文字列」の右端を残す。

正解は 1 つだけ（AJIMEE-Bench のような表記の許容解は無い）。表記の揺れ（「行う / おこなう」）も
誤答に数えるので、正解率の絶対値は AJIMEE-Bench より低く出る。モデルどうしの比較に使う。

``exclude_overlap_with`` に学習データ（training/train-full.txt）を渡すと、正解の文字列が
そのまま学習データのどこかに出てくる項目を除く（法令・科研費の概要などは Web にあり、
Common Crawl 由来の学習データに入っていることがある）。探すのは ripgrep（``rg -F``）。
"""
from __future__ import annotations

import json
import random
import shutil
import subprocess
import tempfile
from pathlib import Path

from iroha.config import Config
from iroha.jsonlio import read_jsonl, write_json
from iroha.paths import Paths
from iroha.sources.base import SourceAdapter


def to_katakana(text: str) -> str:
    return "".join(chr(ord(c) + 0x60) if "ぁ" <= c <= "ゖ" else c for c in text)


class IrohaBenchBuilder:
    def __init__(self, cfg: Config, paths: Paths):
        self.cfg = cfg
        self.paths = paths
        self.per_source = int(cfg.get("iroha_bench.items_per_source", 500))
        self.per_document = int(cfg.get("iroha_bench.items_per_document", 2))
        self.min_input = int(cfg.get("iroha_bench.min_input_chars", 8))
        self.max_input = int(cfg.get("iroha_bench.max_input_chars", 40))
        self.context_ratio = float(cfg.get("iroha_bench.context_ratio", 0.5))
        self.max_context = int(cfg.get("iroha_bench.max_context_chars", 40))
        self.allowed_low = set(cfg.get("iroha_bench.allow_low_confidence_reasons", []) or [])
        self.overlap_with = cfg.get("iroha_bench.exclude_overlap_with", None)
        self.seed = int(cfg.get("seed", 42))
        self.name = str(cfg.get("iroha_bench.name", "iroha-bench-v1"))

    def _usable(self, record: dict) -> bool:
        if record.get("reading_confidence", "high") == "high":
            return True
        # 理由は「proper_noun:東京」のように語が付く
        kinds = {r.split(":", 1)[0] for r in record.get("confidence_reasons") or []}
        return bool(kinds) and kinds <= self.allowed_low

    def _span(self, chunks: list[dict], rng: random.Random) -> tuple[int, int] | None:
        """読みの長さが範囲に収まる連続したチャンクの範囲 [s, e) を 1 つ選ぶ。"""
        starts = list(range(len(chunks)))
        rng.shuffle(starts)
        for s in starts:
            length = 0
            ends = []
            for e in range(s, len(chunks)):
                length += len(chunks[e]["reading"])
                if length > self.max_input:
                    break
                if length >= self.min_input:
                    ends.append(e + 1)
            if ends:
                return s, rng.choice(ends)
        return None

    def item_for(self, record: dict, rng: random.Random) -> dict | None:
        chunks = [c for c in (record.get("chunks") or []) if c.get("target") and c.get("reading")]
        if not chunks:
            return None
        span = self._span(chunks, rng)
        if span is None:
            return None
        s, e = span
        context = ""
        if rng.random() < self.context_ratio:
            context = (record.get("previous_text", "") + "".join(c["target"] for c in chunks[:s]))
            context = context[-self.max_context:] if self.max_context > 0 else context
        return {
            "context_text": context,
            "input": to_katakana("".join(c["reading"] for c in chunks[s:e])),
            "expected_output": ["".join(c["target"] for c in chunks[s:e])],
            "original_text": record.get("text", ""),
            "splitted_input_for_limited_input_length": [],
            "source": record["source"],
            "document_id": record["document_id"],
            "record_id": record["id"],
            "reading_confidence": record.get("reading_confidence", "high"),
        }

    def _items_for_source(self, adapter: SourceAdapter, report: dict) -> list[dict]:
        path = self.paths.canonical_for(adapter.name)
        if not path.exists():
            return []
        rng = random.Random(f"{self.seed}-{adapter.name}")
        records = [r for r in read_jsonl(path) if self._usable(r)]
        rng.shuffle(records)
        per_document: dict[str, int] = {}
        seen_targets: set[str] = set()
        items: list[dict] = []
        for record in records:
            if len(items) >= self.per_source:
                break
            doc = record["document_id"]
            if per_document.get(doc, 0) >= self.per_document:
                continue
            item = self.item_for(record, rng)
            if item is None or item["expected_output"][0] in seen_targets:
                continue
            seen_targets.add(item["expected_output"][0])
            per_document[doc] = per_document.get(doc, 0) + 1
            items.append(item)
        report[adapter.name] = {"usable_records": len(records), "items": len(items),
                                "documents": len(per_document)}
        return items

    def _overlapping_targets(self, targets: list[str], corpus: Path) -> set[str]:
        """corpus のどこかにそのまま出てくる正解の集合。"""
        rg = shutil.which("rg")
        if rg is None:
            raise SystemExit("exclude_overlap_with には ripgrep（rg）が要る")
        with tempfile.NamedTemporaryFile("w", encoding="utf-8", suffix=".txt", delete=False) as f:
            f.write("\n".join(sorted(set(targets))) + "\n")
            patterns = f.name
        try:
            out = subprocess.run([rg, "-F", "-o", "--no-filename", "--no-line-number",
                                  "-f", patterns, str(corpus)],
                                 capture_output=True, text=True, check=False)
        finally:
            Path(patterns).unlink(missing_ok=True)
        if out.returncode not in (0, 1):
            raise SystemExit(f"rg が失敗した: {out.stderr}")
        found = set(out.stdout.splitlines())
        # -o は同じ位置で重なる候補のうち 1 つしか出さないので、見つかった文字列に含まれる正解も重なりとみなす
        return {t for t in targets if t in found or any(t in f for f in found)}

    def run(self, adapters: list[SourceAdapter]) -> dict:
        report: dict = {"per_source": {}}
        items: list[dict] = []
        for adapter in adapters:
            items += self._items_for_source(adapter, report["per_source"])

        if self.overlap_with:
            corpus = Path(self.overlap_with).expanduser()
            if not corpus.is_absolute():
                corpus = (Path.cwd() / corpus).resolve()
            overlapping = self._overlapping_targets([i["expected_output"][0] for i in items], corpus)
            dropped: dict[str, int] = {}
            kept = []
            for item in items:
                if item["expected_output"][0] in overlapping:
                    dropped[item["source"]] = dropped.get(item["source"], 0) + 1
                else:
                    kept.append(item)
            items = kept
            report["overlap"] = {"corpus": str(corpus), "dropped": dropped}

        counters: dict[str, int] = {}
        out = []
        for item in items:
            counters[item["source"]] = counters.get(item["source"], 0) + 1
            out.append({"index": f"{item['source']}-{counters[item['source']]:04d}", **item})
        report["items"] = counters
        report["total"] = len(out)
        report["with_context"] = sum(1 for i in out if i["context_text"])
        report["avg_input_chars"] = round(sum(len(i["input"]) for i in out) / max(len(out), 1), 2)

        target_dir = self.paths.root / self.name
        target_dir.mkdir(parents=True, exist_ok=True)
        with open(target_dir / "evaluation_items.json", "w", encoding="utf-8") as f:
            json.dump(out, f, ensure_ascii=False, indent=1)
        write_json(target_dir / "stats.json", report)
        return report
