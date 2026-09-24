"""KAKEN（科学研究費助成事業データベース、国立情報学研究所）。

使うのは研究概要・報告書本文の段落（``paragraphList/paragraph``）。学術語彙と
長めの現代日本語が取れて、段落の中で文が続くので**文をまたぐ左文脈**が作れる。

ライセンス上の扱い（詳細は LICENSES.md）:

* KAKEN のコンテンツは「文部科学省ウェブサイト利用規約」準拠で **CC BY 4.0 と互換**。
  ただし「著作物であることを明示されている部分（**2015年（平成27年度）以前の採択課題**に
  関して提出された報告書等の一部）」は別扱いなので、**採択年度 2016 以降の課題だけ**を
  使う（``min_award_year``）。判定に使う採択年度は ``periodOfAward/startDate``。
  これが取れない課題は使わない（安全側）。
* 出典表記が必須。課題ごとの表記を Document.attribution に入れてある。

取得の方法は 2 つ:

* ``mode: ids``  … 課題ごとの公開 XML を 1 件ずつ取る。appid 不要。
  ``https://kaken.nii.ac.jp/ja/grant/KAKENHI-PROJECT-<課題番号>.xml``
保存形式は 2 つあり、``documents()`` はどちらも読む:

* ``store: jsonl``（既定） … 年度ごとの JSONL に 1 行 1 課題で追記する。
  11 万課題でもファイルは十数個なので、同期フォルダに置いても重くならない
* ``store: xml``           … 1 課題 1 ファイル（``xml/<課題番号>.xml``）。
  取得したままの XML を残したいとき

* ``mode: search`` … OpenSearch API。**appid が必須**（環境変数 ``KAKEN_APPID``）。
  https://support.nii.ac.jp/ja/kaken/api/api_outline で登録する。
  パラメータは KAKEN_API_parameters_document に従う
  （s1/o1 = 開始年度、rw = 件数、st = 開始番号、format=xml）。
"""
from __future__ import annotations

import os
import random
import time
import urllib.parse
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Iterator

from iroha.download.http import DownloadError, RateLimiter, http_get
from iroha.jsonlio import dump_line, read_jsonl
from iroha.sources.base import Document, SourceAdapter, SourceInfo, register

XML_LANG = "{http://www.w3.org/XML/1998/namespace}lang"
GRANT_XML = "https://kaken.nii.ac.jp/ja/grant/KAKENHI-PROJECT-{num}.xml"
GRANT_URL = "https://kaken.nii.ac.jp/grant/KAKENHI-PROJECT-{num}/"
SEARCH_URL = "https://kaken.nii.ac.jp/opensearch/"
LICENSE = "CC BY 4.0 互換（文部科学省ウェブサイト利用規約準拠）"


# documents() が読むフィールドだけを残した grantAward を作る。
# 検索 API の応答は 1 課題あたり平均 83KB（productList が大きい）あり、
# 12 万課題そのまま置くと 10GB になるため。
_KEEP_SUMMARY_CHILDREN = ("title", "periodOfAward", "paragraphList")
_KEEP_REPORT_CHILDREN = ("paragraphList",)


def _trim_award(award: ET.Element) -> ET.Element:
    """@awardNumber / ja の summary（題名・期間・段落）/ ja の報告書段落だけを残す。"""
    out = ET.Element("grantAward")
    for key in ("id", "awardNumber", "recordSet", "projectType"):
        value = award.get(key)
        if value:
            out.set(key, value)
    for summary in award.findall("summary"):
        if summary.get(XML_LANG) != "ja":
            continue
        kept = ET.SubElement(out, "summary")
        kept.set(XML_LANG, "ja")
        for name in _KEEP_SUMMARY_CHILDREN:
            for child in summary.findall(name):
                kept.append(child)
    reports = [r for r in award.findall("reportList/report") if r.get(XML_LANG) == "ja"]
    if reports:
        report_list = ET.SubElement(out, "reportList")
        for report in reports:
            kept = ET.SubElement(report_list, "report")
            for key in ("id", "type", "fiscalYear"):
                value = report.get(key)
                if value:
                    kept.set(key, value)
            kept.set(XML_LANG, "ja")
            for name in _KEEP_REPORT_CHILDREN:
                for child in report.findall(name):
                    kept.append(child)
    return out


def _search_failure_message(error: Exception) -> str:
    text = str(error)
    if "Invalid APPID" in text:
        return (
            "KAKEN の検索 API が appid を受け付けなかった（Invalid APPID）。\n"
            "  ・https://api.ci.nii.ac.jp/ja/ のデベロッパー登録が完了しているか"
            "（確認メールのリンクを開いたか）を確認する\n"
            "  ・環境変数 KAKEN_APPID の値に余計な空白・改行が入っていないか確認する\n"
            "  ・appid なしで進めるなら sources.kaken.mode を ids にする\n"
            f"  応答: {text}")
    return f"KAKEN の検索 API が失敗した: {text}"


def _award_year(root: ET.Element) -> int | None:
    """採択年度。summary/periodOfAward/startDate の年を使う。"""
    for summary in root.findall("summary"):
        start = summary.findtext("periodOfAward/startDate")
        if start and len(start) >= 4 and start[:4].isdigit():
            return int(start[:4])
    for grant in root.findall("grantList/grant"):
        start = grant.findtext("periodOfAward/startDate")
        if start and len(start) >= 4 and start[:4].isdigit():
            return int(start[:4])
    return None


def _ja_summary(root: ET.Element) -> ET.Element | None:
    for summary in root.findall("summary"):
        if summary.get(XML_LANG) == "ja":
            return summary
    return None


def _paragraphs(node: ET.Element) -> list[str]:
    out = []
    for p in node.findall("paragraphList/paragraph"):
        text = (p.text or "").strip()
        if text:
            out.append(text)
    return out


@register
class KakenAdapter(SourceAdapter):
    name = "kaken"

    @classmethod
    def info(cls) -> SourceInfo:
        return SourceInfo(
            name="kaken",
            title="KAKEN：科学研究費助成事業データベース（国立情報学研究所）",
            homepage="https://kaken.nii.ac.jp/",
            license=LICENSE,
            license_url="https://support.nii.ac.jp/ja/kaken/about/terms",
            attribution=("出典：KAKEN：科学研究費助成事業データベース（国立情報学研究所）"
                         "（https://kaken.nii.ac.jp/ ）をもとに iroha-dataset が加工して作成"),
            used_fields=[
                "summary/paragraphList/paragraph（研究概要・研究成果の概要）",
                "reportList/report/paragraphList/paragraph（報告書本文）",
                "summary/title, awardNumber, periodOfAward/startDate（出典表記とフィルタ用）",
            ],
            notes=[
                "採択年度 2016 以降の課題だけを使う（2015年度以前の報告書には著作物である"
                "ことを明示された部分が含まれうるため、安全側に切っている）",
                "採択年度が取れない課題は使わない",
                "出典表記が必須。課題ごとの表記を canonical の attribution に入れてある",
                "編集・加工したことの明示も必須（LICENSES.md の記載例を使う）",
                "API の検索（mode: search）には appid の登録が必要（環境変数 KAKEN_APPID）",
                "公開サーバなので request_interval を 1 秒以上空ける",
            ],
        )

    # ---------------------------------------------------------------- download
    @property
    def xml_dir(self) -> Path:
        return self.raw_dir / "xml"

    def _candidate_numbers(self) -> list[str]:
        id_file = self.source_cfg.get("id_file")
        if id_file:
            path = Path(id_file)
            if not path.is_absolute():
                path = Path.cwd() / path
            numbers = [ln.strip() for ln in path.read_text(encoding="utf-8").splitlines()]
            return [n for n in numbers if n and not n.startswith("#")]
        sample = self.source_cfg.sub("id_sample")
        years = sample.get("years", [2016, 2017, 2018, 2019, 2020])
        categories = sample.get("categories", ["K", "H"])
        lo, hi = sample.get("number_range", [1, 25000])
        out = []
        for year in years:
            for cat in categories:
                for n in range(int(lo), int(hi) + 1):
                    out.append(f"{int(year) % 100:02d}{cat}{n:05d}")
        rng = random.Random(int(self.cfg.get("seed", 42)))
        rng.shuffle(out)
        return out

    def download(self, *, force: bool = False) -> dict:
        mode = str(self.source_cfg.get("mode", "ids"))
        if mode == "search":
            return self._download_search(force=force)
        if mode != "ids":
            raise ValueError(f"sources.kaken.mode は ids / search: {mode!r}")
        return self._download_ids(force=force)

    @property
    def store_trimmed(self) -> bool:
        return bool(self.source_cfg.get("store_trimmed_xml", True))

    @property
    def store(self) -> str:
        value = str(self.source_cfg.get("store", "jsonl"))
        if value not in ("jsonl", "xml"):
            raise ValueError(f"sources.kaken.store は jsonl / xml: {value!r}")
        return value

    @property
    def jsonl_dir(self) -> Path:
        return self.raw_dir / "projects"

    def _jsonl_path(self, year: int | None) -> Path:
        return self.jsonl_dir / f"{year if year is not None else 'unknown'}.jsonl"

    def _known_numbers(self) -> set[str]:
        """すでに持っている課題番号（JSONL と XML の両方から）。"""
        have = {p.stem for p in self.xml_dir.glob("*.xml")} if self.xml_dir.exists() else set()
        if self.jsonl_dir.exists():
            for path in self.jsonl_dir.glob("*.jsonl"):
                for record in read_jsonl(path):
                    number = record.get("award_number")
                    if number:
                        have.add(number)
        return have

    def _save_xml(self, number: str, data: bytes) -> Path:
        self.xml_dir.mkdir(parents=True, exist_ok=True)
        dest = self.xml_dir / f"{number}.xml"
        dest.write_bytes(data)
        return dest

    def _record_for(self, award: ET.Element) -> dict | None:
        """grantAward から JSONL 1 行ぶんの dict を作る。使えなければ None。"""
        number = award.get("awardNumber") or ""
        year = _award_year(award)
        if not number or year is None:
            return None
        summary = _ja_summary(award)
        title = (summary.findtext("title") or "").strip() if summary is not None else ""
        paragraphs = _paragraphs(summary) if summary is not None else []
        reports: list[dict] = []
        for report in award.findall("reportList/report"):
            if report.get(XML_LANG) != "ja":
                continue
            body = _paragraphs(report)
            if not body:
                continue
            reports.append({
                "type": report.get("type") or "",
                "fiscal_year": report.get("fiscalYear") or "",
                "paragraphs": body,
            })
        if not paragraphs and not reports:
            return None
        return {
            "award_number": number,
            "award_year": year,
            "title": title,
            "summary_paragraphs": paragraphs,
            "reports": reports,
        }

    class _JsonlStore:
        """年度ごとの JSONL に追記する。年度ごとに 1 つだけファイルを開く。"""

        def __init__(self, adapter: "KakenAdapter"):
            self._adapter = adapter
            self._handles: dict[int | None, object] = {}

        def append(self, year: int | None, record: dict) -> None:
            handle = self._handles.get(year)
            if handle is None:
                path = self._adapter._jsonl_path(year)
                path.parent.mkdir(parents=True, exist_ok=True)
                handle = open(path, "a", encoding="utf-8")
                self._handles[year] = handle
            handle.write(dump_line(record))
            handle.write("\n")

        def close(self) -> None:
            for handle in self._handles.values():
                handle.close()
            self._handles.clear()

        def __enter__(self) -> "KakenAdapter._JsonlStore":
            return self

        def __exit__(self, *exc) -> None:
            self.close()

    def _save_award_xml(self, number: str, award: ET.Element, raw: bytes | None = None) -> Path:
        """1 課題を XML で保存する。store_trimmed_xml なら使うフィールドだけに削る。"""
        if self.store_trimmed:
            payload = ET.tostring(_trim_award(award), encoding="utf-8")
        elif raw is not None:
            payload = raw
        else:
            payload = ET.tostring(award, encoding="utf-8")
        return self._save_xml(number, payload)

    def _download_ids(self, *, force: bool) -> dict:
        target = int(self.source_cfg.get("max_projects", 200))
        limiter = RateLimiter(float(self.source_cfg.get("request_interval", 1.0)))
        min_year = int(self.source_cfg.get("min_award_year", 2016))
        have = self._known_numbers()
        stats = {"requested": 0, "saved": len(have), "new": 0, "missing": 0, "too_old": 0,
                 "errors": 0, "mode": "ids", "store": self.store}
        # 存在しない課題番号は HTTP 200 + 空の本文で返る（404 ではない）ので、
        # 空 or パースできない本文は「その番号の課題は無い」として missing に数える
        if not force and len(have) >= target:
            return stats
        store = self._JsonlStore(self) if self.store == "jsonl" else None
        try:
            self._download_ids_loop(stats, have, target, limiter, min_year, store, force)
        finally:
            if store is not None:
                store.close()
        return stats

    def _download_ids_loop(self, stats, have, target, limiter, min_year, store, force) -> None:
        for number in self._candidate_numbers():
            if stats["saved"] >= target:
                break
            if number in have and not force:
                continue
            limiter.wait()
            stats["requested"] += 1
            try:
                data = http_get(GRANT_XML.format(num=number), retries=2)
            except DownloadError:
                # 存在しない課題番号（404/500）。候補を機械的に作っているので普通に起きる
                stats["missing"] += 1
                continue
            except Exception:
                stats["errors"] += 1
                continue
            if not data.strip():
                stats["missing"] += 1
                continue
            try:
                root = ET.fromstring(data)
            except ET.ParseError:
                stats["errors"] += 1
                continue
            year = _award_year(root)
            if year is None or year < min_year:
                stats["too_old"] += 1
                continue
            if store is not None:
                record = self._record_for(root)
                if record is None:
                    stats["too_old"] += 1
                    continue
                store.append(record["award_year"], record)
            else:
                self._save_award_xml(number, root, raw=data)
            have.add(number)
            stats["saved"] += 1
            stats["new"] += 1

    def _download_search(self, *, force: bool) -> dict:
        """OpenSearch API で課題を集める（appid 必須）。

        分かっている API の癖（実測 2026-09-22）:

        * ``kw`` は**必須**。``s1``（助成期間）や ``qc``（研究種目）だけを指定しても
          totalResults=0 になるので、キーワードを振って集める（``search.keywords``）
        * ``kw=*`` はワイルドカードではなく文字通り「*」で検索される
        * **throttle されると totalResults=0 を黙って返す**（エラーにならない）。
          ``Invalid APPID`` や ``Exceeds allowed rate`` の 403 が返ることもある。
          どちらも「データが無い」ではないので、0 件が続いたら止めて知らせる
        * 応答には本文段落がそのまま入る（課題ごとの XML と同じスキーマ）。
          1 リクエストで最大 500 課題（``rw``）
        """
        appid = os.environ.get("KAKEN_APPID", "").strip().strip('"\'')
        if not appid:
            raise RuntimeError(
                "mode: search には appid が必要。https://api.ci.nii.ac.jp/ja/ で登録して "
                "環境変数 KAKEN_APPID に入れる（appid 不要で動かすなら "
                "sources.kaken.mode を ids にする）")
        search = self.source_cfg.sub("search")
        rows = min(500, int(search.get("rows", 500)))
        max_pages = int(search.get("max_pages", 20))
        keywords = [k for k in (search.get("keywords") or []) if k]
        if not keywords:
            raise RuntimeError("sources.kaken.search.keywords が空（kw は API の必須パラメータ）")
        max_zero = int(search.get("max_consecutive_empty", 3))
        backoff = float(search.get("empty_backoff_seconds", 120))
        empty_retries = int(search.get("empty_retries", 2))
        progress_every = int(search.get("progress_every", 10))
        min_year = int(self.source_cfg.get("min_award_year", 2016))
        target = int(self.source_cfg.get("max_projects", 200))
        limiter = RateLimiter(float(self.source_cfg.get("request_interval", 1.0)))
        have = self._known_numbers()

        # 年度を指定すると s1=s2=<年度> で 1 年ずつ取る。
        # 指定しないと API の既定ソート（研究開始年:新しい順）で新しい課題から取れるが、
        # 始まったばかりの課題は研究成果報告書が無いので本文が 1/3 しかない
        # （実測 2026-09-22: 2026年度採択 254 文字/課題 vs 2016-2025年度 799 文字/課題）。
        # 年度ごとに取れば完了済みの課題を狙える。
        years = [int(y) for y in (search.get("years") or [])]
        plans: list[tuple[str, int | None]] = (
            [(kw, y) for y in years for kw in keywords] if years
            else [(kw, None) for kw in keywords])

        stats = {"requested": 0, "saved": len(have), "new": 0, "too_old": 0, "errors": 0,
                 "empty_responses": 0, "mode": "search", "store": self.store,
                 "plans_used": 0, "total_results": {}}
        store = self._JsonlStore(self) if self.store == "jsonl" else None
        try:
            self._search_loop(stats, have, store, plans, appid, rows, max_pages, limiter,
                              min_year, target, max_zero, backoff, empty_retries,
                              progress_every, force)
        finally:
            if store is not None:
                store.close()

        # 投げたリクエストが全部 0 件で、1 件も取れずに終わった場合。
        # max_consecutive_empty は「連続回数」を見るので、キーワードが 1 語だと
        # そこに到達せずに正常終了してしまう。空のデータセットを作らないための歯止め。
        if stats["new"] == 0 and stats["requested"] > 0 \
                and stats["requested"] == stats["empty_responses"]:
            raise RuntimeError(
                f"{stats['requested']} 回投げて検索結果が全部 0 件だった。"
                "KAKEN の API は throttle されると totalResults=0 を黙って返すので、"
                "レート制限にかかっている可能性が高い。\n"
                "  ・時間をおいて、sources.kaken.search.request_interval を大きくして試す\n"
                "  ・取得済みの課題は消えないので、再実行すれば続きから集まる")
        return stats

    def _search_loop(self, stats, have, store, plans, appid, rows, max_pages, limiter,
                     min_year, target, max_zero, backoff, empty_retries,
                     progress_every, force) -> None:
        consecutive_zero = 0
        for keyword, year in plans:
            if stats["saved"] >= target:
                break
            stats["plans_used"] += 1
            label = f"{keyword}/{year}" if year is not None else keyword
            total_results = None
            page = 0
            retries_left = empty_retries
            while page < max_pages:
                if stats["saved"] >= target:
                    break
                st = 1 + page * rows
                if st > 200000:      # API の最大検索件数
                    break
                if total_results is not None and st > total_results:
                    break
                params = {
                    "appid": appid, "format": "xml", "lang": "ja",
                    "kw": keyword, "rw": rows, "st": st, "o1": 1,
                }
                if year is not None:
                    params["s1"] = year
                    params["s2"] = year
                else:
                    params["s1"] = min_year      # 開始年度が min_year 以降
                limiter.wait()
                stats["requested"] += 1
                first_request = stats["requested"] == 1
                try:
                    data = http_get(f"{SEARCH_URL}?{urllib.parse.urlencode(params)}")
                    root = ET.fromstring(data)
                except DownloadError as e:
                    # 最初のリクエストで落ちたのは設定の問題（appid が通っていない等）。
                    # 黙って errors に数えると原因が分からないので、そのまま上げる
                    if first_request:
                        raise RuntimeError(_search_failure_message(e)) from e
                    stats["errors"] += 1
                    break
                except ET.ParseError:
                    stats["errors"] += 1
                    break

                awards = [e for e in root.iter() if e.tag.split("}")[-1] == "grantAward"]
                # totalResults は**課題が返ってきた応答からだけ**覚える。
                # throttle された応答は totalResults=0 なので、これを覚えてしまうと
                # 「st > total_results」で引き直しが即座に打ち切られる
                if total_results is None and awards:
                    raw_total = root.findtext("totalResults")
                    total_results = int(raw_total) if (raw_total or "").isdigit() else None
                    stats["total_results"][label] = total_results

                if not awards:
                    # throttle されると totalResults=0 が黙って返る。
                    # 1 語だけなら「その語に該当なし」もありうるので、続いたときに止める
                    stats["empty_responses"] += 1
                    consecutive_zero += 1
                    if consecutive_zero >= max_zero:
                        raise RuntimeError(
                            f"検索結果 0 件が {consecutive_zero} 回続いた。"
                            "KAKEN の API は throttle されると totalResults=0 を黙って返すので、"
                            "レート制限にかかっている可能性が高い。\n"
                            "  ・sources.kaken.search.request_interval を大きくして時間をおく\n"
                            f"  ・ここまでに {stats['new']} 件を保存済み"
                            "（取得済みは消えないので、あとで再実行すれば続きから集まる）")
                    # 同じページを一度だけ待って引き直す（長い取得を throttle で
                    # 丸ごと落とさないため）。それでも 0 件ならこの語を諦める
                    if retries_left > 0 and backoff > 0:
                        retries_left -= 1
                        print(f"[kaken] 0 件が返った（{label} st={st}）。"
                              f"{backoff:.0f} 秒待って引き直す", flush=True)
                        time.sleep(backoff)
                        continue
                    break
                consecutive_zero = 0
                retries_left = empty_retries

                for award in awards:
                    number = award.get("awardNumber") or ""
                    if not number:
                        continue
                    if number in have and not force:
                        continue
                    award_year = _award_year(award)
                    if award_year is None or award_year < min_year:
                        stats["too_old"] += 1
                        continue
                    if store is not None:
                        record = self._record_for(award)
                        if record is None:
                            continue
                        store.append(record["award_year"], record)
                    else:
                        self._save_award_xml(number, award)
                    have.add(number)
                    stats["saved"] += 1
                    stats["new"] += 1
                if progress_every and stats["requested"] % progress_every == 0:
                    print(f"[kaken] {stats['requested']} リクエスト / "
                          f"新規 {stats['new']} 件 / いま {label} st={st}", flush=True)
                page += 1

    # ---------------------------------------------------------------- 読み込み
    def _documents_from_jsonl(self, min_year: int, fields: list[str]):
        want_summary = "summary" in fields
        want_reports = {f.split(":", 1)[1] for f in fields if f.startswith("report:")}
        if not self.jsonl_dir.exists():
            return
        for path in sorted(self.jsonl_dir.glob("*.jsonl")):
            for record in read_jsonl(path):
                year = record.get("award_year")
                if not isinstance(year, int) or year < min_year:
                    continue
                paragraphs: list[str] = []
                if want_summary:
                    paragraphs.extend(record.get("summary_paragraphs") or [])
                for report in record.get("reports") or []:
                    if (report.get("type") or "") not in want_reports:
                        continue
                    fiscal = str(report.get("fiscal_year") or "")
                    # 念のため、年度が min_year より前の報告書は使わない
                    if fiscal.isdigit() and int(fiscal) < min_year:
                        continue
                    paragraphs.extend(report.get("paragraphs") or [])
                if not paragraphs:
                    continue
                yield self._document(record.get("award_number") or "",
                                     record.get("title") or "", year, paragraphs)

    def _documents_from_xml(self, min_year: int, fields: list[str]):
        want_summary = "summary" in fields
        want_reports = {f.split(":", 1)[1] for f in fields if f.startswith("report:")}
        if not self.xml_dir.exists():
            return
        for path in sorted(self.xml_dir.glob("*.xml")):
            try:
                root = ET.parse(path).getroot()
            except ET.ParseError:
                continue
            year = _award_year(root)
            if year is None or year < min_year:
                continue
            summary = _ja_summary(root)
            title = (summary.findtext("title") or "").strip() if summary is not None else ""
            paragraphs: list[str] = []
            if want_summary and summary is not None:
                paragraphs.extend(_paragraphs(summary))
            for report in root.findall("reportList/report"):
                if report.get(XML_LANG) != "ja":
                    continue
                if (report.get("type") or "") not in want_reports:
                    continue
                fiscal = report.get("fiscalYear")
                if fiscal and fiscal.isdigit() and int(fiscal) < min_year:
                    continue
                paragraphs.extend(_paragraphs(report))
            if not paragraphs:
                continue
            yield self._document(root.get("awardNumber") or path.stem, title, year, paragraphs)

    def _document(self, number: str, title: str, year: int, paragraphs: list[str]) -> Document:
        url = GRANT_URL.format(num=number)
        attribution = (f"出典：「{title}」課題番号{number}"
                       "（KAKEN：科学研究費助成事業データベース（国立情報学研究所））"
                       f"（ {url} ）を加工して作成")
        return Document(
            document_id=f"kaken_{number}",
            source=self.name,
            paragraphs=paragraphs,
            license=LICENSE,
            url=url,
            attribution=attribution,
            meta={"award_number": number, "award_year": year, "title": title},
        )

    def documents(self) -> Iterator[Document]:
        """JSONL と XML の両方から読む（保存形式を変えても取得済みが無駄にならない）。"""
        if not self.jsonl_dir.exists() and not self.xml_dir.exists():
            raise FileNotFoundError(
                f"{self.jsonl_dir} も {self.xml_dir} も無い。"
                "先に `python -m iroha download --source kaken` を実行する")
        min_year = int(self.source_cfg.get("min_award_year", 2016))
        fields = list(self.source_cfg.get("fields", ["summary"]))
        # 読み込み数の上限は download の目標（max_projects）とは別にする。
        # 兼用にすると、キャッシュに 300 課題あるのに max_projects=200 のせいで
        # 黙って 200 課題しか使われない、という事故が起きる
        limit = int(self.source_cfg.get("max_documents", 0) or 0)
        seen: set[str] = set()
        n = 0
        for source in (self._documents_from_jsonl(min_year, fields),
                       self._documents_from_xml(min_year, fields)):
            for doc in source:
                if doc.document_id in seen:
                    continue
                seen.add(doc.document_id)
                yield doc
                n += 1
                if limit and n >= limit:
                    return
