"""typo normalizer 用の「正しい読み」の一覧（``build-readings``）。

typo を学習中にオンザフライで付ける（training/typo-normalizer の ``OnTheFlyTypos``）ときは、
typo 付きの example（``build-typo``）ではなく clean の読みの集合だけあればよい。
canonical record から ``build-typo`` と同じ単位（文・文節）と長さで読みを取り出し、

* 既存の評価セットと同じ読みを除く（``typo.exclude_readings_from``）
* 打鍵列に戻せない読みを除く（オンザフライで typo を付けられない）
* 読みで全体の重複を除く（最初に出た split に残るので、同じ読みが train と test にまたがらない）

を行って ``<data_dir>/readings/{train,validation,test}.jsonl`` に 1 行 1 読みで書く:

    {"reading": "…", "unit": "sentence" | "chunk", "source": "…", "document_id": "…"}
"""
from __future__ import annotations

from iroha_dataset.config import Config
from iroha_dataset.dedup import KeyDeduplicator
from iroha_dataset.jsonlio import SplitWriter, read_jsonl, write_json
from iroha_dataset.paths import Paths, SPLITS
from iroha_dataset.sources.base import SourceAdapter
from iroha_dataset.typo.build import load_readings
from iroha_dataset.typo.romanize import RomanizeError, romanize_units


def _romanizable(reading: str) -> bool:
    try:
        romanize_units(reading, "nn")
        romanize_units(reading, "contextual")
    except RomanizeError:
        return False
    return True


class ReadingsBuilder:
    def __init__(self, cfg: Config, paths: Paths):
        typo = cfg.sub("typo")
        self.paths = paths
        self.units = list(typo.get("units", ["sentence"]))
        self.min_chars = int(typo.get("min_chars", 4))
        self.max_chars = int(typo.get("max_chars", 60))
        self.excluded, self.exclude_files = load_readings(
            list(typo.get("exclude_readings_from", []) or []))

    @property
    def out(self):
        return self.paths.root / "readings"

    def _readings(self, record: dict):
        if "sentence" in self.units:
            yield record.get("reading", ""), "sentence"
        if "chunk" in self.units:
            for chunk in record.get("chunks") or []:
                yield chunk.get("reading", ""), "chunk"

    def run(self, adapters: list[SourceAdapter]) -> dict:
        dedup = KeyDeduplicator()
        skipped = {"length": 0, "eval_reading": 0, "not_romanizable": 0, "duplicate": 0}
        per_source: dict[str, dict] = {}
        with SplitWriter(self.out, SPLITS) as writer:
            for adapter in adapters:
                path = self.paths.canonical_for(adapter.name)
                if not path.exists():
                    continue
                counts = per_source.setdefault(adapter.name, {"sentence": 0, "chunk": 0,
                                                              **{s: 0 for s in SPLITS}})
                for record in read_jsonl(path):
                    split = record.get("split", "train")
                    for reading, unit in self._readings(record):
                        if not (self.min_chars <= len(reading) <= self.max_chars):
                            skipped["length"] += 1
                            continue
                        if reading in self.excluded:
                            skipped["eval_reading"] += 1
                            continue
                        if dedup.is_duplicate(reading):
                            skipped["duplicate"] += 1
                            continue
                        if not _romanizable(reading):
                            skipped["not_romanizable"] += 1
                            continue
                        writer.write(split, {"reading": reading, "unit": unit,
                                             "source": record["source"],
                                             "document_id": record["document_id"]})
                        counts[unit] += 1
                        counts[split] += 1
            totals = writer.counts
        stats = {"readings": totals, "per_source": per_source, "skipped": skipped,
                 "units": self.units, "min_chars": self.min_chars, "max_chars": self.max_chars,
                 "exclude_readings_from": self.exclude_files}
        write_json(self.paths.stage_stats("readings"), stats)
        return stats
