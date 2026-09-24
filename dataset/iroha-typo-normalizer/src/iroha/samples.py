"""spot check 用のランダム抽出（``samples`` コマンド）。

人間が目で見て気づけることがいちばん多いので、canonical / kkc / typo それぞれから
数百件を落として ``data/samples/*.txt`` に書く。seed 固定で再現する。
"""
from __future__ import annotations

import random
from pathlib import Path

from iroha.config import Config
from iroha.jsonlio import read_jsonl
from iroha.paths import Paths


def _reservoir(iterable, k: int, rng: random.Random) -> list:
    """1 パスで k 件のランダム抽出（ファイルを 2 度読まない）。"""
    out: list = []
    for i, item in enumerate(iterable):
        if i < k:
            out.append(item)
        else:
            j = rng.randint(0, i)
            if j < k:
                out[j] = item
    return out


def _canonical_records(paths: Paths):
    for path in sorted(paths.canonical.glob("*.jsonl")):
        if path.name.endswith(".morphemes.jsonl"):
            continue
        yield from read_jsonl(path)


def _split_records(directory: Path):
    for name in ("train", "validation", "test"):
        path = directory / f"{name}.jsonl"
        if path.exists():
            yield from read_jsonl(path)


def write_canonical_samples(paths: Paths, k: int, rng: random.Random) -> int:
    records = _reservoir(_canonical_records(paths), k, rng)
    lines = []
    for r in records:
        lines.append(f"[{r['id']}] source={r['source']} license={r.get('license')} "
                     f"conf={r.get('reading_confidence')} oov={r.get('has_oov')}")
        if r.get("previous_text"):
            lines.append(f"  前文  : {r['previous_text']}")
        lines.append(f"  原文  : {r['text']}")
        lines.append(f"  読み  : {r['reading']}")
        lines.append("  チャンク: " + " / ".join(
            f"{c['target']}({c['reading']})" for c in r.get("chunks", [])))
        if r.get("confidence_reasons"):
            lines.append("  low の理由: " + ", ".join(r["confidence_reasons"][:5]))
        lines.append("")
    path = paths.samples / "canonical_samples.txt"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")
    return len(records)


def write_kkc_samples(paths: Paths, k: int, rng: random.Random) -> int:
    records = _reservoir(_split_records(paths.kkc), k, rng)
    lines = []
    for r in records:
        lines.append(f"[{r['id']}] source={r['source']} conf={r.get('reading_confidence')}")
        lines.append(f"  context: {r['context']}")
        lines.append(f"  input  : {r['input']}")
        lines.append(f"  target : {r['target']}")
        lines.append("")
    path = paths.samples / "kkc_samples.txt"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")
    return len(records)


def write_typo_samples(paths: Paths, k: int, rng: random.Random) -> int:
    records = _reservoir(_split_records(paths.typo), k, rng)
    lines = []
    for r in records:
        lines.append(f"[{r['id']}] source={r['source']} unit={r.get('unit')} "
                     f"type={r['error_type']} style={r.get('romaji_style')}")
        lines.append(f"  input : {r['input']}")
        lines.append(f"  target: {r['target']}")
        if r.get("detail"):
            lines.append(f"  detail: {r['detail']}")
        lines.append("")
    path = paths.samples / "typo_samples.txt"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")
    return len(records)


def run_samples(cfg: Config, paths: Paths) -> dict:
    k = int(cfg.get("samples.per_file", 150))
    seed = int(cfg.get("seed", 42))
    out = {}
    # ファイルごとに別の seed（同じ並びの先頭が 3 つ並ばないように）
    out["canonical"] = write_canonical_samples(paths, k, random.Random(f"{seed}:canonical"))
    out["kkc"] = write_kkc_samples(paths, k, random.Random(f"{seed}:kkc"))
    out["typo"] = write_typo_samples(paths, k, random.Random(f"{seed}:typo"))
    return out
