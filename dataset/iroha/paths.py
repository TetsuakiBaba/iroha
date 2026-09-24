"""生成物の置き場所。すべて data/ 以下（.gitignore 済み）。"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from iroha.config import Config, PROJECT_ROOT

SPLITS = ("train", "validation", "test")


@dataclass(frozen=True)
class Paths:
    root: Path
    # ダウンロードしたままの生データ。ビルド先（root）とは別に持てるようにしてある
    # （生データの取得は高い・ビルドは安いので、設定を変えて何度もビルドできるように）
    raw_root: Path

    @property
    def raw(self) -> Path:
        return self.raw_root

    @property
    def canonical(self) -> Path:
        return self.root / "canonical"

    @property
    def kkc(self) -> Path:
        return self.root / "kkc"

    @property
    def typo(self) -> Path:
        return self.root / "typo"

    @property
    def samples(self) -> Path:
        return self.root / "samples"

    @property
    def stats_json(self) -> Path:
        return self.root / "stats.json"

    @property
    def report_md(self) -> Path:
        return self.root / "REPORT.md"

    def raw_for(self, source: str) -> Path:
        return self.raw / source

    def canonical_for(self, source: str) -> Path:
        return self.canonical / f"{source}.jsonl"

    def morphemes_for(self, source: str) -> Path:
        return self.canonical / f"{source}.morphemes.jsonl"

    def stage_stats(self, stage: str) -> Path:
        return self.root / f"_stats.{stage}.json"

    def ensure(self) -> "Paths":
        for p in (self.raw, self.canonical, self.kkc, self.typo, self.samples):
            p.mkdir(parents=True, exist_ok=True)
        return self


def _resolve(value: str) -> Path:
    path = Path(value).expanduser()
    if not path.is_absolute():
        path = PROJECT_ROOT / path
    return path.resolve()


def paths_from_config(cfg: Config) -> Paths:
    root = _resolve(cfg.get("data_dir", "data"))
    raw_root = _resolve(cfg.get("raw_dir", "data/raw"))
    return Paths(root, raw_root)
