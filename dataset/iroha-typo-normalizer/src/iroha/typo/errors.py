"""打鍵レベルの typo。

**かな文字をランダムに消すのではなく**、かな → 打鍵列 → 打鍵レベルの誤り →
かなへ再変換、の順で作る（README の設計方針）。ここにあるのは「打鍵列をどう崩すか」
だけで、かなに戻すのは ``build.py``。

error type は config の ``typo.error_types`` と同じ名前で登録する。足すときは
関数を書いて ``ERROR_TYPES`` に入れ、default.yaml の比率にも書く。
"""
from __future__ import annotations

import random
from dataclasses import dataclass, field

from iroha.typo import keyboard
from iroha.typo.romanize import SOKUON_CONSONANTS, RomanizeError, romanize, to_kana

LETTERS = "abcdefghijklmnopqrstuvwxyz"


@dataclass
class KeyStream:
    """かな 1 文の打鍵列と、その単位ごとの内訳。"""
    keys: str
    units: list[str] = field(default_factory=list)       # かなの打鍵単位（「きょ」など）
    unit_keys: list[str] = field(default_factory=list)   # 各単位の打鍵列


@dataclass
class Typo:
    keys: str                     # 崩したあとの打鍵列
    error_type: str
    detail: str
    position: int
    # 打鍵列からかなに戻すのではなく、直接この文字列を出力する場合に使う
    # （mixed_input のように「かなにならない」崩れを表すため）
    raw_output: str | None = None


@dataclass
class ErrorContext:
    rng: random.Random
    key_weights: dict[str, float]
    mixed_min_units: int = 1
    mixed_max_units: int = 4
    repeat_sokuon_bias: float = 0.6
    # 実誤りから推定した分布（typo.dist。iroha/wild/jwtd_dist.py が書く）。
    # 空なら各 type は従来どおりの一様な選び方をする
    dist: dict = field(default_factory=dict)


def _dist_table(ctx: ErrorContext, *path: str) -> dict:
    d = ctx.dist
    for p in path:
        d = d.get(p) if isinstance(d, dict) else None
        if d is None:
            return {}
    return d if isinstance(d, dict) else {}


def _choose(rng: random.Random, table: dict) -> str | None:
    """{候補: 重み} から 1 つ引く"""
    items = [(k, float(w)) for k, w in table.items() if float(w) > 0]
    if not items:
        return None
    r = rng.random() * sum(w for _, w in items)
    acc = 0.0
    for k, w in items:
        acc += w
        if r <= acc:
            return k
    return items[-1][0]


def weighted_index(keys: str, ctx: ErrorContext,
                   candidates: list[int] | None = None, kind: str | None = None) -> int | None:
    """打鍵位置を選ぶ。``typo.key_weights`` でキー別の起こりやすさを与えられる。

    ``kind`` を渡し、``typo.dist.key_rates.<kind>`` があればそちらを使う
    （実誤りから推定した「そのキーでその誤りが起きる率」。出現頻度で正規化済み）。
    """
    idxs = candidates if candidates is not None else list(range(len(keys)))
    if not idxs:
        return None
    rates = _dist_table(ctx, "key_rates", kind) if kind else {}
    if rates:
        weights = [max(0.0, float(rates.get(keys[i], 0.0))) for i in idxs]
    else:
        weights = [max(0.0, ctx.key_weights.get(keys[i], 1.0)) for i in idxs]
    total = sum(weights)
    if total <= 0:
        return ctx.rng.choice(idxs)
    r = ctx.rng.random() * total
    acc = 0.0
    for i, w in zip(idxs, weights):
        acc += w
        if r <= acc:
            return i
    return idxs[-1]


def _starts_syllable(keys: str, i: int) -> bool:
    """i の子音が音節の頭か。

    shi の h や tsu の s のような音節内の子音は促音にならない（人は shhi とは打たない）。
    「ん」（n）の後ろは音節の頭とみなす。
    """
    if i == 0:
        return True
    prev = keys[i - 1]
    return prev == "n" or prev not in SOKUON_CONSONANTS


def _double_consonant_positions(keys: str) -> list[int]:
    """すでに子音が反復している位置（促音を作っている位置）。"""
    out = [i for i in range(len(keys) - 1)
           if keys[i] == keys[i + 1] and keys[i] in SOKUON_CONSONANTS]
    # matcha 綴りの tc も促音
    out += [i for i in range(len(keys) - 1) if keys[i] == "t" and keys[i + 1] == "c"]
    return sorted(set(out))


# ---------------------------------------------------------------- error types
def deletion(stream: KeyStream, ctx: ErrorContext) -> Typo | None:
    """キーの押し損ね。位置は key_weights で重み付けできる。"""
    keys = stream.keys
    if len(keys) <= 1:
        return None
    i = weighted_index(keys, ctx, kind="deletion")
    if i is None:
        return None
    return Typo(keys[:i] + keys[i + 1:], "deletion", f"omit {keys[i]!r}@{i}", i)


def weak_finger_omission(stream: KeyStream, ctx: ErrorContext) -> Typo | None:
    """小指・薬指の担当キーが押し切れずに落ちる。"""
    keys = stream.keys
    if len(keys) <= 1:
        return None
    cands = [i for i, c in enumerate(keys) if keyboard.is_weak_finger(c)]
    if not cands:
        return None
    i = weighted_index(keys, ctx, cands)
    if i is None:
        return None
    return Typo(keys[:i] + keys[i + 1:], "weak_finger_omission",
                f"omit {keys[i]!r}@{i} ({keyboard.FINGERS.get(keys[i], '?')})", i)


def insertion(stream: KeyStream, ctx: ErrorContext) -> Typo | None:
    """余分なキー入力。隣接キーが割り込んだ形にする（無関係な文字は入れない）。"""
    keys = stream.keys
    if not keys:
        return None
    i = weighted_index(keys, ctx, kind="insertion")
    if i is None:
        return None
    near = keyboard.neighbors(keys[i])
    far_ratio = float(ctx.dist.get("insertion_far_ratio", 0.0) or 0.0)
    # far_ratio が 0 のときは乱数を引かない（既定の設定で従来と同じ列になるように）
    if near and (far_ratio <= 0 or ctx.rng.random() >= far_ratio):
        extra = ctx.rng.choice(near)
    else:
        # 離れたキー（実誤りでは余分な打鍵の一部が隣接キーではない）
        far = [c for c in LETTERS if c not in near and c != keys[i]]
        extra = ctx.rng.choice(far)
    return Typo(keys[:i] + extra + keys[i:], "insertion", f"insert {extra!r}@{i}", i)


def substitution(stream: KeyStream, ctx: ErrorContext) -> Typo | None:
    """QWERTY 上で隣接したキーへの誤入力。"""
    keys = stream.keys
    cands = [i for i, c in enumerate(keys) if keyboard.neighbors(c)]
    if not cands:
        return None
    i = weighted_index(keys, ctx, cands, kind="substitution")
    if i is None:
        return None
    near = keyboard.neighbors(keys[i])
    conf = {k: w for k, w in _dist_table(ctx, "confusion", "substitution", keys[i]).items() if k in near}
    sub = _choose(ctx.rng, conf) or ctx.rng.choice(near)
    return Typo(keys[:i] + sub + keys[i + 1:], "substitution",
                f"{keys[i]!r}->{sub!r}@{i}", i)


def key_far(stream: KeyStream, ctx: ErrorContext) -> Typo | None:
    """離れたキーへの 1 打鍵の誤り。

    実誤りでは w↔n（を ⇄ の）・g↔k（が ⇄ か）・d↔t・b↔p のように、助詞や濁点の取り違えが
    1 打鍵差として現れる。``typo.dist.confusion.key_far`` があればその混同表で選ぶ。
    """
    keys = stream.keys
    cands = [i for i, c in enumerate(keys) if c in LETTERS]
    if not cands:
        return None
    i = weighted_index(keys, ctx, cands, kind="key_far")
    if i is None:
        return None
    near = keyboard.neighbors(keys[i])
    conf = {k: w for k, w in _dist_table(ctx, "confusion", "key_far", keys[i]).items()
            if k not in near and k != keys[i]}
    sub = _choose(ctx.rng, conf)
    if sub is None:
        far = [c for c in LETTERS if c not in near and c != keys[i]]
        sub = ctx.rng.choice(far)
    return Typo(keys[:i] + sub + keys[i + 1:], "key_far", f"{keys[i]!r}->{sub!r}@{i}", i)


def transposition(stream: KeyStream, ctx: ErrorContext) -> Typo | None:
    """隣接する打鍵の順序逆転。"""
    keys = stream.keys
    cands = [i for i in range(len(keys) - 1) if keys[i] != keys[i + 1]]
    if not cands:
        return None
    i = weighted_index(keys, ctx, cands)
    if i is None:
        return None
    swapped = keys[:i] + keys[i + 1] + keys[i] + keys[i + 2:]
    return Typo(swapped, "transposition", f"swap {keys[i]!r}{keys[i+1]!r}@{i}", i)


def repeated_key(stream: KeyStream, ctx: ErrorContext) -> Typo | None:
    """キーを余分に複数回入力。

    促音の入力（tt / kk / pp）でよく起きるので、既定では音節頭の子音を
    優先して選ぶ（``repeat_sokuon_bias``）。母音や「ん」でも起きる。
    """
    keys = stream.keys
    if not keys:
        return None
    sokuon_cands = [i for i, c in enumerate(keys)
                    if c in SOKUON_CONSONANTS and c != "n" and _starts_syllable(keys, i)]
    use_sokuon = bool(sokuon_cands) and ctx.rng.random() < ctx.repeat_sokuon_bias
    i = weighted_index(keys, ctx, sokuon_cands if use_sokuon else None)
    if i is None:
        return None
    times = 1 if ctx.rng.random() < 0.8 else 2
    inserted = keys[i] * times
    return Typo(keys[:i + 1] + inserted + keys[i + 1:], "repeated_key",
                f"repeat {keys[i]!r}x{times}@{i}", i)


def missing_double_consonant(stream: KeyStream, ctx: ErrorContext) -> Typo | None:
    """促音に必要な二重子音の一方が欠ける（kitte → kite）。"""
    keys = stream.keys
    cands = _double_consonant_positions(keys)
    if not cands:
        return None
    i = weighted_index(keys, ctx, cands)
    if i is None:
        return None
    return Typo(keys[:i] + keys[i + 1:], "missing_double_consonant",
                f"unrepeat {keys[i]!r}@{i}", i)


def excessive_double_consonant(stream: KeyStream, ctx: ErrorContext) -> Typo | None:
    """促音の子音を余分に打つ（kitte → kittte）。"""
    keys = stream.keys
    cands = _double_consonant_positions(keys)
    if not cands:
        return None
    i = weighted_index(keys, ctx, cands)
    if i is None:
        return None
    return Typo(keys[:i] + keys[i] + keys[i:], "excessive_double_consonant",
                f"overrepeat {keys[i]!r}@{i}", i)


def mixed_input(stream: KeyStream, ctx: ErrorContext) -> Typo | None:
    """IME の切替・入力状態の問題を模して、一部を打鍵列のまま残す。

    「きょうはdaigaku」のように、かなの途中から先がローマ字のまま出る形。
    打鍵列レベルの編集ではないので raw_output を使う。
    """
    units, unit_keys = stream.units, stream.unit_keys
    if len(units) < 2 or len(units) != len(unit_keys):
        return None
    span = ctx.rng.randint(ctx.mixed_min_units, max(ctx.mixed_min_units, ctx.mixed_max_units))
    span = min(span, len(units) - 1)
    if span <= 0:
        return None
    # 末尾寄りのほうが起きやすい（切替に気づかないまま打ち続ける）ので、
    # 半分の確率で末尾から、残りは任意の位置から
    if ctx.rng.random() < 0.5:
        start = len(units) - span
    else:
        start = ctx.rng.randint(1, len(units) - span)
    head = "".join(units[:start])
    middle = "".join(unit_keys[start:start + span])
    tail = "".join(units[start + span:])
    output = head + middle + tail
    return Typo(stream.keys, "mixed_input",
                f"raw keys for units [{start}:{start + span}] = {middle!r}",
                start, raw_output=output)


# ---------------------------------------------------------------- 仮名単位の誤り
#
# 実誤り（JWTD）の推敲系・二重打ちは打鍵 1 つの差ではなく仮名の出し入れとして現れる
# （郡属する ← 郡に属する、ををを、からから）。打鍵列をいったんかなに戻して仮名を編集し、
# 打鍵列を作り直す（2 個目の typo でも 1 個目の結果の上に載る）。
# ローマ字が残っている（かなに戻らない）打鍵列には当てない。
# 何をどこで編集するかは typo.dist の表で決める（jwtd_dist.py が推定する）。表が無ければ一様。

def _current_kana(stream: KeyStream) -> str | None:
    kana = to_kana(stream.keys)
    return kana if kana and all("ぁ" <= c <= "ゖ" or c in "ー、。・「」〜！？" for c in kana) else None


def _rekey(kana: str) -> str | None:
    """かな → 打鍵列。打鍵列から同じかなに戻らなければ None（「ん」+ 母音など）"""
    try:
        keys = romanize(kana, "nn")
    except RomanizeError:
        return None
    return keys if to_kana(keys) == kana else None


def _occurrences(kana: str, table: dict, *, allow_final: bool = True) -> list[tuple[int, str, float]]:
    """kana の中で table の文字列が現れる (位置, 文字列, 重み)"""
    out = []
    for b, w in table.items():
        w = float(w)
        if w <= 0 or not b:
            continue
        start = kana.find(b)
        while start >= 0:
            if allow_final or start + len(b) < len(kana):
                out.append((start, b, w))
            start = kana.find(b, start + 1)
    return out


def _pick(rng: random.Random, items: list[tuple]) -> tuple | None:
    if not items:
        return None
    r = rng.random() * sum(x[-1] for x in items)
    acc = 0.0
    for x in items:
        acc += x[-1]
        if r <= acc:
            return x
    return items[-1]


def _kana_typo(stream: KeyStream, new_kana: str, kind: str, detail: str, pos: int) -> Typo | None:
    keys = _rekey(new_kana)
    if keys is None:
        return None
    return Typo(keys, kind, detail, pos)


def _uniform_units(kana: str, lengths: tuple[int, ...]) -> dict:
    return {kana[i:i + n]: 1.0 for n in lengths for i in range(len(kana) - n + 1)}


def mora_missing(stream: KeyStream, ctx: ErrorContext) -> Typo | None:
    """仮名（主に助詞）が抜ける（ぐんにぞくする → ぐんぞくする）。

    読みの最後の仮名は落とさない。入力中は末尾が足りないのが普通で、本体も末尾に足すだけの
    訂正は捨てる（TypoNormalizer の isTrailingInsertionOnly）。
    """
    kana = _current_kana(stream)
    if kana is None or len(kana) < 2:
        return None
    table = _dist_table(ctx, "mora_missing") or _uniform_units(kana, (1,))
    occ = _occurrences(kana, table, allow_final=False)
    # 直前の仮名で条件づけた率があればそちらを使う（無い組み合わせは条件なしの率のまま）
    ctx_table = _dist_table(ctx, "mora_missing_ctx")
    if ctx_table:
        occ = [(j, b, float(ctx_table.get(kana[j - 1] if j else "^", {}).get(b, w))) for j, b, w in occ]
    hit = _pick(ctx.rng, occ)
    if hit is None:
        return None
    j, b, _ = hit
    return _kana_typo(stream, kana[:j] + kana[j + len(b):], "mora_missing", f"drop {b!r}@{j}", j)


def mora_extra(stream: KeyStream, ctx: ErrorContext) -> Typo | None:
    """余分な仮名が入る（主に助詞。直前の仮名で入りやすさが変わる）。"""
    kana = _current_kana(stream)
    if kana is None:
        return None
    table = _dist_table(ctx, "mora_extra")      # {直前の仮名（先頭は "^"）: {入る仮名: 重み}}
    items = []
    for i in range(len(kana) + 1):
        prev = kana[i - 1] if i else "^"
        row = table.get(prev) if table else {c: 1.0 for c in "のにをがはでと"}
        if row:
            items.append((i, row, sum(float(w) for w in row.values())))
    hit = _pick(ctx.rng, items)
    if hit is None:
        return None
    i, row, _ = hit
    a = _choose(ctx.rng, row)
    if a is None:
        return None
    return _kana_typo(stream, kana[:i] + a + kana[i:], "mora_extra", f"insert {a!r}@{i}", i)


def mora_substitution(stream: KeyStream, ctx: ErrorContext) -> Typo | None:
    """仮名の置き換え（を → が、が → の など助詞の取り違えが多い）。"""
    kana = _current_kana(stream)
    if kana is None:
        return None
    table = _dist_table(ctx, "mora_substitution")   # {意図した仮名: {打った仮名: 重み}}
    if not table:
        table = {c: {d: 1.0 for d in "のにをがはでと" if d != c} for c in set(kana)}
    occ = _occurrences(kana, {b: sum(float(w) for w in row.values()) for b, row in table.items()})
    hit = _pick(ctx.rng, occ)
    if hit is None:
        return None
    j, b, _ = hit
    a = _choose(ctx.rng, table[b])
    if a is None or a == b:
        return None
    return _kana_typo(stream, kana[:j] + a + kana[j + len(b):], "mora_substitution",
                      f"{b!r}->{a!r}@{j}", j)


def mora_duplication(stream: KeyStream, ctx: ErrorContext) -> Typo | None:
    """仮名 1 つの二重打ち（をを・がが）。"""
    kana = _current_kana(stream)
    if kana is None:
        return None
    table = _dist_table(ctx, "mora_duplication") or _uniform_units(kana, (1,))
    hit = _pick(ctx.rng, _occurrences(kana, {k: w for k, w in table.items() if len(k) == 1}))
    if hit is None:
        return None
    j, b, _ = hit
    return _kana_typo(stream, kana[:j + 1] + b + kana[j + 1:], "mora_duplication", f"dup {b!r}@{j}", j)


def word_duplication(stream: KeyStream, ctx: ErrorContext) -> Typo | None:
    """2〜3 仮名の二重（するする・からから）。編集の残骸とされるが、実誤りとしては多い。"""
    kana = _current_kana(stream)
    if kana is None or len(kana) < 2:
        return None
    table = _dist_table(ctx, "word_duplication") or _uniform_units(kana, (2, 3))
    hit = _pick(ctx.rng, _occurrences(kana, {k: w for k, w in table.items() if 2 <= len(k) <= 3}))
    if hit is None:
        return None
    j, b, _ = hit
    return _kana_typo(stream, kana[:j + len(b)] + b + kana[j + len(b):], "word_duplication",
                      f"dup {b!r}@{j}", j)


def _has_weak_finger(stream: KeyStream) -> bool:
    return any(keyboard.is_weak_finger(c) for c in stream.keys)


def _has_double_consonant(stream: KeyStream) -> bool:
    return bool(_double_consonant_positions(stream.keys))


def _has_transposable(stream: KeyStream) -> bool:
    return any(stream.keys[i] != stream.keys[i + 1] for i in range(len(stream.keys) - 1))


# その打鍵列にその error type を当てられるか。
# 当てられない type を引いてから引き直すと、その type の重みが他に流れて
# config の比率が崩れる（促音系は「っ」を含む読みにしか当てられない）。
# 生成のたびに当てられる type だけへ重みを配り直すために使う。
APPLICABLE = {
    "deletion": lambda s: len(s.keys) > 1,
    "insertion": lambda s: bool(s.keys),
    "substitution": lambda s: any(keyboard.neighbors(c) for c in s.keys),
    "transposition": _has_transposable,
    "repeated_key": lambda s: bool(s.keys),
    "missing_double_consonant": _has_double_consonant,
    "excessive_double_consonant": _has_double_consonant,
    "mixed_input": lambda s: len(s.units) >= 2 and len(s.units) == len(s.unit_keys),
    "weak_finger_omission": _has_weak_finger,
    "key_far": lambda s: any(c in LETTERS for c in s.keys),
    "mora_missing": lambda s: len(s.keys) > 2,
    "mora_extra": lambda s: bool(s.keys),
    "mora_substitution": lambda s: bool(s.keys),
    "mora_duplication": lambda s: bool(s.keys),
    "word_duplication": lambda s: len(s.keys) > 2,
}


ERROR_TYPES = {
    "deletion": deletion,
    "insertion": insertion,
    "substitution": substitution,
    "transposition": transposition,
    "repeated_key": repeated_key,
    "missing_double_consonant": missing_double_consonant,
    "excessive_double_consonant": excessive_double_consonant,
    "mixed_input": mixed_input,
    "weak_finger_omission": weak_finger_omission,
    "key_far": key_far,
    "mora_missing": mora_missing,
    "mora_extra": mora_extra,
    "mora_substitution": mora_substitution,
    "mora_duplication": mora_duplication,
    "word_duplication": word_duplication,
}
