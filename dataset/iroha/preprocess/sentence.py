"""文分割。

句点（。！？）で切る。ただし 「」『』（）〈〉 の中では切らない（会話文・引用が
1 文として残るように）。閉じ括弧が句点の直後に来る場合はそれも同じ文に含める。
"""
from __future__ import annotations

# 半角の . は NFKC 後の ．（全角ピリオド）。文末に来たら句点として扱う
TERMINATORS = "。！？."
OPEN_CLOSE = {"「": "」", "『": "』", "（": "）", "(": ")", "〈": "〉", "《": "》", "【": "】"}
CLOSERS = set(OPEN_CLOSE.values())
TRAILING = CLOSERS | {"”", "'", '"', "）", ")"}


def split_sentences(text: str) -> list[str]:
    if not text:
        return []
    sentences: list[str] = []
    buf: list[str] = []
    depth = 0
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        buf.append(ch)
        if ch in OPEN_CLOSE:
            depth += 1
        elif ch in CLOSERS and depth > 0:
            depth -= 1
        elif ch in TERMINATORS and depth == 0:
            # 句点の直後の閉じ括弧・連続する句点は同じ文に入れる
            j = i + 1
            while j < n and (text[j] in TRAILING or text[j] in TERMINATORS):
                buf.append(text[j])
                j += 1
            sentences.append("".join(buf).strip())
            buf = []
            i = j
            continue
        i += 1
    tail = "".join(buf).strip()
    if tail:
        sentences.append(tail)
    return [s for s in sentences if s]


def strip_terminator(text: str) -> str:
    """末尾の句点（と、その後ろの閉じ括弧）を落とす。

    canonical の ``reading`` と最後のチャンクは句点を含めない
    （IME では句点を打たずに確定することも多いため）。
    """
    out = text
    while out and (out[-1] in TERMINATORS or out[-1] in TRAILING):
        if out[-1] in TRAILING and len(out) >= 2 and out[-2] not in TERMINATORS:
            break
        out = out[:-1]
    return out
