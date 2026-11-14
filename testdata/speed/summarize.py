#!/usr/bin/env python3
"""iroha-cli ajimee の IROHA_TIMINGS（項目ごとの変換時間の TSV）を集計して Markdown の表にする。

macOS・Windows・Linux の iroha-cli が書く TSV は先頭の 4 列（index・reading_length・context_length・ms）が同じ。
その後ろの内訳の列は OS で違う（macOS: prompt_ms・decode_ms・decode_steps・select_ms、Windows・Linux: nn_ms・nn_calls）。
このスクリプトは列の名前で読むので、どの OS の TSV もそのまま渡せる。標準ライブラリだけで動く。

使い方:
  python3 testdata/speed/summarize.py <ラベル>=<TSV> [<ラベル>=<TSV> ...]
  python3 testdata/speed/summarize.py a.tsv b.tsv          # ラベルを省くとファイル名
手順は同じディレクトリの README.md。
"""

import csv
import math
import os
import sys

# 読みの文字数の区切り（長い読みほど生成の手数が増えるので、長さ別にも見る）
LENGTH_BINS = [(1, 10), (11, 20), (21, 30), (31, 40), (41, None)]


def percentile(sorted_values, p):
    """最近順位法（nearest-rank）。p は 0〜100"""
    if not sorted_values:
        return float("nan")
    rank = max(1, math.ceil(p / 100.0 * len(sorted_values)))
    return sorted_values[min(rank, len(sorted_values)) - 1]


def load(path):
    with open(path, encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f, delimiter="\t"))


def stats(rows):
    times = sorted(float(r["ms"]) for r in rows)
    n = len(times)
    return {
        "n": n,
        "mean": sum(times) / n if n else float("nan"),
        "p50": percentile(times, 50),
        "p90": percentile(times, 90),
        "p99": percentile(times, 99),
        "max": times[-1] if times else float("nan"),
    }


def nn_share(rows):
    """変換時間のうち NN の計算が占める割合（列があるときだけ）"""
    if not rows:
        return None
    if "nn_ms" in rows[0]:
        nn = sum(float(r["nn_ms"]) for r in rows)
    elif "prompt_ms" in rows[0] and "decode_ms" in rows[0]:
        nn = sum(float(r["prompt_ms"]) + float(r["decode_ms"]) + float(r.get("select_ms") or 0) for r in rows)
    else:
        return None
    total = sum(float(r["ms"]) for r in rows)
    return nn / total if total > 0 else None


def main(argv):
    if not argv:
        print(__doc__)
        return 1
    runs = []
    for arg in argv:
        label, _, path = arg.partition("=") if "=" in arg and not os.path.exists(arg) else ("", "", arg)
        if not label:
            label = os.path.splitext(os.path.basename(path))[0]
        runs.append((label, load(path)))

    print("| 条件 | 件数 | 平均 | 中央値 | 90% | 99% | 最大 | NN の割合 |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|")
    for label, rows in runs:
        s = stats(rows)
        share = nn_share(rows)
        share_text = f"{share * 100:.0f}%" if share is not None else "—"
        print(f"| {label} | {s['n']} | {s['mean']:.1f} | {s['p50']:.1f} | {s['p90']:.1f} | {s['p99']:.1f} "
              f"| {s['max']:.1f} | {share_text} |")
    print()
    print("（単位はミリ秒。1 件 = 読み 1 つの変換 1 回）")
    print()

    headers = []
    for low, high in LENGTH_BINS:
        headers.append(f"{low}〜{high}" if high else f"{low}〜")
    print("読みの文字数別の中央値（括弧内は件数）")
    print()
    print("| 条件 | " + " | ".join(headers) + " |")
    print("|---|" + "---:|" * len(headers))
    for label, rows in runs:
        cells = []
        for low, high in LENGTH_BINS:
            part = [r for r in rows
                    if int(r["reading_length"]) >= low and (high is None or int(r["reading_length"]) <= high)]
            if part:
                cells.append(f"{stats(part)['p50']:.1f} ({len(part)})")
            else:
                cells.append("—")
        print(f"| {label} | " + " | ".join(cells) + " |")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
