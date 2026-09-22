"""表記の正規化。

NFKC をかけたうえで、日本語として壊してはいけないもの（かな・漢字・、。「」ー）は
そのまま通す。NFKC は全角英数 → 半角、半角カナ → 全角、㈱ → (株)、① → 1 などを
直すが、かな・漢字・句読点には触らない。
"""
from __future__ import annotations

import re
import unicodedata

# 制御文字（改行・タブは空白に潰してから消す）とゼロ幅・方向制御
_CONTROL = re.compile(r"[\x00-\x08\x0b-\x0c\x0e-\x1f\x7f]")
_ZERO_WIDTH = re.compile(r"[​-‏‪-‮⁠﻿]")
_SPACES = re.compile(r"[ \t 　]+")
# 行内に残った改行
_NEWLINES = re.compile(r"[\r\n  ]+")

HIRAGANA = (0x3041, 0x3096)
KATAKANA = (0x30A1, 0x30FA)
KANJI = (0x4E00, 0x9FFF)
KANJI_EXT_A = (0x3400, 0x4DBF)

# NFKC は ！？ を半角にしてしまう。日本語の文としては全角が本来の形で、
# 文分割（TERMINATORS）も全角で見ているので戻す
_RESTORE_JP_PUNCT = str.maketrans({"!": "！", "?": "？"})

_WAVE_DASH_MAP = str.maketrans({
    "～": "〜",   # 全角チルダ → 波ダッシュ
    "−": "-",        # 全角マイナス
    "‐": "-",
    "―": "—",   # 横棒 → ダッシュ
    "·": "・",   # middle dot → 中黒
    "‘": "'", "’": "'",
    "“": '"', "”": '"',
})


def has_broken_unicode(text: str) -> bool:
    """壊れた Unicode（置換文字・サロゲート・未割り当て）を含むか。"""
    if "�" in text:
        return True
    for ch in text:
        o = ord(ch)
        if 0xD800 <= o <= 0xDFFF:
            return True
        if unicodedata.category(ch) in ("Cs", "Co", "Cn"):
            return True
    return False


def normalize_text(text: str) -> str:
    """1 行のテキストを正規化する（文分割の前に通す）。"""
    if not text:
        return ""
    text = _NEWLINES.sub(" ", text)
    text = _ZERO_WIDTH.sub("", text)
    text = _CONTROL.sub("", text)
    text = unicodedata.normalize("NFKC", text)
    text = text.translate(_WAVE_DASH_MAP)
    text = text.translate(_RESTORE_JP_PUNCT)
    text = _SPACES.sub(" ", text)
    return text.strip()


def katakana_to_hiragana(text: str) -> str:
    out = []
    for ch in text:
        o = ord(ch)
        if KATAKANA[0] <= o <= 0x30F6:
            out.append(chr(o - 0x60))
        else:
            out.append(ch)
    return "".join(out)


def hiragana_to_katakana(text: str) -> str:
    out = []
    for ch in text:
        o = ord(ch)
        if HIRAGANA[0] <= o <= 0x3096:
            out.append(chr(o + 0x60))
        else:
            out.append(ch)
    return "".join(out)


def is_hiragana(ch: str) -> bool:
    return HIRAGANA[0] <= ord(ch) <= 0x3096


def is_katakana(ch: str) -> bool:
    return KATAKANA[0] <= ord(ch) <= KATAKANA[1]


def is_kanji(ch: str) -> bool:
    o = ord(ch)
    return KANJI[0] <= o <= KANJI[1] or KANJI_EXT_A[0] <= o <= KANJI_EXT_A[1] or ch == "々"


def is_japanese(ch: str) -> bool:
    return is_hiragana(ch) or is_katakana(ch) or is_kanji(ch) or ch in "ーヽヾゝゞ々"


def japanese_ratio(text: str) -> float:
    if not text:
        return 0.0
    n = sum(1 for ch in text if is_japanese(ch))
    return n / len(text)
