"""データソース。新しいソースは SourceAdapter を実装して register する。"""
from iroha_dataset.sources.base import (
    Document, SourceAdapter, SourceInfo, register, get_adapter, adapter_names,
    enabled_adapters, all_source_info,
)
from iroha_dataset.sources import tatoeba, kaken, aozora  # noqa: F401  （register のため）

__all__ = ["Document", "SourceAdapter", "SourceInfo", "register", "get_adapter",
           "adapter_names", "enabled_adapters", "all_source_info"]
