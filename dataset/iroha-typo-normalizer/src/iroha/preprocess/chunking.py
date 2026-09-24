"""文を「IME で 1 回に入力・変換しそうな単位」へ切る。

Sudachi の形態素をまず文節にまとめ、そのあと連体修飾の文節を後ろにくっつける。
形態素そのままでは細かすぎる（「新しい」「入力」「手法を」）ので、

  本研究では / 新しい入力手法を / 提案する

くらいの粒度になるようにしてある。手順:

1. 自立語で新しい文節を始める。助詞・助動詞・接尾辞・非自立語・記号は前にくっつける。
   接頭辞は後ろにくっつける（本 + 研究 → 本研究）
2. 助詞・助動詞で終わっていない文節（= 連体修飾や裸の名詞）を次の文節に合流させる
   （chunk.merge_modifiers）。「新しい」+「入力手法を」→「新しい入力手法を」
3. min_chars に届かない文節を後ろ（無ければ前）に寄せる
4. max_chars を超える文節を形態素境界で割る
"""
from __future__ import annotations

from dataclasses import dataclass

from iroha.config import Config
from iroha.preprocess.reading import Morpheme
from iroha.preprocess.sentence import TERMINATORS, TRAILING

# 自立語（ここから新しい文節が始まる）
INDEPENDENT_POS = {"名詞", "動詞", "形容詞", "形状詞", "副詞", "連体詞", "接続詞",
                   "感動詞", "代名詞", "接頭辞"}
# 前の文節にくっつく
ATTACH_POS = {"助詞", "助動詞", "接尾辞", "補助記号", "空白", "記号"}
# 「非自立可能」の動詞（する・いる・ある）は前にくっつける
NON_INDEPENDENT_SUBPOS = {"非自立可能", "非自立"}
# 文節の終わりとして「自然」な品詞（ここで終わっていれば連体修飾ではない）
CLOSING_POS = {"助詞", "助動詞", "補助記号"}
# 助詞の直後に来るかなだけの用言は前にくっつける。
# 「に + つい + て」→「について」、「と + し + て」→「として」のような複合辞を
# 1 つの入力単位として扱うため（IME では区切らずに打つ）
_CONTINUATION_POS = {"動詞", "形容詞", "助動詞"}


@dataclass
class Chunk:
    target: str
    reading: str
    morphemes: list[Morpheme]

    def as_dict(self) -> dict:
        return {"target": self.target, "reading": self.reading}


def _is_kana_only(surface: str) -> bool:
    from iroha.preprocess import normalize
    return bool(surface) and all(
        normalize.is_hiragana(c) or normalize.is_katakana(c) or c == "ー" for c in surface)


def _starts_new_segment(m: Morpheme, prev: Morpheme | None) -> bool:
    pos0 = m.pos[0] if m.pos else ""
    pos1 = m.pos[1] if len(m.pos) > 1 else ""
    if pos0 in ATTACH_POS:
        return False
    if (prev is not None and prev.pos and prev.pos[0] == "助詞"
            and pos0 in _CONTINUATION_POS and _is_kana_only(m.surface)):
        return False
    if pos1 in NON_INDEPENDENT_SUBPOS:
        return False
    if prev is not None and prev.pos and prev.pos[0] == "接頭辞":
        return False
    if pos0 not in INDEPENDENT_POS:
        return False
    return prev is not None


def _segment(morphemes: list[Morpheme]) -> list[list[Morpheme]]:
    segments: list[list[Morpheme]] = []
    cur: list[Morpheme] = []
    for i, m in enumerate(morphemes):
        prev = morphemes[i - 1] if i > 0 else None
        if cur and _starts_new_segment(m, prev):
            segments.append(cur)
            cur = []
        cur.append(m)
    if cur:
        segments.append(cur)
    return segments


def _is_modifier(segment: list[Morpheme]) -> bool:
    """助詞・助動詞で終わっていない（= 後ろの語を修飾している）文節か。"""
    last = segment[-1]
    pos0 = last.pos[0] if last.pos else ""
    if pos0 == "補助記号":
        return False
    return pos0 not in CLOSING_POS


def _merge_modifiers(segments: list[list[Morpheme]], max_chars: int) -> list[list[Morpheme]]:
    out: list[list[Morpheme]] = []
    i = 0
    while i < len(segments):
        cur = list(segments[i])
        while (i + 1 < len(segments) and _is_modifier(cur)
               and _length(cur) + _length(segments[i + 1]) <= max_chars):
            i += 1
            cur.extend(segments[i])
        out.append(cur)
        i += 1
    return out


def _length(segment: list[Morpheme]) -> int:
    return sum(len(m.surface) for m in segment)


def _merge_short(segments: list[list[Morpheme]], min_chars: int, max_chars: int) -> list[list[Morpheme]]:
    out: list[list[Morpheme]] = []
    for seg in segments:
        if out and _length(out[-1]) < min_chars and _length(out[-1]) + _length(seg) <= max_chars:
            out[-1].extend(seg)
        else:
            out.append(list(seg))
    # 最後の文節が短ければ前に寄せる
    while len(out) >= 2 and _length(out[-1]) < min_chars:
        tail = out.pop()
        out[-1].extend(tail)
    return out


def _split_long(segments: list[list[Morpheme]], max_chars: int) -> list[list[Morpheme]]:
    out: list[list[Morpheme]] = []
    for seg in segments:
        if _length(seg) <= max_chars or len(seg) == 1:
            out.append(seg)
            continue
        cur: list[Morpheme] = []
        for m in seg:
            if cur and _length(cur) + len(m.surface) > max_chars:
                out.append(cur)
                cur = []
            cur.append(m)
        if cur:
            out.append(cur)
    return out


def _strip_trailing_punct(chunks: list[Chunk]) -> list[Chunk]:
    """最後のチャンクから文末の句点を落とす（canonical の reading と揃える）。"""
    while chunks:
        last = chunks[-1]
        target = last.target
        reading = last.reading
        changed = False
        while target and (target[-1] in TERMINATORS or target[-1] in TRAILING):
            target = target[:-1]
            if reading and (reading[-1] in TERMINATORS or reading[-1] in TRAILING):
                reading = reading[:-1]
            changed = True
        if not changed:
            break
        if target:
            chunks[-1] = Chunk(target, reading, last.morphemes)
            break
        chunks.pop()
    return chunks


class Chunker:
    def __init__(self, cfg: Config):
        self.min_chars = int(cfg.get("chunk.min_chars", 2))
        self.max_chars = int(cfg.get("chunk.max_chars", 40))
        self.merge_modifiers = bool(cfg.get("chunk.merge_modifiers", True))

    def chunk(self, morphemes: list[Morpheme]) -> list[Chunk]:
        if not morphemes:
            return []
        segments = _segment(morphemes)
        if self.merge_modifiers:
            segments = _merge_modifiers(segments, self.max_chars)
        segments = _merge_short(segments, self.min_chars, self.max_chars)
        segments = _split_long(segments, self.max_chars)
        chunks = [
            Chunk("".join(m.surface for m in seg), "".join(m.reading for m in seg), seg)
            for seg in segments if seg
        ]
        return _strip_trailing_punct(chunks)
