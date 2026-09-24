"""KAKEN の検索モード。API を叩かずに、保存済みの応答を差し込んで検査する。

実測（2026-09-22）で分かった API の癖を固定するのが目的:

* 応答には本文段落がそのまま入る（課題ごとの XML と同じスキーマ）
* **throttle されると totalResults=0 が HTTP 200 で黙って返る。**
  これを「データが無い」と解釈すると空のデータセットができてしまうので、
  0 件が続いたら止まること
* appid が通らないと 403 + ``<detail>Invalid APPID</detail>``。
  原因が分かるメッセージになっていること
"""
import xml.etree.ElementTree as ET
from pathlib import Path

import pytest

from iroha.config import load_config
from iroha.download.http import DownloadError
from iroha.jsonlio import read_jsonl
from iroha.paths import Paths
from iroha.sources import kaken
from iroha.sources.kaken import KakenAdapter, _trim_award

FIXTURE = Path(__file__).parent / "fixtures" / "kaken_search_page.xml"
EMPTY = (b'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
         b'<grantAwards><totalResults>0</totalResults></grantAwards>')


@pytest.fixture
def adapter(tmp_path, monkeypatch):
    monkeypatch.setenv("KAKEN_APPID", "dummy-appid")
    cfg = load_config(overrides=[
        "sources.kaken.mode=search",
        "sources.kaken.request_interval=0",
        "sources.kaken.max_projects=100",
        "sources.kaken.search.max_pages=1",
        "sources.kaken.search.empty_backoff_seconds=0",   # テストで待たない
        "sources.kaken.search.progress_every=0",
        # 既定の 47 語を回すとリクエスト数が読めないので、テストでは 1 語にする
        "sources.kaken.search.keywords=[研究]",
        # 年度分割は専用のテストで確かめるので、既定ではリクエスト数を読めるように切る
        "sources.kaken.search.years=[]",
    ])
    paths = Paths(tmp_path / "out", tmp_path / "raw").ensure()
    return KakenAdapter(cfg, paths)


def _stub(monkeypatch, responses):
    """http_get を差し替える。responses は呼ばれた順に返すもののリスト。"""
    calls = []

    def fake_http_get(url, **kwargs):
        calls.append(url)
        item = responses[min(len(calls) - 1, len(responses) - 1)]
        if isinstance(item, Exception):
            raise item
        return item

    monkeypatch.setattr(kaken, "http_get", fake_http_get)
    return calls


def test_search_saves_projects_to_jsonl(adapter, monkeypatch):
    """既定は年度ごとの JSONL（1 課題 1 ファイルにすると 11 万ファイルになるため）。"""
    calls = _stub(monkeypatch, [FIXTURE.read_bytes()])
    stats = adapter.download()
    assert stats["mode"] == "search"
    assert stats["store"] == "jsonl"
    assert stats["new"] == 2
    files = sorted(p.name for p in adapter.jsonl_dir.glob("*.jsonl"))
    assert files, "JSONL が書かれていない"
    records = [r for f in adapter.jsonl_dir.glob("*.jsonl") for r in read_jsonl(f)]
    assert len(records) == 2
    for r in records:
        assert r["award_number"] and isinstance(r["award_year"], int)
        assert r["summary_paragraphs"] or r["reports"]
    # 年度ごとのファイル名になっている
    assert all(name[:-6].isdigit() for name in files)
    assert calls and "appid=dummy-appid" in calls[0]
    # 必須パラメータ（kw）と年度フィルタが乗っていること
    assert "kw=" in calls[0] and "s1=2016" in calls[0]


def test_search_can_still_save_xml(adapter, monkeypatch):
    adapter.cfg.set("sources.kaken.store", "xml")
    _stub(monkeypatch, [FIXTURE.read_bytes()])
    stats = adapter.download()
    assert stats["store"] == "xml"
    assert len(list(adapter.xml_dir.glob("*.xml"))) == 2
    assert list(adapter.documents())


def test_already_downloaded_projects_are_not_saved_twice(adapter, monkeypatch):
    _stub(monkeypatch, [FIXTURE.read_bytes()])
    first = adapter.download()
    assert first["new"] == 2
    second = adapter.download()
    assert second["new"] == 0, "同じ課題を二重に保存している"
    records = [r for f in adapter.jsonl_dir.glob("*.jsonl") for r in read_jsonl(f)]
    assert len(records) == 2


def test_documents_reads_jsonl_and_xml_together(adapter, monkeypatch):
    """保存形式を途中で変えても、取得済みが無駄にならない。"""
    _stub(monkeypatch, [FIXTURE.read_bytes()])
    adapter.download()                      # jsonl に 2 件
    jsonl_ids = {d.document_id for d in adapter.documents()}
    assert jsonl_ids

    # 別の課題番号を XML 側に置く
    root = ET.parse(FIXTURE).getroot()
    award = next(e for e in root.iter() if e.tag.split("}")[-1] == "grantAward")
    award.set("awardNumber", "99K99999")
    adapter._save_award_xml("99K99999", award)

    both = {d.document_id for d in adapter.documents()}
    assert both == jsonl_ids | {"kaken_99K99999"}


def test_saved_projects_are_readable_as_documents(adapter, monkeypatch):
    _stub(monkeypatch, [FIXTURE.read_bytes()])
    adapter.download()
    docs = list(adapter.documents())
    assert docs, "保存した課題が document として読めない"
    for doc in docs:
        assert doc.paragraphs and all(p.strip() for p in doc.paragraphs)
        assert doc.document_id.startswith("kaken_")
        assert "KAKEN" in (doc.attribution or "")
        assert doc.meta["award_year"] >= 2016


def test_silent_throttle_raises(adapter, monkeypatch):
    """totalResults=0 が続いたら「データなし」と誤解せずに止まる。"""
    _stub(monkeypatch, [EMPTY])
    with pytest.raises(RuntimeError, match="throttle|0 件"):
        adapter.download()


def test_single_empty_keyword_does_not_raise(adapter, monkeypatch):
    """1 語だけ 0 件（その語に該当なし）では止めない。"""
    adapter.cfg.set("sources.kaken.search.keywords", ["でたらめな語", "研究"])
    adapter.cfg.set("sources.kaken.search.max_consecutive_empty", 3)
    _stub(monkeypatch, [EMPTY, FIXTURE.read_bytes()])
    stats = adapter.download()
    assert stats["empty_responses"] == 1
    assert stats["new"] == 2


def test_invalid_appid_gives_an_actionable_message(adapter, monkeypatch):
    _stub(monkeypatch, [DownloadError(
        "https://kaken.nii.ac.jp/opensearch/: HTTP 403 — <detail>Invalid APPID</detail>")])
    with pytest.raises(RuntimeError, match="Invalid APPID") as e:
        adapter.download()
    assert "api.ci.nii.ac.jp" in str(e.value)
    assert "mode を ids" in str(e.value)


def test_missing_appid_is_reported(adapter, monkeypatch):
    monkeypatch.delenv("KAKEN_APPID", raising=False)
    with pytest.raises(RuntimeError, match="appid"):
        adapter.download()


def test_empty_backoff_retries_the_same_page(adapter, monkeypatch):
    """0 件が返っても、待って引き直せば続行できる（長い取得を throttle で落とさない）。"""
    adapter.cfg.set("sources.kaken.search.empty_backoff_seconds", 0.01)
    adapter.cfg.set("sources.kaken.search.empty_retries", 2)
    calls = _stub(monkeypatch, [EMPTY, FIXTURE.read_bytes()])
    stats = adapter.download()
    assert stats["empty_responses"] == 1
    assert stats["new"] == 2
    assert len(calls) == 2
    # 同じページ（st=1）を引き直している
    assert "st=1&" in calls[1] or calls[1].endswith("st=1")


def test_years_are_queried_one_at_a_time(adapter, monkeypatch):
    """search.years を指定すると s1=s2=<年度> で年度ごとに引く。

    指定しないと API の既定ソートで新しい課題から返り、始まったばかりで
    報告書が無い課題ばかりになる（本文が 1/3 になる）。
    """
    adapter.cfg.set("sources.kaken.search.keywords", ["研究"])
    adapter.cfg.set("sources.kaken.search.years", [2016, 2017])
    adapter.cfg.set("sources.kaken.max_projects", 100000)
    calls = _stub(monkeypatch, [FIXTURE.read_bytes()])
    adapter.download()
    assert len(calls) == 2, "年度ごとに 1 回ずつ引いていない"
    assert "s1=2016" in calls[0] and "s2=2016" in calls[0]
    assert "s1=2017" in calls[1] and "s2=2017" in calls[1]


def test_without_years_uses_min_award_year(adapter, monkeypatch):
    adapter.cfg.set("sources.kaken.search.keywords", ["研究"])
    adapter.cfg.set("sources.kaken.search.years", [])
    calls = _stub(monkeypatch, [FIXTURE.read_bytes()])
    adapter.download()
    assert "s1=2016" in calls[0] and "s2=" not in calls[0]


def test_trim_keeps_only_what_documents_needs():
    root = ET.parse(FIXTURE).getroot()
    award = next(e for e in root.iter() if e.tag.split("}")[-1] == "grantAward")
    trimmed = _trim_award(award)
    assert trimmed.get("awardNumber")
    summaries = trimmed.findall("summary")
    assert summaries and summaries[0].findtext("title")
    assert summaries[0].findall("paragraphList/paragraph")
    assert summaries[0].findtext("periodOfAward/startDate")
    # 大きくて使わないものは落ちている
    assert trimmed.find("productList") is None
    assert trimmed.find("memberList") is None
