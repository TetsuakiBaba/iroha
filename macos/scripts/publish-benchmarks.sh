#!/bin/bash
# 変換のベンチマーク（設定 > モデル > 変換のベンチマーク）で使う評価セットを配布用に整え、
# カタログ models/benchmarks.json を書き、GitHub Releases の専用タグへ上げるコマンドを表示する。
#
#   ./macos/scripts/publish-benchmarks.sh            # dist/benchmarks/ を作り、カタログを書く（上げはしない）
#   ./macos/scripts/publish-benchmarks.sh --upload   # 続けて Release を作り（無ければ）ファイルを上げる
#
# 配るもの:
#   iroha-bench-v1.jsonl        testdata/iroha-bench-v1/release/iroha-bench-v1.jsonl そのもの（3,253 件）
#   iroha-bench-v1-quick.jsonl  出典ごとにファイルの順で先頭 40 件を抜き出したもの（9 出典 × 40 = 360 件）
#   LICENSE.md / README.md      testdata/iroha-bench-v1/release/ のもの
#
# 配布の考えかた:
#   - iroha-bench-v1 は CC BY-SA 4.0（項目によって CC BY 4.0・MIT の出典が混ざる）。本体のコード（MIT）とは分けて、
#     アプリは設定画面で「取得」を押したときに落とす（専門辞書・打ち間違いの訂正モデルと同じ）
#   - **必ずプレリリースとして作る**。通常リリースにすると releases/latest がこれを指し、UpdateChecker が壊れる
#   - **評価セットの中身を変えたら ID（ファイル名）を変える**。同じ ID の結果どうしを比べるため。
#     作り直した版を同じ名前で上げ直さない
set -euo pipefail
cd "$(dirname "$0")/../.."   # リポジトリのルート

TAG="benchmarks-v1"
REPO="TetsuakiBaba/iroha"
SOURCE="testdata/iroha-bench-v1/release"
DEST="dist/benchmarks"
CATALOG="models/benchmarks.json"

rm -rf "$DEST"
mkdir -p "$DEST"
cp "$SOURCE/iroha-bench-v1.jsonl" "$SOURCE/LICENSE.md" "$SOURCE/README.md" "$DEST/"

python3 - "$DEST" "$CATALOG" "$TAG" "$REPO" "$SOURCE/LICENSE.md" <<'PY'
import hashlib, json, os, re, sys
dest, catalog_path, tag, repo, license_path = sys.argv[1:]

full = os.path.join(dest, "iroha-bench-v1.jsonl")
with open(full, encoding="utf-8") as f:
    lines = [line for line in f if line.strip()]

# 抜粋: 出典ごとにファイルの順で先頭 40 件。ファイルの並び（出典 → 番号）を保つ
PER_SOURCE = 40
taken, quick = {}, []
for line in lines:
    source = json.loads(line)["source"]
    if taken.get(source, 0) < PER_SOURCE:
        taken[source] = taken.get(source, 0) + 1
        quick.append(line)
with open(os.path.join(dest, "iroha-bench-v1-quick.jsonl"), "w", encoding="utf-8") as f:
    f.writelines(quick)

# MIT の出典（newschat）は、写しに著作権表示と許諾文を添えることを求める。LICENSE.md のコードブロックから取る
license_text = open(license_path, encoding="utf-8").read()
mit = re.search(r"```\n(MIT License\n.*?)```", license_text, re.S)
notice = ("iroha-bench-v1 の newschat の項目は、感想付きニュース雑談対話コーパス"
          "（https://github.com/fukanarita/newschat-with-impression）を加工したもので、次の条件で配布されています。\n\n"
          + mit.group(1).strip()) if mit else None

sources = [
    {"name": "LLM-jp Corpus v4（ja_kaken・ja_e-gov・ja_patent・ja_aozorabunko）", "holder": "LLM-jp コーパス構築 WG",
     "license": "CC BY 4.0", "url": "https://gitlab.llm-jp.nii.ac.jp/datasets/llm-jp-corpus-v4"},
    {"name": "RealPersonaChat", "holder": "nu-dialogue", "license": "CC BY-SA 4.0",
     "url": "https://github.com/nu-dialogue/real-persona-chat"},
    {"name": "Multi-Relational Multi-Party Chat Corpus", "holder": "nu-dialogue", "license": "CC BY-SA 4.0",
     "url": "https://github.com/nu-dialogue/multi-relational-multi-party-chat-corpus"},
    {"name": "JMRD", "holder": "京都大学 黒橋・褚・村脇研究室", "license": "CC BY-SA 4.0",
     "url": "https://github.com/ku-nlp/JMRD"},
    {"name": "J-CRe3", "holder": "理化学研究所 ほか", "license": "CC BY-SA 4.0",
     "url": "https://github.com/riken-grp/J-CRe3"},
    {"name": "感想付きニュース雑談対話コーパス", "holder": "Fuka Narita", "license": "MIT",
     "url": "https://github.com/fukanarita/newschat-with-impression"},
]

def entry(file_name, id_, name, summary, count):
    path = os.path.join(dest, file_name)
    data = open(path, "rb").read()
    return {
        "id": id_, "name": name, "summary": summary, "items": count,
        "file": {"url": f"https://github.com/{repo}/releases/download/{tag}/{file_name}",
                 "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()},
        "license": "CC BY-SA 4.0", "sources": sources, "notice": notice,
    }

common = ("学習データ（zenz-v2.5-dataset の出典）と重ならない文から作った評価セット。書き言葉（科研費の研究概要・法令・"
          "特許・青空文庫）と話し言葉（雑談の対話）の 9 出典。正解は 1 項目に 1 つで、表記の揺れも誤答になる。")
catalog = {
    "formatVersion": 1,
    "benchmarks": [
        entry("iroha-bench-v1-quick.jsonl", "iroha-bench-v1-quick", "iroha-bench-v1（抜粋）",
              common + f"出典ごとに先頭 {PER_SOURCE} 件を抜き出した {len(quick)} 件。手早く比べるとき用。", len(quick)),
        entry("iroha-bench-v1.jsonl", "iroha-bench-v1", "iroha-bench-v1",
              common + f"全 {len(lines)} 件。モデルどうしの差はこちらで比べる。", len(lines)),
    ],
}
with open(catalog_path, "w", encoding="utf-8") as f:
    json.dump(catalog, f, ensure_ascii=False, indent=2)
    f.write("\n")
for e in catalog["benchmarks"]:
    print(f"  {e['id']}: {e['items']} 件、{e['file']['bytes']} バイト、sha256 {e['file']['sha256'][:12]}…")
PY

echo "==> $DEST と $CATALOG を書きました"
UPLOAD=(gh release upload "$TAG" --repo "$REPO" --clobber
        "$DEST/iroha-bench-v1.jsonl" "$DEST/iroha-bench-v1-quick.jsonl" "$DEST/LICENSE.md" "$DEST/README.md")
CREATE=(gh release create "$TAG" --repo "$REPO" --prerelease --title "変換のベンチマークの評価セット"
        --notes "iroha の設定 > モデル > 変換のベンチマーク が取得する評価セット（iroha-bench-v1、CC BY-SA 4.0。出典ごとのライセンスは LICENSE.md）。アプリの更新ではありません。")

if [ "${1:-}" = "--upload" ]; then
  if ! gh release view "$TAG" --repo "$REPO" >/dev/null 2>&1; then
    "${CREATE[@]}"
  fi
  "${UPLOAD[@]}"
  echo "==> 上げました。$CATALOG をコミットして main に push すると、アプリから取得できます"
else
  echo "上げるには --upload を付けて実行する（タグ $TAG のプレリリースが無ければ作り、4 つのファイルを上げる）"
fi
