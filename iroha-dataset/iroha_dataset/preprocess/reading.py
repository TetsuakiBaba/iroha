"""Sudachi による読み生成と、その信頼度の判定。

**Sudachi の読みを 100% 正解とは仮定しない。** ここでやることは 3 つ。

1. 形態素ごとの surface / reading / normalized_form / POS / OOV を残す
2. 読みがひらがなに落ちない・取れない語を含む文は捨てる
3. 怪しいものに ``reading_confidence = "low"`` を立てる
   （別分割での読みとの不一致、固有名詞、数詞、表層そのままの読み、長さ比）

「別解析器との一致確認」は SecondOpinion プロトコルで後から足せる。同梱のものは
Sudachi の別分割（mode A）との突き合わせと、Tatoeba の jpn_indices（田中コーパス
由来の語＋読み注記）。
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Iterable, Protocol, runtime_checkable

from iroha_dataset.config import Config
from iroha_dataset.preprocess import normalize

_SPLIT_MODES = {"A": "A", "B": "B", "C": "C"}

# 読みとして通すひらがな以外の文字は config（reading.allow_extra_chars）で決める
DEFAULT_EXTRA = "ー、。・「」"

# 読みを持たなくてよい品詞（記号・空白）。表層をそのまま読みにする
_NO_READING_POS = ("補助記号", "記号", "空白")


@dataclass
class Morpheme:
    surface: str
    reading: str            # ひらがな
    normalized_form: str
    pos: list[str]
    is_oov: bool

    def as_dict(self) -> dict:
        return {
            "surface": self.surface,
            "reading": self.reading,
            "normalized_form": self.normalized_form,
            "pos": "/".join(self.pos),
            "oov": self.is_oov,
        }


@dataclass
class ReadingResult:
    ok: bool
    reading: str = ""
    morphemes: list[Morpheme] = field(default_factory=list)
    has_oov: bool = False
    confidence: str = "high"
    reasons: list[str] = field(default_factory=list)
    failure: str = ""       # ok=False のときの理由

    @property
    def low_confidence(self) -> bool:
        return self.confidence == "low"


@runtime_checkable
class SecondOpinion(Protocol):
    """別の情報源で読みを突き合わせる。理由の一覧を返す（空なら異議なし）。"""

    name: str

    def check(self, text: str, result: ReadingResult, meta: dict) -> list[str]:
        ...


class ModeSecondOpinion:
    """Sudachi の別の分割単位で読みを作り、一致するかを見る。

    複合語の読みは分割単位で変わりうる（東京都立大学 = C なら 1 語、A なら 3 語）。
    一致しないときは、どちらかが外れている可能性が高いので low にする。
    """

    def __init__(self, tokenizer, mode, extra_chars: str):
        self.name = "sudachi_mode"
        self._tokenizer = tokenizer
        self._mode = mode
        self._extra = set(extra_chars)

    def check(self, text: str, result: ReadingResult, meta: dict) -> list[str]:
        try:
            morphemes = self._tokenizer.tokenize(text, self._mode)
        except Exception:  # pragma: no cover - Sudachi 内部エラーは異議なしにする
            return []
        parts = []
        for m in morphemes:
            r = _morpheme_reading(m)
            if r is None:
                return []
            parts.append(r)
        other = "".join(parts)
        if other != result.reading:
            return [f"mode_disagreement:{other}"]
        return []


def _morpheme_reading(m) -> str | None:
    """形態素の読みをひらがなで返す。取れなければ None。"""
    pos = m.part_of_speech()
    surface = m.surface()
    # 記号・空白は表層をそのまま読みにする（。、・ など）。Sudachi は（）『』〜 / ♪ 全角空白などに
    # reading_form = キゴウ を返すので、reading_form より先に見る（採ると「司法全般(警察」が
    # しほうぜんぱんきごうけいさつ になる）。許可文字以外の記号は is_valid_reading で文ごと落ちる
    if pos and pos[0] in _NO_READING_POS:
        return surface
    reading = m.reading_form() or ""
    if not reading:
        return None
    return normalize.katakana_to_hiragana(reading)


def _surface_is_kana_only(surface: str) -> bool:
    return all(normalize.is_hiragana(c) or normalize.is_katakana(c) or c in "ー" for c in surface)


class ReadingAnalyzer:
    """1 文 → 読み。Sudachi の辞書は最初の 1 回だけ読み込む。"""

    def __init__(self, cfg: Config, second_opinions: Iterable[SecondOpinion] = ()):
        from sudachipy import Dictionary, SplitMode  # 遅延 import（テストで無くても読める）

        dict_kind = str(cfg.get("reading.dictionary", "full"))
        self._dictionary = Dictionary(dict=dict_kind)
        self._tokenizer = self._dictionary.create()
        mode_name = str(cfg.get("reading.mode", "C")).upper()
        if mode_name not in _SPLIT_MODES:
            raise ValueError(f"reading.mode は A/B/C: {mode_name!r}")
        self._mode = getattr(SplitMode, mode_name)
        self.extra_chars = set(cfg.get("reading.allow_extra_chars", DEFAULT_EXTRA))
        self.reject_oov = bool(cfg.get("reading.reject_oov", True))
        low = cfg.sub("reading.low_confidence")
        self._low_on_disagreement = bool(low.get("on_mode_disagreement", True))
        self._low_on_proper = bool(low.get("on_proper_noun", True))
        self._low_on_numeral = bool(low.get("on_numeral", True))
        self._low_on_fallback = bool(low.get("on_surface_fallback", True))
        self._max_ratio = float(low.get("max_reading_surface_ratio", 3.0))

        self.second_opinions: list[SecondOpinion] = list(second_opinions)
        second_mode = cfg.get("reading.second_opinion_mode")
        if second_mode and self._low_on_disagreement:
            name = str(second_mode).upper()
            if name in _SPLIT_MODES and name != mode_name:
                self.second_opinions.append(
                    ModeSecondOpinion(self._tokenizer, getattr(SplitMode, name), "".join(self.extra_chars))
                )
        self.failures: dict[str, int] = {}
        self.low_reasons: dict[str, int] = {}

    def _bump_failure(self, reason: str) -> None:
        self.failures[reason] = self.failures.get(reason, 0) + 1

    def _bump_low(self, reason: str) -> None:
        key = reason.split(":", 1)[0]
        self.low_reasons[key] = self.low_reasons.get(key, 0) + 1

    def tokenize(self, text: str, mode: str | None = None) -> list[Morpheme]:
        """mode（"A"/"B"/"C"）を渡すと既定の分割単位の代わりにそれを使う。"""
        split_mode = self._mode
        if mode is not None:
            from sudachipy import SplitMode
            split_mode = getattr(SplitMode, _SPLIT_MODES[mode.upper()])
        out = []
        for m in self._tokenizer.tokenize(text, split_mode):
            reading = _morpheme_reading(m)
            out.append(Morpheme(
                surface=m.surface(),
                reading=reading if reading is not None else "",
                normalized_form=m.normalized_form(),
                pos=list(m.part_of_speech()),
                is_oov=m.is_oov(),
            ))
        return out

    def is_valid_reading(self, reading: str) -> bool:
        return all(normalize.is_hiragana(ch) or ch in self.extra_chars for ch in reading)

    def analyze(self, text: str, meta: dict | None = None) -> ReadingResult:
        meta = meta or {}
        try:
            morphemes = self.tokenize(text)
        except Exception as e:  # pragma: no cover
            self._bump_failure("tokenize_error")
            return ReadingResult(ok=False, failure=f"tokenize_error:{e}")

        if not morphemes:
            self._bump_failure("no_morphemes")
            return ReadingResult(ok=False, failure="no_morphemes")

        reasons: list[str] = []
        has_oov = False
        parts: list[str] = []
        for m in morphemes:
            if m.is_oov:
                has_oov = True
            if not m.reading:
                self._bump_failure("no_reading")
                return ReadingResult(ok=False, failure=f"no_reading:{m.surface}")
            parts.append(m.reading)

            # 表層に漢字があるのに読みが表層のまま = 辞書に読みが無い（推定された）
            if self._low_on_fallback and m.reading == m.surface and any(
                    normalize.is_kanji(c) for c in m.surface):
                reasons.append(f"surface_fallback:{m.surface}")
            if self._low_on_proper and len(m.pos) > 1 and m.pos[1] == "固有名詞":
                reasons.append(f"proper_noun:{m.surface}")
            if self._low_on_numeral and len(m.pos) > 1 and m.pos[1] == "数詞":
                reasons.append(f"numeral:{m.surface}")
            if (self._max_ratio > 0 and not _surface_is_kana_only(m.surface)
                    and len(m.surface) > 0
                    and len(m.reading) / len(m.surface) > self._max_ratio):
                reasons.append(f"long_reading:{m.surface}->{m.reading}")

        if has_oov and self.reject_oov:
            self._bump_failure("oov")
            return ReadingResult(ok=False, has_oov=True, failure="oov")

        reading = "".join(parts)
        # 「読みと surface の対応が明らかに不自然」— 読みが取れても、ひらがなに
        # 落ちない文字（ラテン文字・数字など）が混ざっていたら教師にできない
        if not self.is_valid_reading(reading):
            bad = "".join(sorted({ch for ch in reading if not (
                normalize.is_hiragana(ch) or ch in self.extra_chars)}))
            self._bump_failure("non_kana_reading")
            return ReadingResult(ok=False, failure=f"non_kana_reading:{bad}")
        if not reading:
            self._bump_failure("empty_reading")
            return ReadingResult(ok=False, failure="empty_reading")

        result = ReadingResult(
            ok=True, reading=reading, morphemes=morphemes, has_oov=has_oov,
        )
        for opinion in self.second_opinions:
            reasons.extend(opinion.check(text, result, meta))

        if reasons:
            result.confidence = "low"
            result.reasons = reasons
            for r in reasons:
                self._bump_low(r)
        return result
