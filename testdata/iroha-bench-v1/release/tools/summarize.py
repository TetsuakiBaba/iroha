#!/usr/bin/env python3
"""かな漢字変換の評価結果を 1 表にまとめる。

入力:
  --items  評価セット（AJIMEE-Bench と同じ形式の JSON。index の先頭がソース名）
  --iroha  NAME=FILE  iroha-cli ajimee の出力（誤答の行 ``  ✗ [index] 読み -> 出力 （正解: …）``）
  --anco   NAME=FILE  azooKey の anco evaluate の出力 JSON（items[].outputs[0].text が第一候補）
  --ref    対応のある比較（McNemar 検定）の基準にするモデル名

出力: ソースごとの acc@1、書き言葉・話し言葉・全体、MinCER、基準との勝ち負け（Markdown）。
"""
from __future__ import annotations

import argparse
import difflib
import json
import math
import re
from collections import defaultdict

WRITTEN = ["llmjp_kaken", "llmjp_egov", "llmjp_patent", "llmjp_aozora"]
SPOKEN = ["realpersonachat", "mrmp", "jmrd", "newschat", "jcre3"]
LINE = re.compile(r"^  ✗ \[(?P<index>[^\]]+)\] (?P<input>.*?) -> (?P<output>.*) （正解: (?P<expected>.*)）$")


def edit_distance(a: str, b: str) -> int:
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i] + [0] * len(b)
        for j, cb in enumerate(b, 1):
            cur[j] = min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb))
        prev = cur
    return prev[-1]


KANA = re.compile(r"^[ぁ-ゖァ-ヺー・]*$")


def notation_only(output: str, expected: str) -> bool:
    """違う部分がどれも「片方がかなだけ」なら表記の揺れとみなす（「美味しい / おいしい」「及び / および」）。
    漢字どうしの違い（「後者 / 校舎」）は揺れに数えない。"""
    ops = difflib.SequenceMatcher(None, output, expected, autojunk=False).get_opcodes()
    diffs = [(output[i1:i2], expected[j1:j2]) for tag, i1, i2, j1, j2 in ops if tag != "equal"]
    return bool(diffs) and all(KANA.match(a) or KANA.match(b) for a, b in diffs)


def source_of(index: str) -> str:
    return index.rsplit("-", 1)[0]


def outputs_from_iroha(path: str, items: list[dict]) -> dict[str, str]:
    wrong = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = LINE.match(line.rstrip("\n"))
            if m:
                wrong[m["index"]] = m["output"]
    # 誤答に出てこない項目は正解（出力 = 正解の 1 つ目）
    return {it["index"]: wrong.get(it["index"], it["expected_output"][0]) for it in items}


def outputs_from_anco(path: str, items: list[dict]) -> dict[str, str]:
    with open(path, encoding="utf-8") as f:
        result = json.load(f)
    rows = result["items"]
    assert len(rows) == len(items), f"{path}: 件数が合わない（{len(rows)} / {len(items)}）"
    out = {}
    for it, row in zip(items, rows):
        outputs = row.get("outputs") or []
        out[it["index"]] = outputs[0]["text"] if outputs else it["input"]
    return out


def mcnemar_p(b: int, c: int) -> float:
    """両側の正確二項検定（b: 基準だけ正解、c: 比較先だけ正解）。"""
    n = b + c
    if n == 0:
        return 1.0
    k = min(b, c)
    tail = sum(math.comb(n, i) for i in range(k + 1)) / 2 ** n
    return min(1.0, 2 * tail)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--items", required=True)
    ap.add_argument("--iroha", action="append", default=[])
    ap.add_argument("--anco", action="append", default=[])
    ap.add_argument("--ref", default=None)
    args = ap.parse_args()

    with open(args.items, encoding="utf-8") as f:
        items = json.load(f)
    expected = {it["index"]: it["expected_output"] for it in items}
    sources = [s for s in WRITTEN + SPOKEN if any(source_of(i["index"]) == s for i in items)]

    models: dict[str, dict[str, str]] = {}
    for spec in args.iroha:
        name, path = spec.split("=", 1)
        models[name] = outputs_from_iroha(path, items)
    for spec in args.anco:
        name, path = spec.split("=", 1)
        models[name] = outputs_from_anco(path, items)

    hits = {name: {i: outs[i] in expected[i] for i in outs} for name, outs in models.items()}
    loose = {name: {i: outs[i] in expected[i] or any(notation_only(outs[i], e) for e in expected[i])
                    for i in outs} for name, outs in models.items()}

    def acc(name: str, pred) -> str:
        keys = [i for i in hits[name] if pred(i)]
        return f"{sum(hits[name][k] for k in keys) / len(keys) * 100:.1f}" if keys else "-"

    def min_cer(name: str) -> float:
        total = 0.0
        for i, out in models[name].items():
            total += min(edit_distance(out, e) / max(len(e), 1) for e in expected[i])
        return total / len(models[name]) * 100

    written = lambda i: source_of(i) in WRITTEN
    spoken = lambda i: source_of(i) in SPOKEN
    def loose_acc(name: str) -> str:
        return f"{sum(loose[name].values()) / len(loose[name]) * 100:.1f}"

    header = ["モデル", *sources, "書き言葉", "話し言葉", "全体", "MinCER", "全体（表記の揺れを許す）"]
    counts = defaultdict(int)
    for it in items:
        counts[source_of(it["index"])] += 1
    print("| " + " | ".join(header) + " |")
    print("|" + "---|" * len(header))
    print("| 件数 | " + " | ".join(str(counts[s]) for s in sources)
          + f" | {sum(counts[s] for s in WRITTEN)} | {sum(counts[s] for s in SPOKEN)} | {len(items)} | | |")
    for name in models:
        row = [name] + [acc(name, lambda i, s=s: source_of(i) == s) for s in sources]
        row += [acc(name, written), acc(name, spoken), acc(name, lambda i: True), f"{min_cer(name):.2f}",
                loose_acc(name)]
        print("| " + " | ".join(row) + " |")

    if args.ref and args.ref in hits:
        print()
        print(f"基準 {args.ref} との対応のある比較（両方の結果が分かれた項目だけを数える。p は McNemar の正確検定）")
        print()
        print("| モデル | 基準だけ正解 | このモデルだけ正解 | 差（pt） | p |")
        print("|---|---|---|---|---|")
        ref = hits[args.ref]
        for name in models:
            if name == args.ref:
                continue
            b = sum(1 for i in ref if ref[i] and not hits[name][i])
            c = sum(1 for i in ref if not ref[i] and hits[name][i])
            print(f"| {name} | {b} | {c} | {(c - b) / len(ref) * 100:+.1f} | {mcnemar_p(b, c):.3g} |")


if __name__ == "__main__":
    main()
