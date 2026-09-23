"""コマンドライン。

    python -m iroha_dataset download
    python -m iroha_dataset preprocess
    python -m iroha_dataset build-kkc      # KKC = かな漢字変換（Kana-Kanji Conversion）
    python -m iroha_dataset build-typo
    python -m iroha_dataset build-jwtd     # JWTD（Wikipedia の実 typo）→ 学習データ + ベンチ
    python -m iroha_dataset stats
    python -m iroha_dataset samples
    python -m iroha_dataset build-all      # download 以降を一括
    python -m iroha_dataset sources        # 使えるソースとライセンスを出す

共通オプション:

    --config config/smoke.yaml     設定ファイル（default.yaml に重ねる）
    --set typo.clean_ratio=0.3     個別の上書き（YAML として解釈）
    --source tatoeba               対象ソースを絞る（複数指定可）
"""
from __future__ import annotations

import argparse
import json
import sys
import time

from iroha_dataset import __version__
from iroha_dataset.config import load_config
from iroha_dataset.paths import paths_from_config
from iroha_dataset.sources import adapter_names, all_source_info, enabled_adapters, get_adapter


def _adapters(cfg, paths, selected: list[str] | None):
    if selected:
        unknown = [s for s in selected if s not in adapter_names()]
        if unknown:
            raise SystemExit(f"未知のソース: {', '.join(unknown)}　"
                             f"（使えるのは {', '.join(adapter_names())}）")
        return [get_adapter(name, cfg, paths) for name in selected]
    adapters = enabled_adapters(cfg, paths)
    if not adapters:
        raise SystemExit("有効なソースが無い。config の sources.<name>.enabled を true にするか "
                         "--source で指定する")
    return adapters


def _echo(obj) -> None:
    print(json.dumps(obj, ensure_ascii=False, indent=2))


def cmd_sources(args, cfg, paths) -> int:
    for info in all_source_info():
        enabled = cfg.get(f"sources.{info.name}.enabled", False)
        print(f"■ {info.name}  [{'有効' if enabled else '無効'}]")
        print(f"  {info.title}")
        print(f"  homepage : {info.homepage}")
        print(f"  license  : {info.license}")
        print(f"  出典表記 : {info.attribution}")
        if info.used_fields:
            print("  使用フィールド:")
            for f in info.used_fields:
                print(f"    - {f}")
        if info.notes:
            print("  注意:")
            for n in info.notes:
                print(f"    - {n}")
        print()
    return 0


def cmd_download(args, cfg, paths) -> int:
    paths.ensure()
    out = {}
    for adapter in _adapters(cfg, paths, args.source):
        print(f"[download] {adapter.name} …", flush=True)
        started = time.time()
        out[adapter.name] = adapter.download(force=args.force)
        print(f"[download] {adapter.name} 完了 ({time.time() - started:.1f}s)", flush=True)
    _echo(out)
    return 0


def cmd_preprocess(args, cfg, paths) -> int:
    from iroha_dataset.preprocess.pipeline import run_preprocess
    paths.ensure()
    adapters = _adapters(cfg, paths, args.source)
    print(f"[preprocess] {', '.join(a.name for a in adapters)}", flush=True)
    stats = run_preprocess(cfg, paths, adapters)
    _echo(stats["totals"])
    print(f"→ {paths.canonical}")
    return 0


def cmd_build_kkc(args, cfg, paths) -> int:
    from iroha_dataset.kkc.build import KkcBuilder
    if not cfg.get("kkc.enabled", True):
        print("kkc.enabled が false なので何もしない")
        return 0
    paths.ensure()
    stats = KkcBuilder(cfg, paths).run(_adapters(cfg, paths, args.source))
    _echo({k: stats[k] for k in ("examples", "total", "avg_input_chars", "avg_context_chars")})
    print(f"→ {paths.kkc}")
    return 0


def cmd_build_typo(args, cfg, paths) -> int:
    from iroha_dataset.typo.build import TypoBuilder
    if not cfg.get("typo.enabled", True):
        print("typo.enabled が false なので何もしない")
        return 0
    paths.ensure()
    stats = TypoBuilder(cfg, paths).run(_adapters(cfg, paths, args.source))
    _echo({k: stats[k] for k in ("examples", "total", "clean_ratio", "error_types")})
    print(f"→ {paths.typo}")
    return 0


def cmd_build_jwtd(args, cfg, paths) -> int:
    from iroha_dataset.wild.jwtd import JwtdBuilder
    paths.ensure()
    builder = JwtdBuilder(cfg, paths)
    stats = builder.run()
    if builder.pairs_only:
        _echo({"kept_pairs": stats["kept_pairs"], "pairs_train": stats["pairs_train"]})
        print(f"→ {builder.out / 'pairs_train.jsonl'}")
        print(f"→ {paths.stage_stats('jwtd_pairs')}")
        return 0
    _echo({"kept_pairs": stats["kept_pairs"], "bench": stats["bench"],
           "train": {k: stats["train"][k] for k in ("examples", "clean", "excluded")},
           "pairs_train": stats["pairs_train"]["pairs"]})
    print(f"→ {builder.out}")
    print(f"→ {paths.stage_stats('jwtd')}")
    return 0


def cmd_estimate_typo_dist(args, cfg, paths) -> int:
    """JWTD の実誤りから typo 生成器の抽出確率を推定する（build-jwtd の後に実行）"""
    from iroha_dataset.wild.jwtd_dist import estimate
    pairs = paths.root / "jwtd" / "pairs_train.jsonl"
    if not pairs.exists():
        raise SystemExit(f"{pairs} が無い。先に build-jwtd を実行する")
    stats = paths.stage_stats("jwtd_pairs")
    if not stats.exists():
        stats = paths.stage_stats("jwtd")
    res = estimate(pairs, stats, paths.root / "typo-dist",
                   mixed_input=float(cfg.get("typo_dist.mixed_input", 0.05)))
    _echo(res)
    print(f"→ {paths.root / 'typo-dist' / 'jwtd.yaml'}")
    print(f"→ {paths.root / 'typo-dist' / 'REPORT.md'}")
    return 0


def cmd_stats(args, cfg, paths) -> int:
    from iroha_dataset.stats import run_stats
    paths.ensure()
    stats = run_stats(cfg, paths)
    print(f"→ {paths.stats_json}")
    print(f"→ {paths.report_md}")
    print(f"原文 {stats['canonical']['total_sentences']:,} 文 / "
          f"kkc {stats['kkc'].get('total', 0):,} example / "
          f"typo {stats['typo'].get('total', 0):,} example")
    return 0


def cmd_samples(args, cfg, paths) -> int:
    from iroha_dataset.samples import run_samples
    paths.ensure()
    out = run_samples(cfg, paths)
    _echo(out)
    print(f"→ {paths.samples}")
    return 0


def cmd_build_all(args, cfg, paths) -> int:
    for fn in (cmd_download, cmd_preprocess, cmd_build_kkc, cmd_build_typo,
               cmd_samples, cmd_stats):
        rc = fn(args, cfg, paths)
        if rc != 0:
            return rc
    return 0


COMMANDS = {
    "sources": cmd_sources,
    "download": cmd_download,
    "preprocess": cmd_preprocess,
    "build-kkc": cmd_build_kkc,
    "build-typo": cmd_build_typo,
    "build-jwtd": cmd_build_jwtd,
    "estimate-typo-dist": cmd_estimate_typo_dist,
    "stats": cmd_stats,
    "samples": cmd_samples,
    "build-all": cmd_build_all,
}


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m iroha_dataset",
        description="iroha 用の学習データセット生成"
                    "（KKC = かな漢字変換 / typo = 打ち間違いの訂正）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("command", choices=sorted(COMMANDS), help="実行するコマンド")
    parser.add_argument("--config", default=None, help="設定 YAML（config/default.yaml に重ねる）")
    parser.add_argument("--set", dest="overrides", action="append", default=[],
                        metavar="KEY=VALUE", help="設定の個別上書き（例 --set typo.clean_ratio=0.3）")
    parser.add_argument("--source", action="append", default=None,
                        help=f"対象ソース（{', '.join(adapter_names())}）。既定は config で有効なもの")
    parser.add_argument("--force", action="store_true", help="download で既存ファイルを取り直す")
    parser.add_argument("--version", action="version", version=f"iroha-dataset {__version__}")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    cfg = load_config(args.config, args.overrides)
    paths = paths_from_config(cfg)
    return COMMANDS[args.command](args, cfg, paths)


if __name__ == "__main__":
    sys.exit(main())
