"""train / validation / test の割り当て。

**document_id 単位**で決める。同じ原文・同じ document から派生した example が
split をまたがないことがこの関数の唯一の役目なので、乱数ではなくハッシュで決める
（データを足しても既存 document の行き先が変わらない）。
"""
from __future__ import annotations

import hashlib

from iroha_dataset.config import Config
from iroha_dataset.paths import SPLITS


def _unit_hash(seed: int, document_id: str) -> float:
    """seed と document_id から [0, 1) の一意な値を作る。"""
    h = hashlib.blake2b(f"{seed}:{document_id}".encode("utf-8"), digest_size=8).digest()
    return int.from_bytes(h, "big") / 2 ** 64


class Splitter:
    def __init__(self, cfg: Config):
        self.seed = int(cfg.get("seed", 42))
        ratios = {s: float(cfg.get(f"split.{s}", 0.0)) for s in SPLITS}
        total = sum(ratios.values())
        if total <= 0:
            raise ValueError("split の比率がすべて 0")
        # 合計が 1 でなくても比率として解釈する
        self.ratios = {s: v / total for s, v in ratios.items()}
        # 累積境界（train → validation → test の順に並べる）
        self._bounds: list[tuple[float, str]] = []
        acc = 0.0
        for s in SPLITS:
            acc += self.ratios[s]
            self._bounds.append((acc, s))

    def split_for(self, document_id: str) -> str:
        x = _unit_hash(self.seed, document_id)
        for bound, name in self._bounds:
            if x < bound:
                return name
        return self._bounds[-1][1]
