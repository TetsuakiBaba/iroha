"""設定の読み込み。config/default.yaml を土台に、ユーザ指定の YAML を深くマージする。"""
from __future__ import annotations

import copy
import os
from pathlib import Path
from typing import Any

import yaml

PACKAGE_ROOT = Path(__file__).resolve().parent
PROJECT_ROOT = PACKAGE_ROOT.parent
DEFAULT_CONFIG_PATH = PROJECT_ROOT / "config" / "default.yaml"


def _deep_merge(base: dict, over: dict) -> dict:
    out = copy.deepcopy(base)
    for k, v in over.items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = _deep_merge(out[k], v)
        else:
            out[k] = copy.deepcopy(v)
    return out


def _coerce(text: str) -> Any:
    """CLI の --set で来た文字列を YAML として解釈する（123 / true / [a,b] など）。"""
    try:
        return yaml.safe_load(text)
    except yaml.YAMLError:
        return text


class Config:
    """ドット記法で引ける設定。``cfg["typo.clean_ratio"]`` / ``cfg.get("a.b", default)``"""

    def __init__(self, data: dict):
        self._data = data

    @property
    def data(self) -> dict:
        return self._data

    def get(self, path: str, default: Any = None) -> Any:
        cur: Any = self._data
        for part in path.split("."):
            if not isinstance(cur, dict) or part not in cur:
                return default
            cur = cur[part]
        return cur

    def __getitem__(self, path: str) -> Any:
        sentinel = object()
        value = self.get(path, sentinel)
        if value is sentinel:
            raise KeyError(path)
        return value

    def __contains__(self, path: str) -> bool:
        sentinel = object()
        return self.get(path, sentinel) is not sentinel

    def set(self, path: str, value: Any) -> None:
        parts = path.split(".")
        cur = self._data
        for part in parts[:-1]:
            cur = cur.setdefault(part, {})
            if not isinstance(cur, dict):
                raise TypeError(f"{path}: 途中の {part} が辞書ではない")
        cur[parts[-1]] = value

    def sub(self, path: str) -> "Config":
        return Config(self.get(path, {}) or {})

    def __repr__(self) -> str:  # pragma: no cover
        return f"Config({self._data!r})"


def load_config(path: str | os.PathLike | None = None,
                overrides: list[str] | None = None) -> Config:
    """default.yaml → path の YAML → ``key=value`` の overrides の順に重ねる。"""
    if not DEFAULT_CONFIG_PATH.exists():
        raise FileNotFoundError(
            f"{DEFAULT_CONFIG_PATH} が無い。iroha-dataset は自分のディレクトリから実行する"
            "（`pip install -e .` で入れて、リポジトリの iroha-dataset/ で動かす）")
    with open(DEFAULT_CONFIG_PATH, encoding="utf-8") as f:
        data = yaml.safe_load(f) or {}
    if path:
        with open(path, encoding="utf-8") as f:
            data = _deep_merge(data, yaml.safe_load(f) or {})
    cfg = Config(data)
    for item in overrides or []:
        if "=" not in item:
            raise ValueError(f"--set は key=value の形で指定する: {item!r}")
        key, raw = item.split("=", 1)
        cfg.set(key.strip(), _coerce(raw.strip()))
    return cfg
