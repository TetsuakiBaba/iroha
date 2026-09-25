"""canonical record の作成（``preprocess`` コマンド）。

各ソースの Document を

  段落 → 文分割 → 正規化 → 品質フィルタ → 重複除去 → 読み生成 → 文節チャンク

の順に通し、``data/canonical/<source>.jsonl`` に 1 文 1 行で書く。

canonical record は「この文の教師ラベルとその由来」をすべて持つ。下流（kkc / typo）は
Sudachi を使わずにここから作れる（そのためにチャンクも record に入れてある）。
split は document_id で決まるので、この時点で record に入れておけば下流で混ざらない。
"""
from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass, field
from typing import Iterator

from iroha.config import Config
from iroha.dedup import Deduplicator
from iroha.jsonlio import JsonlWriter, write_json
from iroha.paths import Paths
from iroha.preprocess.chunking import Chunker
from iroha.preprocess.filtering import SentenceFilter
from iroha.preprocess.normalize import normalize_text
from iroha.preprocess.reading import ReadingAnalyzer
from iroha.preprocess.sentence import split_sentences, strip_terminator
from iroha.sources.base import Document, SourceAdapter
from iroha.split import Splitter


@dataclass
class SourceReport:
    source: str
    documents: int = 0
    skipped_documents: int = 0
    paragraphs: int = 0
    sentences_seen: int = 0
    records: int = 0
    low_confidence: int = 0
    has_oov: int = 0
    filtered: dict = field(default_factory=dict)
    reading_failures: dict = field(default_factory=dict)
    low_reasons: dict = field(default_factory=dict)
    duplicates: dict = field(default_factory=dict)
    splits: dict = field(default_factory=dict)
    license_counts: dict = field(default_factory=dict)
    extraction: dict | None = None

    def as_dict(self) -> dict:
        out = {
            "source": self.source,
            "documents": self.documents,
            "skipped_documents": self.skipped_documents,
            "paragraphs": self.paragraphs,
            "sentences_seen": self.sentences_seen,
            "records": self.records,
            "low_confidence": self.low_confidence,
            "has_oov": self.has_oov,
            "filtered": self.filtered,
            "reading_failures": self.reading_failures,
            "low_confidence_reasons": self.low_reasons,
            "duplicates": self.duplicates,
            "splits": self.splits,
            "licenses": self.license_counts,
        }
        if self.extraction is not None:
            out["extraction"] = self.extraction
        return out


def sampled(document_id: str, ratio: float) -> bool:
    """文書を ratio の割合で選ぶ（document_id で決まるので再実行で同じになる）。

    split（``Splitter``）とは別の塩でハッシュする。同じハッシュで選ぶと、選んだ文書が
    split の境界の片側（train）に偏る。
    """
    if ratio >= 1.0:
        return True
    h = int.from_bytes(hashlib.blake2b(f"sample:{document_id}".encode(), digest_size=8).digest(), "big")
    return h / 2**64 < ratio


def _previous_text(previous: list[str], max_sentences: int, max_chars: int) -> str:
    """直前の文を新しいものから max_sentences 文・max_chars 文字まで。"""
    if max_sentences <= 0 or not previous:
        return ""
    picked = previous[-max_sentences:]
    text = "".join(picked)
    if max_chars > 0 and len(text) > max_chars:
        text = text[-max_chars:]
    return text


class CanonicalBuilder:
    def __init__(self, cfg: Config, paths: Paths):
        self.cfg = cfg
        self.paths = paths
        self.splitter = Splitter(cfg)
        self.chunker = Chunker(cfg)
        self.max_prev_sentences = int(cfg.get("context.max_previous_sentences", 2))
        self.max_context_chars = int(cfg.get("context.max_context_chars", 256))
        self.record_morphemes = bool(cfg.get("reading.record_morphemes", True))
        self.drop_low_confidence = bool(cfg.get("reading.drop_low_confidence", False))

    def build_source(self, adapter: SourceAdapter) -> SourceReport:
        cfg = self.cfg
        report = SourceReport(source=adapter.name)
        sentence_filter = SentenceFilter(cfg)
        analyzer = ReadingAnalyzer(cfg, second_opinions=adapter.second_opinions())
        dedup = Deduplicator(cfg)

        out_path = self.paths.canonical_for(adapter.name)
        morph_path = self.paths.morphemes_for(adapter.name)
        with JsonlWriter(out_path) as writer:
            morph_writer = JsonlWriter(morph_path) if self.record_morphemes else None
            if morph_writer is not None:
                morph_writer.__enter__()
            try:
                for record, morphemes in self._records(adapter, sentence_filter, analyzer,
                                                       dedup, report):
                    writer.write(record)
                    if morph_writer is not None:
                        morph_writer.write(morphemes)
            finally:
                if morph_writer is not None:
                    morph_writer.__exit__(None, None, None)

        report.records = report.records
        report.filtered = sentence_filter.stats.as_dict()
        report.reading_failures = dict(sorted(analyzer.failures.items(), key=lambda kv: -kv[1]))
        report.low_reasons = dict(sorted(analyzer.low_reasons.items(), key=lambda kv: -kv[1]))
        report.duplicates = dedup.stats.as_dict()
        report.extraction = adapter.extraction_stats()
        return report

    def _records(self, adapter: SourceAdapter, sentence_filter: SentenceFilter,
                 analyzer: ReadingAnalyzer, dedup: Deduplicator,
                 report: SourceReport) -> Iterator[tuple[dict, dict]]:
        # 大きいソースは文書単位で間引く（sources.<name>.document_ratio。1.0 = 全部）
        ratio = float(adapter.source_cfg.get("document_ratio", 1.0))
        for doc in adapter.documents():
            if not sampled(doc.document_id, ratio):
                report.skipped_documents += 1
                continue
            report.documents += 1
            split = self.splitter.split_for(doc.document_id)
            sentence_index = 0
            for paragraph in doc.paragraphs:
                report.paragraphs += 1
                # 左文脈は段落の中だけで繋ぐ（別の段落の文は前文として使わない）
                previous: list[str] = []
                text = normalize_text(paragraph)
                if not text:
                    continue
                for raw_sentence in split_sentences(text):
                    report.sentences_seen += 1
                    sentence = normalize_text(raw_sentence)
                    if not sentence_filter.accept(sentence):
                        continue
                    if dedup.is_duplicate(sentence):
                        continue
                    # 読みは文末の句点を含めない（IME では句点を打たずに確定することも多い）
                    body = strip_terminator(sentence)
                    if not body:
                        continue
                    result = analyzer.analyze(body, meta=doc.meta)
                    if not result.ok:
                        continue
                    if result.low_confidence and self.drop_low_confidence:
                        continue
                    chunks = self.chunker.chunk(result.morphemes)
                    if not chunks:
                        continue
                    record_id = f"{doc.document_id}_{sentence_index:04d}"
                    record = {
                        "id": record_id,
                        "source": doc.source,
                        "document_id": doc.document_id,
                        "sentence_index": sentence_index,
                        "text": sentence,
                        "reading": result.reading,
                        "previous_text": _previous_text(previous, self.max_prev_sentences,
                                                        self.max_context_chars),
                        "license": doc.license,
                        "attribution": doc.attribution,
                        "url": doc.url,
                        "has_oov": result.has_oov,
                        "reading_confidence": result.confidence,
                        "confidence_reasons": result.reasons,
                        "chunks": [c.as_dict() for c in chunks],
                        "split": split,
                    }
                    morphemes = {"id": record_id,
                                 "morphemes": [m.as_dict() for m in result.morphemes]}
                    report.records += 1
                    if result.low_confidence:
                        report.low_confidence += 1
                    if result.has_oov:
                        report.has_oov += 1
                    report.splits[split] = report.splits.get(split, 0) + 1
                    report.license_counts[doc.license] = report.license_counts.get(doc.license, 0) + 1
                    yield record, morphemes
                    previous.append(sentence)
                    sentence_index += 1


def run_preprocess(cfg: Config, paths: Paths, adapters: list[SourceAdapter]) -> dict:
    """ソースごとに ``_stats.preprocess.<source>.json`` を書き、全体の ``_stats.preprocess.json`` は
    有効なソースのうち統計のあるものを合わせて作る（``--source`` で分けて並列に回せるように）。"""
    from iroha.sources import enabled_adapters
    builder = CanonicalBuilder(cfg, paths)
    for adapter in adapters:
        report = builder.build_source(adapter)
        write_json(paths.stage_stats(f"preprocess.{adapter.name}"), report.as_dict())
    names = [a.name for a in adapters]
    names += [a.name for a in enabled_adapters(cfg, paths) if a.name not in names]
    reports = {}
    for name in names:
        p = paths.stage_stats(f"preprocess.{name}")
        if p.exists():
            reports[name] = json.loads(p.read_text(encoding="utf-8"))
    stats = {"sources": reports,
             "totals": {
                 "records": sum(r["records"] for r in reports.values()),
                 "documents": sum(r["documents"] for r in reports.values()),
                 "low_confidence": sum(r["low_confidence"] for r in reports.values()),
             }}
    write_json(paths.stage_stats("preprocess"), stats)
    return stats
