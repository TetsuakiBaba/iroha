#!/usr/bin/env python3
"""専門辞書（設定 > 辞書・学習 > 専門辞書 から入れる、読み → 語の一覧）を元データから作る。

    python3 -I macos/scripts/build-term-dictionaries.py fetch   # 元データを vendor/term-dictionary-sources/ に取る
    python3 -I macos/scripts/build-term-dictionaries.py build   # dist/term-dictionaries/ に辞書と catalog.json を書く

語の一覧が前の版（models/term-dictionaries.json）と同じ辞書は、書き出さずに前の版をそのまま使う（URL も変えない。
アプリも入れ直さない）。JMdict 以外の元データが取れなかったときも前の版を使う。毎月 GitHub Actions
（.github/workflows/term-dictionaries.yml）が fetch → build → 公開 を回す。

公開は macos/scripts/publish-term-dictionaries.sh（Release への載せ方とカタログの更新）。

元データとライセンス（再配布の条件を確かめたものだけを使う。2026-11-08 確認）:
  - JMdict（EDRDG、CC BY-SA 4.0）: 語義に付いた分野の印（&med; &comp; など）で分野ごとに分ける。
    EDRDG のライセンスは**配る版を新しく保つこと**を求める（Section 4）。作り直して公開し直す
  - 百薬辞書（奈良先端科学技術大学院大学 ソーシャル・コンピューティング研究室、CC BY 4.0）
  - 郵便番号データ（日本郵便。郵便番号データについては著作権を主張せず、配布は自由と明記）
  - Wikidata（CC0）: かなの読み（P1814）を持つ項目。読みが付いているのは人名・駅・神社と寺院・地名などの
    固有名詞にほぼ限られる（病名・化合物にはほとんど付いていない）ので、固有名詞の辞書として作る
使わないもの: 万病辞書・JMED-DICT mini・ComeJisyo（再配布の条件を確認できない）、SKK の専門辞書（GPL v2）、
Mozc の dictionary_oss（第三者のデータ）。

辞書の形式（アプリの `TermDictionary` が読む）: UTF-8 のテキストで、1 行が「読み<TAB>語」。読みはひらがな
（ー・ゔ を含む）。「#」で始まる行は注記（出典・ライセンス）で、読み込み時に飛ばす。
"""

from __future__ import annotations

import csv
import gzip
import hashlib
import io
import json
import re
import sys
import time
import unicodedata
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
import zipfile
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCES = ROOT / "vendor" / "term-dictionary-sources"
DIST = ROOT / "dist" / "term-dictionaries"
USER_AGENT = "iroha-term-dictionary-builder/1.0 (https://github.com/TetsuakiBaba/iroha)"

JMDICT_URL = "http://ftp.edrdg.org/pub/Nihongo/JMdict_e.gz"
KEN_ALL_URL = "https://www.post.japanpost.jp/service/search/zipcode/download/utf/zip/utf_ken_all.zip"
# 百薬辞書の配布ページはダウンロード用の URL を毎回作るので、ページから拾う
HYAKUYAKU_PAGE = "https://sociocom.naist.jp/download/hyakuyaku_202107/"
WIKIDATA_SPARQL = "https://query.wikidata.org/sparql"

# MARK: - 出典（カタログの sources と辞書の注記に使う）

# 各項目: name・holder・license・url（アプリの 設定 > 情報 に出す）、licenseURL（ライセンス文書。アプリにも出す）、
# notices（辞書ファイルの先頭と NOTICE.txt に書く注記）。条文は 2026-11-08 に読み直して合わせた:
#   - CC BY / BY-SA 4.0 の 3(a): 作成者・著作権表示・ライセンスへの言及と URI・無保証への言及・元データの URI・加工した旨
#   - CC BY-SA 4.0 の 3(b): 加工したものも CC BY-SA 4.0 で配る。追加の条件を付けない
#   - EDRDG: 出典を示す、ライセンス文書かその場所へのリンクを添える、データを新しい版へ定期的に更新する手順を持つ、
#     そのデータに著作権を主張しない、無保証（利用者が責任を負う）
SOURCE_JMDICT = {
    "name": "JMdict",
    "holder": "James William Breen and the Electronic Dictionary Research and Development Group",
    "license": "CC BY-SA 4.0",
    "url": "https://www.edrdg.org/wiki/JMdict-EDICT_Dictionary_Project.html",
    "licenseURL": "https://www.edrdg.org/edrdg/licence.html",
    "notices": [
        "This dictionary uses the JMdict dictionary files. These files are the property of the Electronic Dictionary"
        " Research and Development Group, and are used in conformance with the Group's licence"
        " (https://www.edrdg.org/edrdg/licence.html).",
        "Copyright: James William Breen and the Electronic Dictionary Research and Development Group",
        "ライセンス: CC BY-SA 4.0（https://creativecommons.org/licenses/by-sa/4.0/deed.ja）。この辞書も CC BY-SA 4.0 で配布する",
        "使ったのは日本語の見出し（keb）・読み（reb）・分野の印（field）だけで、英語などの訳語は含めていない",
        "無保証: EDRDG のライセンス（Section 5）のとおり、データは無保証で、利用者が責任を負う",
    ],
}
SOURCE_HYAKUYAKU = {
    "name": "百薬辞書（HYAKUYAKU_v20210706）",
    "holder": "奈良先端科学技術大学院大学 ソーシャル・コンピューティング研究室",
    "license": "CC BY 4.0",
    "url": "https://sociocom.naist.jp/hyakuyaku-dic/",
    "licenseURL": "https://creativecommons.org/licenses/by/4.0/deed.ja",
    "notices": [
        "著作権者: 奈良先端科学技術大学院大学 ソーシャル・コンピューティング研究室（© SOCIOCOM 2021）",
        "ライセンス: CC BY 4.0（https://creativecommons.org/licenses/by/4.0/deed.ja）。この辞書も CC BY 4.0 で配布する",
        "使ったのは「出現形」「出現形よみ」の列だけで、「一般名」（KEGG DRUG にもとづく）などは含めていない",
        "無保証: 元データの読みは自動で付けたもので誤りを含みうる。元データの提供者は、完全な信頼性を保証せず責任を負わないとしている",
    ],
}
SOURCE_KEN_ALL = {
    "name": "郵便番号データ",
    "holder": "日本郵便株式会社",
    "license": "著作権を主張しない（配布自由）",
    "url": "https://www.post.japanpost.jp/service/search/zipcode/download/readme.html",
    "licenseURL": "https://www.post.japanpost.jp/service/search/zipcode/download/utf-readme.html",
    "notices": [
        "利用条件: 日本郵便株式会社は郵便番号データについて著作権を主張せず、自由に配布してよいとしている"
        "（https://www.post.japanpost.jp/service/search/zipcode/download/utf-readme.html の「使用・再配布・移植・改良について」）",
        "使ったのは都道府県・市区町村・町域の名前と読みだけで、郵便番号は含めていない",
    ],
}
SOURCE_WIKIDATA = {
    "name": "Wikidata",
    "holder": "Wikidata の編集者",
    "license": "CC0 1.0",
    "url": "https://www.wikidata.org/wiki/Wikidata:Licensing",
    "licenseURL": "https://creativecommons.org/publicdomain/zero/1.0/deed.ja",
    "notices": [
        "ライセンス: Wikidata の構造化データ（項目のラベル・かなの読み P1814）は CC0 1.0"
        "（https://creativecommons.org/publicdomain/zero/1.0/deed.ja）。この辞書も CC0 1.0 で配布する",
    ],
}

# 全部の辞書ファイルに書く注記
COMMON_NOTICES = [
    "加工: 元データから語を選び、読みをひらがなにそろえ、重複を除くなど加工して作成した"
    "（iroha、https://github.com/TetsuakiBaba/iroha、macos/scripts/build-term-dictionaries.py）",
    "この辞書は現状のまま提供し、内容の正確さを保証しない。元データのライセンスのほかに条件は付けない",
]

# MARK: - JMdict の分野のまとめ方

# 分野の印（JMdict の entity 名）→ 辞書。印の一覧は JMdict の DTD にある。学術分野だけを辞書にする
# （料理・スポーツ・囲碁将棋などの印もあるが、専門辞書としては後回しにした）
JMDICT_GROUPS = [
    ("jmdict-medicine", "医学", ["med", "anat", "dent", "physiol", "pharm", "pathol", "surg", "embryo", "vet"]),
    ("jmdict-computing", "情報・コンピュータ", ["comp", "internet"]),
    ("jmdict-law", "法律・政治", ["law", "politics"]),
    ("jmdict-economics", "経済・金融", ["econ", "finc", "bus", "stockm"]),
    ("jmdict-mathematics", "数学・統計", ["math", "stat", "geom", "logic"]),
    ("jmdict-physics", "物理・天文", ["physics", "astron"]),
    ("jmdict-chemistry", "化学", ["chem", "cryst", "biochem"]),
    ("jmdict-engineering", "工学", ["engr", "elec", "electr", "mech", "civeng", "telec", "archit", "aviat", "met", "mining", "motor"]),
    ("jmdict-biology", "生物", ["biol", "bot", "zool", "ent", "ornith", "ecol", "genet", "fish", "agric", "gardn"]),
    ("jmdict-geoscience", "地学", ["geol", "min", "geogr", "paleo"]),
    ("jmdict-linguistics", "言語学", ["ling", "gramm"]),
    ("jmdict-religion", "宗教・哲学", ["Buddh", "Shinto", "Christn", "phil", "jpmyth"]),
    ("jmdict-psychology", "心理学", ["psy", "psych", "psyanal"]),
]

# 表記・読みの注記のうち、変換の候補に出さないもの（古い・不規則・検索用の表記）
JMDICT_SKIP_KANJI_INFO = {"iK", "oK", "rK", "sK"}
JMDICT_SKIP_READING_INFO = {"ik", "ok", "rk", "sk"}

# MARK: - Wikidata の辞書

WIKIDATA_GROUPS = [
    # (ID, 名前, 項目の条件, 読みの頭文字で分けて取るか, 語の種類)
    ("wikidata-people", "人名", "?item wdt:P31 wd:Q5 .", True, "person"),
    ("wikidata-stations", "駅", "?item wdt:P31/wdt:P279* wd:Q55488 .", False, "place"),
    ("wikidata-shrines-temples", "神社・寺院",
     "{ ?item wdt:P31/wdt:P279* wd:Q845945 . } UNION { ?item wdt:P31/wdt:P279* wd:Q5393308 . }", False, "properNoun"),
    ("wikidata-former-municipalities", "旧市区町村", "?item wdt:P31 wd:Q18663566 .", False, "place"),
]

# 人名は件数が多く 1 回の問い合わせでは打ち切られるので、読みの頭文字 1 文字ずつに分けて取る。
# Wikidata の問い合わせサービスは 60 秒で打ち切って途中までの応答を返し、しかもそれを同じ問い合わせに対して
# キャッシュする（2026-11-08、GitHub Actions で 5 文字ずつの分け方が毎回同じ位置で途切れた）。
# 1 文字ずつにして時間内に収め、正規表現より速い前方一致（STRSTARTS）を使う
KANA_INITIALS = [chr(c) for c in range(ord("ぁ"), ord("ゖ") + 1)] + [chr(c) for c in range(ord("ァ"), ord("ヺ") + 1)]


# MARK: - 正規化


def to_hiragana(text: str) -> str:
    return "".join(chr(ord(c) - 0x60) if "ァ" <= c <= "ヶ" else c for c in text)


def normalized_reading(text: str) -> str | None:
    """読みをひらがなにする。iroha の読みとして成り立たなければ None"""
    text = unicodedata.normalize("NFKC", text)  # 半角カナ・全角英数を寄せる
    text = re.sub(r"[\s・･=＝]", "", text)
    text = to_hiragana(text)
    if len(text) < 2:
        return None
    if not all(("ぁ" <= c <= "ゖ") or c in "ゝゞー" for c in text):
        return None
    return text


def normalized_word(text: str) -> str | None:
    text = text.strip()
    # 全角の英数字・記号は半角にする（かな・漢字は NFKC にかけない。異体字が寄ってしまう）
    text = "".join(chr(ord(c) - 0xFEE0) if "！" <= c <= "～" else c for c in text)
    if not text or len(text) > 40 or "\t" in text or "\n" in text:
        return None
    return text


class Pack:
    """1 つの辞書（読み・語の組を集めて重複を除く）"""

    def __init__(self, pack_id: str, name: str, group: str, summary: str, sources: list[dict], kind: str = "noun"):
        self.id = pack_id
        # 語の種類（アプリが辞書ラティスでの品詞に使う）: noun / person / place / properNoun
        self.kind = kind
        self.name = name
        self.group = group
        self.summary = summary
        self.sources = sources
        self.pairs: set[tuple[str, str]] = set()

    def add(self, reading: str, word: str) -> None:
        r = normalized_reading(reading)
        w = normalized_word(word)
        if r is None or w is None or w == r:
            return
        self.pairs.add((r, w))

    @property
    def license(self) -> str:
        licenses = sorted({s["license"] for s in self.sources})
        return " / ".join(licenses)


# MARK: - 取得


def http_get(url: str, timeout: int = 300) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read()


def sparql(query: str) -> list[dict]:
    for attempt in range(4):
        # 取り直すときは問い合わせ文に注記を足して、キャッシュされた（途中で打ち切られた）応答を使わせない
        text = query if attempt == 0 else f"{query}\n# retry {attempt} {time.time()}"
        url = WIKIDATA_SPARQL + "?" + urllib.parse.urlencode({"query": text, "format": "json"})
        try:
            # 打ち切られた応答は JSON の途中で終わるので、ここで読めずに例外になる
            data = json.loads(http_get(url))
            return data["results"]["bindings"]
        except Exception as error:  # noqa: BLE001  打ち切り・混雑はしばらく待って取り直す
            print(f"    取り直し（{attempt + 1}回目）: {error}", file=sys.stderr)
            time.sleep(10 * (attempt + 1))
    raise RuntimeError("Wikidata から取得できませんでした")


def wikidata_query(pattern: str, kana_filter: str | None) -> str:
    filters = 'FILTER(LANG(?label) = "ja")'
    if kana_filter is not None:
        filters += f" {kana_filter}"
    return (
        "SELECT ?item ?kana ?label WHERE { "
        f"{pattern} ?item wdt:P1814 ?kana ; rdfs:label ?label . {filters} }}"
    )


def fetch_jmdict() -> None:
    (SOURCES / "JMdict_e.gz").write_bytes(http_get(JMDICT_URL))


def fetch_ken_all() -> None:
    (SOURCES / "utf_ken_all.zip").write_bytes(http_get(KEN_ALL_URL))


def fetch_hyakuyaku() -> None:
    page = http_get(HYAKUYAKU_PAGE).decode("utf-8", "replace")
    match = re.search(r'data-downloadurl="([^"]+)"', page)
    if match is None:
        raise RuntimeError("百薬辞書のダウンロード先がページに見つかりません")
    (SOURCES / "hyakuyaku.zip").write_bytes(http_get(match.group(1).replace("&#038;", "&")))


def fetch_wikidata() -> None:
    for pack_id, name, pattern, partitioned, _ in WIKIDATA_GROUPS:
        print(f"    {name}")
        rows: list[dict] = []
        if partitioned:
            filters = [f'FILTER(STRSTARTS(?kana, "{c}"))' for c in KANA_INITIALS]
            filters.append('FILTER(!REGEX(?kana, "^[ぁ-ゖァ-ヺ]"))')  # かな以外で始まる読み
            for kana_filter in filters:
                rows += sparql(wikidata_query(pattern, kana_filter))
                time.sleep(1)
        else:
            rows = sparql(wikidata_query(pattern, None))
        simplified = [
            {"item": r["item"]["value"].rsplit("/", 1)[-1], "kana": r["kana"]["value"], "label": r["label"]["value"]}
            for r in rows
        ]
        (SOURCES / f"{pack_id}.json").write_text(json.dumps(simplified, ensure_ascii=False))
        print(f"      {len(simplified)} 件")


# (名前, 取得する関数, 取れなかったら止めるか)。JMdict はライセンスが新しい版への更新を求めるので、取れなければ止める。
# ほかは取れなければ前の版（models/term-dictionaries.json に載っているもの）をそのまま使う
FETCHERS = [
    ("jmdict", fetch_jmdict, True),
    ("japanpost", fetch_ken_all, False),
    ("hyakuyaku", fetch_hyakuyaku, False),
    ("wikidata", fetch_wikidata, False),
]


def fetch() -> None:
    SOURCES.mkdir(parents=True, exist_ok=True)
    fetched = []
    for name, function, required in FETCHERS:
        print(f"==> {name}")
        try:
            function()
            fetched.append(name)
        except Exception as error:  # noqa: BLE001
            if required:
                raise
            print(f"    取得できませんでした（前の版を使います）: {error}", file=sys.stderr)
    (SOURCES / "fetched.json").write_text(json.dumps({"date": date.today().isoformat(), "sources": fetched}))


# MARK: - 作成


def build_jmdict() -> list[Pack]:
    raw = gzip.open(SOURCES / "JMdict_e.gz", "rt", encoding="utf-8").read()
    created = re.search(r"<!-- JMdict created: (\d{4}-\d{2}-\d{2}) -->", raw)
    body = raw[raw.index("<JMdict>"):]
    # 分野の印などは DTD の entity で書かれている。展開すると英語の説明文になるので、名前のまま残す
    body = re.sub(
        r"&([A-Za-z0-9_-]+);",
        lambda m: m.group(0) if m.group(1) in ("amp", "lt", "gt", "quot", "apos") else m.group(1),
        body,
    )
    root = ET.fromstring(body)
    version = created.group(1) if created else "不明"
    packs = []
    field_to_pack: dict[str, Pack] = {}
    for pack_id, name, fields in JMDICT_GROUPS:
        pack = Pack(pack_id, name, "分野別の用語（JMdict）", f"JMdict の分野の印から作成（JMdict {version} 版）", [SOURCE_JMDICT])
        packs.append(pack)
        for field in fields:
            field_to_pack[field] = pack

    for entry in root.iter("entry"):
        targets = {
            field_to_pack[f.text]
            for sense in entry.iter("sense")
            for f in sense.iter("field")
            if f.text in field_to_pack
        }
        if not targets:
            continue
        kanji = [
            k.findtext("keb")
            for k in entry.findall("k_ele")
            if not {i.text for i in k.findall("ke_inf")} & JMDICT_SKIP_KANJI_INFO
        ]
        for r in entry.findall("r_ele"):
            if {i.text for i in r.findall("re_inf")} & JMDICT_SKIP_READING_INFO:
                continue
            reading = r.findtext("reb") or ""
            restricted = [x.text for x in r.findall("re_restr")]
            if entry.find("k_ele") is None or r.find("re_nokanji") is not None:
                words = [reading]  # かなだけの語（カタカナ語など。ひらがなだけなら add が捨てる）
            else:
                words = [k for k in kanji if not restricted or k in restricted]
            for pack in targets:
                for word in words:
                    # カタカナ語は「スタイラス・ペン」のように中黒で区切って書かれていることが多い。
                    # 区切らずに書く人も多いので、中黒を除いた形を先に加える
                    if "・" in word and re.fullmatch(r"[ァ-ヺー・]+", word):
                        pack.add(reading, word.replace("・", ""))
                    pack.add(reading, word)
    return packs


def build_hyakuyaku() -> Pack:
    pack = Pack("hyakuyaku", "薬の名前", "医療（百薬辞書）",
                "医療文書から集めた医薬品名と読み（2021年7月版。読みは自動で付けたもので誤りを含むことがある）",
                [SOURCE_HYAKUYAKU])
    with zipfile.ZipFile(SOURCES / "hyakuyaku.zip") as archive:
        name = next(n for n in archive.namelist() if n.endswith(".csv") and "__MACOSX" not in n)
        text = archive.read(name).decode("utf-8-sig")
    for row in csv.DictReader(io.StringIO(text)):
        surface = row["出現形"]
        # 用量・規格つきの名前（「フリバス錠50mg」「ミダゾラム注射液10mg「テバ」」）は、読み（ごじゅうみりぐらむ）で
        # 打つことがないので入れない。数字と括弧のない名前（「ノボラピッド注イノレット」）だけを使う
        if re.search(r"[0-9０-９「」（）()％%]", surface):
            continue
        # 英語名（「Epalrestat」）はかなの読みから出す語ではないので入れない。略語（「ＰＳＬ」）は残す
        if re.search(r"[a-zａ-ｚ]", surface) or re.search(r"\s|　", surface):
            continue
        pack.add(row["出現形よみ"], surface)
    return pack


def clean_town(kanji: str, kana: str) -> tuple[str, str] | None:
    """町域名から括弧書き（丁目・番地の範囲など）を除く。地名として使えなければ None"""
    kanji = re.sub(r"（.*", "", kanji)
    kana = re.sub(r"[（(].*", "", kana)
    if not kanji or kanji.endswith("一円") or "以下に掲載がない場合" in kanji or "番地" in kanji:
        return None
    if re.search(r"[0-9０-９、～〜「」]", kanji):
        return None
    return kanji, kana


def build_ken_all() -> Pack:
    pack = Pack("japanpost-places", "地名（市区町村・町域）", "地名・固有名詞",
                "日本郵便の郵便番号データの都道府県・市区町村・町域の名前", [SOURCE_KEN_ALL], "place")
    with zipfile.ZipFile(SOURCES / "utf_ken_all.zip") as archive:
        text = archive.read(archive.namelist()[0]).decode("utf-8")
    for row in csv.reader(io.StringIO(text)):
        pref_kana, city_kana, town_kana, pref, city, town = row[3:9]
        pack.add(pref_kana, pref)
        pack.add(city_kana, city)
        cleaned = clean_town(town, town_kana)
        if cleaned:
            pack.add(cleaned[1], cleaned[0])
    return pack


def build_wikidata() -> list[Pack]:
    packs = []
    for pack_id, name, _, _, kind in WIKIDATA_GROUPS:
        pack = Pack(pack_id, name, "地名・固有名詞", f"Wikidata でかなの読み（P1814）が付いている{name}の項目",
                    [SOURCE_WIKIDATA], kind)
        for row in json.loads((SOURCES / f"{pack_id}.json").read_text()):
            label = row["label"]
            # 曖昧さ回避の括弧書き（「山田太郎 (政治家)」）は語の一部ではない
            label = re.sub(r"\s*[（(][^）)]*[）)]$", "", label)
            pack.add(row["kana"], label)
            # 駅は「相生駅」の形で載っている。駅名だけ（「あいおい」→ 相生）でも出るように、「駅」を除いた形も加える
            kana = row["kana"].strip()
            if pack_id == "wikidata-stations" and label.endswith("駅") and kana.endswith(("えき", "エキ")):
                pack.add(kana[:-2], label[:-1])
        packs.append(pack)
    return packs


PREVIOUS_CATALOG = ROOT / "models" / "term-dictionaries.json"
RELEASE_URL = "https://github.com/TetsuakiBaba/iroha/releases/download/term-dictionaries-v1"
ALL_SOURCES = [SOURCE_JMDICT, SOURCE_HYAKUYAKU, SOURCE_KEN_ALL, SOURCE_WIKIDATA]


def content_sha256(pack: Pack) -> str:
    """語の一覧（注記の行を除く）の SHA-256。前の版と同じなら載せ直さない"""
    body = "\n".join(f"{r}\t{w}" for r, w in sorted(pack.pairs))
    return hashlib.sha256(body.encode("utf-8")).hexdigest()


def write_pack(pack: Pack, version: str) -> dict:
    DIST.mkdir(parents=True, exist_ok=True)
    # 同じ日に作り直しても公開中のファイルを上書きしないよう、語の一覧のハッシュの先頭をファイル名に入れる
    file_name = f"{pack.id}-{version}-{content_sha256(pack)[:8]}.tsv"
    lines = [f"# iroha 専門辞書: {pack.name}（{pack.id}、{version}）。{pack.summary}"]
    for source in pack.sources:
        lines.append(f"# 出典: {source['name']}（{source['holder']}、{source['url']}）")
        lines += [f"# {notice}" for notice in source["notices"]]
    lines += [f"# {line}" for line in COMMON_NOTICES]
    lines.append("# 形式: 読み<TAB>語")
    lines += [f"{r}\t{w}" for r, w in sorted(pack.pairs)]
    data = ("\n".join(lines) + "\n").encode("utf-8")
    (DIST / file_name).write_bytes(data)
    return {
        "id": pack.id,
        "name": pack.name,
        "group": pack.group,
        "summary": pack.summary,
        "entries": len(pack.pairs),
        "kind": pack.kind,
        "file": {
            "url": f"{RELEASE_URL}/{file_name}",
            "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
        },
        "license": pack.license,
        "sources": [{k: source[k] for k in ("name", "holder", "license", "url", "licenseURL")} for source in pack.sources],
        "version": version,
        "contentSha256": content_sha256(pack),
    }


def write_notice(entries: list[dict], version: str) -> None:
    """Release に一緒に載せる、出典とライセンスの一覧（ライセンス文書の場所と注記）"""
    lines = [
        f"iroha 専門辞書の出典とライセンス（{version} 更新）",
        "",
        "各辞書は元データのライセンスに従って配布する。辞書ファイルの先頭の「#」行にも同じ注記がある。",
        "",
    ]
    for source in ALL_SOURCES:
        files = [
            e["file"]["url"].rsplit("/", 1)[-1] for e in entries
            if any(s["name"] == source["name"] for s in e["sources"])
        ]
        if not files:
            continue
        lines.append(f"■ {source['name']}（{source['holder']}）")
        lines.append(f"  出典: {source['url']}")
        lines.append(f"  ライセンス文書: {source['licenseURL']}")
        lines += [f"  {notice}" for notice in source["notices"]]
        lines.append(f"  辞書: {', '.join(files)}")
        lines.append("")
    lines += COMMON_NOTICES
    (DIST / "NOTICE.txt").write_text("\n".join(lines) + "\n")


def build() -> None:
    fetched = json.loads((SOURCES / "fetched.json").read_text())
    version = fetched["date"].replace("-", "")
    available = set(fetched.get("sources", [name for name, _, _ in FETCHERS]))
    previous = {}
    if PREVIOUS_CATALOG.exists():
        previous = {e["id"]: e for e in json.loads(PREVIOUS_CATALOG.read_text())["dictionaries"]}

    # 元データごとに作る。取れなかった元データの辞書は前の版を使う
    groups: list[tuple[str, list[str], object]] = [
        ("jmdict", [g[0] for g in JMDICT_GROUPS], build_jmdict),
        ("hyakuyaku", ["hyakuyaku"], lambda: [build_hyakuyaku()]),
        ("japanpost", ["japanpost-places"], lambda: [build_ken_all()]),
        ("wikidata", [g[0] for g in WIKIDATA_GROUPS], build_wikidata),
    ]
    DIST.mkdir(parents=True, exist_ok=True)
    for old in DIST.glob("*.tsv"):
        old.unlink()
    entries = []
    changed = 0
    for source_name, ids, builder in groups:
        if source_name not in available:
            missing = [i for i in ids if i not in previous]
            if missing:
                raise RuntimeError(f"{source_name} を取得できず、前の版もありません: {missing}")
            for pack_id in ids:
                entries.append(previous[pack_id])
                print(f"{pack_id:34} 前の版のまま（元データを取得できなかった）")
            continue
        for pack in builder():
            old = previous.get(pack.id)
            if old is not None and old.get("contentSha256") == content_sha256(pack):
                entries.append(old)
                print(f"{pack.id:34} {old['entries']:>8,} 語  変更なし（{old['version']} 版のまま）")
                continue
            entry = write_pack(pack, version)
            entries.append(entry)
            changed += 1
            print(f"{pack.id:34} {entry['entries']:>8,} 語  {entry['file']['bytes'] / 1e6:6.2f} MB  {pack.name}（新しい版）")
    write_notice(entries, version)
    catalog = {"formatVersion": 1, "dictionaries": entries}
    (DIST / "catalog.json").write_text(json.dumps(catalog, ensure_ascii=False, indent=2) + "\n")
    print(f"==> 新しい版 {changed} 個。{DIST / 'catalog.json'}")


if __name__ == "__main__":
    command = sys.argv[1] if len(sys.argv) > 1 else ""
    if command == "fetch":
        fetch()
    elif command == "build":
        build()
    else:
        print(__doc__)
        sys.exit(1)
