"""話し言葉の発話の清掃（正規化・除外・重複除去）。

対話コーパス・掲示板の 1 発話を、既存の前処理（``preprocess``）に渡せる 1 段落にする。
既存の前処理は書き言葉の文を前提にしているので、その手前で話し言葉に特有のものを落とす:

* 正規化（落とすだけで発話は残す）: 絵文字、顔文字、装飾記号、末尾の笑い（笑・w）、
  記号の繰り返し（！！！・。。。・ーーー）、同じ文字の過剰な繰り返し、改行と空白
* 除外（発話ごと捨てる）: URL・メールアドレス・@ の宛先、日本語を含まない、日本語の割合が低い、
  短すぎる・長すぎる、正規化で中身が無くなった
* 重複除去: 同一の発話と、ほぼ同一の発話（記号・空白・長音・文字の繰り返しを落とした形が同じ）

掲示板（open2ch）に特有のものは ``Open2chFilter``（ネットスラング・AA・半角カナ・不適切語）。

除外の理由も正規化の回数も ``CleanStats`` に数え、preprocess の統計に ``extraction`` として出る。
"""
from __future__ import annotations

import hashlib
import re
import unicodedata
from dataclasses import dataclass, field

from iroha.preprocess import normalize

# ---------------------------------------------------------------- 正規化

# 絵文字（記号・絵文字の各ブロック、異体字セレクタ、ZWJ、肌の色、国旗の地域指示子）
_EMOJI = re.compile(
    "[\U0001F000-\U0001FAFF\U0001FC00-\U0001FFFF☀-➿⬀-⯿⌀-⏿"
    "︎️‍⃣\U0001F1E6-\U0001F1FF]+")
# 装飾の記号（読みを持たない）
_DECORATION = re.compile("[♪♫♬♩♡♥❤☆★◎●○◯■□◆◇▲△▼▽→←↑↓⇒⇔※♂♀✨✧✦✩✪〆∞≧≦]+")
# 顔文字: かな・漢字を含まない括弧の中身（(^^) (*^_^*) (T_T) (・∀・)）
_KAOMOJI = re.compile(r"[（(][^（()）ぁ-んァ-ヶ一-龯々]{1,12}[)）]")
_KAOMOJI_BARE = re.compile(r"\^\^;?|\^_\^|\^o\^|orz|OTL|m\(_ _\)m|\(\s*\)", re.I)
# 末尾の笑い（(笑)・笑・w の連続）
_LAUGH_TAIL = re.compile(r"(?:[（(]笑[)）]|笑|[wｗＷW]+)+[。！？…、]*$")
# 記号の繰り返し
_REPEAT_MARK = [
    (re.compile(r"[！!]{2,}"), "！"),
    (re.compile(r"[？?]{2,}"), "？"),
    (re.compile(r"[！？!?]{2,}"), "？"),
    (re.compile(r"。{2,}"), "。"),
    (re.compile(r"、{2,}"), "、"),
    (re.compile(r"(?:…|・・|\.\.)[…・.]*"), "…"),
    (re.compile(r"ー{3,}"), "ーー"),
    (re.compile(r"〜{2,}"), "〜"),
    (re.compile(r"(?<=[。！？])、+"), ""),     # 「です。、今は」
]
# 同じかなの過剰な繰り返し（ああああああ → あああ）
_REPEAT_KANA = re.compile(r"([ぁ-んァ-ヶ])\1{3,}")
_SPACE = re.compile(r"[ 　\t]+")
_NEWLINE = re.compile(r"\s*(?:\r\n|\r|\n)\s*")

# ---------------------------------------------------------------- 除外
_URL = re.compile(r"(?:https?://|ftp://|www\.)\S+|\S+\.(?:com|net|org|jp|io|me|ly)(?:/\S*)?", re.I)
_MAIL = re.compile(r"[\w.+-]+@[\w-]+\.[\w.-]+")
# @ の宛先。日本語の名前にも当たるように @ か ＠ があれば宛先とみなす（名前を知っているソースは先に取り除く）
_MENTION = re.compile(r"[@＠]")
_TERMINATORS = "。！？…"


@dataclass
class CleanStats:
    """ソースごとの抽出の統計。``as_dict`` が preprocess の統計の ``extraction`` になる。"""
    dialogues: int = 0
    dialogues_sampled_out: int = 0
    utterances_raw: int = 0
    utterances_skipped_speaker: int = 0
    utterances_kept: int = 0
    rejected: dict[str, int] = field(default_factory=dict)
    normalized: dict[str, int] = field(default_factory=dict)

    def reject(self, reason: str) -> None:
        self.rejected[reason] = self.rejected.get(reason, 0) + 1

    def note(self, what: str) -> None:
        self.normalized[what] = self.normalized.get(what, 0) + 1

    def as_dict(self) -> dict:
        return {
            "dialogues": self.dialogues,
            "dialogues_sampled_out": self.dialogues_sampled_out,
            "utterances_raw": self.utterances_raw,
            "utterances_skipped_speaker": self.utterances_skipped_speaker,
            "utterances_kept": self.utterances_kept,
            "rejected": dict(sorted(self.rejected.items(), key=lambda kv: -kv[1])),
            "rejected_total": sum(self.rejected.values()),
            "normalized": dict(sorted(self.normalized.items(), key=lambda kv: -kv[1])),
        }


def _h64(s: str) -> int:
    return int.from_bytes(hashlib.blake2b(s.encode("utf-8"), digest_size=8).digest(), "big")


def near_key(text: str) -> str:
    """ほぼ同一の発話を同じにするキー（記号・空白・長音・文字の繰り返しの揺れを落とす）。

    終助詞（ね・よ）の違いは残す。「そうだね」と「そうだよ」は IME で打ち分ける別の入力なので、
    重複として落とすと語尾の種類が減る。
    """
    key = unicodedata.normalize("NFKC", text).lower()
    # 句読点・記号・空白（Unicode の分類 P* / S* / Z*）と長音を落とす
    key = "".join(ch for ch in key if ch != "ー" and unicodedata.category(ch)[0] not in "PSZ")
    # かなの繰り返しだけまとめる（数字の 11 と 1 は別物）
    return re.sub(r"([ぁ-んァ-ヶ])\1+", r"\1", key)


class UtteranceCleaner:
    """1 発話を清掃する。捨てるときは None を返し、理由を stats に数える。"""

    def __init__(self, cfg: dict, stats: CleanStats):
        self.min_chars = int(cfg.get("min_chars", 4))
        self.max_chars = int(cfg.get("max_chars", 150))
        self.min_japanese_ratio = float(cfg.get("min_japanese_ratio", 0.6))
        self.strip_laugh = bool(cfg.get("strip_laugh", True))
        # ひらがなの割合の下限。0 で見ない。掲示板の実況の断片（「残塁テーマ」）は漢字・カタカナだけになる
        self.min_hiragana_ratio = float(cfg.get("min_hiragana_ratio", 0.0) or 0.0)
        self.stats = stats

    def _normalize(self, text: str) -> str:
        st = self.stats
        # 改行は文の区切り。前が句読点でなければ句点を補う
        parts = [p for p in _NEWLINE.split(text) if p.strip()]
        if len(parts) > 1:
            st.note("newline")
            text = "".join(p if p[-1] in _TERMINATORS + "、" else p + "。" for p in parts[:-1]) + parts[-1]
        text = normalize.normalize_text(text)
        for pattern, name in ((_EMOJI, "emoji"), (_KAOMOJI, "kaomoji"), (_KAOMOJI_BARE, "kaomoji"),
                              (_DECORATION, "decoration_symbol")):
            text, n = pattern.subn("", text)
            if n:
                st.note(name)
        if self.strip_laugh:
            stripped = _LAUGH_TAIL.sub("", text)
            if stripped != text:
                st.note("laugh_tail")
                text = stripped
        for pattern, repl in _REPEAT_MARK:
            text, n = pattern.subn(repl, text)
            if n:
                st.note("repeated_mark")
        text, n = _REPEAT_KANA.subn(r"\1\1\1", text)
        if n:
            st.note("repeated_kana")
        # 日本語の間の空白は読点にする（「そうそう それそれ」）。ほかの空白は詰める
        if _SPACE.search(text):
            st.note("space")
            # 句読点のあとの空白はただ詰める（「です。 私は」→「です。私は」）
            text = re.sub(r"(?<=[^\sA-Za-z0-9。、！？…])[ 　\t]+(?=[^\sA-Za-z0-9])", "、", text)
            text = _SPACE.sub("", text)
        text = re.sub(r"^[、。…]+", "", text)
        text = re.sub(r"、+(?=[。！？…]|$)", "", text)
        return text.strip()

    def clean(self, raw: str) -> str | None:
        st = self.stats
        if not raw or not raw.strip():
            st.reject("empty")
            return None
        if _URL.search(raw) or _MAIL.search(raw):
            st.reject("url")
            return None
        if _MENTION.search(raw):
            st.reject("mention")
            return None
        if normalize.has_broken_unicode(raw):
            st.reject("broken_unicode")
            return None
        text = self._normalize(raw)
        if not text:
            st.reject("empty_after_normalize")
            return None
        if not any(normalize.is_japanese(ch) for ch in text):
            st.reject("no_japanese")
            return None
        if len(text) < self.min_chars:
            st.reject("too_short")
            return None
        if len(text) > self.max_chars:
            st.reject("too_long")
            return None
        if normalize.japanese_ratio(text) < self.min_japanese_ratio:
            st.reject("low_japanese_ratio")
            return None
        if self.min_hiragana_ratio and \
                sum(1 for ch in text if normalize.is_hiragana(ch)) / len(text) < self.min_hiragana_ratio:
            st.reject("low_hiragana_ratio")
            return None
        return text


class UtteranceDeduplicator:
    """ソースの中で同一・ほぼ同一の発話を落とす（最初に出たものを残す）。"""

    def __init__(self, stats: CleanStats):
        self._exact: set[int] = set()
        self._near: set[int] = set()
        self.stats = stats

    def is_duplicate(self, text: str) -> bool:
        h = _h64(text)
        if h in self._exact:
            self.stats.reject("duplicate_exact")
            return True
        self._exact.add(h)
        k = _h64(near_key(text))
        if k in self._near:
            self.stats.reject("duplicate_near")
            return True
        self._near.add(k)
        return False


# ---------------------------------------------------------------- open2ch

_HALFWIDTH_KANA = re.compile("[｡-ﾟ]")
# AA に使われる文字（罫線・ブロック要素・顔文字の部品）
_AA_CHARS = set("─━│┃┌┐└┘├┤┬┴┼╋▁▂▃▄▅▆▇█▓▒░⊂⊃∩∪∀∧∨ω´｀`ﾟ゜゛ヽ丿彡≡Д∠＿＼／|/\\")
_ANCHOR = re.compile(r">>|＞＞|>\d")
# 「>」「＞」で始まる引用（ニュース記事や他人の投稿の文章で、話し言葉ではない）
_QUOTE = re.compile(r"(?:^|\n)\s*[>＞]")
# 笑いとしての「草」（草原・雑草などの語は除く）
_KUSA = re.compile(r"(?<![雑牧若薬干起道水煙七野根海香夏芝枯])草(?![原木花地案稿履刈むぶ])|大草原|草生え")
_LAUGH_ANY = re.compile(r"[wｗＷ]{2,}|(?<![A-Za-z])[wｗＷ](?:[。、！？…]|$)|ワロ|わろた|藁")


class Open2chFilter:
    """掲示板の投稿から「普通の現代日本語の話し言葉」でないものを落とす。

    ``clean`` の前（生の投稿）に見るもの: 半角カナ・AA・アンカー・笑いの w/草・スラング・不適切語。
    スラングは ``sources.open2ch.slang``（**正規表現**。NFKC 後の本文に search）で設定する。
    部分一致にしないのは「ンゴ」が「リンゴ」に、「イッチ」が「スイッチ」に含まれるため。
    """

    def __init__(self, cfg: dict, stats: CleanStats, ng_words: list[str]):
        self.stats = stats
        self.slang = [re.compile(s) for s in (cfg.get("slang") or []) if s]
        self.ng_words = [w.lower() for w in ng_words if w]
        self.max_aa_ratio = float(cfg.get("max_aa_ratio", 0.1))

    def reject_reason(self, raw: str) -> str | None:
        if _HALFWIDTH_KANA.search(raw):
            return "halfwidth_kana"
        n_aa = sum(1 for ch in raw if ch in _AA_CHARS)
        if n_aa >= 3 and n_aa / max(1, len(raw)) > self.max_aa_ratio:
            return "ascii_art"
        if _ANCHOR.search(raw):
            return "anchor"
        if _QUOTE.search(raw):
            return "quote"
        text = unicodedata.normalize("NFKC", raw)
        if _LAUGH_ANY.search(raw) or _LAUGH_ANY.search(text):
            return "net_laugh"
        if _KUSA.search(text):
            return "net_laugh"
        for s in self.slang:
            if s.search(text):
                return "net_slang"
        low = text.lower()
        for w in self.ng_words:
            if w in low:
                return "ng_word"
        return None

    def accept(self, raw: str) -> bool:
        reason = self.reject_reason(raw)
        if reason is None:
            return True
        self.stats.reject(reason)
        return False
