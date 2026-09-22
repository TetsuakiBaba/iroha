"""文の品質フィルタ。

各ルールは「捨てる理由（文字列）」か None を返す。理由は stats で数えるので、
ルールを増やすときは REJECT_REASONS にも足して REPORT.md に出るようにする。

方針は「件数より教師ラベルの質」（README の設計方針）。迷ったら捨てる。
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field

from iroha_dataset.config import Config
from iroha_dataset.preprocess import normalize

_URL = re.compile(r"(?:https?://|www\.)\S+|\S+\.(?:com|net|org|jp|ac\.jp|co\.jp)(?:/\S*)?", re.I)
_HTML_TAG = re.compile(r"</?[a-zA-Z][a-zA-Z0-9]*(?:\s[^<>]*)?/?>")
_HTML_ENTITY = re.compile(r"&(?:[a-zA-Z]{2,10}|#\d{2,5}|#x[0-9a-fA-F]{2,5});")
_LATIN_RUN = re.compile(r"[A-Za-z]+")
_DIGIT = re.compile(r"[0-9]")
# 数式・記号だけの文の判定に使う「中身のある文字」
_SYMBOLS = set("+-*/=<>≦≧≠±×÷√∫∑∞()[]{}|~^_%$#@&\\\"'`:;,.!?・…‥〜ー：；，．！？＝＋－")

REJECT_REASONS = (
    "empty",
    "broken_unicode",
    "control_char",
    "html_fragment",
    "url",
    "too_short",
    "too_long",
    "latin_run",
    "digits",
    "low_japanese_ratio",
    "high_symbol_ratio",
    "symbols_only",
    "no_terminator_content",
)


@dataclass
class FilterStats:
    counts: dict[str, int] = field(default_factory=dict)
    kept: int = 0
    seen: int = 0

    def bump(self, reason: str) -> None:
        self.counts[reason] = self.counts.get(reason, 0) + 1

    def as_dict(self) -> dict:
        return {"seen": self.seen, "kept": self.kept,
                "rejected": dict(sorted(self.counts.items(), key=lambda kv: -kv[1]))}


class SentenceFilter:
    def __init__(self, cfg: Config):
        self.min_chars = int(cfg.get("filter.min_chars", 6))
        self.max_chars = int(cfg.get("filter.max_chars", 120))
        self.max_latin_run = int(cfg.get("filter.max_latin_run", 0))
        self.allow_digits = bool(cfg.get("filter.allow_digits", False))
        self.min_japanese_ratio = float(cfg.get("filter.min_japanese_ratio", 0.5))
        self.max_symbol_ratio = float(cfg.get("filter.max_symbol_ratio", 0.25))
        self.reject_urls = bool(cfg.get("filter.reject_urls", True))
        self.reject_html = bool(cfg.get("filter.reject_html", True))
        self.min_content_chars = int(cfg.get("filter.min_content_chars", 4))
        self.stats = FilterStats()

    def reject_reason(self, text: str) -> str | None:
        """捨てる理由。通るなら None。"""
        if not text or not text.strip():
            return "empty"
        if normalize.has_broken_unicode(text):
            return "broken_unicode"
        if any(ord(ch) < 0x20 and ch not in "\t" for ch in text):
            return "control_char"
        if self.reject_html and (_HTML_TAG.search(text) or _HTML_ENTITY.search(text)):
            return "html_fragment"
        if self.reject_urls and _URL.search(text):
            return "url"
        if len(text) < self.min_chars:
            return "too_short"
        if len(text) > self.max_chars:
            return "too_long"
        runs = _LATIN_RUN.findall(text)
        if runs and max(len(r) for r in runs) > self.max_latin_run:
            return "latin_run"
        if not self.allow_digits and _DIGIT.search(text):
            return "digits"
        content = [ch for ch in text if not ch.isspace() and ch not in _SYMBOLS]
        if len(content) < self.min_content_chars:
            return "symbols_only"
        symbol_ratio = sum(1 for ch in text if ch in _SYMBOLS) / len(text)
        if symbol_ratio > self.max_symbol_ratio:
            return "high_symbol_ratio"
        if normalize.japanese_ratio(text) < self.min_japanese_ratio:
            return "low_japanese_ratio"
        return None

    def accept(self, text: str) -> bool:
        self.stats.seen += 1
        reason = self.reject_reason(text)
        if reason is None:
            self.stats.kept += 1
            return True
        self.stats.bump(reason)
        return False
