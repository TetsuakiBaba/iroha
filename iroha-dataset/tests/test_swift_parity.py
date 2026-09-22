"""ローマ字テーブルが Swift 側（RomajiComposer.swift）と一致していることを確かめる。

typo は打鍵列の上で起こし、それを ``romanize.py`` の移植でかなに戻している。
Swift 側のテーブルが変わるとこのデータセットが「実際の iroha が出さないかな」を
教師にしてしまうので、テーブルの食い違いはここで落とす。

iroha のリポジトリの外にこのディレクトリだけを置いた場合は skip する。
"""
import re

import pytest

from iroha_dataset.config import PROJECT_ROOT
from iroha_dataset.typo.romanize import SOKUON_CONSONANTS, TABLE

SWIFT = PROJECT_ROOT.parent / "macos" / "Sources" / "IrohaCore" / "RomajiComposer.swift"

needs_swift = pytest.mark.skipif(
    not SWIFT.exists(), reason=f"{SWIFT} が無い（iroha リポジトリの外に置かれている）")

_PAIR = re.compile(r'"((?:[^"\\]|\\.)*)"\s*:\s*"((?:[^"\\]|\\.)*)"')


def _swift_table() -> dict[str, str]:
    text = SWIFT.read_text(encoding="utf-8")
    start = text.index("static let defaultTable")
    # 宣言から最初の「行頭 4 スペース + ]」まで
    end = text.index("\n    ]", start)
    body = text[start:end]
    table = {}
    for line in body.splitlines():
        line = line.split("//", 1)[0]     # 行コメントを落とす
        for key, value in _PAIR.findall(line):
            table[key] = value
    return table


def _swift_sokuon_consonants() -> set[str]:
    text = SWIFT.read_text(encoding="utf-8")
    m = re.search(r'sokuonConsonants\s*=\s*Set\("([a-z]+)"\)', text)
    assert m, "sokuonConsonants が読めない"
    return set(m.group(1))


@needs_swift
def test_swift_table_is_parsed():
    table = _swift_table()
    # パースできていることの最低限の確認（取りこぼしがあると比較が無意味になる）
    assert len(table) > 200
    assert table["ka"] == "か"
    assert table["kyo"] == "きょ"
    assert table["~"] == "〜"


@needs_swift
def test_table_matches_swift():
    swift = _swift_table()
    assert TABLE == swift, {
        "python_only": {k: v for k, v in TABLE.items() if swift.get(k) != v},
        "swift_only": {k: v for k, v in swift.items() if TABLE.get(k) != v},
    }


@needs_swift
def test_sokuon_consonants_match_swift():
    assert SOKUON_CONSONANTS == _swift_sokuon_consonants()
