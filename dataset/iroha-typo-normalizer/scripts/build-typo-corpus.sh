#!/bin/bash
# typo normalizer の学習用コーパス（正しい読みの一覧）を 1 回の実行で作る。
#
#   ./scripts/build-typo-corpus.sh [balanced|full|both|spoken]     （既定 balanced）
#
#   balanced  ソースごとに文書を間引いて割合を揃えたもの（2,038 万読み。readings-balanced-8ep の学習データ）
#   full      間引かずに全部入れたもの（4,392 万読み）
#   spoken    話し言葉（対話コーパス 5 つ + open2ch。config/typo-spoken.yaml。LICENSES.md H 節）。
#             ソースごとの件数・除外理由を data/typo-corpus/readings-spoken/SPOKEN_REPORT.md にも置く
#
# やること: 取得（約 7GB）→ 前処理（ソースごとに並列）→ 読み一覧の作成 → SHA-256 の照合 →
# data/typo-corpus/readings-<種類>/ に置く（**既にあれば上書きせず、照合の結果だけ出す**）。
# 途中のファイル（元データ・canonical）は Dropbox の外の $IROHA_TYPO_WORK（既定 ~/iroha-typo-data）に置く（both で約 50GB）。
# 終わったら消してよい（rm -rf ~/iroha-typo-data）。
#
# 同じものができる前提:
#   - 元データの版は固定してある（zenz_wiki.py / llmjp.py の REVISION）
#   - 除外する評価セット（config/typo-corpus.yaml の exclude_readings_from / exclude_overlap_with）は
#     ../../training/typo-normalizer/data/ にある。**無いファイルは黙って飛ばされ、別物ができる**ので、先に確かめる
#   - Sudachi の辞書の版で読みが変わりうる（2026-09-23 は sudachipy 0.6.11 / sudachidict-full 20260723）
set -euo pipefail
cd "$(dirname "$0")/.."   # dataset/iroha-typo-normalizer/

VARIANT="${1:-balanced}"
case "${VARIANT}" in balanced|full|both|spoken) ;; *) echo "使い方: $0 [balanced|full|both|spoken]" >&2; exit 1 ;; esac
WORK="${IROHA_TYPO_WORK:-$HOME/iroha-typo-data}"
PY=./.venv/bin/python
mkdir -p "${WORK}"
CONFIG=config/typo-corpus.yaml
SOURCES="zenz_wiki llmjp_kaken llmjp_egov llmjp_patent llmjp_aozora"
if [ "${VARIANT}" = spoken ]; then
  CONFIG=config/typo-spoken.yaml
  SOURCES="realpersonachat mrmp jmrd newschat jcre3 open2ch"
fi

# 2026-09-23 に作ったもの（data/typo-corpus/）の SHA-256
expected_sha() {
  case "$1" in
    balanced/train.jsonl)      echo 70d3a7e6ceb2df43dc11f9726ceebeacebeaf7dbaed4f734175f80abcd655b63 ;;
    balanced/validation.jsonl) echo 97062cd87223fcebf7b563f0690728bbdae39e4a50694b971880c6d3ca4bf24f ;;
    balanced/test.jsonl)       echo f33518a9d162630db6e8d065a6d74c2e42403fc5fde21c0b630f4c6553d2a917 ;;
    full/train.jsonl)          echo 0497c2bd69faa5a37a073d48f8c62b85db360493538d49a201e02c91353646c3 ;;
    full/validation.jsonl)     echo 7f822f0e733d7a6dc5e22a36f1eca92c146f64df282f3b5341e47bf03e4a83e1 ;;
    full/test.jsonl)           echo 7aa6b711f72207c404ef3afb4f26a04405601bf8436f13c00c7b100576f7fc47 ;;
    # 2026-09-25 に作った話し言葉の一覧（config/typo-spoken.yaml の設定を変えたら作り直して更新する）
    spoken/train.jsonl)        echo 8c618e6cb4eeb89da6b49ddadfcac6403b3fd9cf2322c0651c774b88751c9430 ;;
    spoken/validation.jsonl)   echo 29d51bd203da3e72159194d027f65e9cd3d13d279745bc5f6ba17bc0d1f20473 ;;
    spoken/test.jsonl)         echo 2c15377642a33440b72f54736f77d5f427e60e46e5e75b054fc0d90caaf9ac01 ;;
  esac
}

if [ ! -x "${PY}" ]; then
  echo "error: ${PY} がありません。先に: python3 -m venv .venv && ./.venv/bin/python -m pip install -e '.[dev]'" >&2
  exit 1
fi

echo "==> 除外する評価セットがそろっているか"
"${PY}" - "${CONFIG}" <<'EOF'
import sys
from pathlib import Path
import yaml
cfg = yaml.safe_load(open(sys.argv[1], encoding="utf-8"))
paths = list(cfg["typo"]["exclude_readings_from"]) + list(
    (cfg["sources"].get("zenz_wiki") or {}).get("exclude_overlap_with") or [])
missing = [p for p in paths if not Path(p).expanduser().exists()]
if missing:
    sys.exit("error: 次のファイルが無い（無いと黙って飛ばされ、別の一覧ができる）:\n  " + "\n  ".join(missing))
print(f"  {len(paths)} 件ともある")
EOF
"${PY}" -c "import importlib.metadata as m; print('  sudachipy', m.version('sudachipy'), '/ sudachidict-full', m.version('sudachidict-full'))"

echo "==> 取得（${WORK}/raw。取得済みのファイルは飛ばす）"
"${PY}" -m iroha download --config "${CONFIG}" --set raw_dir="${WORK}/raw"

build() {   # $1 = balanced|full|spoken
  local name=$1 dir ratio_args=()
  dir="${WORK}/typo-corpus-${name}"
  [ "${name}" = spoken ] && dir="${WORK}/typo-spoken"
  echo "==> 前処理（${name}、ソースごとに並列）: ${dir}"
  local pids=()
  for s in ${SOURCES}; do
    [ "${name}" = full ] && ratio_args=(--set "sources.${s}.document_ratio=1.0")
    "${PY}" -m iroha preprocess --config "${CONFIG}" --set raw_dir="${WORK}/raw" \
        --set data_dir="${dir}" ${ratio_args[@]+"${ratio_args[@]}"} --source "${s}" > "${WORK}/preprocess-${name}-${s}.log" 2>&1 &
    pids+=($!)
  done
  for p in "${pids[@]}"; do wait "${p}" || { echo "error: 前処理が失敗（${WORK}/preprocess-${name}-*.log）" >&2; exit 1; }; done

  echo "==> 読み一覧（${name}）"
  "${PY}" -m iroha build-readings --config "${CONFIG}" --set raw_dir="${WORK}/raw" --set data_dir="${dir}"
  if [ "${name}" = spoken ]; then
    echo "==> ソースごとの件数（${dir}/SPOKEN_REPORT.md）"
    "${PY}" -m iroha spoken-stats --config "${CONFIG}" --set raw_dir="${WORK}/raw" --set data_dir="${dir}" > /dev/null
  fi

  echo "==> 照合（${name}）"
  local ok=1 f got want
  for f in train.jsonl validation.jsonl test.jsonl; do
    got=$(shasum -a 256 "${dir}/readings/${f}" | cut -d' ' -f1)
    want=$(expected_sha "${name}/${f}")
    if [ -z "${want}" ]; then echo "  --  ${f}（期待値が未登録。${got}）"
    elif [ "${got}" = "${want}" ]; then echo "  OK  ${f}"; else echo "  NG  ${f}（${got}、期待 ${want}）"; ok=0; fi
  done

  local dest="data/typo-corpus/readings-${name}"
  if [ -e "${dest}" ]; then
    echo "==> ${dest} は既にあるので置かない（照合の結果だけ）"
  elif [ "${ok}" = 1 ]; then
    mkdir -p "${dest}"
    cp "${dir}/readings/"{train,validation,test}.jsonl "${dest}/"
    cp "${dir}/_stats.readings.json" "${dest}/stats.json"
    if [ "${name}" = spoken ]; then cp "${dir}/SPOKEN_REPORT.md" "${dir}/_stats.spoken.json" "${dest}/"; fi
    echo "==> 置いた: ${dest}"
  else
    echo "error: 2026-09-23 のものと一致しないので置かない（${dir}/readings/ に残してある）" >&2
    exit 1
  fi
}

[ "${VARIANT}" = balanced ] || [ "${VARIANT}" = both ] && build balanced
[ "${VARIANT}" = full ] || [ "${VARIANT}" = both ] && build full
[ "${VARIANT}" = spoken ] && build spoken
echo "==> 終わり。途中のファイルは ${WORK} にある（要らなければ消す）"
