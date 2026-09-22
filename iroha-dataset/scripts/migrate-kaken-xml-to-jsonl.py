#!/usr/bin/env python3
"""取得済みの KAKEN の XML（1 課題 1 ファイル）を年度ごとの JSONL にまとめる。

    ./.venv/bin/python scripts/migrate-kaken-xml-to-jsonl.py [--delete-xml]

documents() は XML も JSONL も読むので移行は必須ではないが、課題数が増えると
ファイル数が問題になる（11 万課題 = 11 万ファイル）ので、まとめておくとよい。
--delete-xml を付けると、JSONL に入ったことを確認してから XML を消す。
"""
from __future__ import annotations

import argparse
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from iroha_dataset.config import load_config          # noqa: E402
from iroha_dataset.jsonlio import read_jsonl          # noqa: E402
from iroha_dataset.paths import paths_from_config     # noqa: E402
from iroha_dataset.sources import get_adapter         # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", default=None)
    parser.add_argument("--delete-xml", action="store_true",
                        help="JSONL に入ったことを確認してから XML を消す")
    args = parser.parse_args()

    cfg = load_config(args.config)
    adapter = get_adapter("kaken", cfg, paths_from_config(cfg))
    xml_files = sorted(adapter.xml_dir.glob("*.xml")) if adapter.xml_dir.exists() else []
    if not xml_files:
        print(f"{adapter.xml_dir} に XML が無い。何もしない")
        return 0

    already = {r.get("award_number") for f in adapter.jsonl_dir.glob("*.jsonl")
               for r in read_jsonl(f)} if adapter.jsonl_dir.exists() else set()
    moved, skipped, unusable = 0, 0, 0
    migrated: list[Path] = []
    with adapter._JsonlStore(adapter) as store:
        for path in xml_files:
            try:
                root = ET.parse(path).getroot()
            except ET.ParseError:
                unusable += 1
                continue
            number = root.get("awardNumber") or path.stem
            if number in already:
                skipped += 1
                migrated.append(path)
                continue
            record = adapter._record_for(root)
            if record is None:
                unusable += 1
                continue
            store.append(record["award_year"], record)
            already.add(number)
            migrated.append(path)
            moved += 1

    print(f"XML {len(xml_files)} 件 → JSONL に {moved} 件追加"
          f"（すでに入っていた {skipped} 件 / 使えなかった {unusable} 件）")
    print(f"→ {adapter.jsonl_dir}")
    if args.delete_xml:
        for path in migrated:
            path.unlink()
        print(f"XML {len(migrated)} 件を削除した")
    else:
        print("XML はそのまま残してある（消すなら --delete-xml）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
