"""JWTD（日本語 Wikipedia 入力誤りデータセット v2.0）→ typo normalizer の学習データとベンチマーク。

JWTD は Wikipedia の編集履歴から取った「直す前の文 / 直した後の文」の組で、**表層
（漢字仮名交じり）**の誤りである。typo normalizer は「読み → 読み」なので、ここで読みに直す。

1. 差分が 1 か所で、その差分が**両側とも仮名だけ**の組に絞る。漢字の同音誤り
   （固体 → 個体）は読みにすると同じになるので対象外
2. 直した後の文を文節に切り、差分を含む文節とその前後だけを切り出す（窓）。
   JWTD の文は中央値 54 字で、モデルが受け取れる長さ（48 字）を超えるため
3. 両側の窓を Sudachi で読みにし、**読みの差が表層の仮名差分と完全に一致する**ものだけを採る。
   誤入力側の文は形態素解析が崩れやすい（誤りのせいで隣の漢字の読みが変わる等）ので、
   Sudachi の読みをそのまま信じない
4. 読みの差をローマ字の打鍵列で比べて層に分ける
   - keystroke（打鍵系）: 1 打鍵の脱落・余分・隣接キー・転置、または仮名 1 つの二重打ち（をを）
   - editing（推敲系）: それ以外。多くは助詞 1 つの丸ごとの出し入れ（郡属する → 郡に属する）で、
     読みだけからは直せない・直すべきでないもの。離れたキーの置換・語の二重（からから）もここ

学習データ（train.jsonl）は **keystroke だけ**を使う。editing を学習させると、正しい読みに
助詞を足す過剰訂正を覚える。ベンチマーク（bench/）は両方の層を、同じ窓の clean（直した後の読み）
と対にして出す。JWTD の test と gold から作り、それらに出るページは train から除く。
"""
from __future__ import annotations

import json
import multiprocessing as mp
import re
import random
import time
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

from iroha_dataset.config import Config
from iroha_dataset.jsonlio import JsonlWriter, write_json
from iroha_dataset.paths import Paths
from iroha_dataset.preprocess import normalize
from iroha_dataset.preprocess.sentence import TERMINATORS
from iroha_dataset.typo.keyboard import neighbors
from iroha_dataset.typo.romanize import RomanizeError, is_romanizable, romanize

# JWTD の分類のうち、読みにすると差が消える（または誤りではない）もの
SKIP_CATEGORIES = {"kanji-conversion_a", "kanji-conversion_b", "not-typo"}

_LATIN_OR_DIGIT = re.compile(r"[A-Za-z0-9]")

KEYSTROKE = "keystroke"
EDITING = "editing"


# ------------------------------------------------------------------ 純粋関数（Sudachi 不要）

def is_kana(text: str) -> bool:
    return all(normalize.is_hiragana(c) or normalize.is_katakana(c) or c == "ー" for c in text)


def surface_diff(pre: str, post: str) -> tuple[int, str, str]:
    """pre = L + a + R, post = L + b + R となる (len(L), a, b)。L を最長に取る。

    二重打ち（「をを締結」→「を締結」）では余分な方が後ろ側に来る。
    """
    n = min(len(pre), len(post))
    p = 0
    while p < n and pre[p] == post[p]:
        p += 1
    s = 0
    while s < n - p and pre[len(pre) - 1 - s] == post[len(post) - 1 - s]:
        s += 1
    return p, pre[p:len(pre) - s], post[p:len(post) - s]


def locate_edit(r_pre: str, r_post: str, a: str, b: str) -> int | None:
    """r_post の k 文字目の b を a に置き換えると r_pre になる k（最初のもの）。無ければ None。

    読みの差が表層の仮名差分と一致しているかの判定に使う。
    """
    if len(r_pre) - len(r_post) != len(a) - len(b):
        return None
    for k in range(len(r_post) - len(b) + 1):
        if r_post[k:k + len(b)] == b and r_post[:k] == r_pre[:k] \
                and r_post[k + len(b):] == r_pre[k + len(a):]:
            return k
    return None


def osa_distance(x: str, y: str) -> int:
    """隣接転置を 1 と数える編集距離（optimal string alignment）。"""
    prev2: list[int] | None = None
    prev = list(range(len(y) + 1))
    for i in range(1, len(x) + 1):
        cur = [i] + [0] * len(y)
        for j in range(1, len(y) + 1):
            cost = 0 if x[i - 1] == y[j - 1] else 1
            cur[j] = min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost)
            if (prev2 is not None and i > 1 and j > 1
                    and x[i - 1] == y[j - 2] and x[i - 2] == y[j - 1]):
                cur[j] = min(cur[j], prev2[j - 2] + 1)
        prev2, prev = prev, cur
    return prev[len(y)]


def _single_key_op(typed: str, intended: str) -> tuple[str, str]:
    """距離 1 の打鍵列の組がどの操作か。(error_type, detail)"""
    if len(typed) + 1 == len(intended):
        i = next((i for i in range(len(typed)) if typed[i] != intended[i]), len(typed))
        return "key_missing", f"missing {intended[i]!r}"
    if len(typed) == len(intended) + 1:
        i = next((i for i in range(len(intended)) if typed[i] != intended[i]), len(intended))
        return "key_extra", f"extra {typed[i]!r}"
    diff = [i for i in range(len(typed)) if typed[i] != intended[i]]
    if len(diff) == 2 and diff[1] == diff[0] + 1:
        return "key_transposition", f"swap {intended[diff[0]:diff[0] + 2]!r}"
    i = diff[0]
    t, c = typed[i], intended[i]
    if t in neighbors(c):
        return "key_adjacent", f"{c!r}->{t!r}"
    return "key_far", f"{c!r}->{t!r}"


def classify(left: str, a: str, b: str, right: str, max_dup: int = 3) -> tuple[str, str, str]:
    """読み left + a + right（打った方）と left + b + right（意図した方）の差を層に分ける。

    戻り値は (tier, error_type, detail)。打鍵列は前後 2 文字を含めて作る
    （っ・ん の打鍵が後ろの文字で決まるため）。
    """
    if not b and a and len(a) <= max_dup and (left.endswith(a) or right.startswith(a)):
        # 仮名 1 つの二重打ち（をを・がが）は打鍵系。語の二重（からから・されたされた）は
        # 編集の残骸で、しかも いろいろ・ここ のような正しい畳語と区別できないので推敲系にする
        if len(a) == 1:
            return KEYSTROKE, "mora_duplication", f"dup {a!r}"
        return EDITING, "word_duplication", f"dup {a!r}"
    lc, rc = left[-2:], right[:2]
    # 「ん」の打ち方（nn / 子音の前は n 1 つ）で距離が変わるので、近い方を採る
    d, typed, intended = min(
        (osa_distance(t, i), t, i)
        for t, i in ((romanize(lc + a + rc, st), romanize(lc + b + rc, st))
                     for st in ("nn", "contextual")))
    if d == 0:
        return "", "same_keys", ""
    if d == 1:
        et, detail = _single_key_op(typed, intended)
        return (EDITING if et == "key_far" else KEYSTROKE), et, detail
    if not b:
        return EDITING, "mora_extra", f"extra {a!r}"
    if not a:
        return EDITING, "mora_missing", f"missing {b!r}"
    return EDITING, "mora_substitution", f"{b!r}->{a!r}"


# ------------------------------------------------------------------ 読み（Sudachi）

class WindowReader:
    """窓の表層 → 読み。reading.py の ReadingAnalyzer を使うが、判定はここで行う。

    - 仮名だけの OOV は許す（表層がそのまま読みになる）。カタカナ語の打ち間違い
      （サイービス）はほぼ必ず OOV になるので、捨てると外来語の typo が消える
    - 漢字を含む OOV は捨てる
    - 低信頼の理由のうち drop_low_reasons に挙げたものは捨てる。
      固有名詞は Wikipedia では大半の窓に出るので既定では捨てない
    """

    def __init__(self, cfg: Config):
        from iroha_dataset.preprocess.chunking import Chunker
        from iroha_dataset.preprocess.reading import ReadingAnalyzer

        self.analyzer = ReadingAnalyzer(cfg)
        self.chunker = Chunker(cfg)
        self.drop_low = set(cfg.get("jwtd.drop_low_reasons",
                                    ["mode_disagreement", "surface_fallback", "long_reading",
                                     "numeral"]))

    def _join(self, morphemes, check_confidence: bool) -> tuple[str | None, str]:
        parts = []
        for m in morphemes:
            pos0 = m.pos[0] if m.pos else ""
            if is_kana(m.surface):
                # 仮名で書かれた語は、打つのも表層のまま（あるいは → Sudachi の読みは あるいわ）
                parts.append(normalize.katakana_to_hiragana(m.surface))
                continue
            if m.is_oov:
                return None, "oov_kanji"
            if pos0 in ("補助記号", "記号", "空白"):
                # 「(」の読みが キゴウ になるなど、記号の読みは打つ文字と一致しない
                if m.surface not in self.analyzer.extra_chars:
                    return None, "symbol"
                parts.append(m.surface)
                continue
            if not m.reading:
                return None, "no_reading"
            if check_confidence:
                if "surface_fallback" in self.drop_low and m.reading == m.surface \
                        and any(normalize.is_kanji(c) for c in m.surface):
                    return None, "low:surface_fallback"
                if "numeral" in self.drop_low and len(m.pos) > 1 and m.pos[1] == "数詞":
                    return None, "low:numeral"
                if "long_reading" in self.drop_low and len(m.reading) / len(m.surface) > 3.0:
                    return None, "low:long_reading"
            parts.append(m.reading)
        reading = "".join(parts)
        if not reading:
            return None, "empty_reading"
        if not self.analyzer.is_valid_reading(reading):
            return None, "non_kana_reading"
        return reading, ""

    def read(self, text: str, check_confidence: bool) -> tuple[str | None, str]:
        """窓の読み。check_confidence なら低信頼の窓を捨て、別の分割単位（mode A）でも
        同じ読みになるかを確かめる（複合語の読みは分割単位で変わりうる）。"""
        try:
            morphemes = self.analyzer.tokenize(text)
        except Exception:
            return None, "tokenize_error"
        reading, why = self._join(morphemes, check_confidence)
        if reading is None or not check_confidence or "mode_disagreement" not in self.drop_low:
            return reading, why
        try:
            other, _ = self._join(self.analyzer.tokenize(text, "A"), False)
        except Exception:
            other = None
        if other is not None and other != reading:
            return None, "low:mode_disagreement"
        return reading, ""

    def chunk_spans(self, text: str) -> list[tuple[int, int]]:
        """文節の表層上の範囲 [start, end) の一覧。"""
        morphemes = self.analyzer.tokenize(text)
        spans = []
        pos = 0
        for ch in self.chunker.chunk(morphemes):
            n = sum(len(m.surface) for m in ch.morphemes)
            spans.append((pos, pos + n))
            pos += n
        return spans


# ------------------------------------------------------------------ 1 組の処理

@dataclass
class Sample:
    noisy: str
    clean: str
    tier: str
    error_type: str
    detail: str
    at_end: bool
    window: int
    surface_pre: str
    surface_post: str


def _strip_tail(text: str, limit: int) -> int:
    """末尾の句点を落とした長さ。limit より短くはしない（差分に食い込まないため）。"""
    n = len(text)
    while n > limit and text[n - 1] in TERMINATORS:
        n -= 1
    return n


def extract(reader: WindowReader, pre: str, post: str, window: int, max_chars: int,
            max_dup: int) -> tuple[Sample | None, str]:
    """1 組から 1 サンプルを作る。作れなければ (None, 捨てた理由)。

    window は差分の文節の前後に足す文節の数。max_chars を超えたら 1 ずつ減らす。
    """
    pre = normalize.normalize_text(pre)
    post = normalize.normalize_text(post)
    if pre == post:
        return None, "same_after_normalize"
    p, a_s, b_s = surface_diff(pre, post)
    if not a_s and not b_s:
        return None, "same_after_normalize"
    if not is_kana(a_s) or not is_kana(b_s):
        return None, "diff_not_kana"
    try:
        spans = reader.chunk_spans(post)
    except Exception:
        return None, "tokenize_error"
    if not spans:
        return None, "no_chunks"
    end_b = p + len(b_s)
    if b_s:
        idx = [i for i, (s, e) in enumerate(spans) if s < end_b and e > p]
    else:
        q = max(p - 1, 0)
        idx = [i for i, (s, e) in enumerate(spans) if s <= q < e] or [len(spans) - 1]
    if not idx:
        return None, "diff_outside_chunks"
    first, last = idx[0], idx[-1]

    reason = "too_long"
    for w in range(window, -1, -1):
        lo = spans[max(first - w, 0)][0]
        hi = spans[min(last + w, len(spans) - 1)][1]
        hi = max(hi, end_b)
        hi = _strip_tail(post[:hi], end_b)
        post_win = post[lo:hi]
        pre_win = pre[lo:hi + len(a_s) - len(b_s)]
        if len(post_win) > max_chars * 2:
            continue
        if _LATIN_OR_DIGIT.search(post_win) or _LATIN_OR_DIGIT.search(pre_win):
            # Sudachi は英字を カタカナ読み（TEAM → チーム）にするが、打つのは英字のまま
            return None, "latin_or_digit"
        r_post, why = reader.read(post_win, check_confidence=True)
        if r_post is None:
            return None, why
        r_pre, why = reader.read(pre_win, check_confidence=False)
        if r_pre is None:
            return None, "pre:" + why
        if max(len(r_pre), len(r_post)) > max_chars:
            continue
        a = normalize.katakana_to_hiragana(a_s)
        b = normalize.katakana_to_hiragana(b_s)
        k = locate_edit(r_pre, r_post, a, b)
        if k is None:
            return None, "reading_mismatch"
        if not (is_romanizable(r_pre) and is_romanizable(r_post)):
            return None, "not_romanizable"
        left, right = r_post[:k], r_post[k + len(b):]
        try:
            tier, et, detail = classify(left, a, b, right, max_dup)
        except RomanizeError:
            return None, "not_romanizable"
        if not tier:
            return None, et
        return Sample(noisy=r_pre, clean=r_post, tier=tier, error_type=et, detail=detail,
                      at_end=(right == ""), window=w, surface_pre=pre_win,
                      surface_post=post_win), ""
    return None, reason


# ------------------------------------------------------------------ 並列処理

_READER: WindowReader | None = None
_OPTS: dict = {}


def _init_worker(cfg_data: dict, opts: dict) -> None:
    global _READER, _OPTS
    cfg = Config(cfg_data)
    _READER = WindowReader(cfg)
    _OPTS = opts


def _pick_window(key: str) -> int:
    weights = _OPTS["train_window_weights"]
    rng = random.Random(f"{_OPTS['seed']}:{key}")
    return rng.choices(range(len(weights)), weights=weights)[0]


def _work(job: tuple[str, str]) -> tuple[str, dict | None, str]:
    """(split, JSON 行) → (split, 行の情報 + サンプル, 捨てた理由)"""
    split, line = job
    d = json.loads(line)
    info = {"page": str(d.get("page", "")), "title": d.get("title", ""),
            "pre_rev": str(d.get("pre_rev", "")), "post_rev": str(d.get("post_rev", ""))}
    diffs = d.get("diffs")
    if diffs is not None:
        if len(diffs) != 1:
            return split, info, "multi_diff"
        category = diffs[0].get("category", "")
        if category in SKIP_CATEGORIES:
            return split, info, "category:" + category
        if not (is_kana(diffs[0].get("pre_str", "")) and is_kana(diffs[0].get("post_str", ""))):
            return split, info, "diff_not_kana"
    else:
        category = "gold"
    info["category"] = category
    key = f"{info['page']}:{info['pre_rev']}:{info['post_rev']}"
    window = _pick_window(key) if split == "train" else _OPTS["bench_window"]
    assert _READER is not None
    sample, why = extract(_READER, d["pre_text"], d["post_text"], window,
                          _OPTS["max_chars"], _OPTS["max_dup"])
    if sample is None:
        return split, info, why
    info["sample"] = sample.__dict__
    return split, info, ""


# ------------------------------------------------------------------ 組み立て

SRC_FILES = (("train", "train.jsonl"), ("test", "test.jsonl"), ("gold", "gold.jsonl"))


class JwtdBuilder:
    def __init__(self, cfg: Config, paths: Paths):
        self.cfg = cfg
        self.paths = paths
        src = Path(str(cfg.get("jwtd.src", "data/raw/jwtd")))
        if not src.is_absolute():
            from iroha_dataset.config import PROJECT_ROOT
            src = (PROJECT_ROOT / src).resolve()
        self.src = src
        self.out = paths.root / "jwtd"
        self.opts = {
            "seed": int(cfg.get("jwtd.seed", 42)),
            "max_chars": int(cfg.get("jwtd.max_chars", 48)),
            "max_dup": int(cfg.get("jwtd.max_duplication_chars", 3)),
            "bench_window": int(cfg.get("jwtd.bench_window", 1)),
            "train_window_weights": list(cfg.get("jwtd.train_window_weights", [0.4, 0.35, 0.25])),
        }
        self.clean_ratio = float(cfg.get("jwtd.clean_ratio", 0.25))
        self.workers = int(cfg.get("jwtd.workers", 0)) or max(mp.cpu_count() - 1, 1)
        self.limit = int(cfg.get("jwtd.limit", 0))

    def _jobs(self, split: str, fname: str):
        path = self.src / fname
        if not path.exists():
            raise SystemExit(f"JWTD が見つからない: {path}（--set jwtd.src=<dir> で指定する）")
        with open(path, encoding="utf-8") as f:
            for i, line in enumerate(f):
                if self.limit and i >= self.limit:
                    break
                if line.strip():
                    yield split, line

    def _process(self) -> dict[str, list[dict]]:
        out: dict[str, list[dict]] = {s: [] for s, _ in SRC_FILES}
        self.drops: dict[str, Counter] = {s: Counter() for s, _ in SRC_FILES}
        self.pages: dict[str, set[str]] = {s: set() for s, _ in SRC_FILES}

        def jobs():
            for split, fname in SRC_FILES:
                yield from self._jobs(split, fname)

        started = time.time()
        n = 0
        cfg_data = self.cfg.data
        with mp.get_context("spawn").Pool(self.workers, initializer=_init_worker,
                                          initargs=(cfg_data, self.opts)) as pool:
            for split, info, why in pool.imap(_work, jobs(), chunksize=64):
                n += 1
                self.pages[split].add(info["page"])
                if why:
                    self.drops[split][why] += 1
                else:
                    out[split].append(info)
                if n % 50_000 == 0:
                    print(f"   {n:,} 組（{time.time() - started:.0f}s）", flush=True)
        return out

    def run(self) -> dict:
        print(f"[jwtd] {self.src} を読む（workers {self.workers}）", flush=True)
        got = self._process()
        self.out.mkdir(parents=True, exist_ok=True)
        stats: dict = {"src": str(self.src), "options": self.opts, "clean_ratio": self.clean_ratio,
                       "input_pairs": {s: len(got[s]) + sum(self.drops[s].values())
                                       for s, _ in SRC_FILES},
                       "kept_pairs": {s: len(got[s]) for s, _ in SRC_FILES},
                       "dropped": {s: dict(self.drops[s].most_common()) for s, _ in SRC_FILES}}

        # ---- ベンチ（test + gold）。gold を先に入れて、同じ組は gold 側に残す
        bench_pages = self.pages["test"] | self.pages["gold"]
        bench_keys: set[tuple[str, str]] = set()
        bench_clean: set[str] = set()
        bench_rows: dict[str, list[dict]] = {KEYSTROKE: [], EDITING: []}
        for split in ("gold", "test"):
            for info in got[split]:
                s = info["sample"]
                key = (s["noisy"], s["clean"])
                if key in bench_keys:
                    continue
                bench_keys.add(key)
                meta = {"tier": s["tier"], "subset": split, "category": info["category"],
                        "at_end": s["at_end"], "page": info["page"], "pre_rev": info["pre_rev"],
                        "post_rev": info["post_rev"], "surface_noisy": s["surface_pre"],
                        "surface_clean": s["surface_post"]}
                rows = bench_rows[s["tier"]]
                rows.append({"clean": s["clean"], "noisy": s["noisy"], "error_type": s["error_type"],
                             "detail": s["detail"], **meta})
                if s["clean"] not in bench_clean:
                    bench_clean.add(s["clean"])
                    rows.append({"clean": s["clean"], "noisy": s["clean"], "error_type": "none",
                                 "detail": "", **meta, "surface_noisy": s["surface_post"]})

        bench_dir = self.out / "bench"
        stats["bench"] = {}
        for tier, rows in bench_rows.items():
            with JsonlWriter(bench_dir / f"{tier}.jsonl") as w:
                for r in rows:
                    w.write(r)
            stats["bench"][tier] = _summ(rows)

        # ---- train（keystroke だけ）。ベンチに出るページは丸ごと除く
        rng = random.Random(self.opts["seed"])
        p_clean = self.clean_ratio / (1 - self.clean_ratio) if self.clean_ratio < 1 else 1.0
        seen: set[tuple[str, str]] = set()
        excluded = Counter()
        train_rows: list[dict] = []
        tier_all = Counter()
        for info in got["train"]:
            s = info["sample"]
            tier_all[(s["tier"], s["error_type"])] += 1
            if info["page"] in bench_pages:
                excluded["bench_page"] += 1
                continue
            if s["tier"] != KEYSTROKE:
                excluded["editing"] += 1
                continue
            key = (s["noisy"], s["clean"])
            if key in bench_keys:
                excluded["bench_pair"] += 1
                continue
            if key in seen:
                excluded["duplicate"] += 1
                continue
            seen.add(key)
            base = {"source": "jwtd", "document_id": f"jwtd_{info['page']}",
                    "unit": "chunk" if s["window"] == 0 else "window", "romaji_style": "nn",
                    "tier": s["tier"], "category": info["category"], "page": info["page"],
                    "pre_rev": info["pre_rev"], "post_rev": info["post_rev"]}
            rid = f"jwtd_{info['page']}_{info['post_rev']}"
            train_rows.append({"id": rid + "v01", **base, "input": s["noisy"], "target": s["clean"],
                               "error_type": s["error_type"], "n_errors": 1,
                               "detail": s["detail"]})
            ck = (s["clean"], s["clean"])
            if rng.random() < p_clean and ck not in seen and s["clean"] not in bench_clean:
                seen.add(ck)
                train_rows.append({"id": rid + "v00", **base, "input": s["clean"],
                                   "target": s["clean"], "error_type": "clean", "n_errors": 0,
                                   "detail": ""})
        rng.shuffle(train_rows)
        # 漏れの確認（ここで落ちるならバグ）
        assert not any(r["page"] in bench_pages for r in train_rows)
        assert not any((r["input"], r["target"]) in bench_keys for r in train_rows if r["n_errors"])
        with JsonlWriter(self.out / "train.jsonl") as w:
            for r in train_rows:
                w.write(r)
        stats["train"] = {
            "examples": len(train_rows),
            "clean": sum(1 for r in train_rows if r["n_errors"] == 0),
            "error_types": dict(Counter(r["error_type"] for r in train_rows).most_common()),
            "excluded": dict(excluded),
            "all_train_pairs_by_tier": {f"{t}/{e}": c for (t, e), c in tier_all.most_common()},
        }
        write_json(self.paths.stage_stats("jwtd"), stats)
        return stats


def _summ(rows: list[dict]) -> dict:
    typo = [r for r in rows if r["error_type"] != "none"]
    return {
        "rows": len(rows),
        "typo": len(typo),
        "clean": len(rows) - len(typo),
        "by_subset": dict(Counter(r["subset"] for r in typo)),
        "error_types": dict(Counter(r["error_type"] for r in typo).most_common()),
        "at_end": sum(1 for r in typo if r["at_end"]),
        "avg_chars": round(sum(len(r["clean"]) for r in typo) / max(len(typo), 1), 1),
    }
