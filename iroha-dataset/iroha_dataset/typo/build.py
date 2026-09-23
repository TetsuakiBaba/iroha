"""typo normalizer 用データ（``build-typo``）。

canonical record の ``reading`` を正解として

    {"input": "きょはとうきょうとりつだいがくにいきます",
     "target": "きょうはとうきょうとりつだいがくにいきます",
     "error_type": "deletion"}

を作る。生成は必ず

    正しいひらがな → ローマ字（打鍵列）→ 疑似キー入力エラー → かなへ再変換

の順（``typo/errors.py``）。かな文字を直接消したりはしない。

* ``clean_ratio`` の割合で「正常入力 → 同一正常入力」も入れる
  （normalizer が何でも書き換えようとするのを防ぐ）。1 つの読みから作れる clean は
  1 件だけ（同じ (input, target) なので重複除去で落ちる）なので、達成できる clean 比率は
  ``1 / variants_per_clean_sample`` が上限。既定は 4 variants / clean_ratio 0.25 で
  ちょうど「clean 1 件 + typo 3 件」になる。届かない設定のときは REPORT の
  ``clean_ratio_requested`` と ``clean_ratio`` がずれるので、そこで気づけるようにしてある
* 1 サンプルに入れる typo は既定 1 個、``second_error_ratio`` の割合で 2 個
  （``max_errors_per_sample`` が上限）
* かなが変わらない崩し（shi の h が落ちて si → し）は typo ではないので引き直す
"""
from __future__ import annotations

import random
from dataclasses import dataclass, field

from iroha_dataset.config import Config
from iroha_dataset.dedup import KeyDeduplicator
from iroha_dataset.jsonlio import SplitWriter, read_jsonl, write_json
from iroha_dataset.paths import Paths, SPLITS
from iroha_dataset.sources.base import SourceAdapter
from iroha_dataset.typo.errors import (APPLICABLE, ERROR_TYPES, ErrorContext,
                                       KeyStream, Typo)
from iroha_dataset.typo.romanize import RomanizeError, romanize_units, to_kana

CLEAN = "clean"


@dataclass
class TypoReport:
    examples: dict = field(default_factory=dict)
    per_source: dict = field(default_factory=dict)
    error_types: dict = field(default_factory=dict)
    clean: int = 0
    noisy: int = 0
    clean_ratio_requested: float = 0.0
    clean_ratio_reachable: float = 0.0
    error_types_requested: dict = field(default_factory=dict)
    skipped: dict = field(default_factory=dict)
    retries: dict = field(default_factory=dict)
    duplicates: int = 0
    input_chars: int = 0
    max_input_chars: int = 0
    units: dict = field(default_factory=dict)

    def _achieved_ratio(self) -> dict:
        """typo あり example のうち、各 error type が何割だったか（config との比較用）。

        1 example に 2 つ typo が入ることがあるので、type の出現回数で数える。
        """
        counts: dict[str, int] = {}
        for key, n in self.error_types.items():
            if key == CLEAN:
                continue
            for part in key.split("+"):
                counts[part] = counts.get(part, 0) + n
        total = sum(counts.values())
        if not total:
            return {}
        return {k: round(v / total, 4) for k, v in sorted(counts.items(), key=lambda kv: -kv[1])}

    def bump(self, table: dict, key: str) -> None:
        table[key] = table.get(key, 0) + 1

    def as_dict(self) -> dict:
        total = sum(self.examples.values())
        return {
            "examples": dict(self.examples),
            "total": total,
            "per_source": self.per_source,
            "error_types": dict(sorted(self.error_types.items(), key=lambda kv: -kv[1])),
            "error_types_requested": self.error_types_requested,
            "error_types_achieved_ratio": self._achieved_ratio(),
            "clean": self.clean,
            "noisy": self.noisy,
            "clean_ratio": round(self.clean / total, 4) if total else 0.0,
            "clean_ratio_requested": self.clean_ratio_requested,
            "clean_ratio_reachable": self.clean_ratio_reachable,
            "skipped": self.skipped,
            "retry_reasons": dict(sorted(self.retries.items(), key=lambda kv: -kv[1])),
            "duplicates_dropped": self.duplicates,
            "avg_input_chars": round(self.input_chars / total, 2) if total else 0.0,
            "max_input_chars": self.max_input_chars,
            "by_unit": dict(self.units),
        }


class TypoGenerator:
    """1 つの clean な読みから typo サンプルを作る。"""

    def __init__(self, cfg: Config):
        typo = cfg.sub("typo")
        weights = dict(typo.get("error_types", {}) or {})
        self.types = [t for t, w in weights.items() if float(w) > 0]
        unknown = [t for t in self.types if t not in ERROR_TYPES]
        if unknown:
            raise ValueError(f"未知の error type: {unknown}（使えるのは {sorted(ERROR_TYPES)}）")
        self.weights = [float(weights[t]) for t in self.types]
        if not self.types:
            raise ValueError("typo.error_types がすべて 0")
        self.max_errors = max(1, int(typo.get("max_errors_per_sample", 2)))
        self.second_ratio = float(typo.get("second_error_ratio", 0.2))
        self.max_attempts = int(typo.get("max_attempts", 12))
        styles = dict(typo.get("romaji_style", {"nn": 1.0}) or {"nn": 1.0})
        self.styles = list(styles)
        self.style_weights = [float(styles[s]) for s in self.styles]
        self.key_weights = {str(k): float(v) for k, v in (typo.get("key_weights", {}) or {}).items()}
        self.match_ratios = bool(typo.get("match_error_ratios", True))
        # 実績が config の比率に寄るように、不足している type を優先して引く
        # （促音の過不足は「っ」を含む読みにしか当てられないので、素直に引くと
        #   比率が 1/7 くらいまで落ちる。README「error type の比率」参照）
        self._drawn: dict[str, int] = {}
        self._drawn_total = 0
        mixed = typo.sub("mixed_input")
        self.mixed_enabled = bool(mixed.get("enabled", True))
        self.mixed_min_units = int(mixed.get("min_units", 1))
        self.mixed_max_units = int(mixed.get("max_units", 4))
        if not self.mixed_enabled and "mixed_input" in self.types:
            i = self.types.index("mixed_input")
            self.types.pop(i)
            self.weights.pop(i)
        self.retries: dict[str, int] = {}
        # 実誤りから推定した分布（jwtd_dist.py が書く typo.dist）。無ければ従来の一様な選び方
        dist = typo.get("dist", {}) or {}
        self.dist = dist.data if hasattr(dist, "data") else dict(dist)
        self.repeat_sokuon_bias = float(typo.get("repeat_sokuon_bias", 0.6))

    def _bump_retry(self, key: str) -> None:
        self.retries[key] = self.retries.get(key, 0) + 1

    def _context(self, rng: random.Random) -> ErrorContext:
        return ErrorContext(rng=rng, key_weights=self.key_weights,
                            mixed_min_units=self.mixed_min_units,
                            mixed_max_units=self.mixed_max_units,
                            repeat_sokuon_bias=self.repeat_sokuon_bias, dist=self.dist)

    def stream_for(self, clean: str, rng: random.Random) -> tuple[KeyStream, str] | None:
        style = rng.choices(self.styles, self.style_weights)[0]
        try:
            keys, units, unit_keys = romanize_units(clean, style)
        except RomanizeError:
            return None
        # 打鍵列に戻したときに元のかなに戻らない読みは扱わない（教師が壊れる）
        if to_kana(keys) != clean:
            return None
        return KeyStream(keys=keys, units=units, unit_keys=unit_keys), style

    def generate(self, clean: str, rng: random.Random, *, force_clean: bool = False,
                 force_type: str | None = None) -> dict | None:
        built = self.stream_for(clean, rng)
        if built is None:
            self._bump_retry("not_romanizable")
            return None
        stream, style = built
        if force_clean:
            return {"input": clean, "target": clean, "error_type": CLEAN,
                    "romaji_style": style, "keys": stream.keys, "n_errors": 0, "detail": ""}

        n_errors = 1
        if self.max_errors >= 2 and rng.random() < self.second_ratio:
            n_errors = 2
        applied: list[Typo] = []
        current = stream
        output = clean
        for _ in range(n_errors):
            typo = self._one_typo(current, output, clean, rng, force_type)
            if typo is None:
                break
            applied.append(typo)
            if typo.raw_output is not None:
                # mixed_input はかなに戻せないので、ここで打ち止め
                output = typo.raw_output
                break
            output = to_kana(typo.keys)
            current = KeyStream(keys=typo.keys, units=current.units,
                                unit_keys=current.unit_keys)
        if not applied:
            self._bump_retry("gave_up")
            return None
        if output == clean:
            # 2 つの typo が打ち消し合って元に戻った（_one_typo で弾いているが念のため）
            self._bump_retry("cancelled_out")
            return None
        return {
            "input": output,
            "target": clean,
            "error_type": "+".join(t.error_type for t in applied),
            "romaji_style": style,
            "keys": applied[-1].keys,
            "n_errors": len(applied),
            "detail": "; ".join(t.detail for t in applied),
        }

    def _one_typo(self, stream: KeyStream, before_kana: str, clean: str,
                  rng: random.Random, force_type: str | None) -> Typo | None:
        """かな（出力）が実際に変わる typo を 1 つ引く。変わらなければ引き直す。

        ``clean`` と同じに戻ってしまう崩しも弾く（2 つ目の typo が 1 つ目を
        打ち消して「error_type は付いているのに input == target」になるのを防ぐ）。
        """
        ctx = self._context(rng)
        # この打鍵列に当てられる type だけへ重みを配り直す。
        # 当てられない type を引いて引き直すと、その重みが他の type に流れてしまう
        # （促音の過不足は「っ」を含む読みにしか当てられないので、そのままだと
        #   config の比率よりずっと少なくなる）
        pairs = [(t, w) for t, w in zip(self.types, self.weights) if APPLICABLE[t](stream)]
        if not pairs:
            self._bump_retry("no_applicable_type")
            return None
        types = [t for t, _ in pairs]
        weights = [w for _, w in pairs]
        if self.match_ratios:
            weights = self._deficit_weights(types, weights)
        for _ in range(self.max_attempts):
            if force_type:
                kind = force_type
            else:
                kind = rng.choices(types, weights)[0]
            typo = ERROR_TYPES[kind](stream, ctx)
            if typo is None:
                self._bump_retry(f"not_applicable:{kind}")
                continue
            after = typo.raw_output if typo.raw_output is not None else to_kana(typo.keys)
            if after == before_kana:
                # 例: shi → si。打鍵は変わったがかなは同じなので typo ではない
                self._bump_retry(f"no_change:{kind}")
                continue
            if after == clean:
                # 直前の typo を打ち消して元に戻った
                self._bump_retry(f"cancelled:{kind}")
                continue
            if kind == "missing_double_consonant" and after.count("っ") >= before_kana.count("っ"):
                self._bump_retry("no_change:missing_double_consonant")
                continue
            if kind == "excessive_double_consonant" and after.count("っ") <= before_kana.count("っ"):
                self._bump_retry("no_change:excessive_double_consonant")
                continue
            self._drawn[kind] = self._drawn.get(kind, 0) + 1
            self._drawn_total += 1
            return typo
        return None

    def _deficit_weights(self, types: list[str], weights: list[float]) -> list[float]:
        """当てられる type のうち、目標比率に対して不足している分を重みにする。

        目標 ``target_i`` に対して今までに ``count_i`` 回引いているとき、
        ``target_i * (total + 1) - count_i`` を重みにする（負なら 0）。すべて 0 なら
        元の重みで引く。これで「っ を含む読みに来たときだけ促音系を強く引く」形になり、
        全体の実績が config の比率に寄る。順序に依存するが seed とデータ順で決まるので
        再実行しても同じ結果になる。
        """
        total_target = sum(self.weights) or 1.0
        targets = {t: w / total_target for t, w in zip(self.types, self.weights)}
        n = self._drawn_total + 1
        deficits = [max(0.0, targets.get(t, 0.0) * n - self._drawn.get(t, 0)) for t in types]
        if sum(deficits) <= 0:
            return weights
        return deficits


class TypoBuilder:
    def __init__(self, cfg: Config, paths: Paths):
        self.cfg = cfg
        self.paths = paths
        self.generator = TypoGenerator(cfg)
        typo = cfg.sub("typo")
        # 1 つの読みから作る example の総数（clean を含む）
        self.variants = max(1, int(typo.get("variants_per_clean_sample", 4)))
        self.clean_ratio = float(typo.get("clean_ratio", 0.25))
        self.units = list(typo.get("units", ["sentence"]))
        self.min_chars = int(typo.get("min_chars", 4))
        self.max_chars = int(typo.get("max_chars", 60))
        self.seed = int(cfg.get("seed", 42))

    def _clean_readings(self, record: dict) -> list[tuple[str, str]]:
        """(読み, 単位名) のリスト。"""
        out: list[tuple[str, str]] = []
        if "sentence" in self.units:
            out.append((record.get("reading", ""), "sentence"))
        if "chunk" in self.units:
            for chunk in record.get("chunks") or []:
                out.append((chunk.get("reading", ""), "chunk"))
        return [(r, u) for r, u in out if self.min_chars <= len(r) <= self.max_chars]

    def run(self, adapters: list[SourceAdapter]) -> dict:
        report = TypoReport()
        dedup = KeyDeduplicator()
        with SplitWriter(self.paths.typo, SPLITS) as writer:
            for adapter in adapters:
                path = self.paths.canonical_for(adapter.name)
                if not path.exists():
                    continue
                per_source = report.per_source.setdefault(adapter.name, {s: 0 for s in SPLITS})
                for record in read_jsonl(path):
                    split = record.get("split", "train")
                    for index, (clean, unit) in enumerate(self._clean_readings(record)):
                        # 読みごとに決まった seed を使う（再実行で同じデータになる）
                        rng = random.Random(f"{self.seed}:{record['id']}:{index}")
                        # この読みに clean を 1 件入れるか。入れるなら残りを typo にする
                        # （clean は 1 読みにつき 1 件しか作れないので、比率はこの確率で決める）
                        want_clean = rng.random() < min(1.0, self.clean_ratio * self.variants)
                        for v in range(self.variants):
                            force_clean = want_clean and v == 0
                            sample = self.generator.generate(clean, rng, force_clean=force_clean)
                            if sample is None:
                                report.bump(report.skipped, "no_sample")
                                continue
                            if dedup.is_duplicate(sample["input"], sample["target"]):
                                continue
                            example = {
                                "id": f"{record['id']}_{unit[:1]}{index:02d}v{v:02d}",
                                "source": record["source"],
                                "document_id": record["document_id"],
                                "input": sample["input"],
                                "target": sample["target"],
                                "error_type": sample["error_type"],
                                "unit": unit,
                                "romaji_style": sample["romaji_style"],
                                "n_errors": sample["n_errors"],
                                "detail": sample["detail"],
                            }
                            writer.write(split, example)
                            report.examples[split] = report.examples.get(split, 0) + 1
                            per_source[split] = per_source.get(split, 0) + 1
                            report.bump(report.error_types, sample["error_type"])
                            report.bump(report.units, unit)
                            report.input_chars += len(sample["input"])
                            report.max_input_chars = max(report.max_input_chars,
                                                         len(sample["input"]))
                            if sample["error_type"] == CLEAN:
                                report.clean += 1
                            else:
                                report.noisy += 1
        report.duplicates = dedup.dropped
        report.retries = self.generator.retries
        report.clean_ratio_requested = self.clean_ratio
        report.clean_ratio_reachable = round(1.0 / self.variants, 4)
        total_w = sum(self.generator.weights) or 1.0
        report.error_types_requested = {
            t: round(w / total_w, 4)
            for t, w in sorted(zip(self.generator.types, self.generator.weights),
                               key=lambda tw: -tw[1])}
        stats = report.as_dict()
        write_json(self.paths.stage_stats("typo"), stats)
        return stats
