"""QWERTY の物理配列。

隣接キーは「Unicode 上で近い文字」ではなく**配列の座標**から作る。段ごとの横ずれは
標準的なスタッガード配列の値（数字段 0・上段 0・中段 0.25・下段 0.75 キー分）。
ANSI / JIS で位置が共通の英字と記号だけを対象にする（@ や : は JIS と ANSI で
位置が違うので入れない）。
"""
from __future__ import annotations

# (キー列, 段の横ずれ)
ROWS: tuple[tuple[str, float], ...] = (
    ("1234567890-", 0.0),
    ("qwertyuiop", 0.0),
    ("asdfghjkl;", 0.25),
    ("zxcvbnm,./", 0.75),
)

POSITION: dict[str, tuple[float, float]] = {}
for _row, (_keys, _offset) in enumerate(ROWS):
    for _i, _key in enumerate(_keys):
        POSITION[_key] = (float(_row), _i + _offset)

# 段をまたぐときに「隣」とみなす横方向の距離（キー幅）
_CROSS_ROW_DX = 0.65

# 指の担当（weak_finger_omission と既定の key_weights の根拠。
# 小指 = 薬指より押し損ねやすい、という設計上の仮定）
FINGERS: dict[str, str] = {}
for _keys, _finger in (
    ("1qaz", "left_pinky"), ("2wsx", "left_ring"), ("3edc", "left_middle"),
    ("45rfvtgb", "left_index"), ("67yhnujm", "right_index"),
    ("8ik,", "right_middle"), ("9ol.", "right_ring"), ("0p;/-", "right_pinky"),
):
    for _k in _keys:
        FINGERS[_k] = _finger

WEAK_FINGERS = ("left_pinky", "right_pinky", "left_ring", "right_ring")


def _build_adjacency(cross_row_dx: float = _CROSS_ROW_DX) -> dict[str, tuple[str, ...]]:
    graph: dict[str, tuple[str, ...]] = {}
    for key, (row, x) in POSITION.items():
        near = []
        for other, (row2, x2) in POSITION.items():
            if other == key:
                continue
            drow = abs(row2 - row)
            dx = abs(x2 - x)
            if drow == 0 and dx <= 1.01:
                near.append(other)
            elif drow == 1 and dx <= cross_row_dx:
                near.append(other)
        graph[key] = tuple(sorted(near))
    return graph


ADJACENT: dict[str, tuple[str, ...]] = _build_adjacency()


def neighbors(key: str) -> tuple[str, ...]:
    return ADJACENT.get(key, ())


def is_weak_finger(key: str) -> bool:
    return FINGERS.get(key, "") in WEAK_FINGERS


def weak_finger_keys() -> tuple[str, ...]:
    return tuple(sorted(k for k in POSITION if is_weak_finger(k)))
