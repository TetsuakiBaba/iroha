"""話し言葉のソースの統計（``spoken-stats``）。

ソースごとに、次の 3 段の件数を 1 つにまとめて ``<data_dir>/_stats.spoken.json`` と
``<data_dir>/SPOKEN_REPORT.md`` に書く:

1. 抽出（``extraction``。発話単位）: 対話数・間引いた対話・生の発話・使わない話者・理由ごとの除外・残った発話
2. 前処理（preprocess。文単位）: 文分割後の文・品質フィルタの理由ごとの除外・重複・読みの失敗・canonical の文
3. 読み一覧（build-readings）: 文・文節の読みの数（= 最終サンプル数）と split ごとの数

1 と 2 は ``_stats.preprocess.<source>.json``、3 は ``_stats.readings.json`` から読むので、
preprocess と build-readings を回したあとに呼ぶ。
"""
from __future__ import annotations

import json

from iroha.jsonlio import write_json
from iroha.paths import Paths


def _read(path) -> dict:
    return json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}


def collect(paths: Paths, sources: list[str]) -> dict:
    readings = _read(paths.stage_stats("readings")).get("per_source", {})
    out: dict = {}
    for name in sources:
        pre = _read(paths.stage_stats(f"preprocess.{name}"))
        if not pre:
            continue
        ext = pre.get("extraction") or {}
        filt = pre.get("filtered") or {}
        rd = readings.get(name, {})
        out[name] = {
            "extraction": ext,
            "preprocess": {
                "documents": pre.get("documents", 0),
                "sentences_seen": pre.get("sentences_seen", 0),
                "filtered": filt.get("rejected", {}),
                "duplicates": pre.get("duplicates", {}),
                "reading_failures": pre.get("reading_failures", {}),
                "records": pre.get("records", 0),
            },
            "readings": {
                "sentence": rd.get("sentence", 0), "chunk": rd.get("chunk", 0),
                "train": rd.get("train", 0), "validation": rd.get("validation", 0), "test": rd.get("test", 0),
                "total": rd.get("sentence", 0) + rd.get("chunk", 0),
            },
        }
    return out


def _fmt_reasons(d: dict, limit: int = 12) -> str:
    items = list(d.items())[:limit]
    return "、".join(f"{k} {v:,}" for k, v in items) or "なし"


def render(stats: dict) -> str:
    lines = ["# 話し言葉のソースの統計（spoken-stats）", "",
             "| ソース | 対話 | 生の発話 | 抽出後の発話 | 前処理後の文 | 読み（文） | 読み（文節） | 最終サンプル |",
             "|---|---:|---:|---:|---:|---:|---:|---:|"]
    total = {"raw": 0, "kept": 0, "records": 0, "s": 0, "c": 0}
    for name, s in stats.items():
        e, p, r = s["extraction"], s["preprocess"], s["readings"]
        lines.append(f"| {name} | {e.get('dialogues', 0) - e.get('dialogues_sampled_out', 0):,} | "
                     f"{e.get('utterances_raw', 0):,} | {e.get('utterances_kept', 0):,} | {p['records']:,} | "
                     f"{r['sentence']:,} | {r['chunk']:,} | {r['total']:,} |")
        total["raw"] += e.get("utterances_raw", 0)
        total["kept"] += e.get("utterances_kept", 0)
        total["records"] += p["records"]
        total["s"] += r["sentence"]
        total["c"] += r["chunk"]
    lines.append(f"| 計 | | {total['raw']:,} | {total['kept']:,} | {total['records']:,} | "
                 f"{total['s']:,} | {total['c']:,} | {total['s'] + total['c']:,} |")
    lines += ["", "最終サンプル = 読み一覧に入った読みの数（文と文節。ほかのソースと重なる読みは先に出たソースに残る）。", ""]
    for name, s in stats.items():
        e, p = s["extraction"], s["preprocess"]
        lines += [f"## {name}", "",
                  f"- 抽出（発話単位）: 対話 {e.get('dialogues', 0):,}（うち間引き {e.get('dialogues_sampled_out', 0):,}）、"
                  f"生の発話 {e.get('utterances_raw', 0):,}、使わない話者 {e.get('utterances_skipped_speaker', 0):,}、"
                  f"除外 {e.get('rejected_total', 0):,}、残り {e.get('utterances_kept', 0):,}",
                  f"  - 除外の理由: {_fmt_reasons(e.get('rejected', {}))}",
                  f"  - 正規化（発話は残す）: {_fmt_reasons(e.get('normalized', {}))}",
                  f"- 前処理（文単位）: 文 {p['sentences_seen']:,} → canonical {p['records']:,}",
                  f"  - 品質フィルタ: {_fmt_reasons(p['filtered'])}",
                  f"  - 重複: {_fmt_reasons({k: v for k, v in p['duplicates'].items() if k != 'total'})}",
                  f"  - 読みの失敗: {_fmt_reasons(p['reading_failures'])}",
                  f"- 読み一覧: 文 {s['readings']['sentence']:,}・文節 {s['readings']['chunk']:,}"
                  f"（train {s['readings']['train']:,} / validation {s['readings']['validation']:,} / "
                  f"test {s['readings']['test']:,}）", ""]
    return "\n".join(lines)


def run_spoken_report(paths: Paths, sources: list[str]) -> dict:
    stats = collect(paths, sources)
    write_json(paths.stage_stats("spoken"), stats)
    (paths.root / "SPOKEN_REPORT.md").write_text(render(stats), encoding="utf-8")
    return stats
