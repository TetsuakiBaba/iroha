"""JSONL の読み書き。書き込みは tmp → rename で途中失敗を残さない。"""
from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any, Iterable, Iterator


def read_jsonl(path: str | os.PathLike) -> Iterator[dict]:
    with open(path, encoding="utf-8") as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                yield json.loads(line)
            except json.JSONDecodeError as e:
                raise ValueError(f"{path}:{lineno} が JSON として読めない: {e}") from e


def count_lines(path: str | os.PathLike) -> int:
    n = 0
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.strip():
                n += 1
    return n


def dump_line(record: dict) -> str:
    return json.dumps(record, ensure_ascii=False, separators=(",", ":"))


class JsonlWriter:
    """1 ファイル分のライタ。with を抜けたときに rename する。"""

    def __init__(self, path: str | os.PathLike):
        self.path = Path(path)
        self._tmp = self.path.with_suffix(self.path.suffix + ".tmp")
        self._f = None
        self.count = 0

    def __enter__(self) -> "JsonlWriter":
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._f = open(self._tmp, "w", encoding="utf-8")
        return self

    def write(self, record: dict) -> None:
        assert self._f is not None, "with の外で write した"
        self._f.write(dump_line(record))
        self._f.write("\n")
        self.count += 1

    def __exit__(self, exc_type, exc, tb) -> None:
        assert self._f is not None
        self._f.close()
        if exc_type is None:
            os.replace(self._tmp, self.path)
        else:
            self._tmp.unlink(missing_ok=True)


class SplitWriter:
    """train / validation / test をまとめて開くライタ。"""

    def __init__(self, directory: str | os.PathLike, splits: Iterable[str]):
        self.directory = Path(directory)
        self._writers = {s: JsonlWriter(self.directory / f"{s}.jsonl") for s in splits}

    def __enter__(self) -> "SplitWriter":
        for w in self._writers.values():
            w.__enter__()
        return self

    def write(self, split: str, record: dict) -> None:
        self._writers[split].write(record)

    @property
    def counts(self) -> dict[str, int]:
        return {s: w.count for s, w in self._writers.items()}

    def __exit__(self, exc_type, exc, tb) -> None:
        for w in self._writers.values():
            w.__exit__(exc_type, exc, tb)


def write_json(path: str | os.PathLike, obj: Any) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(obj, f, ensure_ascii=False, indent=2, sort_keys=False)
        f.write("\n")
    os.replace(tmp, path)


def read_json(path: str | os.PathLike, default: Any = None) -> Any:
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except FileNotFoundError:
        return default
