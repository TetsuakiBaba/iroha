"""lattice-dump の JSONL から学習グループ（1 読み = 正例 1 + 負例 ≤K−1）を作る。

比較実験の 3 条件は入力列の作り方（`fields`）で切り替える:
  cand              [CLS] U+EE01 cand </s>
  reading+cand      [CLS] U+EE00 reading U+EE01 cand </s>
  ctx+reading+cand  [CLS] U+EE02 ctx U+EE00 reading U+EE01 cand </s>
"""
from __future__ import annotations

import json
import random
import zlib
from dataclasses import dataclass

import torch

from tokenizer import (CharTokenizer, EOS, PAD, SEG_CAND, SEG_CTX, SEG_READING, SEG_SPECIAL,
                       SEP_CAND, SEP_CTX, SEP_READING)

FIELD_MODES = ("cand", "reading+cand", "ctx+reading+cand")


@dataclass
class Group:
    context: str
    reading: str
    candidates: list[str]  # [0] が正例
    gold_index: int = 0


def is_kana_only(c: str) -> bool:
    """jev_judge.py と同じ判定（無変換の仮名候補）"""
    return all(("ぁ" <= ch <= "ゖ") or ("ァ" <= ch <= "ヺ") or ch in "ー・、。（）" or not ch.isalnum() for ch in c)


def is_dev(reading: str) -> bool:
    return zlib.crc32(reading.encode("utf-8")) % 50 == 0


def load_groups(path: str, k: int = 8, neg_sample: str = "top", seed: int = 0,
                max_groups: int = 0) -> tuple[list[Group], list[Group], dict]:
    """→ (train, dev, stats)。負例は gold と一致しないラティス候補。`top`: ラティス順の上位 K−1、
    `random`: 全候補から K−1 をサンプル（読み込み時に 1 回）"""
    rng = random.Random(seed)
    train: list[Group] = []
    dev: list[Group] = []
    stats = dict(lines=0, no_negative=0, gold_in_lattice=0, lattice_first=0, neg_hist={})
    with open(path, encoding="utf-8") as f:
        for line in f:
            r = json.loads(line)
            stats["lines"] += 1
            gold = r["gold"]
            negatives = [c for c in r["lattice"] if c != gold]
            if r.get("goldRank", -1) >= 0:
                stats["gold_in_lattice"] += 1
            if r.get("goldRank", -1) == 0:
                stats["lattice_first"] += 1
            if not negatives:
                stats["no_negative"] += 1
                continue
            if neg_sample == "random" and len(negatives) > k - 1:
                negatives = rng.sample(negatives, k - 1)
            else:
                negatives = negatives[: k - 1]
            stats["neg_hist"][len(negatives)] = stats["neg_hist"].get(len(negatives), 0) + 1
            g = Group(context=r.get("context", ""), reading=r["reading"], candidates=[gold] + negatives)
            (dev if is_dev(r["reading"]) else train).append(g)
            if max_groups and len(train) + len(dev) >= max_groups:
                break
    return train, dev, stats


def describe_stats(stats: dict, n_train: int, n_dev: int) -> str:
    n = max(stats["lines"], 1)
    hist = " ".join(f"{k}:{v}" for k, v in sorted(stats["neg_hist"].items()))
    return (f"行数 {stats['lines']}  学習グループ {n_train}  dev {n_dev}  負例なしで除外 {stats['no_negative']}\n"
            f"学習ドメインのラティス: 正解を含む {stats['gold_in_lattice']} ({stats['gold_in_lattice']/n*100:.1f}%)  "
            f"1位が正解 {stats['lattice_first']} ({stats['lattice_first']/n*100:.1f}%)\n"
            f"負例数の分布: {hist}")


class SequenceBuilder:
    def __init__(self, tok: CharTokenizer, fields: str, max_len: int = 320):
        assert fields in FIELD_MODES, fields
        self.tok = tok
        self.fields = fields
        self.max_len = max_len
        self.truncated_context = 0
        self.truncated_candidate = 0

    def build(self, context: str, reading: str, cand: str, allow_truncate_cand: bool = True) -> tuple[list[int], list[int]] | None:
        """→ (ids, segs)。長すぎて収まらない場合、文脈を左から削る。それでも超えれば
        allow_truncate_cand なら候補末尾を切り、そうでなければ None（学習では捨てる）"""
        enc = self.tok.encode
        r = enc(reading) if self.fields != "cand" else []
        c = enc(cand)
        ctx = enc(context) if self.fields == "ctx+reading+cand" and context else []
        # [CLS] (+ EE02 ctx) (+ EE00 reading) + EE01 + cand + </s>
        fixed = 1 + (1 + len(r) if r else 0) + 1 + len(c) + 1
        budget = self.max_len - fixed
        if ctx:
            room = budget - 1  # EE02 のぶん
            if room <= 0:
                ctx = []
            elif len(ctx) > room:
                ctx = ctx[-room:]
                self.truncated_context += 1
        total = fixed + (1 + len(ctx) if ctx else 0)
        if total > self.max_len:
            if not allow_truncate_cand:
                return None
            over = total - self.max_len
            c = c[: max(len(c) - over, 1)]
            self.truncated_candidate += 1
        ids = [self.tok.cls_id]
        segs = [SEG_SPECIAL]
        if ctx:
            ids += [SEP_CTX] + ctx
            segs += [SEG_SPECIAL] + [SEG_CTX] * len(ctx)
        if r:
            ids += [SEP_READING] + r
            segs += [SEG_SPECIAL] + [SEG_READING] * len(r)
        ids += [SEP_CAND] + c + [EOS]
        segs += [SEG_SPECIAL] + [SEG_CAND] * len(c) + [SEG_SPECIAL]
        return ids, segs


def encode_group(builder: SequenceBuilder, g: Group, allow_truncate_cand: bool = False):
    seqs = []
    for cand in g.candidates:
        s = builder.build(g.context, g.reading, cand, allow_truncate_cand=allow_truncate_cand)
        if s is None:
            return None
        seqs.append(s)
    return seqs


def collate(encoded_groups: list[list[tuple[list[int], list[int]]]], k: int = 0, pad_to: int = 32):
    """→ ids [B,K,T], segs [B,K,T], cand_mask [B,K]（True=候補あり）。
    K は k（指定時）に固定し、T は pad_to の倍数に切り上げる。MPS はテンソル形状が
    ステップごとに変わるとアロケータのキャッシュが増えて次第に遅くなるので、形状の種類を絞る"""
    B = len(encoded_groups)
    K = k or max(len(g) for g in encoded_groups)
    T = max(len(s[0]) for g in encoded_groups for s in g)
    if pad_to > 1:
        T = ((T + pad_to - 1) // pad_to) * pad_to
    ids = torch.full((B, K, T), PAD, dtype=torch.long)
    segs = torch.zeros((B, K, T), dtype=torch.long)
    mask = torch.zeros((B, K), dtype=torch.bool)
    for b, g in enumerate(encoded_groups):
        for k, (i, s) in enumerate(g):
            ids[b, k, : len(i)] = torch.tensor(i)
            segs[b, k, : len(s)] = torch.tensor(s)
            mask[b, k] = True
    return ids, segs, mask


def bucketed_batches(items: list, batch_size: int, key, rng: random.Random, chunk_batches: int = 100):
    """シャッフルした塊の中で長さ順に並べてバッチを切る（パディング削減）"""
    order = list(range(len(items)))
    rng.shuffle(order)
    chunk = batch_size * chunk_batches
    batches = []
    for start in range(0, len(order), chunk):
        part = sorted(order[start : start + chunk], key=lambda i: key(items[i]))
        for s in range(0, len(part), batch_size):
            batches.append(part[s : s + batch_size])
    rng.shuffle(batches)
    return batches


if __name__ == "__main__":
    import sys
    train, dev, stats = load_groups(sys.argv[1])
    print(describe_stats(stats, len(train), len(dev)))
    tok = CharTokenizer()
    for fields in FIELD_MODES:
        b = SequenceBuilder(tok, fields)
        lens = []
        dropped = 0
        for g in train[:20000]:
            e = encode_group(b, g)
            if e is None:
                dropped += 1
                continue
            lens.extend(len(s[0]) for s in e)
        lens.sort()
        print(f"{fields:18s} 平均長 {sum(lens)/len(lens):.1f}  q99 {lens[int(len(lens)*0.99)]}  最大 {lens[-1]}  "
              f"文脈切り詰め {b.truncated_context}  収まらず除外 {dropped}")
