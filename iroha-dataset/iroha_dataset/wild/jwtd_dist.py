"""JWTD の実誤りから、typo 生成器の抽出確率を推定する（``estimate-typo-dist``）。

JWTD は学習データには使わず、**誤りの分布の推定だけ**に使う。入力は ``build-jwtd`` が書く
``data/jwtd/pairs_train.jsonl``（train の全ペア。ベンチ＝test / gold のページは除いてある）。

出力は ``typo:`` 節の上書き設定（YAML）。``load_config(path)`` で default.yaml に重ねると、
TypoGenerator がこの比率と表で typo を作る:

* ``error_types`` … 型の比率（生成器の型に対応させたもの）
* ``dist.key_rates.<型>`` … キーごとの「その誤りの起きやすさ」（誤りの数 / 意図した打鍵列でのそのキーの出現数）
* ``dist.confusion.{substitution,key_far}`` … 元キー → 誤ったキーの混同表
* ``dist.insertion_far_ratio`` … 余分な打鍵のうち、前後のキーの隣接でないものの割合
* ``dist.mora_missing`` / ``mora_duplication`` / ``word_duplication`` … 仮名（列）ごとの起きやすさ（出現数で正規化）
* ``dist.mora_missing_ctx`` … 直前の仮名 → 抜ける仮名（「直前 + 仮名」の出現数で正規化し、条件なしの率に向けて縮めたもの。
  条件なしのままだと しぶさわ → ぶさわ のように語の途中を落としやすい。実際に抜けるのは語の後ろの助詞が多い）
* ``dist.mora_extra`` … 直前の仮名 → 入る仮名（直前の仮名の出現数で正規化。先頭は "^"）
* ``dist.mora_substitution`` … 意図した仮名 → 打った仮名（意図した仮名の出現数で正規化）
* ``repeat_sokuon_bias`` / ``second_error_ratio``

推定から除くもの:

* **い抜き・い足し**（していた ⇄ してた）。編集者が文体を直したもので打ち間違いではない
* **読みの末尾での仮名の脱落**（そちら ← そちらは）。入力中は末尾が足りないのが普通で区別できない
  （本体も末尾に足すだけの訂正は捨てる）。生成器の mora_missing も最後の仮名は落とさない
* 漢字の変換誤り・英数字を含む窓など、``build-jwtd`` の段で読みの組にならなかったもの（件数だけレポートに載せる）

推定できないもの: 誤りの無い入力の割合（clean の比率）と ``mixed_input``（英字が残る誤り。
JWTD では抽出の段で捨てられ、保存前にも気づかれやすい）。``mixed_input`` はローマ字の打ち間違いではない
（IME の状態の問題）ので既定では 0 にする。入れたいときは設定 ``typo_dist.mixed_input`` で事前の値を与える
（生成器側の ``typo.mixed_input.enabled`` も true にする）。
"""
from __future__ import annotations

import json
from collections import Counter, defaultdict
from pathlib import Path

import yaml

from iroha_dataset.typo import keyboard
from iroha_dataset.typo.romanize import RomanizeError, romanize
from iroha_dataset.wild.jwtd import osa_distance

GEN_TYPES = ("deletion", "insertion", "substitution", "transposition", "repeated_key",
             "missing_double_consonant", "excessive_double_consonant", "mixed_input",
             "weak_finger_omission", "key_far", "mora_missing", "mora_extra",
             "mora_substitution", "mora_duplication", "word_duplication")
MAX_KANA = 3          # 仮名の出し入れ・置換で扱う長さの上限（それより長いのは語の書き換え）
MIN_COUNT = 3         # 表に載せる最小件数（1〜2 件のものは雑音）
TOP_N = 300           # 仮名の表の行数の上限
# 率（件数 / 出現数）を全体の平均の率に向けて縮める強さ（出現数に換算した擬似件数）。
# これが無いと、打鍵列にほとんど出ないキー（x・!）が数件の誤りで率 1 位になる
SMOOTH_KEYS = 5000
SMOOTH_KANA = 500
# 直前の仮名で条件づけた率を、条件なしの率に向けて縮める強さ
SMOOTH_CTX = 50


def _keys_pair(left: str, typed: str, intended: str, right: str) -> tuple[str, str]:
    """classify と同じく前後 2 文字を付けて打鍵列にし、「ん」の打ち方は距離の近い方を採る"""
    lc, rc = left[-2:], right[:2]
    best = None
    for st in ("nn", "contextual"):
        t, i = romanize(lc + typed + rc, st), romanize(lc + intended + rc, st)
        d = osa_distance(t, i)
        if best is None or d < best[0]:
            best = (d, t, i)
    return best[1], best[2]


def _first_diff(x: str, y: str) -> int:
    return next((i for i in range(min(len(x), len(y))) if x[i] != y[i]), min(len(x), len(y)))


def _is_i_nuki(r: dict) -> bool:
    """い抜き・い足し（差分が「い」1 つで、直前が「て」）"""
    a, b, k = r["typed"], r["intended"], r["edit_at"]
    prev = r["clean"][k - 1:k]
    return prev == "て" and ((b == "い" and a == "") or (a == "い" and b == ""))


class Estimator:
    def __init__(self):
        self.types = Counter()
        self.excluded = Counter()
        self.key_err = defaultdict(Counter)        # 型 → キー → 件数
        self.confusion = {"substitution": defaultdict(Counter), "key_far": defaultdict(Counter)}
        self.insertion_far = Counter()
        self.repeat_sokuon = Counter()
        self.mora = {"mora_missing": Counter(), "mora_duplication": Counter(),
                     "word_duplication": Counter()}
        self.mora_extra = defaultdict(Counter)     # 直前の仮名 → 入る仮名
        self.mora_missing_ctx = defaultdict(Counter)   # 直前の仮名 → 抜ける仮名
        self.mora_sub = defaultdict(Counter)       # 意図した仮名 → 打った仮名
        self.key_freq = Counter()
        self.kana_freq = Counter()                 # 長さ 1〜3 の仮名列の出現数
        self.prev_freq = Counter()                 # 挿入位置の直前の仮名（先頭 "^"）

    # ---- 分母（意図した読みでの出現数）
    def _count_clean(self, clean: str) -> None:
        try:
            self.key_freq.update(romanize(clean, "nn"))
        except RomanizeError:
            return
        for n in range(1, MAX_KANA + 2):     # 「直前 + 仮名列」の分母のため 1 文字長く数える
            self.kana_freq.update(clean[i:i + n] for i in range(len(clean) - n + 1))
            if n <= MAX_KANA:
                self.kana_freq["^" + clean[:n]] += 1
        self.prev_freq["^"] += 1
        self.prev_freq.update(clean)

    def add(self, r: dict) -> None:
        et, a, b, k = r["error_type"], r["typed"], r["intended"], r["edit_at"]
        clean = r["clean"]
        left, right = clean[:k], clean[k + len(b):]
        if _is_i_nuki(r):
            self.excluded["i_nuki"] += 1
            return
        if et == "mora_missing" and r["at_end"]:
            self.excluded["mora_missing_at_end"] += 1
            return
        self._count_clean(clean)

        if et == "mora_duplication":
            self._take("mora_duplication")
            self.mora["mora_duplication"][a] += 1
        elif et == "word_duplication":
            self._take("word_duplication")
            self.mora["word_duplication"][a] += 1
        elif et == "mora_missing":
            if len(b) > MAX_KANA:
                self.excluded["too_long"] += 1
                return
            self._take("mora_missing")
            self.mora["mora_missing"][b] += 1
            self.mora_missing_ctx[left[-1:] or "^"][b] += 1
        elif et == "mora_extra":
            if len(a) > MAX_KANA:
                self.excluded["too_long"] += 1
                return
            self._take("mora_extra")
            self.mora_extra[left[-1:] or "^"][a] += 1
        elif et == "mora_substitution":
            if len(a) > MAX_KANA or len(b) > MAX_KANA:
                self.excluded["too_long"] += 1
                return
            self._take("mora_substitution")
            self.mora_sub[b][a] += 1
        else:
            self._add_keystroke(et, left, a, b, right)

    def _take(self, kind: str) -> None:
        self.types[kind] += 1

    def _add_keystroke(self, et: str, left: str, a: str, b: str, right: str) -> None:
        try:
            typed, intended = _keys_pair(left, a, b, right)
        except RomanizeError:
            self.excluded["not_romanizable"] += 1
            return
        i = _first_diff(typed, intended)
        sokuon_delta = a.count("っ") - b.count("っ")
        if et == "key_missing":
            c = intended[i]
            kind = "missing_double_consonant" if sokuon_delta < 0 else "deletion"
            self._take(kind)
            self.key_err[kind][c] += 1
        elif et == "key_extra":
            t = typed[i]
            nxt = intended[i] if i < len(intended) else ""
            prv = intended[i - 1] if i > 0 else ""
            if t == prv and i >= 2 and intended[i - 2] == prv and sokuon_delta > 0:
                kind = "excessive_double_consonant"         # kitte → kittte
            elif t in (prv, nxt):
                kind = "repeated_key"
                self.repeat_sokuon["sokuon" if sokuon_delta > 0 else "other"] += 1
            else:
                kind = "insertion"
                near = set(keyboard.neighbors(nxt)) | set(keyboard.neighbors(prv))
                self.insertion_far["far" if t not in near else "adjacent"] += 1
                if nxt:
                    self.key_err["insertion"][nxt] += 1
            self._take(kind)
        elif et == "key_adjacent":
            c, t = intended[i], typed[i]
            self._take("substitution")
            self.key_err["substitution"][c] += 1
            self.confusion["substitution"][c][t] += 1
        elif et == "key_far":
            c, t = intended[i], typed[i]
            self._take("key_far")
            self.key_err["key_far"][c] += 1
            self.confusion["key_far"][c][t] += 1
        elif et == "key_transposition":
            self._take("transposition")
        else:
            self.excluded[f"unknown:{et}"] += 1

    # ---- 出力
    def _rates(self, counts: Counter, freq: Counter, smooth: int = SMOOTH_KANA) -> dict:
        kept = {k: c for k, c in counts.items() if c >= MIN_COUNT and freq[k] > 0}
        if not kept:
            return {}
        mean = sum(kept.values()) / sum(freq[k] for k in kept)
        out = {k: (c + smooth * mean) / (freq[k] + smooth) for k, c in kept.items()}
        top = max(out.values())
        items = sorted(out.items(), key=lambda kv: -counts[kv[0]])[:TOP_N]
        return {k: round(v / top, 6) for k, v in items}

    def config(self, mixed_input: float, second_error_ratio: float) -> dict:
        total = sum(self.types.values())
        share = {t: self.types.get(t, 0) / total * (1 - mixed_input) for t in GEN_TYPES}
        share["mixed_input"] = mixed_input
        key_rates = {kind: self._rates(c, self.key_freq, SMOOTH_KEYS) for kind, c in self.key_err.items()}
        confusion = {kind: {c: {t: n for t, n in row.most_common() if n >= MIN_COUNT}
                            for c, row in rows.items() if sum(row.values()) >= MIN_COUNT}
                     for kind, rows in self.confusion.items()}
        confusion = {kind: {c: row for c, row in rows.items() if row} for kind, rows in confusion.items()}
        # 余分な仮名: 直前の仮名ごとの率を、入る仮名の全体の率に向けて縮める
        pos_total = sum(self.prev_freq.values()) or 1
        ins_total = Counter()
        for row in self.mora_extra.values():
            ins_total.update(row)
        mora_extra = {}
        for prev, row in self.mora_extra.items():
            f = self.prev_freq[prev]
            if f <= 0:
                continue
            kept = {a: (n + SMOOTH_CTX * ins_total[a] / pos_total) / (f + SMOOTH_CTX)
                    for a, n in row.items() if n >= MIN_COUNT}
            if kept:
                mora_extra[prev] = kept
        top = max((v for row in mora_extra.values() for v in row.values()), default=1.0)
        mora_extra = {p: {a: round(v / top, 6) for a, v in row.items()} for p, row in mora_extra.items()}
        mora_sub = {}
        for b, row in self.mora_sub.items():
            if self.kana_freq[b] <= 0:
                continue
            kept = {a: n / self.kana_freq[b] for a, n in row.items() if n >= MIN_COUNT}
            if kept:
                mora_sub[b] = kept
        top = max((v for row in mora_sub.values() for v in row.values()), default=1.0)
        mora_sub = {b: {a: round(v / top, 6) for a, v in row.items()} for b, row in mora_sub.items()}
        # 仮名の脱落: 条件なしの率（絶対値）と、直前の仮名で条件づけた率を同じ尺度で出す
        miss = {b: c for b, c in self.mora["mora_missing"].items() if c >= MIN_COUNT and self.kana_freq[b] > 0}
        mean = sum(miss.values()) / max(1, sum(self.kana_freq[b] for b in miss))
        uni = {b: (c + SMOOTH_KANA * mean) / (self.kana_freq[b] + SMOOTH_KANA) for b, c in miss.items()}
        uni = dict(sorted(uni.items(), key=lambda kv: -miss[kv[0]])[:TOP_N])
        ctx_rows = {}
        for prev, row in self.mora_missing_ctx.items():
            kept = {}
            for b, n in row.items():
                f = self.kana_freq[("" if prev == "^" else prev) + b] if prev != "^" else self.kana_freq["^" + b]
                if n >= MIN_COUNT and b in uni and f > 0:
                    kept[b] = (n + SMOOTH_CTX * uni[b]) / (f + SMOOTH_CTX)
            if kept:
                ctx_rows[prev] = kept
        top = max(list(uni.values()) + [v for row in ctx_rows.values() for v in row.values()], default=1.0)
        mora_missing = {b: round(v / top, 6) for b, v in uni.items()}
        mora_missing_ctx = {p: {b: round(v / top, 6) for b, v in row.items()} for p, row in ctx_rows.items()}
        rep = self.repeat_sokuon
        ins = self.insertion_far
        return {"typo": {
            "error_types": {t: round(w, 6) for t, w in share.items()},
            "second_error_ratio": round(second_error_ratio, 6),
            "repeat_sokuon_bias": round(rep["sokuon"] / max(1, sum(rep.values())), 6),
            "dist": {
                "key_rates": key_rates,
                "confusion": confusion,
                "insertion_far_ratio": round(ins["far"] / max(1, sum(ins.values())), 6),
                "mora_missing": mora_missing,
                "mora_missing_ctx": mora_missing_ctx,
                "mora_duplication": self._rates(self.mora["mora_duplication"], self.kana_freq),
                "word_duplication": self._rates(self.mora["word_duplication"], self.kana_freq),
                "mora_extra": mora_extra,
                "mora_substitution": mora_sub,
            },
        }}


def estimate(pairs_path: Path, stats_path: Path | None, out_dir: Path,
             mixed_input: float = 0.0) -> dict:
    est = Estimator()
    n = 0
    with open(pairs_path, encoding="utf-8") as f:
        for line in f:
            est.add(json.loads(line))
            n += 1
    stats = json.load(open(stats_path, encoding="utf-8")) if stats_path and stats_path.exists() else {}
    dropped = stats.get("dropped", {}).get("train", {})
    input_pairs = stats.get("input_pairs", {}).get("train", 0)
    # 差分が複数あって build-jwtd が捨てた組の割合を、2 個目の typo の割合の目安にする
    second = dropped.get("multi_diff", 0) / input_pairs if input_pairs else 0.2
    cfg = est.config(mixed_input, second)
    out_dir.mkdir(parents=True, exist_ok=True)
    with open(out_dir / "jwtd.yaml", "w", encoding="utf-8") as f:
        f.write("# estimate-typo-dist が JWTD train の実誤りから推定した typo 生成器の設定（手で直さない）。\n")
        f.write("# load_config(path) で default.yaml に重ねて使う。出典: 日本語 Wikipedia 入力誤りデータセット v2.0\n")
        yaml.safe_dump(cfg, f, allow_unicode=True, sort_keys=False, width=200)
    report = _report(est, cfg, n, dropped, input_pairs, mixed_input)
    (out_dir / "REPORT.md").write_text(report, encoding="utf-8")
    return {"pairs": n, "used": sum(est.types.values()), "excluded": dict(est.excluded),
            "error_types": cfg["typo"]["error_types"], "out": str(out_dir)}


LABELS = {
    "deletion": "脱字（打鍵の押し損ね）", "missing_double_consonant": "促音の脱落（kitte → kite）",
    "insertion": "余分な打鍵", "repeated_key": "キーの重ね打ち（促音になるものを含む）",
    "excessive_double_consonant": "促音の子音の打ち過ぎ", "substitution": "隣接キーの打ち間違い",
    "key_far": "離れたキーの打ち間違い（を⇄の・が⇄か など）", "transposition": "打鍵の入れ替え",
    "mora_duplication": "仮名の二重打ち（をを・がが）", "word_duplication": "語の二重（からから）",
    "mora_missing": "仮名の脱落（主に助詞）", "mora_extra": "余分な仮名（主に助詞）",
    "mora_substitution": "仮名の置き換え（を→が など）", "mixed_input": "英字の残り（JWTD では観測できない）",
    "weak_finger_omission": "（キー別の重みで表すので 0）",
}


def _report(est: Estimator, cfg: dict, n: int, dropped: dict, input_pairs: int, mixed: float) -> str:
    t = cfg["typo"]
    lines = ["# JWTD から推定した typo の分布", "",
             f"入力: `data/jwtd/pairs_train.jsonl` {n:,} 組（JWTD train のうち読みの組になったもの。ベンチのページは除外済み）。",
             f"推定に使った組: {sum(est.types.values()):,}", "",
             "## 生成器の型の比率", "", "| 型 | 内容 | 件数 | 比率 |", "|---|---|---:|---:|"]
    for k, w in sorted(t["error_types"].items(), key=lambda kv: -kv[1]):
        lines.append(f"| `{k}` | {LABELS.get(k, '')} | {est.types.get(k, 0):,} | {w * 100:.1f}% |")
    lines += ["", (f"`mixed_input` は JWTD で観測できないので事前の値 {mixed * 100:.0f}%（他を按分して縮めた）。" if mixed > 0
                   else "`mixed_input` はローマ字の打ち間違いではないので生成しない（0%）。"),
              f"2 個目の typo の割合: {t['second_error_ratio'] * 100:.1f}%（差分が複数ある組 {dropped.get('multi_diff', 0):,} / train {input_pairs:,}）。",
              f"`repeat_sokuon_bias`: {t['repeat_sokuon_bias']:.3f}、`insertion_far_ratio`: {t['dist']['insertion_far_ratio']:.3f}", "",
              "## 推定から除いたもの", "", "| 理由 | 件数 |", "|---|---:|"]
    for k, v in est.excluded.most_common():
        lines.append(f"| {k} | {v:,} |")
    if input_pairs:
        lines += ["", "`build-jwtd` の段で読みの組にならなかったもの（JWTD train の組に対する割合）:", "",
                  "| 理由 | 件数 | 割合 |", "|---|---:|---:|"]
        for k, v in sorted(dropped.items(), key=lambda kv: -kv[1])[:10]:
            lines.append(f"| {k} | {v:,} | {v / input_pairs * 100:.1f}% |")
    lines += ["", "## キー別の起こりやすさ（上位。出現数で正規化・最大 1）", ""]
    for kind, rates in t["dist"]["key_rates"].items():
        top = sorted(rates.items(), key=lambda kv: -kv[1])[:10]
        lines.append(f"- `{kind}`: " + ", ".join(f"{k} {v:.2f}" for k, v in top))
    lines += ["", "## 混同表（上位）", ""]
    for kind, rows in t["dist"]["confusion"].items():
        pairs = sorted(((c, x, w) for c, row in rows.items() for x, w in row.items()), key=lambda z: -z[2])[:12]
        lines.append(f"- `{kind}`: " + ", ".join(f"{c}→{x} {w}" for c, x, w in pairs))
    lines += ["", "## 仮名の表（上位）", ""]
    for kind in ("mora_missing", "mora_duplication", "word_duplication"):
        top = sorted(est.mora[kind].items(), key=lambda kv: -kv[1])[:12]
        lines.append(f"- `{kind}`（件数）: " + ", ".join(f"{k} {v}" for k, v in top))
    ex = sorted(((p, a, c) for p, row in est.mora_extra.items() for a, c in row.items()), key=lambda z: -z[2])[:12]
    lines.append("- `mora_extra`（直前 + 入る仮名、件数）: " + ", ".join(f"{p}+{a} {c}" for p, a, c in ex))
    sub = sorted(((b, a, c) for b, row in est.mora_sub.items() for a, c in row.items()), key=lambda z: -z[2])[:12]
    lines.append("- `mora_substitution`（件数）: " + ", ".join(f"{b}→{a} {c}" for b, a, c in sub))
    return "\n".join(lines) + "\n"
