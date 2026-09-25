"""ソース共通のインターフェース。

ソースを足すときにやること:

1. ``SourceAdapter`` を継承し、``info`` / ``download`` / ``documents`` を書く
2. モジュールの末尾で ``register(MyAdapter)``
3. ``iroha/sources/__init__.py`` の import に足す
4. ``config/default.yaml`` の ``sources:`` に既定値を書く
5. ``LICENSES.md`` にライセンス・出典表記・使用フィールド・注意事項を書く

``Document`` の ``paragraphs`` は「連続した文章のかたまり」のリスト。左文脈は
段落の中だけで繋ぐ（別の段落の文を前文として使わない）。1 文ずつのソース
（Tatoeba）は 1 段落 1 文の Document を返せばよい。
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Iterator

from iroha.config import Config
from iroha.paths import Paths


@dataclass
class SourceInfo:
    """LICENSES.md / REPORT.md に出す由来の情報。"""
    name: str
    title: str
    homepage: str
    license: str
    license_url: str
    attribution: str
    used_fields: list[str] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)


@dataclass
class Document:
    document_id: str
    source: str
    paragraphs: list[str]
    license: str
    url: str | None = None
    attribution: str | None = None
    meta: dict = field(default_factory=dict)


class SourceAdapter:
    name: str = ""

    def __init__(self, cfg: Config, paths: Paths):
        self.cfg = cfg
        self.paths = paths
        self.source_cfg = cfg.sub(f"sources.{self.name}")

    # ---- 実装するもの ----
    @classmethod
    def info(cls) -> SourceInfo:
        raise NotImplementedError

    def download(self, *, force: bool = False) -> dict:
        """生データを paths.raw_for(name) に置く。統計の dict を返す。"""
        raise NotImplementedError

    def documents(self) -> Iterator[Document]:
        raise NotImplementedError

    # ---- 共通 ----
    @property
    def enabled(self) -> bool:
        return bool(self.source_cfg.get("enabled", False))

    @property
    def raw_dir(self) -> Paths:
        return self.paths.raw_for(self.name)

    def second_opinions(self) -> list:
        """このソース固有の読みの突き合わせ（無ければ空）。"""
        return []

    def extraction_stats(self) -> dict | None:
        """``documents()`` の中で数えた抽出の統計（話し言葉のソース）。preprocess の統計の
        ``extraction`` に入る。数えていなければ None。``documents()`` を回し切ったあとに呼ぶ"""
        return None


_REGISTRY: dict[str, type[SourceAdapter]] = {}


def register(cls: type[SourceAdapter]) -> type[SourceAdapter]:
    if not cls.name:
        raise ValueError(f"{cls.__name__}.name が空")
    _REGISTRY[cls.name] = cls
    return cls


def adapter_names() -> list[str]:
    return sorted(_REGISTRY)


def get_adapter(name: str, cfg: Config, paths: Paths) -> SourceAdapter:
    if name not in _REGISTRY:
        raise KeyError(f"未知のソース {name!r}（使えるのは {', '.join(adapter_names())}）")
    return _REGISTRY[name](cfg, paths)


def enabled_adapters(cfg: Config, paths: Paths) -> list[SourceAdapter]:
    out = []
    for name in adapter_names():
        adapter = _REGISTRY[name](cfg, paths)
        if adapter.enabled:
            out.append(adapter)
    return out


def all_source_info() -> list[SourceInfo]:
    return [_REGISTRY[n].info() for n in adapter_names()]
