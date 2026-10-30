# 変換時間の集計（iroha-cli ajimee の IROHA_TIMINGS の TSV、同じモデルを何周か測ったもの）。
#   python3 tools/latency_stats.py <IROHA_TIMINGS の TSV を置いたディレクトリ>   # ファイル名は <モデル名>.run<周>.tsv
# 項目ごとに周の中央値をとってから、平均・中央値・95/99 パーセンタイルと、読みの長さ別の中央値を出す。
import csv, glob, os, statistics, sys
from collections import defaultdict

d = sys.argv[1]
runs = defaultdict(list)
for f in sorted(glob.glob(os.path.join(d, "*.run*.tsv"))):
    runs[os.path.basename(f).rsplit(".run", 1)[0]].append(f)

def pct(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(round(p / 100 * (len(xs) - 1))))]

bins = [(1, 10), (11, 20), (21, 30), (31, 40)]  # iroha-bench-v1 の読みは 8〜40 字
print("| モデル | 周 | 周ごとの平均 ms | 平均 | 中央値 | p95 | p99 | " + " | ".join(f"読み{a}〜{b}字" for a, b in bins) + " |")
print("|---|---|---|---|---|---|---|" + "---|" * len(bins))
for model, files in runs.items():
    per = defaultdict(list); length = {}
    means = []
    for f in files:
        rows = list(csv.DictReader(open(f), delimiter="\t"))
        means.append(statistics.mean(float(r["ms"]) for r in rows))
        for r in rows:
            per[r["index"]].append(float(r["ms"])); length[r["index"]] = int(r["reading_length"])
    ms = {k: statistics.median(v) for k, v in per.items()}
    xs = list(ms.values())
    cols = []
    for a, b in bins:
        sel = [ms[k] for k in ms if a <= length[k] <= b]
        cols.append(f"{statistics.median(sel):.1f}（{len(sel)}）" if sel else "—")
    print(f"| {model} | {len(files)} | {' / '.join(f'{m:.1f}' for m in means)} | {statistics.mean(xs):.1f} | {statistics.median(xs):.1f} | "
          f"{pct(xs, 95):.1f} | {pct(xs, 99):.1f} | " + " | ".join(cols) + " |")
