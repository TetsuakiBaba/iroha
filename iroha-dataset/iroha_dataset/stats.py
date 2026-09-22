"""stats.json と REPORT.md の生成（``stats`` コマンド）。

各段（preprocess / kkc / typo）が書いた ``_stats.<stage>.json`` を集めて 1 つにまとめる。
「何件できたか」だけでなく「何件をなぜ捨てたか」を必ず載せる（教師ラベルの質が主眼なので、
捨てた理由が見えないと設定を直せない）。
"""
from __future__ import annotations

import datetime as _dt

from iroha_dataset import __version__
from iroha_dataset.config import Config
from iroha_dataset.jsonlio import count_lines, read_json, write_json
from iroha_dataset.paths import Paths, SPLITS
from iroha_dataset.sources.base import all_source_info


def _file_counts(directory) -> dict[str, int]:
    out = {}
    for split in SPLITS:
        path = directory / f"{split}.jsonl"
        out[split] = count_lines(path) if path.exists() else 0
    return out


def collect(cfg: Config, paths: Paths) -> dict:
    preprocess = read_json(paths.stage_stats("preprocess"), {}) or {}
    kkc = read_json(paths.stage_stats("kkc"), {}) or {}
    typo = read_json(paths.stage_stats("typo"), {}) or {}
    sources = preprocess.get("sources", {})

    canonical_total = sum(s.get("records", 0) for s in sources.values())
    oov_rejected = sum(s.get("reading_failures", {}).get("oov", 0) for s in sources.values())
    reading_failed = sum(sum(s.get("reading_failures", {}).values()) for s in sources.values())
    dedup_dropped = sum(s.get("duplicates", {}).get("total", 0) for s in sources.values())
    filtered = {}
    for s in sources.values():
        for reason, n in (s.get("filtered", {}).get("rejected", {}) or {}).items():
            filtered[reason] = filtered.get(reason, 0) + n

    splits = {}
    for s in sources.values():
        for name, n in (s.get("splits", {}) or {}).items():
            splits[name] = splits.get(name, 0) + n
    licenses = {}
    for s in sources.values():
        for name, n in (s.get("licenses", {}) or {}).items():
            licenses[name] = licenses.get(name, 0) + n

    return {
        "generated_at": _dt.datetime.now().astimezone().isoformat(timespec="seconds"),
        "iroha_dataset_version": __version__,
        "config": {
            "seed": cfg.get("seed"),
            "reading": cfg.get("reading"),
            "filter": cfg.get("filter"),
            "context": cfg.get("context"),
            "chunk": cfg.get("chunk"),
            "typo": cfg.get("typo"),
            "split": cfg.get("split"),
            "dedup": cfg.get("dedup"),
            "sources": {k: v.get("enabled", False) for k, v in (cfg.get("sources") or {}).items()},
        },
        "canonical": {
            "total_sentences": canonical_total,
            "per_source": {k: v.get("records", 0) for k, v in sources.items()},
            "documents_per_source": {k: v.get("documents", 0) for k, v in sources.items()},
            "splits": splits,
            "licenses": licenses,
            "low_confidence": sum(s.get("low_confidence", 0) for s in sources.values()),
            "has_oov": sum(s.get("has_oov", 0) for s in sources.values()),
            "sentences_seen": sum(s.get("sentences_seen", 0) for s in sources.values()),
            "rejected_by_filter": dict(sorted(filtered.items(), key=lambda kv: -kv[1])),
            "reading_failures": {
                "total": reading_failed,
                "oov": oov_rejected,
                "per_source": {k: v.get("reading_failures", {}) for k, v in sources.items()},
            },
            "duplicates_removed": {
                "total": dedup_dropped,
                "per_source": {k: v.get("duplicates", {}) for k, v in sources.items()},
            },
            "low_confidence_reasons": {k: v.get("low_confidence_reasons", {})
                                       for k, v in sources.items()},
        },
        "kkc": {**kkc, "files": _file_counts(paths.kkc)},
        "typo": {**typo, "files": _file_counts(paths.typo)},
        "sources_info": [
            {"name": i.name, "title": i.title, "license": i.license, "homepage": i.homepage}
            for i in all_source_info()
        ],
    }


def _table(rows: list[tuple], header: tuple) -> str:
    lines = ["| " + " | ".join(header) + " |",
             "|" + "|".join("---" for _ in header) + "|"]
    for row in rows:
        lines.append("| " + " | ".join(str(c) for c in row) + " |")
    return "\n".join(lines)


def _counts_table(d: dict, header: tuple = ("項目", "件数")) -> str:
    if not d:
        return "（なし）"
    return _table(sorted(d.items(), key=lambda kv: -kv[1] if isinstance(kv[1], int) else 0), header)


def render_report(stats: dict) -> str:
    canonical = stats["canonical"]
    kkc = stats["kkc"]
    typo = stats["typo"]
    parts: list[str] = []
    A = parts.append

    A("# iroha-dataset レポート\n")
    A(f"生成: {stats['generated_at']}　/　iroha-dataset {stats['iroha_dataset_version']}　"
      f"/　seed {stats['config']['seed']}\n")
    A("このファイルは `python -m iroha_dataset stats` が生成する。数値の定義は "
      "`data/stats.json` が正。\n")

    A("## 1. 原文（canonical）\n")
    A(f"- 文数: **{canonical['total_sentences']:,}**")
    A(f"- 走査した文数: {canonical['sentences_seen']:,}")
    A(f"- reading_confidence = low: {canonical['low_confidence']:,}")
    A(f"- OOV を含むまま残した文: {canonical['has_oov']:,}\n")

    A("### ソース別\n")
    rows = []
    for name, n in sorted(canonical["per_source"].items()):
        docs = canonical["documents_per_source"].get(name, 0)
        rows.append((name, f"{docs:,}", f"{n:,}"))
    A(_table(rows, ("ソース", "document 数", "文数")) + "\n")

    A("### ライセンス別\n")
    A(_counts_table(canonical["licenses"], ("ライセンス", "文数")) + "\n")

    A("### split（document 単位。train/test に同じ原文は入らない）\n")
    A(_counts_table(canonical["splits"], ("split", "文数")) + "\n")

    A("### フィルタで捨てた文\n")
    A(_counts_table(canonical["rejected_by_filter"], ("理由", "件数")) + "\n")

    A("### 読み生成の失敗\n")
    A(f"合計 {canonical['reading_failures']['total']:,} 件"
      f"（うち OOV 除外 {canonical['reading_failures']['oov']:,} 件）\n")
    merged: dict = {}
    for per in canonical["reading_failures"]["per_source"].values():
        for k, v in (per or {}).items():
            merged[k] = merged.get(k, 0) + v
    A(_counts_table(merged, ("理由", "件数")) + "\n")

    A("### low confidence の理由\n")
    merged = {}
    for per in canonical["low_confidence_reasons"].values():
        for k, v in (per or {}).items():
            merged[k] = merged.get(k, 0) + v
    A(_counts_table(merged, ("理由", "件数")) + "\n")

    A("### 重複除去\n")
    A(f"合計 {canonical['duplicates_removed']['total']:,} 件\n")
    merged = {}
    for per in canonical["duplicates_removed"]["per_source"].values():
        for k, v in (per or {}).items():
            if k == "total":
                continue
            merged[k] = merged.get(k, 0) + v
    A(_counts_table(merged, ("種類", "件数")) + "\n")

    A("## 2. かな漢字変換データ（kkc）\n")
    if kkc.get("total"):
        A(f"- example 数: **{kkc['total']:,}**")
        A(f"- 平均 input 長: {kkc.get('avg_input_chars', 0)} 文字　"
          f"/　最大 {kkc.get('max_input_chars', 0)} 文字")
        A(f"- 平均 context 長: {kkc.get('avg_context_chars', 0)} 文字　"
          f"/　最大 {kkc.get('max_context_chars', 0)} 文字")
        A(f"- 平均 target 長: {kkc.get('avg_target_chars', 0)} 文字")
        A(f"- 重複除去で落とした example: {kkc.get('duplicates_dropped', 0):,}\n")
        A(_counts_table(kkc.get("files", {}), ("split", "example 数")) + "\n")
        rows = [(s, *(f"{v.get(sp, 0):,}" for sp in SPLITS))
                for s, v in sorted(kkc.get("per_source", {}).items())]
        if rows:
            A("ソース別:\n")
            A(_table(rows, ("ソース", *SPLITS)) + "\n")
    else:
        A("（未生成。`build-kkc` を実行する）\n")

    A("## 3. typo normalizer データ（typo）\n")
    if typo.get("total"):
        A(f"- example 数: **{typo['total']:,}**")
        A(f"- clean: {typo.get('clean', 0):,} / typo あり: {typo.get('noisy', 0):,}")
        A(f"- clean 比率: **{typo.get('clean_ratio', 0)}**"
          f"（設定 {typo.get('clean_ratio_requested', 0)}、"
          f"この variants 数で到達可能な上限 {typo.get('clean_ratio_reachable', 0)}）")
        A(f"- 平均 input 長: {typo.get('avg_input_chars', 0)} 文字　"
          f"/　最大 {typo.get('max_input_chars', 0)} 文字")
        A(f"- 重複除去で落とした example: {typo.get('duplicates_dropped', 0):,}\n")
        A(_counts_table(typo.get("files", {}), ("split", "example 数")) + "\n")
        A("### error type の比率（要求 vs 実績）\n")
        A("促音の過不足は「っ」を含む読みにしか当てられないので、要求どおりには届かない。"
          "`typo.match_error_ratios` が不足分を優先して引いて寄せている。\n")
        requested = typo.get("error_types_requested", {}) or {}
        achieved = typo.get("error_types_achieved_ratio", {}) or {}
        rows = [(name, f"{ratio:.3f}", f"{achieved.get(name, 0.0):.3f}")
                for name, ratio in requested.items()]
        A(_table(rows, ("error_type", "要求", "実績")) + "\n" if rows else "（なし）\n")

        A("### error type 別の件数（単独のもの）\n")
        singles = {k: v for k, v in (typo.get("error_types", {}) or {}).items() if "+" not in k}
        A(_counts_table(singles, ("error_type", "件数")) + "\n")

        combos = {k: v for k, v in (typo.get("error_types", {}) or {}).items() if "+" in k}
        if combos:
            top = dict(sorted(combos.items(), key=lambda kv: -kv[1])[:15])
            A(f"### typo 2 個の組み合わせ（{len(combos)} 種 / "
              f"計 {sum(combos.values()):,} 件。上位 15 件）\n")
            A(_counts_table(top, ("error_type", "件数")) + "\n")
        A("### 生成単位別\n")
        A(_counts_table(typo.get("by_unit", {}), ("unit", "件数")) + "\n")
        if typo.get("retry_reasons"):
            A("### 引き直した理由（typo にならなかった崩し）\n")
            A(_counts_table(typo["retry_reasons"], ("理由", "件数")) + "\n")
    else:
        A("（未生成。`build-typo` を実行する）\n")

    A("## 4. ソースとライセンス\n")
    rows = [(i["name"], i["title"], i["license"]) for i in stats.get("sources_info", [])]
    A(_table(rows, ("name", "title", "license")) + "\n")
    A("出典表記・注意事項は `LICENSES.md` が正。\n")

    A("## 5. 目視確認\n")
    A("`data/samples/` に canonical / kkc / typo それぞれのランダム抽出がある"
      "（seed 固定なので再現する）。\n")
    return "\n".join(parts) + "\n"


def run_stats(cfg: Config, paths: Paths) -> dict:
    stats = collect(cfg, paths)
    write_json(paths.stats_json, stats)
    paths.report_md.write_text(render_report(stats), encoding="utf-8")
    return stats
