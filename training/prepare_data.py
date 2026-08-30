#!/usr/bin/env python3
"""zenz-v2.5-dataset から かな漢字変換の学習用テキストを作る。

出力形式はzenzと同じ（1行1サンプル）:
    [U+EE02<左文脈>] U+EE00<カタカナ読み> U+EE01<変換結果> </s>相当はトークナイザ側で付与

使い方:
    pip install datasets
    python3 prepare_data.py --samples 1000000 --out train.txt
"""
import argparse

CONTEXT_TAG = "\uee02"
INPUT_TAG = "\uee00"
OUTPUT_TAG = "\uee01"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--samples", type=int, default=1_000_000,
                        help="使用するサンプル数（全体は約3300万件）")
    parser.add_argument("--out", default="train.txt")
    parser.add_argument("--context-ratio", type=float, default=0.5,
                        help="左文脈を含めるサンプルの割合（文脈あり/なし両方を学習させる）")
    args = parser.parse_args()

    from datasets import load_dataset
    dataset = load_dataset("Miwa-Keita/zenz-v2.5-dataset", split="train", streaming=True)

    written = 0
    with open(args.out, "w", encoding="utf-8") as f:
        for i, row in enumerate(dataset):
            if written >= args.samples:
                break
            input_reading = (row.get("input") or "").strip()
            output_text = (row.get("output") or "").strip()
            left_context = (row.get("left_context") or "").strip()
            if not input_reading or not output_text:
                continue
            line = ""
            # 文脈は約半分のサンプルにだけ付け、文脈なし変換も学習させる
            if left_context and (i % 100) < int(args.context_ratio * 100):
                line += CONTEXT_TAG + left_context[-40:]
            line += INPUT_TAG + input_reading + OUTPUT_TAG + output_text
            f.write(line + "\n")
            written += 1

    print(f"wrote {written} samples to {args.out}")


if __name__ == "__main__":
    main()
