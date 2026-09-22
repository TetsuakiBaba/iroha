"""かな漢字変換用データ（``build-kkc``）。KKC = Kana-Kanji Conversion。

canonical record から

    {"context": "本研究では", "input": "あたらしいにゅうりょくしゅほうを", "target": "新しい入力手法を"}

を作る。**かな全文 → 漢字全文** ではなく、IME の実利用に近い

    左側の確定済み文字列 + いま入力中のかな → いまの変換結果

の形にする。1 文から文節チャンクの数だけ example ができる。

context は「前の文（あれば）＋ この文でここまでに確定した文字列」を右から
``max_context_chars`` 文字に切ったもの。IME が実際に見るのはカーソル直前なので、
切るのは左側から（右端を残す）。
"""
from __future__ import annotations

from dataclasses import dataclass, field

from iroha_dataset.config import Config
from iroha_dataset.dedup import KeyDeduplicator
from iroha_dataset.jsonlio import SplitWriter, read_jsonl, write_json
from iroha_dataset.paths import Paths, SPLITS
from iroha_dataset.sources.base import SourceAdapter


@dataclass
class KkcReport:
    examples: dict = field(default_factory=dict)
    per_source: dict = field(default_factory=dict)
    skipped: dict = field(default_factory=dict)
    duplicates: int = 0
    input_chars: int = 0
    context_chars: int = 0
    target_chars: int = 0
    max_input_chars: int = 0
    max_context_chars: int = 0
    with_previous_context: int = 0

    def bump_skip(self, reason: str) -> None:
        self.skipped[reason] = self.skipped.get(reason, 0) + 1

    def as_dict(self) -> dict:
        total = sum(self.examples.values())
        return {
            "examples": dict(self.examples),
            "total": total,
            "per_source": self.per_source,
            "skipped": self.skipped,
            "duplicates_dropped": self.duplicates,
            "avg_input_chars": round(self.input_chars / total, 2) if total else 0.0,
            "avg_context_chars": round(self.context_chars / total, 2) if total else 0.0,
            "avg_target_chars": round(self.target_chars / total, 2) if total else 0.0,
            "max_input_chars": self.max_input_chars,
            "max_context_chars": self.max_context_chars,
            "with_previous_sentence_context": self.with_previous_context,
        }


def _truncate_left(text: str, max_chars: int) -> str:
    """右端（カーソル直前）を残して左を切る。"""
    if max_chars > 0 and len(text) > max_chars:
        return text[-max_chars:]
    return text


class KkcBuilder:
    def __init__(self, cfg: Config, paths: Paths):
        self.cfg = cfg
        self.paths = paths
        self.max_context_chars = int(cfg.get("context.max_context_chars", 256))
        self.use_previous = bool(cfg.get("kkc.use_previous_sentences", True))
        self.include_whole = bool(cfg.get("kkc.include_whole_sentence", False))
        self.skip_low_confidence = bool(cfg.get("kkc.skip_low_confidence", False))

    def examples_for(self, record: dict) -> list[dict]:
        chunks = record.get("chunks") or []
        if not chunks:
            return []
        base = record.get("previous_text", "") if self.use_previous else ""
        out: list[dict] = []
        committed = ""
        for i, chunk in enumerate(chunks):
            target = chunk.get("target", "")
            reading = chunk.get("reading", "")
            if not target or not reading:
                continue
            context = _truncate_left(base + committed, self.max_context_chars)
            # context は左から切るので、前の文が残るのは
            # 「切ったあとの長さ > この文で確定した分の長さ」のときだけ
            has_previous = bool(base) and len(context) > len(committed)
            out.append({
                "id": f"{record['id']}_c{i:02d}",
                "source": record["source"],
                "document_id": record["document_id"],
                "context": context,
                "input": reading,
                "target": target,
                "reading_confidence": record.get("reading_confidence", "high"),
                "license": record.get("license"),
                "_has_previous_context": has_previous,
            })
            committed += target
        if self.include_whole and len(chunks) > 1:
            whole_target = "".join(c["target"] for c in chunks)
            whole_reading = "".join(c["reading"] for c in chunks)
            out.append({
                "id": f"{record['id']}_whole",
                "source": record["source"],
                "document_id": record["document_id"],
                "context": _truncate_left(base, self.max_context_chars),
                "input": whole_reading,
                "target": whole_target,
                "reading_confidence": record.get("reading_confidence", "high"),
                "license": record.get("license"),
                "_has_previous_context": bool(base),
            })
        return out

    def run(self, adapters: list[SourceAdapter]) -> dict:
        report = KkcReport()
        dedup = KeyDeduplicator()
        with SplitWriter(self.paths.kkc, SPLITS) as writer:
            for adapter in adapters:
                path = self.paths.canonical_for(adapter.name)
                if not path.exists():
                    continue
                per_source = report.per_source.setdefault(adapter.name, {s: 0 for s in SPLITS})
                for record in read_jsonl(path):
                    if self.skip_low_confidence and record.get("reading_confidence") == "low":
                        report.bump_skip("low_confidence")
                        continue
                    split = record.get("split", "train")
                    for example in self.examples_for(record):
                        if dedup.is_duplicate(example["context"], example["input"],
                                              example["target"]):
                            continue
                        # 統計用の内部フラグは書き出さない
                        if example.pop("_has_previous_context", False):
                            report.with_previous_context += 1
                        writer.write(split, example)
                        report.examples[split] = report.examples.get(split, 0) + 1
                        per_source[split] = per_source.get(split, 0) + 1
                        report.input_chars += len(example["input"])
                        report.context_chars += len(example["context"])
                        report.target_chars += len(example["target"])
                        report.max_input_chars = max(report.max_input_chars, len(example["input"]))
                        report.max_context_chars = max(report.max_context_chars,
                                                       len(example["context"]))
        report.duplicates = dedup.dropped
        stats = report.as_dict()
        write_json(self.paths.stage_stats("kkc"), stats)
        return stats
