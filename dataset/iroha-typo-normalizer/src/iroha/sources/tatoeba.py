"""Tatoeba（日本語文）。

一次配布元 https://downloads.tatoeba.org/exports/ から直接取る（HF のミラーは使わない）。

* ``per_language/jpn/jpn_sentences.tsv.bz2``          … id / lang / text
* ``per_language/jpn/jpn_sentences_detailed.tsv.bz2`` … 投稿者（出典表記に使う）
* ``per_language/jpn/jpn_sentences_CC0.tsv.bz2``      … CC0 の文の一覧
  （残りは CC BY 2.0 FR。record ごとに license を分けるために読む）
* ``jpn_indices.tar.bz2``                             … 田中コーパス由来の語＋読み注記。
  読みの second opinion に使う（別解析器の代わり）

1 文 = 1 document なので、左文脈は文の中だけで作られる。
"""
from __future__ import annotations

import bz2
import tarfile
from pathlib import Path
from typing import Iterator

from iroha.download.http import download_to
from iroha.preprocess import normalize
from iroha.sources.base import Document, SourceAdapter, SourceInfo, register

BASE = "https://downloads.tatoeba.org/exports"
FILES = {
    "sentences": f"{BASE}/per_language/jpn/jpn_sentences.tsv.bz2",
    "detailed": f"{BASE}/per_language/jpn/jpn_sentences_detailed.tsv.bz2",
    "cc0": f"{BASE}/per_language/jpn/jpn_sentences_CC0.tsv.bz2",
    "indices": f"{BASE}/jpn_indices.tar.bz2",
}

LICENSE_DEFAULT = "CC BY 2.0 FR"
LICENSE_CC0 = "CC0 1.0"


class IndicesSecondOpinion:
    """jpn_indices.csv の読み注記と Sudachi の読みを突き合わせる。

    注記の形は ``二十歳(はたち){２０歳}`` で、括弧内が見出し語の読み。
    文中に見出し語がそのまま現れていて、Sudachi がその語に別の読みを当てていたら
    ``reading_confidence = low`` にする。読みが付いている語だけが対象なので
    網羅はしないが、同形異音語（二十歳・一日・日本 など）にはよく効く。
    """

    name = "tatoeba_indices"

    def __init__(self, readings: dict[str, list[tuple[str, str]]]):
        self._readings = readings

    def check(self, text: str, result, meta: dict) -> list[str]:
        sentence_id = str(meta.get("tatoeba_id", ""))
        entries = self._readings.get(sentence_id)
        if not entries:
            return []
        reasons = []
        for headword, reading in entries:
            if headword and headword in text and reading and reading not in result.reading:
                reasons.append(f"indices_disagreement:{headword}({reading})")
        return reasons


def _parse_indices_token(token: str) -> tuple[str, str] | None:
    """``二十歳(はたち){２０歳}`` → ("二十歳", "はたち")。読み注記が無ければ None。"""
    if "(" not in token:
        return None
    head, rest = token.split("(", 1)
    if ")" not in rest:
        return None
    reading = rest.split(")", 1)[0]
    head = head.split("{")[0].split("[")[0].strip("~")
    reading = normalize.katakana_to_hiragana(reading)
    if not head or not reading:
        return None
    if not all(normalize.is_hiragana(c) or c == "ー" for c in reading):
        return None
    return head, reading


@register
class TatoebaAdapter(SourceAdapter):
    name = "tatoeba"

    @classmethod
    def info(cls) -> SourceInfo:
        return SourceInfo(
            name="tatoeba",
            title="Tatoeba Project — Japanese sentences",
            homepage="https://tatoeba.org/",
            license=f"{LICENSE_DEFAULT}（一部 {LICENSE_CC0}）",
            license_url="https://creativecommons.org/licenses/by/2.0/fr/",
            attribution=("出典: Tatoeba Project (https://tatoeba.org/) の日本語文。"
                         "CC BY 2.0 FR。文ごとの投稿者は jpn_sentences_detailed.tsv に含まれる"),
            used_fields=["id", "text", "username（出典表記用）", "jpn_indices の読み注記"],
            notes=[
                "一次配布元 downloads.tatoeba.org から取得する（HF のミラーは使わない）",
                "文の大半は CC BY 2.0 FR。jpn_sentences_CC0.tsv に載っている文だけ CC0 1.0",
                "投稿者による例文なので、1 文 = 1 document。文をまたぐ左文脈は作れない",
                "jpn_indices は田中コーパス由来。読みの second opinion に使っている",
            ],
        )

    # ---- download ----
    def download(self, *, force: bool = False) -> dict:
        out = {}
        for key, url in FILES.items():
            dest = self.raw_dir / Path(url).name
            download_to(url, dest, force=force)
            out[key] = {"url": url, "path": str(dest), "bytes": dest.stat().st_size}
        return out

    # ---- 読み込み ----
    def _path(self, key: str) -> Path:
        return self.raw_dir / Path(FILES[key]).name

    def _cc0_ids(self) -> set[str]:
        path = self._path("cc0")
        ids: set[str] = set()
        if not path.exists():
            return ids
        with bz2.open(path, "rt", encoding="utf-8") as f:
            for line in f:
                parts = line.rstrip("\n").split("\t")
                if parts and parts[0].isdigit():
                    ids.add(parts[0])
        return ids

    def _usernames(self) -> dict[str, str]:
        path = self._path("detailed")
        out: dict[str, str] = {}
        if not path.exists():
            return out
        with bz2.open(path, "rt", encoding="utf-8") as f:
            for line in f:
                parts = line.rstrip("\n").split("\t")
                if len(parts) >= 4 and parts[0].isdigit() and parts[3] not in ("", "\\N"):
                    out[parts[0]] = parts[3]
        return out

    def indices_readings(self) -> dict[str, list[tuple[str, str]]]:
        """sentence_id → [(見出し語, 読み), …]"""
        path = self._path("indices")
        out: dict[str, list[tuple[str, str]]] = {}
        if not path.exists():
            return out
        with tarfile.open(path, "r:bz2") as tar:
            member = next((m for m in tar.getmembers() if m.name.endswith(".csv")), None)
            if member is None:
                return out
            stream = tar.extractfile(member)
            if stream is None:
                return out
            for raw in stream:
                parts = raw.decode("utf-8", "replace").rstrip("\n").split("\t")
                if len(parts) < 3 or not parts[0].isdigit():
                    continue
                entries = []
                for token in parts[2].split():
                    parsed = _parse_indices_token(token)
                    if parsed:
                        entries.append(parsed)
                if entries:
                    out[parts[0]] = entries
        return out

    def second_opinions(self) -> list:
        if not self.source_cfg.get("use_indices_second_opinion", True):
            return []
        readings = self.indices_readings()
        if not readings:
            return []
        return [IndicesSecondOpinion(readings)]

    def documents(self) -> Iterator[Document]:
        path = self._path("sentences")
        if not path.exists():
            raise FileNotFoundError(
                f"{path} が無い。先に `python -m iroha download --source tatoeba` を実行する")
        cc0 = self._cc0_ids()
        usernames = self._usernames()
        limit = int(self.source_cfg.get("max_sentences", 0) or 0)
        info = self.info()
        n = 0
        with bz2.open(path, "rt", encoding="utf-8") as f:
            for line in f:
                parts = line.rstrip("\n").split("\t")
                if len(parts) < 3 or not parts[0].isdigit():
                    continue
                sid, _lang, text = parts[0], parts[1], parts[2]
                if not text.strip():
                    continue
                username = usernames.get(sid)
                yield Document(
                    document_id=f"tatoeba_{sid}",
                    source=self.name,
                    paragraphs=[text],
                    license=LICENSE_CC0 if sid in cc0 else LICENSE_DEFAULT,
                    url=f"https://tatoeba.org/sentences/show/{sid}",
                    attribution=(f"Tatoeba sentence #{sid} by {username} (CC BY 2.0 FR)"
                                 if username else info.attribution),
                    meta={"tatoeba_id": sid, "username": username},
                )
                n += 1
                if limit and n >= limit:
                    return
