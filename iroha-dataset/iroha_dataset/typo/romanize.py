"""ローマ字 ⇄ かな。かな→打鍵列（``romanize``）と 打鍵列→かな（``RomajiComposer``）。

打鍵列→かな は ``macos/Sources/IrohaCore/RomajiComposer.swift`` の移植で、テーブルも
アルゴリズム（pending / prefixes / forceResolveHead）もそのまま写している。
``training/typo-normalizer/typo_generator/romaji.py`` と同じ実装
（あちらは ``test_parity.py`` で Swift 版との一致を確認済み）。

typo は**かな文字列ではなく打鍵列の上**で起こす。その打鍵列が実際の iroha で何に
なるかはこの移植で決まるので、Swift 側のテーブルを変えたらここも合わせること。
"""
from __future__ import annotations

# --- 打鍵列 → かな（RomajiComposer.swift の defaultTable をそのまま） ---
TABLE: dict[str, str] = {
    "a": "あ", "i": "い", "u": "う", "e": "え", "o": "お",
    "ka": "か", "ki": "き", "ku": "く", "ke": "け", "ko": "こ",
    "kya": "きゃ", "kyi": "きぃ", "kyu": "きゅ", "kye": "きぇ", "kyo": "きょ",
    "ga": "が", "gi": "ぎ", "gu": "ぐ", "ge": "げ", "go": "ご",
    "gya": "ぎゃ", "gyu": "ぎゅ", "gyo": "ぎょ",
    "sa": "さ", "si": "し", "su": "す", "se": "せ", "so": "そ",
    "sha": "しゃ", "shi": "し", "shu": "しゅ", "she": "しぇ", "sho": "しょ",
    "sya": "しゃ", "syi": "しぃ", "syu": "しゅ", "sye": "しぇ", "syo": "しょ",
    "za": "ざ", "zi": "じ", "zu": "ず", "ze": "ぜ", "zo": "ぞ",
    "ja": "じゃ", "ji": "じ", "ju": "じゅ", "je": "じぇ", "jo": "じょ",
    "jya": "じゃ", "jyu": "じゅ", "jyo": "じょ",
    "zya": "じゃ", "zyi": "じぃ", "zyu": "じゅ", "zye": "じぇ", "zyo": "じょ",
    "ta": "た", "ti": "ち", "tu": "つ", "te": "て", "to": "と",
    "cha": "ちゃ", "chi": "ち", "chu": "ちゅ", "che": "ちぇ", "cho": "ちょ",
    "tya": "ちゃ", "tyi": "ちぃ", "tyu": "ちゅ", "tye": "ちぇ", "tyo": "ちょ",
    "tsu": "つ", "tsa": "つぁ", "tsi": "つぃ", "tse": "つぇ", "tso": "つぉ",
    "tha": "てゃ", "thi": "てぃ", "thu": "てゅ", "the": "てぇ", "tho": "てょ",
    "twu": "とぅ",
    "da": "だ", "di": "ぢ", "du": "づ", "de": "で", "do": "ど",
    "dya": "ぢゃ", "dyu": "ぢゅ", "dyo": "ぢょ",
    "dha": "でゃ", "dhi": "でぃ", "dhu": "でゅ", "dhe": "でぇ", "dho": "でょ",
    "dwu": "どぅ",
    "na": "な", "ni": "に", "nu": "ぬ", "ne": "ね", "no": "の",
    "nya": "にゃ", "nyi": "にぃ", "nyu": "にゅ", "nye": "にぇ", "nyo": "にょ",
    "nn": "ん", "n'": "ん",
    "ha": "は", "hi": "ひ", "hu": "ふ", "he": "へ", "ho": "ほ",
    "hya": "ひゃ", "hyu": "ひゅ", "hyo": "ひょ",
    "fa": "ふぁ", "fi": "ふぃ", "fu": "ふ", "fe": "ふぇ", "fo": "ふぉ",
    "fya": "ふゃ", "fyu": "ふゅ", "fyo": "ふょ",
    "ba": "ば", "bi": "び", "bu": "ぶ", "be": "べ", "bo": "ぼ",
    "bya": "びゃ", "byu": "びゅ", "byo": "びょ",
    "pa": "ぱ", "pi": "ぴ", "pu": "ぷ", "pe": "ぺ", "po": "ぽ",
    "pya": "ぴゃ", "pyu": "ぴゅ", "pyo": "ぴょ",
    "ma": "ま", "mi": "み", "mu": "む", "me": "め", "mo": "も",
    "mya": "みゃ", "myu": "みゅ", "myo": "みょ",
    "ya": "や", "yu": "ゆ", "yo": "よ", "ye": "いぇ",
    "ra": "ら", "ri": "り", "ru": "る", "re": "れ", "ro": "ろ",
    "rya": "りゃ", "ryu": "りゅ", "ryo": "りょ",
    "wa": "わ", "wi": "うぃ", "wu": "う", "we": "うぇ", "wo": "を",
    "wha": "うぁ", "whi": "うぃ", "whe": "うぇ", "who": "うぉ",
    "va": "ゔぁ", "vi": "ゔぃ", "vu": "ゔ", "ve": "ゔぇ", "vo": "ゔぉ",
    "ca": "か", "ci": "し", "cu": "く", "ce": "せ", "co": "こ",
    "qa": "くぁ", "qi": "くぃ", "qu": "く", "qe": "くぇ", "qo": "くぉ",
    "la": "ぁ", "li": "ぃ", "lu": "ぅ", "le": "ぇ", "lo": "ぉ",
    "xa": "ぁ", "xi": "ぃ", "xu": "ぅ", "xe": "ぇ", "xo": "ぉ",
    "ltu": "っ", "xtu": "っ", "ltsu": "っ",
    "lya": "ゃ", "lyu": "ゅ", "lyo": "ょ",
    "xya": "ゃ", "xyu": "ゅ", "xyo": "ょ",
    "lwa": "ゎ", "xwa": "ゎ",
    "xka": "ヵ", "xke": "ヶ",
    "-": "ー", ",": "、", ".": "。", "/": "・",
    "[": "「", "]": "」", "!": "！", "?": "？", "~": "〜",
}

SOKUON_CONSONANTS = set("bcdfghjklmpqrstvwxyz")

PREFIXES: set[str] = set()
for _key in TABLE:
    _p = _key
    while len(_p) > 1:
        _p = _p[:-1]
        PREFIXES.add(_p)


class RomajiComposer:
    """RomajiComposer.swift の移植（入力 → text + pending、最後に flush）"""

    def __init__(self) -> None:
        self.text = ""
        self.pending = ""

    def input(self, s: str) -> None:
        for ch in s:
            self.pending += ch.lower()
            self._resolve()

    def _resolve(self) -> None:
        while self.pending:
            can_extend = self.pending in PREFIXES
            exact = TABLE.get(self.pending)
            if exact is not None and not can_extend:
                self.text += exact
                self.pending = ""
                continue
            if can_extend:
                return
            self._force_resolve_head()

    def _force_resolve_head(self) -> None:
        for length in range(len(self.pending) - 1, 0, -1):
            prefix = self.pending[:length]
            value = TABLE.get(prefix)
            if value is not None:
                self.text += value
                self.pending = self.pending[length:]
                return
        if len(self.pending) >= 2:
            c0, c1 = self.pending[0], self.pending[1]
            if (c0 == c1 and c0 in SOKUON_CONSONANTS) or (c0 == "t" and c1 == "c"):
                self.text += "っ"
                self.pending = self.pending[1:]
                return
        if self.pending[0] == "n":
            self.text += "ん"
            self.pending = self.pending[1:]
            return
        self.text += self.pending[0]
        self.pending = self.pending[1:]

    def flush(self) -> None:
        while self.pending:
            value = TABLE.get(self.pending)
            if value is not None:
                self.text += value
                self.pending = ""
            else:
                before = self.pending
                self._force_resolve_head()
                if self.pending == before:
                    self.text += self.pending[0]
                    self.pending = self.pending[1:]


def to_kana(keys: str) -> str:
    """打鍵列 → かな（確定まで）"""
    c = RomajiComposer()
    c.input(keys)
    c.flush()
    return c.text


# --- かな → 打鍵列 ---
# 既定の綴り（ヘボン式寄り: shi / tsu / chi / fu / ji）。
# TABLE に複数の綴りがある音は、ここで選んだ 1 つを「正しい打鍵」とする。
KANA_TO_KEYS: dict[str, str] = {
    "あ": "a", "い": "i", "う": "u", "え": "e", "お": "o",
    "か": "ka", "き": "ki", "く": "ku", "け": "ke", "こ": "ko",
    "が": "ga", "ぎ": "gi", "ぐ": "gu", "げ": "ge", "ご": "go",
    "さ": "sa", "し": "shi", "す": "su", "せ": "se", "そ": "so",
    "ざ": "za", "じ": "ji", "ず": "zu", "ぜ": "ze", "ぞ": "zo",
    "た": "ta", "ち": "chi", "つ": "tsu", "て": "te", "と": "to",
    "だ": "da", "ぢ": "di", "づ": "du", "で": "de", "ど": "do",
    "な": "na", "に": "ni", "ぬ": "nu", "ね": "ne", "の": "no",
    "は": "ha", "ひ": "hi", "ふ": "fu", "へ": "he", "ほ": "ho",
    "ば": "ba", "び": "bi", "ぶ": "bu", "べ": "be", "ぼ": "bo",
    "ぱ": "pa", "ぴ": "pi", "ぷ": "pu", "ぺ": "pe", "ぽ": "po",
    "ま": "ma", "み": "mi", "む": "mu", "め": "me", "も": "mo",
    "や": "ya", "ゆ": "yu", "よ": "yo",
    "ら": "ra", "り": "ri", "る": "ru", "れ": "re", "ろ": "ro",
    "わ": "wa", "を": "wo", "ゔ": "vu",
    "ぁ": "xa", "ぃ": "xi", "ぅ": "xu", "ぇ": "xe", "ぉ": "xo",
    "ゃ": "xya", "ゅ": "xyu", "ょ": "xyo", "ゎ": "xwa", "っ": "xtu",
    "ー": "-", "、": ",", "。": ".", "・": "/",
    # TABLE 側に打鍵がある記号（読みに現れうるものだけ）
    "「": "[", "」": "]", "〜": "~", "！": "!", "？": "?",
    # 拗音・外来音（2 文字）
    "きゃ": "kya", "きぃ": "kyi", "きゅ": "kyu", "きぇ": "kye", "きょ": "kyo",
    "ぎゃ": "gya", "ぎゅ": "gyu", "ぎょ": "gyo",
    "しゃ": "sha", "しぃ": "syi", "しゅ": "shu", "しぇ": "she", "しょ": "sho",
    "じゃ": "ja", "じぃ": "zyi", "じゅ": "ju", "じぇ": "je", "じょ": "jo",
    "ちゃ": "cha", "ちぃ": "tyi", "ちゅ": "chu", "ちぇ": "che", "ちょ": "cho",
    "ぢゃ": "dya", "ぢゅ": "dyu", "ぢょ": "dyo",
    "にゃ": "nya", "にぃ": "nyi", "にゅ": "nyu", "にぇ": "nye", "にょ": "nyo",
    "ひゃ": "hya", "ひゅ": "hyu", "ひょ": "hyo",
    "びゃ": "bya", "びゅ": "byu", "びょ": "byo",
    "ぴゃ": "pya", "ぴゅ": "pyu", "ぴょ": "pyo",
    "みゃ": "mya", "みゅ": "myu", "みょ": "myo",
    "りゃ": "rya", "りゅ": "ryu", "りょ": "ryo",
    "ふぁ": "fa", "ふぃ": "fi", "ふぇ": "fe", "ふぉ": "fo",
    "ふゃ": "fya", "ふゅ": "fyu", "ふょ": "fyo",
    "てゃ": "tha", "てぃ": "thi", "てゅ": "thu", "てぇ": "the", "てょ": "tho",
    "でゃ": "dha", "でぃ": "dhi", "でゅ": "dhu", "でぇ": "dhe", "でょ": "dho",
    "とぅ": "twu", "どぅ": "dwu",
    "つぁ": "tsa", "つぃ": "tsi", "つぇ": "tse", "つぉ": "tso",
    "うぁ": "wha", "うぃ": "wi", "うぇ": "we", "うぉ": "who",
    "ゔぁ": "va", "ゔぃ": "vi", "ゔぇ": "ve", "ゔぉ": "vo",
    "いぇ": "ye", "くぁ": "qa", "くぃ": "qi", "くぇ": "qe", "くぉ": "qo",
}

VOWELS_AND_Y = set("aiueoy")


class RomanizeError(ValueError):
    pass


def split_units(kana: str) -> list[str]:
    """かな列を打鍵単位に切る（2 文字の拗音を優先）"""
    units: list[str] = []
    i = 0
    while i < len(kana):
        if i + 1 < len(kana) and kana[i:i + 2] in KANA_TO_KEYS:
            units.append(kana[i:i + 2])
            i += 2
        else:
            units.append(kana[i])
            i += 1
    return units


def romanize(kana: str, n_style: str = "nn") -> str:
    """かな → 打鍵列。

    n_style: "nn"        … 「ん」は常に nn
             "contextual" … 子音の前だけ n 1 つ（実際の打ち方の個人差を再現する）
    未知のかなが含まれていれば RomanizeError。
    """
    units = split_units(kana)
    keys: list[str] = []
    for i, u in enumerate(units):
        nxt = units[i + 1] if i + 1 < len(units) else None
        if u == "っ":
            nxt_keys = KANA_TO_KEYS.get(nxt or "", "")
            if nxt_keys and nxt_keys[0] in SOKUON_CONSONANTS and nxt != "ん":
                keys.append(nxt_keys[0])  # 子音反復
            else:
                keys.append("xtu")
            continue
        if u == "ん":
            if n_style == "contextual" and nxt is not None:
                nxt_keys = KANA_TO_KEYS.get(nxt, "")
                if nxt_keys and nxt_keys[0] not in VOWELS_AND_Y and nxt_keys[0] != "n":
                    keys.append("n")
                    continue
            keys.append("nn")
            continue
        k = KANA_TO_KEYS.get(u)
        if k is None:
            raise RomanizeError(f"打鍵列にできないかな: {u!r} in {kana!r}")
        keys.append(k)
    return "".join(keys)


def romanize_units(kana: str, n_style: str = "nn") -> tuple[str, list[str], list[str]]:
    """かな → (打鍵列, 打鍵単位のリスト, 単位ごとの打鍵列)。

    ``"".join(unit_keys) == keys`` かつ ``"".join(units) == kana`` が常に成り立つ。
    「っ」と「ん」の打鍵は次の単位で変わるので、単位ごとに切るにはここで一度に作る
    必要がある（mixed_input が単位の境界で切るために使う）。
    """
    units = split_units(kana)
    unit_keys: list[str] = []
    for i, u in enumerate(units):
        nxt = units[i + 1] if i + 1 < len(units) else None
        if u == "っ":
            nxt_keys = KANA_TO_KEYS.get(nxt or "", "")
            if nxt_keys and nxt_keys[0] in SOKUON_CONSONANTS and nxt != "ん":
                unit_keys.append(nxt_keys[0])
            else:
                unit_keys.append("xtu")
            continue
        if u == "ん":
            if n_style == "contextual" and nxt is not None:
                nxt_keys = KANA_TO_KEYS.get(nxt, "")
                if nxt_keys and nxt_keys[0] not in VOWELS_AND_Y and nxt_keys[0] != "n":
                    unit_keys.append("n")
                    continue
            unit_keys.append("nn")
            continue
        k = KANA_TO_KEYS.get(u)
        if k is None:
            raise RomanizeError(f"打鍵列にできないかな: {u!r} in {kana!r}")
        unit_keys.append(k)
    return "".join(unit_keys), units, unit_keys


def roundtrip_ok(kana: str, n_style: str = "nn") -> bool:
    try:
        return to_kana(romanize(kana, n_style)) == kana
    except RomanizeError:
        return False


# かな→打鍵列にできる文字の集合（読みのフィルタに使う）
ROMANIZABLE = frozenset(KANA_TO_KEYS) | {"ん", "っ"}


def is_romanizable(kana: str) -> bool:
    """この読みが打鍵列にできるか（できない読みからは typo を作らない）。"""
    try:
        romanize(kana)
    except RomanizeError:
        return False
    return True
