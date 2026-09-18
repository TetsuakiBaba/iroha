#!/usr/bin/env python3
"""jev 方式（生成せず、選択肢ラベルのロジットを1回の forward pass で読む）で
かな漢字変換の候補から正解を選ばせる実験。

入力は `iroha-cli ajimee-dump` の JSONL（辞書ラティスの候補・zenz 生成・zenz 採点）。
一般の LLM（GGUF, llama.cpp）に「左文脈・読み・候補 A〜」を見せ、次トークンが
ラベル A〜 のどれかのロジットだけを読む（OpenJev の direct typed logits と同じ）。

使い方:
  .venv/bin/python experiments/jev/jev_judge.py <model.gguf> <dump.jsonl> [--pool lattice|lattice+gen]
      [--shuffle SEED] [--limit N] [--verbose] [--chat] [--n-gpu-layers N]
"""
import argparse, json, math, random, statistics, sys, time
import numpy as np
from llama_cpp import Llama, llama_get_logits_ith

LABELS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"


def hit(x, rec):
    return x in rec["expected"]


def build_prompt(context, reading, candidates, chat):
    lines = []
    lines.append("次の日本語入力（かな読み）を漢字かな混じり文に変換した候補のうち、"
                 "文脈に最も自然で正しいものを1つ選び、記号だけを答えてください。")
    if context:
        lines.append(f"直前の文脈: {context}")
    lines.append(f"読み: {reading}")
    lines.append("候補:")
    for label, cand in zip(LABELS, candidates):
        lines.append(f"{label}. {cand}")
    body = "\n".join(lines)
    if chat:
        # Qwen 系のチャットテンプレート（thinking なし）。他モデルでも概ね通る
        return ("<|im_start|>system\nあなたは日本語のかな漢字変換の判定器です。<|im_end|>\n"
                f"<|im_start|>user\n{body}<|im_end|>\n<|im_start|>assistant\n答え: ")
    return body + "\n答え: "


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("model")
    ap.add_argument("dump")
    ap.add_argument("--pool", default="lattice+gen", choices=["lattice", "lattice+gen"])
    ap.add_argument("--shuffle", type=int, default=None, help="候補順をシャッフルする乱数シード（位置バイアスの確認）")
    ap.add_argument("--limit", type=int, default=None)
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--chat", action="store_true", help="チャットテンプレートで包む")
    ap.add_argument("--n-gpu-layers", type=int, default=-1)
    ap.add_argument("--out", default=None, help="判定結果を JSONL で書く")
    ap.add_argument("--mode", default="choice", choices=["choice", "yesno", "seqlp"],
                    help="choice: 候補 A〜 から1つ選ぶ（jev の choice）。yesno: 候補ごとに正しいか 1/0 を問う（jev の noul）。"
                         "seqlp: 左文脈に続く候補文の系列対数確率で並べる（zenz と同じ方式を汎用 LLM で）")
    ap.add_argument("--drop-kana", action="store_true", help="読みをそのまま仮名にした候補（無変換）をプールから外す")
    args = ap.parse_args()

    recs = [json.loads(l) for l in open(args.dump)]
    if args.limit:
        recs = recs[: args.limit]

    llm = Llama(model_path=args.model, n_gpu_layers=args.n_gpu_layers, n_ctx=2048, n_batch=512,
                logits_all=(args.mode == "seqlp"), verbose=False)

    def last_logits():
        return np.ctypeslib.as_array(llama_get_logits_ith(llm._ctx.ctx, -1), shape=(llm._n_vocab,))

    def log_softmax(v):
        m = float(np.max(v))
        return v - (m + math.log(float(np.sum(np.exp(v - m)))))

    # yes/no 用: "1"(正しい) / "0"(誤り) のトークン
    def single_id(text):
        t = llm.tokenize(text.encode("utf-8"), add_bos=False, special=False)
        return t[0] if len(t) == 1 else None
    yes_ids = [t for t in (single_id("1"), single_id(" 1")) if t is not None]
    no_ids = [t for t in (single_id("0"), single_id(" 0")) if t is not None]

    def yesno_prompt(context, reading, cand, chat):
        body = ("次の日本語入力（かな読み）に対する漢字かな混じり文への変換候補が、読みと文脈に照らして"
                "正しい変換なら 1、誤りなら 0 と答えてください。\n")
        if context:
            body += f"直前の文脈: {context}\n"
        body += f"読み: {reading}\n候補: {cand}"
        if chat:
            return ("<|im_start|>system\nあなたは日本語のかな漢字変換の判定器です。<|im_end|>\n"
                    f"<|im_start|>user\n{body}<|im_end|>\n<|im_start|>assistant\n答え: ")
        return body + "\n答え: "

    def score_yesno(context, reading, shown, chat):
        scores = []
        n_tok = 0
        for cand in shown:
            toks = llm.tokenize(yesno_prompt(context, reading, cand, chat).encode("utf-8"), add_bos=True, special=True)
            n_tok += len(toks)
            llm.reset()
            llm.eval(toks)
            lg = last_logits()
            yes = np.logaddexp.reduce([float(lg[t]) for t in yes_ids])
            no = np.logaddexp.reduce([float(lg[t]) for t in no_ids])
            scores.append(yes - no)  # log-odds
        return scores, n_tok

    def score_seqlp(context, shown):
        # 左文脈（なければ BOS のみ）に続く候補文の対数確率の和。候補は改行なしで文脈に直結する
        scores = []
        n_tok = 0
        prefix = llm.tokenize(context.encode("utf-8"), add_bos=True, special=False) if context \
            else llm.tokenize("".encode("utf-8"), add_bos=True, special=False)
        for cand in shown:
            full = llm.tokenize((context + cand).encode("utf-8"), add_bos=True, special=False)
            # トークン境界が文脈末でずれることがあるので、共通接頭辞の長さを使う
            k = 0
            while k < min(len(prefix), len(full)) and prefix[k] == full[k]:
                k += 1
            k = max(k, 1)
            n_tok += len(full)
            llm.reset()
            llm.eval(full)
            total = 0.0
            for pos in range(k, len(full)):
                lp = log_softmax(llm.scores[pos - 1].astype(np.float64))
                total += float(lp[full[pos]])
            scores.append(total)
        return scores, n_tok

    # ラベルのトークン ID（"A" と " A" の両方を見る。どちらで出てもよいように logsumexp）
    def ids(s):
        t = llm.tokenize(s.encode("utf-8"), add_bos=False, special=False)
        return t if len(t) == 1 else None
    label_ids = []
    for label in LABELS:
        variants = [v for v in (ids(label), ids(" " + label)) if v]
        label_ids.append([v[0] for v in variants])
    if not all(label_ids[:10]):
        print("警告: ラベルが1トークンにならない", file=sys.stderr)

    rng = random.Random(args.shuffle) if args.shuffle is not None else None
    results = []
    n_hit = 0
    n_oracle = 0
    n_gen_hit = 0
    n_resc_hit = 0
    prompt_tokens = []
    latencies = []
    pick_positions = {}
    out = open(args.out, "w") if args.out else None
    for rec in recs:
        if args.pool == "lattice":
            pool = list(rec["lattice"][:10])
        else:
            pool = list(rec["scoredCandidates"])
        if args.drop_kana:
            def is_kana_only(c):
                return all(("ぁ" <= ch <= "ゖ") or ("ァ" <= ch <= "ヺ") or ch in "ー・、。（）" or not ch.isalnum() for ch in c)
            kept = [c for c in pool if not is_kana_only(c)]
            if kept:
                pool = kept
        if not pool:
            pool = [rec["generated"] or rec["reading"]]
        pool = pool[: len(LABELS)]
        order = list(range(len(pool)))
        if rng:
            rng.shuffle(order)
        shown = [pool[i] for i in order]
        oracle = any(hit(c, rec) for c in shown)
        n_oracle += oracle
        n_gen_hit += hit(rec["generated"], rec)
        if rec["zenzScores"]:
            k = max(range(len(rec["zenzScores"])), key=lambda i: rec["zenzScores"][i])
            n_resc_hit += hit(rec["scoredCandidates"][k], rec)

        if len(shown) == 1:
            pick = shown[0]
            probs = [1.0]
            ms = 0.0
        else:
            t0 = time.perf_counter()
            if args.mode == "choice":
                prompt = build_prompt(rec["context"], rec["reading"], shown, args.chat)
                toks = llm.tokenize(prompt.encode("utf-8"), add_bos=True, special=True)
                llm.reset()
                llm.eval(toks)
                logits = last_logits()
                scores = []
                for i in range(len(shown)):
                    vals = [float(logits[t]) for t in label_ids[i]]
                    scores.append(max(vals) if len(vals) == 1 else np.logaddexp.reduce(vals))
                n_tok = len(toks)
            elif args.mode == "yesno":
                scores, n_tok = score_yesno(rec["context"], rec["reading"], shown, args.chat)
            else:
                scores, n_tok = score_seqlp(rec["context"], shown)
            ms = (time.perf_counter() - t0) * 1000
            toks = [0] * n_tok
            m = max(scores)
            e = [math.exp(s - m) for s in scores]
            z = sum(e)
            probs = [x / z for x in e]
            pos = int(np.argmax(probs))
            pick = shown[pos]
            pick_positions[pos] = pick_positions.get(pos, 0) + 1
            prompt_tokens.append(len(toks))
            latencies.append(ms)
        ok = hit(pick, rec)
        n_hit += ok
        results.append(ok)
        if out:
            out.write(json.dumps({"index": rec["index"], "pick": pick, "hit": ok, "oracle": oracle,
                                  "probs": [round(p, 4) for p in probs], "shown": shown, "ms": ms},
                                 ensure_ascii=False) + "\n")
        if args.verbose and not ok:
            print(f"  ✗ [{rec['index']}] {rec['reading']}\n     選択: {pick} (p={max(probs):.2f})  正解: {' / '.join(rec['expected'])}"
                  f"  {'（候補内に正解あり）' if oracle else '（候補内に正解なし）'}")
    n = len(recs)
    print(f"モデル: {args.model.split('/')[-1]}  mode={args.mode}  pool={args.pool}  chat={args.chat}  shuffle={args.shuffle}  drop_kana={args.drop_kana}")
    print(f"件数 {n}  jev選択 acc@1 {n_hit}/{n} ({n_hit/n*100:.1f}%)   候補内に正解あり(oracle) {n_oracle}/{n} ({n_oracle/n*100:.1f}%)")
    print(f"参考: zenz生成 {n_gen_hit}/{n} ({n_gen_hit/n*100:.1f}%)  zenz再採点(top10+生成) {n_resc_hit}/{n} ({n_resc_hit/n*100:.1f}%)")
    if pick_positions:
        print("選んだ位置の分布:", " ".join(f"{LABELS[k]}:{v}" for k, v in sorted(pick_positions.items())))
    if latencies:
        print(f"プロンプト平均 {statistics.mean(prompt_tokens):.0f} トークン（yesno/seqlp は候補分の合計）  1件あたり平均 {statistics.mean(latencies):.0f}ms  中央値 {statistics.median(latencies):.0f}ms  最大 {max(latencies):.0f}ms")


if __name__ == "__main__":
    main()
