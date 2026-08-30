#!/usr/bin/env python3
"""学習時トークン化の検証。

train.py --llama-vocab が使う llama-cpp-python のトークン化が
iroha推論時（vendor/llama.cpp の llama_tokenize）と完全一致することを確認する。
あわせて、HFトークナイザ（従来の学習時）との分割乖離率も報告する。

使い方:
    python3 verify_tokenization.py --llama-vocab llm-jp-3-150m-vocab.gguf \
        --data train-1m.txt --samples 2000 \
        [--llama-tokenize-bin /path/to/llama-tokenize]  # vendor版バイナリとの照合（任意）

--llama-tokenize-bin を渡すと pip版llama-cpp-python と vendor版llama.cpp の
一致を直接確認できる（バージョン差異の検出。100%一致であること）。
"""
import argparse
import re
import subprocess
import sys


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--llama-vocab", required=True)
    parser.add_argument("--base", default="./base/llm-jp-3-150m")
    parser.add_argument("--data", default="train-1m.txt")
    parser.add_argument("--samples", type=int, default=2000)
    parser.add_argument("--llama-tokenize-bin", default=None,
                        help="vendor/llama.cpp の llama-tokenize バイナリ（任意）")
    args = parser.parse_args()

    from llama_cpp import Llama
    llama = Llama(model_path=args.llama_vocab, vocab_only=True, verbose=False)

    from transformers import AutoTokenizer
    hf = AutoTokenizer.from_pretrained(args.base)
    hf.add_special_tokens({"additional_special_tokens": ["\uee00", "\uee01", "\uee02"]})

    lines = []
    with open(args.data, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if line:
                lines.append(line)
            if len(lines) >= args.samples:
                break

    hf_diverged = 0
    bin_mismatch = 0
    bin_checked = 0
    for i, line in enumerate(lines):
        llama_ids = llama.tokenize(line.encode("utf-8"), add_bos=True, special=True)
        hf_ids = hf(line)["input_ids"]
        if llama_ids != hf_ids:
            hf_diverged += 1

        # vendorバイナリとの照合は重い（毎回モデルロード）ので先頭100件のみ
        if args.llama_tokenize_bin and bin_checked < 100:
            out = subprocess.run(
                [args.llama_tokenize_bin, "-m", args.llama_vocab, "-p", line,
                 "--log-disable"],
                capture_output=True, text=True).stdout
            vendor_ids = [int(m) for m in re.findall(r"^\s*(\d+) ->", out, re.M)]
            bin_checked += 1
            if vendor_ids != llama_ids:
                bin_mismatch += 1
                if bin_mismatch <= 3:
                    print(f"vendor不一致例 (line {i}): {line[:40]}…")
                    print(f"  llama-cpp-python: {llama_ids[:12]}")
                    print(f"  vendor binary   : {vendor_ids[:12]}")

    total = len(lines)
    print(f"検証サンプル: {total}件 ({args.data})")
    print(f"HFトークナイザとの分割乖離: {hf_diverged}/{total} "
          f"({hf_diverged / total * 100:.1f}%) ← これが従来の学習で不一致だった割合")
    if args.llama_tokenize_bin:
        status = "OK" if bin_mismatch == 0 else "NG"
        print(f"vendor llama.cpp との一致: {bin_checked - bin_mismatch}/{bin_checked} [{status}]")
        if bin_mismatch:
            sys.exit(1)


if __name__ == "__main__":
    main()
