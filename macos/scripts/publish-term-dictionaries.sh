#!/bin/bash
# 専門辞書（build-term-dictionaries.py build の書き出し）を GitHub Releases の専用タグへ公開し、
# カタログ models/term-dictionaries.json を書き換える。
#
#   python3 -I macos/scripts/build-term-dictionaries.py fetch
#   python3 -I macos/scripts/build-term-dictionaries.py build
#   ./macos/scripts/publish-term-dictionaries.sh
#   （そのあと models/term-dictionaries.json をコミットして push する。アプリはこのファイルを読む）
#
# 配布の考えかた:
#   - 辞書ごとにライセンスが違う（JMdict: CC BY-SA 4.0、百薬辞書: CC BY 4.0、Wikidata: CC0、
#     郵便番号データ: 日本郵便が著作権を主張しない）。本体コード(MIT)とは別に、アプリが入れるときにだけ取る
#   - 各辞書ファイルの先頭の「#」行に出典とライセンスを書いてある。Release の説明にも同じものを書く
#   - ファイル名に作った日を入れる（<ID>-<YYYYMMDD>.tsv）。作り直しても前の版の URL は残るので、
#     古いカタログを読んだアプリが壊れない。アプリはカタログの SHA-256 が変わったら入れ直す
#   - JMdict のライセンスは配る版を新しく保つことを求める。fetch から作り直してこのスクリプトを回す
#   - **必ずプレリリースとして作る**。通常リリースにすると GitHub の releases/latest が
#     これを指してしまい、UpdateChecker（アプリの更新通知）が壊れる
set -euo pipefail
cd "$(dirname "$0")/../.."   # リポジトリのルート

TAG="term-dictionaries-v1"
REPO="TetsuakiBaba/iroha"
DIST="dist/term-dictionaries"

if [ ! -f "$DIST/catalog.json" ]; then
  echo "error: $DIST/catalog.json がありません。先に build-term-dictionaries.py build を回してください" >&2
  exit 1
fi

if ! gh release view "$TAG" --repo "$REPO" >/dev/null 2>&1; then
  echo "==> プレリリース $TAG を作る"
  NOTES=$(mktemp)
  cat > "$NOTES" <<'EOF'
iroha の設定（辞書・学習 > 専門辞書）から追加する辞書です。アプリは「追加」を押したときにだけ取得します。

各ファイルは「読み<TAB>語」のテキストで、先頭の「#」行に出典・著作権・ライセンス・加工した旨を書いています。
出典とライセンスの一覧は NOTICE.txt にもあります。元データから語を選び、読みをひらがなにそろえ、重複を除くなど
加工して作りました。各辞書は元データと同じライセンスで配布し、ほかに条件は付けません。内容の正確さは保証しません。

- 分野別の用語（jmdict-*）: CC BY-SA 4.0（https://creativecommons.org/licenses/by-sa/4.0/deed.ja）
  This dictionary uses the JMdict dictionary files. These files are the property of the Electronic Dictionary Research
  and Development Group, and are used in conformance with the Group's licence (https://www.edrdg.org/edrdg/licence.html).
  Copyright: James William Breen and the Electronic Dictionary Research and Development Group.
  https://www.edrdg.org/wiki/JMdict-EDICT_Dictionary_Project.html
  JMdict の新しい版から作り直して公開し直します（ファイル名の日付が作った日）。アプリは設定画面を開いたときに入れ直します。
- 薬の名前（hyakuyaku）: CC BY 4.0（https://creativecommons.org/licenses/by/4.0/deed.ja）
  百薬辞書 HYAKUYAKU_v20210706、© 奈良先端科学技術大学院大学 ソーシャル・コンピューティング研究室（SOCIOCOM）
  https://sociocom.naist.jp/hyakuyaku-dic/
- 地名（japanpost-places）: 日本郵便株式会社の郵便番号データ。同社は郵便番号データの著作権を主張せず、自由な配布を認めています
  https://www.post.japanpost.jp/service/search/zipcode/download/utf-readme.html
- 人名・駅・神社と寺院・旧市区町村（wikidata-*）: Wikidata の構造化データ、CC0 1.0（https://creativecommons.org/publicdomain/zero/1.0/deed.ja）
  https://www.wikidata.org/wiki/Wikidata:Licensing
EOF
  gh release create "$TAG" --repo "$REPO" --prerelease \
    --title "専門辞書（term-dictionaries-v1）" --notes-file "$NOTES"
  rm -f "$NOTES"
fi

echo "==> 辞書を載せる"
# NOTICE.txt（出典とライセンスの一覧）は作り直すたびに上書きする
gh release upload "$TAG" --repo "$REPO" --clobber "$DIST"/*.tsv "$DIST/NOTICE.txt"

echo "==> 載せたファイルを確かめる（カタログの大きさと SHA-256 と照合）"
python3 -I - "$DIST/catalog.json" <<'EOF'
import hashlib, json, sys, urllib.request
catalog = json.load(open(sys.argv[1]))
for entry in catalog["dictionaries"]:
    data = urllib.request.urlopen(entry["file"]["url"], timeout=120).read()
    ok = len(data) == entry["file"]["bytes"] and hashlib.sha256(data).hexdigest() == entry["file"]["sha256"]
    print(("OK  " if ok else "NG  ") + entry["id"])
    if not ok:
        sys.exit(1)
EOF

cp "$DIST/catalog.json" models/term-dictionaries.json
echo "==> models/term-dictionaries.json を更新しました。コミットして push すると、アプリの一覧に出ます"
