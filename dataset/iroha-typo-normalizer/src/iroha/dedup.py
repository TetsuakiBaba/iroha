"""重複除去。

exact      … 文字列そのまま
normalized … 記号・空白・繰り返しを落とした形
near       … 文字 n-gram の MinHash + LSH（任意。既定 OFF）

near は「大規模データで極端に遅くならない」ことを優先して、辞書に署名を貯める
バンド方式にした（総当たりの類似度計算はしない）。メモリは 1 文あたり
bands 個の int なので、100 万文で数百 MB 程度。
"""
from __future__ import annotations

import hashlib
import re
from dataclasses import dataclass, field

from iroha.config import Config

_NORMALIZE_DROP = re.compile(r"[\s、。・！？「」『』（）\(\)\[\]【】〜ー:：;；,\.\-—…]+")


def normalize_key(text: str) -> str:
    """表記の揺れを落とした比較用キー。"""
    return _NORMALIZE_DROP.sub("", text)


def _h64(s: str, salt: int = 0) -> int:
    return int.from_bytes(
        hashlib.blake2b(s.encode("utf-8"), digest_size=8, salt=salt.to_bytes(2, "big") + b"\x00" * 14).digest(),
        "big",
    )


@dataclass
class DedupStats:
    exact: int = 0
    normalized: int = 0
    near: int = 0

    def total(self) -> int:
        return self.exact + self.normalized + self.near

    def as_dict(self) -> dict:
        return {"exact": self.exact, "normalized": self.normalized,
                "near_duplicate": self.near, "total": self.total()}


class Deduplicator:
    """``is_duplicate(text)`` が True を返した文は捨てる。副作用として登録もする。"""

    def __init__(self, cfg: Config):
        self.use_exact = bool(cfg.get("dedup.exact", True))
        self.use_normalized = bool(cfg.get("dedup.normalized", True))
        self.use_near = bool(cfg.get("dedup.near_duplicate", False))
        self.num_perm = int(cfg.get("dedup.near.num_perm", 64))
        self.bands = max(1, int(cfg.get("dedup.near.bands", 16)))
        self.shingle = max(1, int(cfg.get("dedup.near.shingle", 5)))
        self.rows = max(1, self.num_perm // self.bands)
        self._exact: set[int] = set()
        self._normalized: set[int] = set()
        self._buckets: list[set[int]] = [set() for _ in range(self.bands)]
        self.stats = DedupStats()

    # ---- near duplicate ----
    def _signature(self, text: str) -> list[int]:
        key = normalize_key(text)
        if len(key) < self.shingle:
            shingles = [key] if key else []
        else:
            shingles = [key[i:i + self.shingle] for i in range(len(key) - self.shingle + 1)]
        if not shingles:
            return []
        sig = []
        for p in range(self.num_perm):
            sig.append(min(_h64(s, p) for s in shingles))
        return sig

    def _near_duplicate(self, text: str) -> bool:
        sig = self._signature(text)
        if not sig:
            return False
        band_keys = []
        for b in range(self.bands):
            chunk = sig[b * self.rows:(b + 1) * self.rows]
            if not chunk:
                continue
            band_keys.append((b, _h64("|".join(map(str, chunk)), 1000 + b)))
        hit = any(k in self._buckets[b] for b, k in band_keys)
        for b, k in band_keys:
            self._buckets[b].add(k)
        return hit

    # ---- 本体 ----
    def is_duplicate(self, text: str) -> bool:
        if self.use_exact:
            h = _h64(text)
            if h in self._exact:
                self.stats.exact += 1
                return True
            self._exact.add(h)
        if self.use_normalized:
            h = _h64(normalize_key(text))
            if h in self._normalized:
                self.stats.normalized += 1
                return True
            self._normalized.add(h)
        if self.use_near and self._near_duplicate(text):
            self.stats.near += 1
            return True
        return False


class KeyDeduplicator:
    """example レベルの単純な重複除去（(context, input, target) など）。"""

    def __init__(self) -> None:
        self._seen: set[int] = set()
        self.dropped = 0

    def is_duplicate(self, *parts: str) -> bool:
        h = _h64("\x1f".join(parts))
        if h in self._seen:
            self.dropped += 1
            return True
        self._seen.add(h)
        return False
