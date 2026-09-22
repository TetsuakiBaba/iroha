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

from iroha_dataset.typo import keyboard
from iroha_dataset.typo.romanize import SOKUON_CONSONANTS

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


def weighted_index(keys: str, ctx: ErrorContext,
                   candidates: list[int] | None = None) -> int | None:
    """打鍵位置を選ぶ。``typo.key_weights`` でキー別の起こりやすさを与えられる。"""
    idxs = candidates if candidates is not None else list(range(len(keys)))
    if not idxs:
        return None
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
    i = weighted_index(keys, ctx)
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
    i = weighted_index(keys, ctx)
    if i is None:
        return None
    near = keyboard.neighbors(keys[i])
    extra = ctx.rng.choice(near) if near else ctx.rng.choice(LETTERS)
    return Typo(keys[:i] + extra + keys[i:], "insertion", f"insert {extra!r}@{i}", i)


def substitution(stream: KeyStream, ctx: ErrorContext) -> Typo | None:
    """QWERTY 上で隣接したキーへの誤入力。"""
    keys = stream.keys
    cands = [i for i, c in enumerate(keys) if keyboard.neighbors(c)]
    if not cands:
        return None
    i = weighted_index(keys, ctx, cands)
    if i is None:
        return None
    sub = ctx.rng.choice(keyboard.neighbors(keys[i]))
    return Typo(keys[:i] + sub + keys[i + 1:], "substitution",
                f"{keys[i]!r}->{sub!r}@{i}", i)


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
}
