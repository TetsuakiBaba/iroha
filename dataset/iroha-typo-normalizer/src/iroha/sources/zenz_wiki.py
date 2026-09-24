"""zenz-v2.5-dataset の Wikipedia サブセット（``train_wikipedia.jsonl``、CC BY-SA 4.0）。

https://huggingface.co/datasets/Miwa-Keita/zenz-v2.5-dataset 。1 行は
``{"input": カタカナの読み, "output": 表層, "left_context": 左文脈 | null}`` で、
``output`` は文の途中から始まる断片（「に選ばれ、アメリカンリーグ史上最年少の」）。

* 使うのは ``output`` の表層だけ。**読みは Sudachi で付け直す**（他のソースと揃えるため。
  zenz の ``input`` は「ニホン／ニッポン」のような読みの揺らしを意図的に入れてあり、
  Sudachi の読みとの一致は 41%）
* 同じ文から来た断片が隣り合って並んでいるので、連続する ``block_rows`` 行を 1 文書にして
  split を文書単位で決める（行ごとに振り分けると同じ文が train と test にまたがる）
* JWTD のベンチ（これも Wikipedia 由来）の表層と ``overlap_chars`` 字以上重なる行は捨てる
* ``train_llm-jp-corpus-v3.jsonl``（Common Crawl 由来、ODC-BY + CC 規約）は使わない

CC BY-SA 4.0 なので、このソースを混ぜた生成物とそれで学習したモデルは SA に縛られる
（LICENSES.md）。
"""
from __future__ import annotations

import json
from pathlib import Path
from typing import Iterator

from iroha.config import PROJECT_ROOT
from iroha.download.http import download_stream_to
from iroha.sources.base import Document, SourceAdapter, SourceInfo, register

# 版を固定する（main を取ると上流の更新で読み一覧が変わる）。2025-01-17 の版で、
# 2026-09-23 に typo-corpus（readings-balanced / readings-full）を作ったときもこの版だった
REVISION = "7d9c9ea36347b638a627c1d264da48ebc7eb38aa"
URL = f"https://huggingface.co/datasets/Miwa-Keita/zenz-v2.5-dataset/resolve/{REVISION}/train_wikipedia.jsonl"
FILE = "train_wikipedia.jsonl"
LICENSE = "CC BY-SA 4.0"


def overlap_grams(paths: list[str], n: int, fields: tuple[str, ...] = ("surface_noisy", "surface_clean")) -> set[str]:
    """ベンチの表層に現れる n 文字の部分文字列の集合（無いファイルは飛ばす）。"""
    grams: set[str] = set()
    for p in paths:
        path = Path(p).expanduser()
        if not path.is_absolute():
            path = PROJECT_ROOT / path
        if not path.exists():
            continue
        with open(path, encoding="utf-8") as fh:
            for line in fh:
                row = json.loads(line)
                for f in fields:
                    s = row.get(f) or ""
                    grams.update(s[i:i + n] for i in range(len(s) - n + 1))
    return grams


def overlaps(text: str, grams: set[str], n: int) -> bool:
    return any(text[i:i + n] in grams for i in range(len(text) - n + 1))


@register
class ZenzWikipediaAdapter(SourceAdapter):
    name = "zenz_wiki"

    @classmethod
    def info(cls) -> SourceInfo:
        return SourceInfo(
            name="zenz_wiki",
            title="zenz-v2.5-dataset / train_wikipedia.jsonl（Wikipedia 日本語版 2024-02 由来）",
            homepage="https://huggingface.co/datasets/Miwa-Keita/zenz-v2.5-dataset",
            license=LICENSE,
            license_url="https://creativecommons.org/licenses/by-sa/4.0/deed.ja",
            attribution=("出典: zenz-v2.5-dataset（Keita Miwa、"
                         "https://huggingface.co/datasets/Miwa-Keita/zenz-v2.5-dataset）の train_wikipedia.jsonl"
                         "（Wikipedia 日本語版、CC BY-SA 4.0）。読みを付け直すなど加工して作成"),
            used_fields=["output（表層）"],
            notes=["読みは Sudachi で付け直す（input は使わない）",
                   "CC BY-SA 4.0: このソースを混ぜた生成物・学習したモデルは SA に縛られる",
                   "JWTD のベンチと表層が重なる行は捨てる（どちらも Wikipedia 由来）"],
        )

    def download(self, *, force: bool = False) -> dict:
        dest = download_stream_to(URL, self.raw_dir / FILE, force=force)
        return {"file": str(dest), "bytes": dest.stat().st_size}

    def documents(self) -> Iterator[Document]:
        path = self.raw_dir / FILE
        if not path.exists():
            raise FileNotFoundError(
                f"{path} が無い。先に `python -m iroha download --source zenz_wiki` を実行する")
        block = max(1, int(self.source_cfg.get("block_rows", 1000)))
        max_rows = int(self.source_cfg.get("max_rows", 0) or 0)
        n = int(self.source_cfg.get("overlap_chars", 12))
        grams = overlap_grams(list(self.source_cfg.get("exclude_overlap_with", []) or []), n)
        self.excluded_overlap = 0
        info = self.info()
        paragraphs: list[str] = []
        start = 0
        with open(path, encoding="utf-8") as fh:
            for i, line in enumerate(fh):
                if max_rows and i >= max_rows:
                    break
                if i - start >= block:
                    if paragraphs:
                        yield self._doc(start, paragraphs, info)
                    paragraphs, start = [], i
                try:
                    out = (json.loads(line).get("output") or "").strip()
                except json.JSONDecodeError:
                    continue
                if not out:
                    continue
                if grams and overlaps(out, grams, n):
                    self.excluded_overlap += 1
                    continue
                # 断片どうしは文としてつながっていないので、1 行 1 段落（左文脈を繋がない）
                paragraphs.append(out)
        if paragraphs:
            yield self._doc(start, paragraphs, info)

    def _doc(self, start: int, paragraphs: list[str], info: SourceInfo) -> Document:
        return Document(document_id=f"zenz_wiki_{start:09d}", source=self.name,
                        paragraphs=paragraphs, license=LICENSE, url=info.homepage,
                        attribution=info.attribution, meta={})
