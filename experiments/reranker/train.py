"""候補選択専用モデルの学習（pairwise ranking）。

使い方:
  .venv/bin/python experiments/reranker/train.py --dump data/train-1m-100k-lattice.jsonl \
      --fields ctx+reading+cand --config s --epochs 1 --out runs/s-ctx

損失（既定 ranknet）:
  ranknet  : グループ内の全 (正例, 負例) 対で -log σ(s_pos - s_neg)。対で平均→グループで平均
  margin   : MarginRankingLoss（margin 1.0）。同じ対の取り方
  listwise : softmax cross entropy（正例 1 つなので -log softmax(s)[gold]）。比較用
"""
from __future__ import annotations

import argparse
import json
import math
import os
import random
import sys
import time
from pathlib import Path

import torch
import torch.nn.functional as F

sys.path.insert(0, str(Path(__file__).resolve().parent))
from data import (FIELD_MODES, SequenceBuilder, bucketed_batches, collate, describe_stats, encode_group,
                  load_groups)
from model import PRESETS, CharReranker, RerankerConfig, describe_params
from tokenizer import CharTokenizer


def pick_device(name: str) -> torch.device:
    if name != "auto":
        return torch.device(name)
    if torch.cuda.is_available():
        return torch.device("cuda")
    if torch.backends.mps.is_available():
        return torch.device("mps")
    return torch.device("cpu")


def sync(device: torch.device):
    if device.type == "cuda":
        torch.cuda.synchronize()
    elif device.type == "mps":
        torch.mps.synchronize()


def group_loss(scores: torch.Tensor, mask: torch.Tensor, kind: str, margin: float = 1.0) -> torch.Tensor:
    """scores [B,K]（[:,0] が正例）, mask [B,K] True=候補あり"""
    pos = scores[:, :1]
    neg = scores[:, 1:]
    neg_mask = mask[:, 1:]
    if kind == "listwise":
        masked = scores.masked_fill(~mask, float("-inf"))
        return -F.log_softmax(masked, dim=-1)[:, 0].mean()
    diff = pos - neg  # [B, K-1]
    if kind == "ranknet":
        per_pair = -F.logsigmoid(diff)
    elif kind == "margin":
        per_pair = F.relu(margin - diff)
    else:
        raise ValueError(kind)
    per_pair = per_pair * neg_mask
    per_group = per_pair.sum(1) / neg_mask.sum(1).clamp(min=1)
    return per_group.mean()


@torch.no_grad()
def evaluate(model, encoded_dev, device, batch_size=64):
    model.eval()
    top1 = 0
    mrr = 0.0
    n = 0
    for s in range(0, len(encoded_dev), batch_size):
        batch = encoded_dev[s : s + batch_size]
        ids, segs, mask = collate(batch)
        B, K, T = ids.shape
        scores = model(ids.view(B * K, T).to(device), segs.view(B * K, T).to(device)).view(B, K)
        scores = scores.masked_fill(~mask.to(device), float("-inf"))
        rank = (scores > scores[:, :1]).sum(1) + 1  # 正例の順位（同点は正例を上位扱い）
        top1 += (rank == 1).sum().item()
        mrr += (1.0 / rank.float()).sum().item()
        n += B
    model.train()
    return top1 / n, mrr / n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", required=True)
    ap.add_argument("--fields", default="ctx+reading+cand", choices=FIELD_MODES)
    ap.add_argument("--config", default="s", choices=list(PRESETS))
    ap.add_argument("--pool", default="cls", choices=["cls", "mean_cand"])
    ap.add_argument("--loss", default="ranknet", choices=["ranknet", "margin", "listwise"])
    ap.add_argument("--k", type=int, default=8)
    ap.add_argument("--neg-sample", default="top", choices=["top", "random"])
    ap.add_argument("--batch-groups", type=int, default=32)
    ap.add_argument("--epochs", type=float, default=1.0)
    ap.add_argument("--lr", type=float, default=3e-4)
    ap.add_argument("--warmup", type=int, default=500)
    ap.add_argument("--weight-decay", type=float, default=0.01)
    ap.add_argument("--dropout", type=float, default=0.1)
    ap.add_argument("--max-len", type=int, default=320)
    ap.add_argument("--eval-every", type=int, default=500)
    ap.add_argument("--max-steps", type=int, default=0, help="疎通用。0 なら epochs に従う")
    ap.add_argument("--max-groups", type=int, default=0, help="読み込むグループ数の上限（疎通用）")
    ap.add_argument("--device", default="auto")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    torch.manual_seed(args.seed)
    random.seed(args.seed)
    torch.backends.mha.set_fastpath_enabled(False)
    device = pick_device(args.device)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    tok = CharTokenizer()
    tok.self_check(["使用商品レンコン蓮根 x", "今日は いい天気"])
    train_groups, dev_groups, stats = load_groups(args.dump, k=args.k, neg_sample=args.neg_sample,
                                                  seed=args.seed, max_groups=args.max_groups)
    print(describe_stats(stats, len(train_groups), len(dev_groups)), flush=True)

    builder = SequenceBuilder(tok, args.fields, max_len=args.max_len)
    t0 = time.time()
    encoded_train = []
    dropped = 0
    for g in train_groups:
        e = encode_group(builder, g)
        if e is None:
            dropped += 1
        else:
            encoded_train.append(e)
    encoded_dev = [e for e in (encode_group(builder, g) for g in dev_groups) if e is not None]
    print(f"符号化 {time.time()-t0:.1f}s  学習 {len(encoded_train)}  dev {len(encoded_dev)}  "
          f"収まらず除外 {dropped}  文脈切り詰め {builder.truncated_context}", flush=True)
    # dev でのラティス 1 位精度（グループ内の候補[1] がラティス 1 位。gold が 1 位なら候補[1] は 2 位）
    # → dev 側の参照値は data.py の stats（全体）で代用する

    cfg = RerankerConfig(vocab_size=tok.vocab_size, max_len=args.max_len, dropout=args.dropout,
                         pool=args.pool, **PRESETS[args.config])
    model = CharReranker(cfg).to(device)
    print(f"設定 {args.config} {cfg.to_dict()}")
    print(describe_params(model), flush=True)

    decay, no_decay = [], []
    for n_, p in model.named_parameters():
        (no_decay if p.ndim < 2 or "emb" in n_ else decay).append(p)
    opt = torch.optim.AdamW([{"params": decay, "weight_decay": args.weight_decay},
                             {"params": no_decay, "weight_decay": 0.0}], lr=args.lr, betas=(0.9, 0.98))
    rng = random.Random(args.seed)
    steps_per_epoch = math.ceil(len(encoded_train) / args.batch_groups)
    total_steps = args.max_steps or int(steps_per_epoch * args.epochs)

    def lr_at(step):
        if step < args.warmup:
            return args.lr * (step + 1) / args.warmup
        return args.lr * max(0.0, (total_steps - step) / max(1, total_steps - args.warmup))

    (out / "config.json").write_text(json.dumps({"args": vars(args), "model": cfg.to_dict(),
                                                 "params": model.count_params(),
                                                 "tokenizer": "training/t5/tokenizer.model (char-level, vocab 6572 + [CLS])",
                                                 "device": str(device)}, ensure_ascii=False, indent=1))
    log = open(out / "log.jsonl", "a")
    print(f"device {device}  総ステップ {total_steps}（{steps_per_epoch}/epoch）", flush=True)

    step = 0
    best = -1.0
    tokens_seen = 0
    t_start = time.time()
    model.train()
    done = False
    epoch = 0
    key = lambda e: max(len(s[0]) for s in e)
    while not done:
        for batch_idx in bucketed_batches(encoded_train, args.batch_groups, key, rng):
            ids, segs, mask = collate([encoded_train[i] for i in batch_idx], k=args.k)
            B, K, T = ids.shape
            ids, segs, mask = ids.to(device), segs.to(device), mask.to(device)
            for g in opt.param_groups:
                g["lr"] = lr_at(step)
            scores = model(ids.view(B * K, T), segs.view(B * K, T)).view(B, K)
            loss = group_loss(scores, mask, args.loss)
            opt.zero_grad(set_to_none=True)
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            opt.step()
            step += 1
            tokens_seen += int(mask.sum().item()) * T
            if device.type == "mps" and step % 100 == 0:
                torch.mps.empty_cache()
            if step % 50 == 0:
                sync(device)
                el = time.time() - t_start
                rec = {"step": step, "epoch": round(step / steps_per_epoch, 3), "loss": round(loss.item(), 4),
                       "lr": lr_at(step), "tok_per_s": int(tokens_seen / el), "elapsed_s": int(el)}
                print(json.dumps(rec), flush=True)
                log.write(json.dumps(rec) + "\n")
            if step % args.eval_every == 0 or step == total_steps:
                top1, mrr = evaluate(model, encoded_dev, device)
                rec = {"step": step, "dev_top1": round(top1, 4), "dev_mrr": round(mrr, 4)}
                print(json.dumps(rec), flush=True)
                log.write(json.dumps(rec) + "\n")
                log.flush()
                if top1 > best:
                    best = top1
                    torch.save({"model": model.state_dict(), "config": cfg.to_dict(), "step": step,
                                "fields": args.fields, "dev_top1": top1, "dev_mrr": mrr}, out / "best.pt")
            if step >= total_steps:
                done = True
                break
        epoch += 1
    el = time.time() - t_start
    print(f"終了: {step} steps  {el/60:.1f} 分  best dev top1 {best:.4f}", flush=True)
    log.write(json.dumps({"final": True, "steps": step, "minutes": round(el / 60, 1), "best_dev_top1": best}) + "\n")
    log.close()


if __name__ == "__main__":
    main()
