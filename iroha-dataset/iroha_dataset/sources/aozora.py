"""青空文庫（著作権消滅作品）。**既定 OFF**。

「将来的な追加ソース」として SourceAdapter の形に載せてある。使う前に注意:

* 著作権消滅（作品著作権フラグ = "なし"）の作品だけを対象にする
* **新字新仮名の作品だけ**（``allowed_orthography``）。旧字旧仮名は読みが現代かなと
  合わないので、Sudachi の読みをそのまま教師にすると壊れる
* ルビ（《》）・入力者注（［＃…］）・傍点は落とすが、青空文庫の記法は作品ごとに
  揺れる。**3 作品での動作は確認済み・大規模では未検証**なので、
  使う前に data/samples/canonical_samples.txt の目視が必要
* 文学作品なので語彙・文体が iroha の用途（現代の実務的な入力）とは離れている。
  かな漢字変換用としては有用だが、typo normalizer 用には向き不向きがある

ライセンス: 著作権消滅（パブリックドメイン）。青空文庫の入力・校正者のクレジットは
作品ファイルの末尾に入っているので、LICENSES.md の方針に従って出典を残す。
"""
from __future__ import annotations

import csv
import io
import re
import zipfile
from pathlib import Path
from typing import Iterator

from iroha_dataset.download.http import DownloadError, RateLimiter, download_to, http_get
from iroha_dataset.sources.base import Document, SourceAdapter, SourceInfo, register

INDEX_URL = "https://www.aozora.gr.jp/index_pages/list_person_all_extended_utf8.zip"

_RUBY = re.compile(r"《[^》]*》")
_RUBY_MARK = re.compile(r"｜")
_EDITOR_NOTE = re.compile(r"［＃[^］]*］")
_SEPARATOR = re.compile(r"^[-—―‐─-╿]{5,}$")


def strip_aozora_markup(text: str) -> str:
    text = _RUBY.sub("", text)
    text = _RUBY_MARK.sub("", text)
    text = _EDITOR_NOTE.sub("", text)
    return text


@register
class AozoraAdapter(SourceAdapter):
    name = "aozora"

    @classmethod
    def info(cls) -> SourceInfo:
        return SourceInfo(
            name="aozora",
            title="青空文庫（著作権消滅作品）",
            homepage="https://www.aozora.gr.jp/",
            license="著作権消滅（パブリックドメイン）",
            license_url="https://www.aozora.gr.jp/guide/kijyunn.html",
            attribution="出典: 青空文庫（https://www.aozora.gr.jp/）の著作権消滅作品",
            used_fields=["本文（ルビ・入力者注を除去）", "index の 作品著作権フラグ / 文字遣い種別"],
            notes=[
                "既定 OFF。旧字旧仮名は読みが現代かなと合わないので新字新仮名だけを対象にする",
                "作品著作権フラグが「なし」の作品だけを使う（翻訳者の著作権が残るものを除く）",
                "青空文庫の記法は作品ごとに揺れる。3 作品での動作は確認済み・大規模では未検証。"
                "使う前に samples/ の目視が必要",
                "文学作品なので現代の実務的な入力とは語彙・文体が離れている",
            ],
        )

    def download(self, *, force: bool = False) -> dict:
        index_zip = self.raw_dir / Path(INDEX_URL).name
        download_to(INDEX_URL, index_zip, force=force)
        works = self._index_works()
        limiter = RateLimiter(float(self.source_cfg.get("request_interval", 1.0)))
        max_works = int(self.source_cfg.get("max_works", 50))
        text_dir = self.raw_dir / "texts"
        text_dir.mkdir(parents=True, exist_ok=True)
        stats = {"index_works": len(works), "saved": 0, "errors": 0}
        for work in works:
            if stats["saved"] >= max_works:
                break
            dest = text_dir / f"{work['id']}.zip"
            if dest.exists() and not force:
                stats["saved"] += 1
                continue
            limiter.wait()
            try:
                data = http_get(work["url"], retries=2)
            except DownloadError:
                stats["errors"] += 1
                continue
            dest.write_bytes(data)
            stats["saved"] += 1
        return stats

    def _index_works(self) -> list[dict]:
        index_zip = self.raw_dir / Path(INDEX_URL).name
        if not index_zip.exists():
            return []
        allowed = set(self.source_cfg.get("allowed_orthography", ["新字新仮名"]))
        out: list[dict] = []
        with zipfile.ZipFile(index_zip) as zf:
            name = next((n for n in zf.namelist() if n.endswith(".csv")), None)
            if name is None:
                return []
            with zf.open(name) as fh:
                reader = csv.DictReader(io.TextIOWrapper(fh, encoding="utf-8-sig"))
                for row in reader:
                    if (row.get("作品著作権フラグ") or "").strip() != "なし":
                        continue
                    if (row.get("文字遣い種別") or "").strip() not in allowed:
                        continue
                    url = (row.get("テキストファイルURL") or "").strip()
                    if not url.endswith(".zip"):
                        continue
                    out.append({
                        "id": f"{row.get('作品ID', '').strip()}-{row.get('人物ID', '').strip()}",
                        "url": url,
                        "title": (row.get("作品名") or "").strip(),
                        "author": f"{(row.get('姓') or '').strip()}{(row.get('名') or '').strip()}",
                    })
        return out

    def documents(self) -> Iterator[Document]:
        text_dir = self.raw_dir / "texts"
        if not text_dir.exists():
            raise FileNotFoundError(
                f"{text_dir} が無い。先に `python -m iroha_dataset download --source aozora` を実行する")
        works = {w["id"]: w for w in self._index_works()}
        for path in sorted(text_dir.glob("*.zip")):
            work = works.get(path.stem, {})
            try:
                with zipfile.ZipFile(path) as zf:
                    name = next((n for n in zf.namelist() if n.lower().endswith(".txt")), None)
                    if name is None:
                        continue
                    raw = zf.read(name).decode("shift_jis", "replace")
            except (zipfile.BadZipFile, OSError):
                continue
            paragraphs = []
            for line in raw.splitlines():
                line = strip_aozora_markup(line).strip()
                if not line or _SEPARATOR.match(line):
                    continue
                paragraphs.append(line)
            if not paragraphs:
                continue
            title = work.get("title", path.stem)
            author = work.get("author", "")
            yield Document(
                document_id=f"aozora_{path.stem}",
                source=self.name,
                paragraphs=paragraphs,
                license="著作権消滅（パブリックドメイン）",
                url=work.get("url"),
                attribution=f"出典: 青空文庫「{title}」{author}",
                meta={"title": title, "author": author},
            )
