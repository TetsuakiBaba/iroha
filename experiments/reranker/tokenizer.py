"""文字単位トークナイザ（training/t5/tokenizer.model の語彙をそのまま使う）。

採用理由: かな漢字変換は読みと候補が文字単位で単調に対応する。サブワード分割だと
学習時と推論時で分割が揺れる（llm-jp で観測。training/README.md「トークン化の一致」）が、
文字単位ならその問題がない。語彙 6,572 で埋め込み表が小さく、パラメータを Transformer 本体に
回せる。既存の T5 と同じ語彙なので比較もしやすい。

語彙の全ピースは 1 文字（<pad>/</s>/<unk> を除く）なので、sentencepiece を呼ばず
dict で写しても結果は同じ。起動時に `self_check` で突き合わせる。
空白は sentencepiece の規約どおり "▁" のピースに写す。
"""
from __future__ import annotations

import os
from pathlib import Path

import sentencepiece as spm

REPO = Path(__file__).resolve().parents[2]
DEFAULT_MODEL = REPO / "training" / "t5" / "tokenizer.model"

PAD = 0
EOS = 1
UNK = 2
SEP_READING = 3   # U+EE00: この後に読み
SEP_CAND = 4      # U+EE01: この後に候補（変換結果）
SEP_CTX = 5       # U+EE02: この後に左文脈

# セグメント ID（入力列の各位置がどの欄か）
SEG_SPECIAL = 0
SEG_CTX = 1
SEG_READING = 2
SEG_CAND = 3
NUM_SEGMENTS = 4


class CharTokenizer:
    def __init__(self, model_path: str | os.PathLike = DEFAULT_MODEL):
        self.sp = spm.SentencePieceProcessor(model_file=str(model_path))
        n = self.sp.get_piece_size()
        self.char2id: dict[str, int] = {}
        for i in range(n):
            piece = self.sp.id_to_piece(i)
            if len(piece) == 1:
                self.char2id[piece] = i
        # 追加の特殊トークン: [CLS]（採点位置）。語彙の末尾に 1 つ足す
        self.cls_id = n
        self.vocab_size = n + 1
        assert self.sp.id_to_piece(SEP_READING) == ""
        assert self.sp.id_to_piece(SEP_CAND) == ""
        assert self.sp.id_to_piece(SEP_CTX) == ""

    def encode(self, text: str) -> list[int]:
        get = self.char2id.get
        return [get("▁" if ch == " " else ch, UNK) for ch in text]

    def self_check(self, samples: list[str]) -> None:
        """dict 写しが sentencepiece と一致することを確かめる"""
        for s in samples:
            mine = self.encode(s)
            ref = self.sp.encode(s)
            if mine != ref:
                raise AssertionError(f"tokenizer mismatch for {s!r}: {mine} != {ref}")


if __name__ == "__main__":
    tok = CharTokenizer()
    tok.self_check(["使用商品レンコン蓮根 x", "今日は いい天気", "髙﨑"])
    print("vocab", tok.vocab_size, "cls", tok.cls_id, "ok")
