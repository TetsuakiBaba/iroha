#!/usr/bin/env python3
"""文字単位トークナイザ（SentencePiece Unigram形式・1文字=1ピース）を学習データから作る。

zenzと同じく文字単位にする理由: カタカナ読みと出力の対応が単調な文字対応になり、
サブワード分割（ジ/シン/ガナ…）が形態素と噛み合わない問題を根本から避ける。

- 正規化は identity（全角英数字・記号をそのまま保つ。NFKCにすると「５０ｃｍ」が「50cm」になる）
- add_dummy_prefix なし、空白は「▁」ピース（SentencePieceの規約。llama.cppのUGMも同じ扱い）
- ID 0=<pad> 1=</s> 2=<unk>（llama.cppのt5トークナイザの既定に合わせる）、3〜5=タグ U+EE00〜EE02
- SentencePieceのトレーナは使わず ModelProto を直接組む（vocab_sizeの制約に振り回されないため）。
  ピースのスコアは対数頻度（Unigram Viterbiは1文字ピースしか無いので分割に影響しない）

使い方:
    python3 build_tokenizer.py --data ../train-10m.txt --lines 2000000 --min-count 3 --out ./tokenizer.model
"""
import argparse
import collections
import math
import os

TAGS = ["\uee00", "\uee01", "\uee02"]  # 読みタグ・出力タグ・文脈タグ


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--data", required=True, help="学習テキスト（prepare_data.py の出力形式）")
    parser.add_argument("--lines", type=int, default=2_000_000, help="先頭から読む行数（0=全部）")
    parser.add_argument("--min-count", type=int, default=3, help="語彙に入れる最小出現回数")
    parser.add_argument("--out", default="tokenizer.model")
    args = parser.parse_args()

    os.environ["PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION"] = "python"
    from sentencepiece import sentencepiece_model_pb2 as pb
    import sentencepiece as spm

    counter: collections.Counter = collections.Counter()
    with open(args.data, encoding="utf-8") as f:
        for i, line in enumerate(f):
            if args.lines and i >= args.lines:
                break
            counter.update(line.rstrip("\n"))
    for tag in TAGS:
        counter.pop(tag, None)
    counter.pop("\n", None)
    # 空白はSentencePieceの規約で「▁」に置き換わるので、ピースも「▁」で持つ
    space = counter.pop(" ", 0)
    if space:
        counter["▁"] += space
    chars = [ch for ch, n in counter.most_common() if n >= args.min_count]
    total = sum(counter[ch] for ch in chars)

    model = pb.ModelProto()

    def add(piece: str, score: float, kind) -> None:
        p = model.pieces.add()
        p.piece, p.score, p.type = piece, score, kind

    add("<pad>", 0.0, pb.ModelProto.SentencePiece.CONTROL)
    add("</s>", 0.0, pb.ModelProto.SentencePiece.CONTROL)
    add("<unk>", 0.0, pb.ModelProto.SentencePiece.UNKNOWN)
    for tag in TAGS:
        add(tag, 0.0, pb.ModelProto.SentencePiece.USER_DEFINED)
    for ch in chars:
        add(ch, math.log(counter[ch] / total), pb.ModelProto.SentencePiece.NORMAL)

    model.trainer_spec.model_type = pb.TrainerSpec.UNIGRAM
    model.trainer_spec.vocab_size = len(model.pieces)
    model.trainer_spec.max_sentencepiece_length = 1
    model.trainer_spec.pad_id, model.trainer_spec.eos_id = 0, 1
    model.trainer_spec.unk_id, model.trainer_spec.bos_id = 2, -1
    model.normalizer_spec.name = "identity"
    model.normalizer_spec.add_dummy_prefix = False
    model.normalizer_spec.remove_extra_whitespaces = False
    model.normalizer_spec.escape_whitespaces = True
    with open(args.out, "wb") as f:
        f.write(model.SerializeToString())

    sp = spm.SentencePieceProcessor(model_file=args.out)
    print(f"vocab={sp.get_piece_size()} (chars={len(chars)}, min_count={args.min_count}, "
          f"distinct_seen={len(counter)})  pad={sp.pad_id()} eos={sp.eos_id()} unk={sp.unk_id()} "
          f"tags={[sp.piece_to_id(t) for t in TAGS]}")
    for sample in ["天気予報によると\uee00キショウ\uee01気象", "５０ｃｍのＷＯＷＯＷ", "Hello 世界"]:
        print(f"  {sample!r} -> {sp.encode(sample, out_type=str)}")


if __name__ == "__main__":
    main()
