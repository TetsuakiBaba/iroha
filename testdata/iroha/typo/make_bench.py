#!/usr/bin/env python3
"""bench.tsv（正本・手で編集する）から typo_bench.jsonl と REVIEW.md を作り、中身を検査する。

    cd testdata/iroha/typo
    ../../../iroha-dataset/.venv/bin/python make_bench.py            # 作る + 検査
    ../../../iroha-dataset/.venv/bin/python make_bench.py --corpus   # 学習コーパスとの重なりも数える（数分）

誤りのある入力（noisy）は、``typed``（実際に打った打鍵列）を iroha と同じ規則でかなに戻して作る
（iroha-dataset の ``to_kana``）。解決できない打鍵がローマ字のまま残るのも iroha の挙動どおり。

検査すること（1 つでも引っかかれば終了コード 1）:

* 誤りなし（error_type = none）は typed が空で noisy == clean、誤りありは noisy != clean
* 正しい読みの打鍵列（nn / contextual の近い方）と typed の距離（OSA）が誤りの数と一致する
  （error_type の ``+`` で数える。2 つなら 2）
* 読みがモデルの語彙（training/typo-normalizer/data/vocab-120.json）に収まる。
  収まらないと ``iroha-cli typo eval`` は素通しにするので測れない
* 読みが 4 文字以上（本体は最低文字数 4 未満では訂正を走らせない）
* id・(noisy, clean) の重複がない
"""
from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / "iroha-dataset"))

from iroha_dataset.typo.romanize import romanize, to_kana  # noqa: E402
from iroha_dataset.wild.jwtd import osa_distance  # noqa: E402

VOCAB = ROOT / "experiments" / "typo-normalizer" / "data" / "vocab-120.json"
CORPUS = [ROOT / "iroha-dataset" / "data" / "typo-corpus" / d / "train.jsonl"
          for d in ("readings-balanced", "readings-full")]
MIN_CHARS = 4


def load_rows() -> list[dict]:
    with open(HERE / "bench.tsv", encoding="utf-8", newline="") as f:
        return [dict(r) for r in csv.DictReader(f, delimiter="\t")]


def keys_for(clean: str, typed: str) -> tuple[str, int]:
    """正しい読みの打鍵列（typed に近い方の打ち方）と距離"""
    best = None
    for style in ("nn", "contextual"):
        k = romanize(clean, style)
        d = osa_distance(k, typed) if typed else 0
        if best is None or d < best[1]:
            best = (k, d)
    return best


def corpus_hits(readings: set[str]) -> dict[str, set[str]]:
    hits: dict[str, set[str]] = {}
    for path in CORPUS:
        if not path.exists():
            continue
        found: set[str] = set()
        with open(path, encoding="utf-8") as f:
            for line in f:
                r = json.loads(line)["reading"]
                if r in readings:
                    found.add(r)
        hits[path.parent.name] = found
    return hits


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", action="store_true", help="学習コーパスとの重なりも数える")
    args = ap.parse_args()

    vocab = set(json.loads(VOCAB.read_text(encoding="utf-8"))) if VOCAB.exists() else None
    rows = load_rows()
    problems: list[str] = []
    out: list[dict] = []
    seen_ids: set[str] = set()
    seen_pairs: set[tuple[str, str]] = set()
    for r in rows:
        rid, clean, typed, et = r["id"], r["clean"], r["typed"], r["error_type"]
        if rid in seen_ids:
            problems.append(f"{rid}: id が重複")
        seen_ids.add(rid)
        if et == "none":
            if typed:
                problems.append(f"{rid}: 誤りなしなのに typed がある")
            noisy, keys, dist, want = clean, romanize(clean, "nn"), 0, 0
        else:
            noisy = to_kana(typed)
            keys, dist = keys_for(clean, typed)
            want = et.count("+") + 1
            if noisy == clean:
                problems.append(f"{rid}: typed をかなに戻すと正しい読みと同じ（{typed} → {noisy}）")
            if dist != want:
                problems.append(f"{rid}: 打鍵列の距離 {dist} が誤りの数 {want} と違う（{keys} / {typed}）")
        for label, s in (("clean", clean), ("noisy", noisy)):
            if len(s) < MIN_CHARS:
                problems.append(f"{rid}: {label} が {MIN_CHARS} 文字未満（{s}）")
            if vocab is not None:
                bad = sorted({c for c in s if c not in vocab and not ("ぁ" <= c <= "ゖ")})
                if bad:
                    problems.append(f"{rid}: {label} に語彙外の文字 {bad}")
        if (noisy, clean) in seen_pairs:
            problems.append(f"{rid}: (noisy, clean) が重複")
        seen_pairs.add((noisy, clean))
        out.append({"id": rid, "noisy": noisy, "clean": clean, "error_type": et,
                    "category": r["category"], "domain": r["domain"], "surface": r["surface"],
                    "typed": typed, "keys_clean": keys, "keystroke_distance": dist, "note": r["note"]})

    hits = {}
    if args.corpus:
        hits = corpus_hits({o["clean"] for o in out} | {o["noisy"] for o in out})
        for o in out:
            o["clean_in_corpus"] = [k for k, v in hits.items() if o["clean"] in v]
            if o["error_type"] != "none":
                # 誤りを入れた読みが、コーパスに正しい読みとして現れる = 別の語として成り立ちうる
                o["noisy_in_corpus"] = [k for k, v in hits.items() if o["noisy"] in v]

    with open(HERE / "typo_bench.jsonl", "w", encoding="utf-8") as f:
        for o in out:
            f.write(json.dumps(o, ensure_ascii=False) + "\n")
    write_review(out, bool(hits))

    n_clean = sum(o["error_type"] == "none" for o in out)
    print(f"{len(out)} 件（誤りあり {len(out) - n_clean} / 誤りなし {n_clean}） → typo_bench.jsonl, REVIEW.md")
    if hits:
        for k, v in hits.items():
            print(f"  コーパス {k}: 読みが重なる {len(v)} 件")
    for p in problems:
        print("  ✗", p)
    return 1 if problems else 0


def write_review(out: list[dict], with_corpus: bool) -> None:
    lines = ["# typo ベンチマーク 確認用一覧", "",
             "`bench.tsv` から `make_bench.py` が作る（手で直さない）。直すときは bench.tsv を編集して作り直す。", ""]
    groups: dict[str, list[dict]] = {}
    for o in out:
        groups.setdefault(o["category"], []).append(o)
    for cat, rows in groups.items():
        lines += [f"## {cat}（{len(rows)} 件）", ""]
        head = "| id | 表層 | 正しい読み | 入力（noisy） | 打鍵 | 型 | メモ |"
        sep = "|---|---|---|---|---|---|---|"
        if with_corpus:
            head = head[:-1] + " コーパス |"
            sep += "---|"
        lines += [head, sep]
        for o in rows:
            typed = f"`{o['typed']}`" if o["typed"] else ""
            row = (f"| {o['id']} | {o['surface']} | {o['clean']} | {o['noisy']} | {typed} | "
                   f"{o['error_type']} | {o['note']} |")
            if with_corpus:
                flags = []
                if o.get("clean_in_corpus"):
                    flags.append("正解が学習にある")
                if o.get("noisy_in_corpus"):
                    flags.append("**入力も読みとして存在**")
                row = row[:-1] + f" {'・'.join(flags)} |"
            lines.append(row)
        lines.append("")
    (HERE / "REVIEW.md").write_text("\n".join(lines), encoding="utf-8")


if __name__ == "__main__":
    sys.exit(main())
