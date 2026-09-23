"""LLM-jp Corpus v4 の日本語サブコーパス（ja_kaken / ja_e-gov / ja_patent / ja_aozorabunko）。

一次配布は NII の GitLab（https://gitlab.llm-jp.nii.ac.jp/datasets/llm-jp-corpus-v4）。
各サブコーパスは ``ja/<サブコーパス>/NNNN.jsonl.gz`` の束で、1 行 1 文書
（``text`` と ``meta``）。4 つとも CC BY 4.0。

サブコーパスごとの扱い:

* **ja_kaken** … KAKEN の研究課題の概要。そのまま使う
* **ja_e-gov** … e-Gov 法令。**ひらがなを含まない段落は捨てる**（明治〜昭和前期の法令は
  「決闘ヲ挑ミタル者ハ」のようなカタカナ文語で、読みが現代の入力と合わない）
* **ja_patent** … 特許公報。【請求項１】などの見出しを段落の区切りにし、図の参照行
  （``0006770505.tif 000002``）を捨て、符号（``(110,710)``）と列挙記号（``ａ．``）を落とす。
  符号が残ると数字のフィルタで文ごと落ちるため。68B トークンと他より桁違いに大きいので、
  ``files`` で等間隔に選んだファイルだけを使う
* **ja_aozorabunko** … 青空文庫（globis-university/aozorabunko-clean 由来）。
  ``文字遣い種別 = 新字新仮名`` かつ ``作品著作権フラグ = なし`` の作品だけ
  （旧仮名は読みが現代かなと合わない。iroha_dataset/sources/aozora.py と同じ方針）

LLM-jp は日本国著作権法の適用のため日本国内のサーバから配布している。生データも生成物も
再配布しない（LICENSES.md の「派生データの公開可否」）。
"""
from __future__ import annotations

import gzip
import json
import re
import urllib.parse
from pathlib import Path
from typing import Iterator

from iroha_dataset.download.http import download_stream_to, http_get
from iroha_dataset.sources.aozora import strip_aozora_markup
from iroha_dataset.sources.base import Document, SourceAdapter, SourceInfo, register

GITLAB = "https://gitlab.llm-jp.nii.ac.jp"
PROJECT = "datasets/llm-jp-corpus-v4"
LICENSE = "CC BY 4.0"
LICENSE_URL = "https://creativecommons.org/licenses/by/4.0/"
HOMEPAGE = f"{GITLAB}/{PROJECT}"

_HIRAGANA = re.compile(r"[ぁ-ゖ]")
_PATENT_HEADING = re.compile(r"【[^】]{1,20}】|\(\d{2}\)|（\d{2}）")
_PATENT_FIGURE = re.compile(r"^\S+\.(?:tif|jpg|png|gif)\b.*$", re.IGNORECASE)
# 図の符号: (110,710,910) / （120）/ (751，752) / (Ｔｃ０) は残す（中に数字と区切りだけのもの）
_PATENT_REFNUM = re.compile(r"[（(][0-9０-９,，、．.\s\-－〜~]+[)）]")
# 行頭の列挙記号: ａ．／ｂ．／(a) / 1. など
_ENUM = re.compile(r"^(?:[a-zａ-ｚA-ZＡ-Ｚ][.．、]|[（(][a-zａ-ｚ0-9０-９]{1,2}[)）]|[0-9０-９]{1,2}[.．])\s*")


def _file_names(subset: str) -> list[str]:
    """GitLab の API でサブコーパスのファイル名（NNNN.jsonl.gz）を全部取る。"""
    names: list[str] = []
    path = urllib.parse.quote(PROJECT, safe="")
    page = 1
    while True:
        url = (f"{GITLAB}/api/v4/projects/{path}/repository/tree"
               f"?path=ja/{subset}&per_page=100&page={page}")
        rows = json.loads(http_get(url).decode("utf-8"))
        if not rows:
            break
        names += [r["name"] for r in rows if r.get("type") == "blob" and r["name"].endswith(".jsonl.gz")]
        if len(rows) < 100:
            break
        page += 1
    return sorted(names)


def pick_evenly(names: list[str], n: int) -> list[str]:
    """n 本を等間隔に選ぶ（ファイルは年代順に並んでいるので、先頭から取ると年代が偏る）。"""
    if n <= 0 or n >= len(names):
        return list(names)
    return [names[round(i * len(names) / n)] for i in range(n)]


class _LlmJpAdapter(SourceAdapter):
    subset: str = ""
    title: str = ""
    notes: list[str] = []

    @classmethod
    def info(cls) -> SourceInfo:
        return SourceInfo(
            name=cls.name,
            title=f"LLM-jp Corpus v4 / ja/{cls.subset}（{cls.title}）",
            homepage=f"{HOMEPAGE}/-/tree/main/ja/{cls.subset}",
            license=LICENSE,
            license_url=LICENSE_URL,
            attribution=(f"出典: LLM-jp Corpus v4（{HOMEPAGE}）ja/{cls.subset}、"
                         f"LLM-jp コーパス構築 WG、CC BY 4.0。文への分割・読みの付与など加工して作成"),
            used_fields=["text", "meta（サブコーパスごとの選別にだけ使う）"],
            notes=list(cls.notes),
        )

    # ---- 取得
    def _selected(self) -> list[str]:
        names = _file_names(self.subset)
        files = self.source_cfg.get("files", "all")
        if isinstance(files, list):
            want = {str(f) if str(f).endswith(".jsonl.gz") else f"{int(f):04d}.jsonl.gz" for f in files}
            return [n for n in names if n in want]
        if files in (None, "all"):
            return names
        return pick_evenly(names, int(files))

    def download(self, *, force: bool = False) -> dict:
        picked = self._selected()
        for name in picked:
            download_stream_to(f"{HOMEPAGE}/-/raw/main/ja/{self.subset}/{name}",
                               self.raw_dir / name, force=force)
        return {"files": picked}

    # ---- 読み出し
    def _paths(self) -> list[Path]:
        paths = sorted(self.raw_dir.glob("*.jsonl.gz"))
        if not paths:
            raise FileNotFoundError(
                f"{self.raw_dir} に *.jsonl.gz が無い。先に "
                f"`python -m iroha_dataset download --source {self.name}` を実行する")
        return paths

    def keep(self, meta: dict) -> bool:
        return True

    def paragraphs(self, text: str) -> list[str]:
        return [p.strip() for p in text.split("\n") if p.strip()]

    def attribution(self, meta: dict) -> str:
        return self.info().attribution

    def documents(self) -> Iterator[Document]:
        max_docs = int(self.source_cfg.get("max_documents", 0) or 0)
        n = 0
        for path in self._paths():
            stem = path.name.split(".")[0]
            with gzip.open(path, "rt", encoding="utf-8") as fh:
                for i, line in enumerate(fh):
                    if max_docs and n >= max_docs:
                        return
                    try:
                        row = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    meta = row.get("meta") or {}
                    if not self.keep(meta):
                        continue
                    paragraphs = self.paragraphs(row.get("text") or "")
                    if not paragraphs:
                        continue
                    n += 1
                    yield Document(
                        document_id=f"{self.name}_{stem}_{i:06d}",
                        source=self.name,
                        paragraphs=paragraphs,
                        license=LICENSE,
                        url=f"{HOMEPAGE}/-/tree/main/ja/{self.subset}",
                        attribution=self.attribution(meta),
                        meta={},
                    )


@register
class LlmJpKakenAdapter(_LlmJpAdapter):
    name = "llmjp_kaken"
    subset = "ja_kaken"
    title = "KAKEN 研究課題の概要"
    notes = ["iroha-dataset 自前の kaken ソース（KAKEN API から取得）と中身が重なる。"
             "同時に有効にしない（評価セット iroha-ds の読みは build-typo の exclude_readings_from で除く）"]


@register
class LlmJpEgovAdapter(_LlmJpAdapter):
    name = "llmjp_egov"
    subset = "ja_e-gov"
    title = "e-Gov 法令"
    notes = ["一次配布: https://huggingface.co/datasets/nlp-waseda/e_gov",
             "ひらがなを含まない段落（カタカナ文語の旧法令）は捨てる"]

    def paragraphs(self, text: str) -> list[str]:
        return [p for p in super().paragraphs(text) if _HIRAGANA.search(p)]

    def attribution(self, meta: dict) -> str:
        law = meta.get("LawNum")
        base = self.info().attribution
        return f"{base}（{law}）" if law else base


@register
class LlmJpPatentAdapter(_LlmJpAdapter):
    name = "llmjp_patent"
    subset = "ja_patent"
    title = "特許公報"
    notes = ["68B トークンあるので files で等間隔に選んだファイルだけを使う",
             "【】の見出しを段落の区切りにし、図の参照行・符号・列挙記号を落とす"]

    def paragraphs(self, text: str) -> list[str]:
        out = []
        for block in _PATENT_HEADING.sub("\n", text).split("\n"):
            block = block.strip()
            if not block or _PATENT_FIGURE.match(block):
                continue
            block = _ENUM.sub("", _PATENT_REFNUM.sub("", block)).strip()
            if block:
                out.append(block)
        return out


@register
class LlmJpAozoraAdapter(_LlmJpAdapter):
    name = "llmjp_aozora"
    subset = "ja_aozorabunko"
    title = "青空文庫"
    notes = ["一次配布: https://huggingface.co/datasets/globis-university/aozorabunko-clean",
             "文字遣い種別 = 新字新仮名 かつ 作品著作権フラグ = なし の作品だけ",
             "文学作品なので語彙・文体が現代の実務的な入力とは離れている"]

    def keep(self, meta: dict) -> bool:
        allowed = set(self.source_cfg.get("allowed_orthography", ["新字新仮名"]))
        return (meta.get("文字遣い種別") in allowed
                and (meta.get("作品著作権フラグ") or "").strip() == "なし")

    def paragraphs(self, text: str) -> list[str]:
        return super().paragraphs(strip_aozora_markup(text))

    def attribution(self, meta: dict) -> str:
        title = meta.get("作品名", "")
        author = f"{meta.get('姓', '')}{meta.get('名', '')}"
        return f"{self.info().attribution}。作品: 「{title}」{author}（青空文庫）"
