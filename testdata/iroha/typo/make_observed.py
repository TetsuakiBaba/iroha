#!/usr/bin/env python3
"""observed.tsv（正本・手で編集する）から observed.jsonl を作り、中身を検査する（2026-10-28）。

observed.tsv は**実際に打って見つけた打ち間違い**（source = memo）と、それを組み合わせて作った確認用の例
（source = derived）。typo_bench.jsonl（書き下ろし 200 件）とは別に持ち、件数を足しても 200 件の数字は変わらない。
`typed` を iroha と同じ規則でかなに戻したものが `noisy` で、利用者の画面に出た形 `shown` と一致することを確かめる。

  ../../../dataset/iroha-typo-normalizer/.venv/bin/python make_observed.py
  macos/.build/release/iroha-cli typo eval testdata/iroha/typo/observed.jsonl
"""
from __future__ import annotations

import csv
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[2] / "dataset" / "iroha-typo-normalizer" / "src"))
from iroha.typo.romanize import to_kana  # noqa: E402


def main() -> int:
    with open(HERE / "observed.tsv", encoding="utf-8", newline="") as f:
        rows = list(csv.DictReader(f, delimiter="\t"))
    errors, out = [], []
    for r in rows:
        noisy = to_kana(r["typed"])
        if noisy != r["shown"]:
            errors.append(f"{r['id']}: typed {r['typed']} → {noisy}（shown は {r['shown']}）")
        out.append({"id": r["id"], "noisy": noisy, "clean": r["clean"],
                    "error_type": "none" if noisy == r["clean"] else "observed",
                    "source": r["source"], "typed": r["typed"], "note": r["note"]})
    if errors:
        print("\n".join(errors))
        return 1
    with open(HERE / "observed.jsonl", "w", encoding="utf-8") as f:
        f.writelines(json.dumps(o, ensure_ascii=False) + "\n" for o in out)
    n_clean = sum(o["error_type"] == "none" for o in out)
    print(f"{len(out)} 件（誤りあり {len(out) - n_clean} / 誤りなし {n_clean}） → observed.jsonl")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
