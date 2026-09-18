"""AJIMEE-Bench の既存候補プール（experiments/jev/ajimee-candidates.jsonl）で候補選択モデルを評価し、
同じプール上の参照値（ラティス順位・zenz 再採点・llm-jp 150M 再採点・Qwen 系列尤度）と並べる。

使い方:
  .venv/bin/python experiments/reranker/eval_ajimee.py runs/s-ctx [runs/s-read ...] \
      [--candidates experiments/jev/ajimee-candidates.jsonl] \
      [--llmjp-dump experiments/reranker/data/ajimee-candidates-llmjp150m.jsonl] \
      [--qwen experiments/jev/results/qwen3-4b-modeseqlpdropkana.jsonl ...] [--train-dump data/...jsonl]

プール:
  lattice : JSONL の `lattice`（読み一致候補の全件。oracle 138/200 を再現する）
  full    : `scoredCandidates`（ラティス上位 10 + zenz 生成。oracle 177/200）
それぞれ「仮名のみ候補あり / なし」で測る（なしの判定は jev_judge.py と同じ）。
レイテンシは 1 件（候補集合 1 バッチ）ごとの中央値・平均。ラティス生成の時間（約 13ms）は含まない。
"""
from __future__ import annotations

import argparse
import json
import statistics
import sys
import time
from pathlib import Path

import torch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from data import SequenceBuilder, collate, is_kana_only
from model import CharReranker, RerankerConfig
from tokenizer import CharTokenizer
from train import pick_device, sync

REPO = Path(__file__).resolve().parents[2]


def hit(x, expected):
    return x in expected


def top1_mrr(ranked: list[str], expected: list[str]) -> tuple[int, float]:
    for i, c in enumerate(ranked):
        if hit(c, expected):
            return (1 if i == 0 else 0), 1.0 / (i + 1)
    return 0, 0.0


def pool_of(rec, name, drop_kana):
    cands = list(rec["lattice"] if name == "lattice" else rec["scoredCandidates"])
    if drop_kana:
        kept = [c for c in cands if not is_kana_only(c)]
        if kept:
            cands = kept
    return cands


def rank_by_scores(cands, score_of):
    return sorted(cands, key=lambda c: -score_of(c))


def summarize(rows):
    """rows: list of (top1, rr, oracle)"""
    n = len(rows)
    return {"n": n, "top1": sum(r[0] for r in rows), "mrr": sum(r[1] for r in rows) / n,
            "oracle": sum(r[2] for r in rows)}


def fmt(s):
    return f"{s['top1']}/{s['n']} ({s['top1']/s['n']*100:.1f}%) MRR {s['mrr']:.3f}"


def load_model(run_dir: Path, device):
    ck = torch.load(run_dir / "best.pt", map_location="cpu")
    cfg = RerankerConfig(**ck["config"])
    model = CharReranker(cfg)
    model.load_state_dict(ck["model"])
    model.to(device).eval()
    return model, ck


@torch.no_grad()
def score_with_model(model, builder, rec, cands, device):
    seqs = [builder.build(rec["context"], rec["reading"], c, allow_truncate_cand=True) for c in cands]
    ids, segs, _ = collate([seqs])
    K, T = ids.shape[1], ids.shape[2]
    return model(ids.view(K, T).to(device), segs.view(K, T).to(device)).float().cpu().tolist()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="*")
    ap.add_argument("--candidates", default=str(REPO / "experiments/jev/ajimee-candidates.jsonl"))
    ap.add_argument("--llmjp-dump", default=str(REPO / "experiments/reranker/data/ajimee-candidates-llmjp150m.jsonl"))
    ap.add_argument("--qwen", nargs="*", default=[
        str(REPO / "experiments/jev/results/qwen3-4b-modeseqlpdropkana.jsonl"),
        str(REPO / "experiments/jev/results/qwen2.5-7b-modeseqlpdropkana.jsonl")])
    ap.add_argument("--train-dump", default=None, help="学習ダンプ。AJIMEE の読みとの重複件数を数える")
    ap.add_argument("--device", default="auto")
    ap.add_argument("--out", default=None, help="結果 JSON の書き出し先")
    args = ap.parse_args()

    recs = [json.loads(l) for l in open(args.candidates, encoding="utf-8")]
    assert len(recs) == 200, len(recs)
    by_index = {r["index"]: r for r in recs}

    # --- 参照値の再現チェック（ずれていたら表を出さずに止める）
    oracle_lattice = sum(any(hit(c, r["expected"]) for c in r["lattice"]) for r in recs)
    oracle_full = sum(any(hit(c, r["expected"]) for c in r["scoredCandidates"]) for r in recs)
    zenz_full = 0
    for r in recs:
        z = dict(zip(r["scoredCandidates"], r["zenzScores"]))
        zenz_full += hit(max(z, key=z.get), r["expected"])
    print(f"再現チェック: oracle(lattice)={oracle_lattice} (期待138)  oracle(full)={oracle_full} (期待177)  "
          f"zenz再採点(full)={zenz_full} (期待169)")
    if (oracle_lattice, oracle_full, zenz_full) != (138, 177, 169):
        print("参照値が再現できません。候補ファイルが変わっています", file=sys.stderr)
        sys.exit(1)
    gen_hit = sum(hit(r["generated"], r["expected"]) for r in recs)

    llmjp = None
    if args.llmjp_dump and Path(args.llmjp_dump).exists():
        llmjp = {json.loads(l)["index"]: json.loads(l) for l in open(args.llmjp_dump, encoding="utf-8")}

    pools = [("lattice", False), ("lattice", True), ("full", False), ("full", True)]
    table = {}  # (system) -> {pool_key: summary}

    def add(system, pool_key, rows):
        table.setdefault(system, {})[pool_key] = summarize(rows)

    for name, drop in pools:
        key = f"{name}{'-nokana' if drop else ''}"
        rows_lat, rows_zenz, rows_llmjp = [], [], []
        for r in recs:
            cands = pool_of(r, name, drop)
            oracle = any(hit(c, r["expected"]) for c in cands)
            # ラティス順位（full プールでは zenz 生成は末尾に付いているのでラティス順のまま）
            t, rr = top1_mrr(cands, r["expected"])
            rows_lat.append((t, rr, oracle))
            z = dict(zip(r["scoredCandidates"], r["zenzScores"]))
            ranked = rank_by_scores(cands, lambda c: z.get(c, float("-inf")))
            t, rr = top1_mrr(ranked, r["expected"])
            rows_zenz.append((t, rr, oracle))
            if llmjp is not None and name == "lattice":
                lr = llmjp[r["index"]]
                s = dict(zip(lr["scoredCandidates"], lr["zenzScores"]))
                ranked = rank_by_scores(cands, lambda c: s.get(c, float("-inf")))
                t, rr = top1_mrr(ranked, r["expected"])
                rows_llmjp.append((t, rr, oracle))
        add("ラティス順位1位", key, rows_lat)
        add("zenz-v3.1-small 再採点（生成LM尤度, 95M）", key, rows_zenz)
        if rows_llmjp:
            add("iroha-llmjp-150m 再採点（生成LM尤度）", key, rows_llmjp)

    # Qwen seqlp（プールは drop-kana の full と同じ集合）
    for path in args.qwen:
        p = Path(path)
        if not p.exists():
            continue
        rows = []
        for l in open(p, encoding="utf-8"):
            q = json.loads(l)
            r = by_index[q["index"]]
            ranked = [c for _, c in sorted(zip(q["probs"], q["shown"]), key=lambda x: -x[0])]
            t, rr = top1_mrr(ranked, r["expected"])
            rows.append((t, rr, q["oracle"]))
        tag = p.stem.split("-mode")[0]
        add(f"{tag} 系列尤度（生成LM, jev実験）", "full-nokana", rows)

    # --- 本モデル
    device = pick_device(args.device)
    torch.backends.mha.set_fastpath_enabled(False)
    tok = CharTokenizer()
    latency = {}
    for run in args.runs:
        run_dir = Path(run)
        model, ck = load_model(run_dir, device)
        builder = SequenceBuilder(tok, ck["fields"], max_len=model.cfg.max_len)
        params = model.count_params()
        label = (f"Decision Model {run_dir.name}（{ck['fields']}, {params['total']/1e6:.1f}M, "
                 f"dev top1 {ck['dev_top1']*100:.1f}%）")
        for name, drop in pools:
            key = f"{name}{'-nokana' if drop else ''}"
            rows = []
            for r in recs:
                cands = pool_of(r, name, drop)
                oracle = any(hit(c, r["expected"]) for c in cands)
                scores = score_with_model(model, builder, r, cands, device)
                ranked = [c for _, c in sorted(zip(scores, cands), key=lambda x: -x[0])]
                t, rr = top1_mrr(ranked, r["expected"])
                rows.append((t, rr, oracle))
            add(label, key, rows)
        # レイテンシ（full プール、仮名あり。20 件ウォームアップ後に 200 件）
        lat = {}
        for dev_name in [device.type, "cpu"]:
            d = torch.device(dev_name)
            m = model.to(d)
            for r in recs[:20]:
                score_with_model(m, builder, r, pool_of(r, "full", False), d)
            times = []
            for r in recs:
                cands = pool_of(r, "full", False)
                sync(d)
                t0 = time.perf_counter()
                score_with_model(m, builder, r, cands, d)
                sync(d)
                times.append((time.perf_counter() - t0) * 1000)
            lat[dev_name] = {"median_ms": statistics.median(times), "mean_ms": statistics.mean(times),
                             "max_ms": max(times)}
            if dev_name == device.type and device.type == "cpu":
                break
        model.to(device)
        size_bytes = (run_dir / "best.pt").stat().st_size
        latency[label] = {"latency": lat, "params": params, "best_pt_bytes": size_bytes,
                          "truncated_candidate": builder.truncated_candidate}

    overlap = None
    if args.train_dump:
        readings = set()
        with open(args.train_dump, encoding="utf-8") as f:
            for l in f:
                readings.add(json.loads(l)["reading"])
        overlap = sum(r["reading"] in readings for r in recs)

    # --- 表
    print(f"\n参考: zenz-v3.1-small 生成（プール外の自由生成）= {gen_hit}/200 ({gen_hit/200*100:.1f}%)")
    if overlap is not None:
        print(f"AJIMEE の読みが学習ダンプに含まれる件数: {overlap}/200")
    cols = [k for _, k in [("", f"{n}{'-nokana' if d else ''}") for n, d in pools]]
    heads = {"lattice": "ラティスのみ (oracle 138)", "lattice-nokana": "ラティスのみ・仮名なし",
             "full": "ラティス+zenz生成 (oracle 177)", "full-nokana": "＋zenz生成・仮名なし"}
    print("\n| 採点方式 | " + " | ".join(heads[c] for c in cols) + " |")
    print("|---|" + "---|" * len(cols))
    for system, per in table.items():
        cells = [fmt(per[c]) if c in per else "—" for c in cols]
        print(f"| {system} | " + " | ".join(cells) + " |")
    if latency:
        print("\n| モデル | 合計 / 非埋め込み | best.pt | " + " | ".join(
            f"{d} 中央値 / 平均 ms" for d in next(iter(latency.values()))["latency"]) + " |")
        print("|---|---|---|" + "---|" * len(next(iter(latency.values()))["latency"]))
        for label, info in latency.items():
            p = info["params"]
            cells = [f"{v['median_ms']:.1f} / {v['mean_ms']:.1f}" for v in info["latency"].values()]
            print(f"| {label} | {p['total']/1e6:.1f}M / {p['non_embedding']/1e6:.1f}M | "
                  f"{info['best_pt_bytes']/1e6:.0f}MB | " + " | ".join(cells) + " |")
    oracles = {c: next(iter(table.values()))[c]["oracle"] for c in cols}
    print("\nプールの oracle:", "  ".join(f"{heads[c]}: {v}/200" for c, v in oracles.items()))
    if args.out:
        Path(args.out).write_text(json.dumps({"table": table, "latency": latency, "oracle": oracles,
                                              "zenz_generation": gen_hit, "overlap": overlap},
                                             ensure_ascii=False, indent=1))


if __name__ == "__main__":
    main()
