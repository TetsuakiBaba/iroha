"""話し言葉のソース（typo normalizer 用。``config/typo-spoken.yaml``）。

対話構造は使わない。人の発話を 1 発話 = 1 段落として取り出し、1 対話（掲示板は 1 スレッドの
連鎖 1 本）を 1 文書にする。split は既存どおり文書（= 対話）単位で決まるので、同じ対話の発話が
train と test にまたがらない。段落の間で左文脈は繋がないので、前後の発話は文脈に入らない。

各ソースは ``dialogues()`` で「対話 ID と発話の列」を返すだけにし、清掃（``iroha.spoken.clean``）・
重複除去・対話単位の間引き（``dialogue_ratio``）・統計は ``SpokenAdapter`` がまとめて行う。
統計は preprocess の ``_stats.preprocess.<source>.json`` の ``extraction`` に入る。

元データはリポジトリに入れない。``download`` が一次配布元から、版（コミット）を固定して取る。
ライセンス・出典は各アダプタの ``info()`` と LICENSES.md の H 節。
"""
from __future__ import annotations

import hashlib
import io
import json
import re
import tarfile
import zipfile
from dataclasses import dataclass
from typing import Iterator

from iroha.download.http import download_stream_to, download_to
from iroha.sources.base import Document, SourceAdapter, SourceInfo, register
from iroha.spoken.clean import CleanStats, Open2chFilter, UtteranceCleaner, UtteranceDeduplicator

CC_BY_SA_4 = "CC BY-SA 4.0"
CC_BY_SA_4_URL = "https://creativecommons.org/licenses/by-sa/4.0/deed.ja"


@dataclass
class Utterance:
    text: str
    speaker: str = ""
    # アダプタが先に決めた除外理由（知識文の写し・コピペなど）。None なら清掃へ進む
    exclude: str | None = None


def _sampled(document_id: str, ratio: float) -> bool:
    """preprocess の document_ratio と同じ選び方（document_id のハッシュで決まる）。"""
    if ratio >= 1.0:
        return True
    h = int.from_bytes(hashlib.blake2b(f"sample:{document_id}".encode(), digest_size=8).digest(), "big")
    return h / 2**64 < ratio


def _codeload(repo: str, revision: str) -> str:
    return f"https://codeload.github.com/{repo}/tar.gz/{revision}"


class SpokenAdapter(SourceAdapter):
    """話し言葉のソースの共通部分。サブクラスは info / download / dialogues を書く。"""

    license: str = CC_BY_SA_4

    def dialogues(self) -> Iterator[tuple[str, list[Utterance]]]:
        raise NotImplementedError

    def raw_filter(self, stats: CleanStats):
        """清掃の前に生の発話を見るフィルタ（open2ch だけ）。"""
        return None

    def dialogue_ratio(self, dialogue_id: str) -> float:
        """この対話を残す割合（``dialogue_ratio``）。open2ch は板ごとに変える"""
        return float(self.source_cfg.get("dialogue_ratio", 1.0))

    def extraction_stats(self) -> dict | None:
        stats = getattr(self, "_stats", None)
        return stats.as_dict() if stats is not None else None

    def documents(self) -> Iterator[Document]:
        stats = CleanStats()
        self._stats = stats
        cleaner = UtteranceCleaner(self.source_cfg, stats)
        dedup = UtteranceDeduplicator(stats)
        raw_filter = self.raw_filter(stats)
        speakers = self.source_cfg.get("speakers", None)
        speakers = set(speakers) if speakers else None
        info = self.info()
        for dialogue_id, utterances in self.dialogues():
            document_id = f"{self.name}_{dialogue_id}"
            stats.dialogues += 1
            if not _sampled(document_id, self.dialogue_ratio(dialogue_id)):
                stats.dialogues_sampled_out += 1
                continue
            paragraphs: list[str] = []
            for u in utterances:
                stats.utterances_raw += 1
                if speakers is not None and u.speaker not in speakers:
                    stats.utterances_skipped_speaker += 1
                    continue
                if u.exclude:
                    stats.reject(u.exclude)
                    continue
                if raw_filter is not None and not raw_filter.accept(u.text):
                    continue
                text = cleaner.clean(u.text)
                if text is None or dedup.is_duplicate(text):
                    continue
                stats.utterances_kept += 1
                paragraphs.append(text)
            if paragraphs:
                yield Document(document_id=document_id, source=self.name, paragraphs=paragraphs,
                               license=self.license, url=info.homepage,
                               attribution=info.attribution, meta={})

    # ---- 共通の取得 ----
    def _tarball(self, repo: str, revision: str, force: bool) -> dict:
        dest = download_stream_to(_codeload(repo, revision),
                                  self.raw_dir / f"{repo.split('/')[1]}-{revision[:12]}.tar.gz", force=force)
        return {"file": str(dest), "bytes": dest.stat().st_size, "revision": revision}

    def _tar_members(self, repo: str, revision: str, pattern: str) -> Iterator[tuple[str, bytes]]:
        path = self.raw_dir / f"{repo.split('/')[1]}-{revision[:12]}.tar.gz"
        if not path.exists():
            raise FileNotFoundError(f"{path} が無い。先に `python -m iroha download --source {self.name}` を実行する")
        rx = re.compile(pattern)
        with tarfile.open(path, "r:gz") as tar:
            members = sorted((m for m in tar.getmembers() if m.isfile() and rx.search(m.name)),
                             key=lambda m: m.name)
            for m in members:
                fh = tar.extractfile(m)
                if fh is not None:
                    yield m.name, fh.read()


# ---------------------------------------------------------------- RealPersonaChat

RPC_REPO, RPC_REVISION = "nu-dialogue/real-persona-chat", "28d0b6b3865b29cabc26c230a2db37cdf315e937"


@register
class RealPersonaChatAdapter(SpokenAdapter):
    name = "realpersonachat"

    @classmethod
    def info(cls) -> SourceInfo:
        return SourceInfo(
            name=cls.name, title="RealPersonaChat（初対面の 2 者によるテキスト雑談、nu-dialogue）",
            homepage="https://github.com/nu-dialogue/real-persona-chat",
            license=CC_BY_SA_4, license_url=CC_BY_SA_4_URL,
            attribution=("出典: RealPersonaChat（nu-dialogue、https://github.com/nu-dialogue/real-persona-chat）、"
                         "CC BY-SA 4.0。発話の本文だけを取り出し加工して作成"),
            used_fields=["utterances[].text"],
            notes=["ペルソナ・性格特性・話者 ID は使わない（README の注意: 個人の特定・なりすまし・属性推定に使わない）",
                   f"版を固定: {RPC_REVISION}"])

    def download(self, *, force: bool = False) -> dict:
        return self._tarball(RPC_REPO, RPC_REVISION, force)

    def dialogues(self) -> Iterator[tuple[str, list[Utterance]]]:
        for name, data in self._tar_members(RPC_REPO, RPC_REVISION, r"/real_persona_chat/dialogues/[^/]+\.json$"):
            d = json.loads(data)
            yield str(d["dialogue_id"]), [Utterance(u.get("text") or "", u.get("interlocutor_id", ""))
                                          for u in d.get("utterances", [])]


# ---------------------------------------------------------------- MRMP

MRMP_REPO = "nu-dialogue/multi-relational-multi-party-chat-corpus"
MRMP_REVISION = "e6e39cb896df88781c7a2e0451226a527c5f1e2e"


@register
class MrmpAdapter(SpokenAdapter):
    name = "mrmp"

    @classmethod
    def info(cls) -> SourceInfo:
        return SourceInfo(
            name=cls.name,
            title="Multi-Relational Multi-Party Chat Corpus（3 者のテキスト雑談、初対面・家族を含む、nu-dialogue）",
            homepage=f"https://github.com/{MRMP_REPO}",
            license=CC_BY_SA_4, license_url=CC_BY_SA_4_URL,
            attribution=(f"出典: Multi-Relational Multi-Party Chat Corpus（nu-dialogue、https://github.com/{MRMP_REPO}）、"
                         "CC BY-SA 4.0。発話の本文だけを取り出し加工して作成"),
            used_fields=["utterances[].text"],
            notes=["話者名・関係・評価は使わない（README の注意は RealPersonaChat と同じ）",
                   f"版を固定: {MRMP_REVISION}"])

    def download(self, *, force: bool = False) -> dict:
        return self._tarball(MRMP_REPO, MRMP_REVISION, force)

    def dialogues(self) -> Iterator[tuple[str, list[Utterance]]]:
        for name, data in self._tar_members(MRMP_REPO, MRMP_REVISION, r"/dialogues/[^/]+/[^/]+\.json$"):
            d = json.loads(data)
            # 発話の頭などに「@てばさき」のように参加者の名前（日本語のニックネーム）で宛先が付く。
            # 名前の境目は文字からは分からないので、その対話の参加者名で取り除く
            names = sorted({str(i if isinstance(i, str) else (i.get("interlocutor_id") or i.get("name") or ""))
                            for i in d.get("interlocutors", [])}
                           | {str(u.get("interlocutor_id", "")) for u in d.get("utterances", [])}, key=len, reverse=True)
            rx = re.compile("[@＠](?:" + "|".join(re.escape(n) for n in names if n) + r")[、,，:：\s]*") if any(names) else None
            yield str(d["dialogue_id"]), [Utterance(rx.sub("", u.get("text") or "") if rx else (u.get("text") or ""),
                                                    u.get("interlocutor_id", ""))
                                          for u in d.get("utterances", [])]


# ---------------------------------------------------------------- JMRD

JMRD_REPO, JMRD_REVISION = "ku-nlp/JMRD", "a20b0a89b4cf6ce1db320039f6e27df10604bf4c"
JMRD_FILES = ("train.json", "valid.json", "test.json")


def _strings(obj) -> Iterator[str]:
    if isinstance(obj, str):
        yield obj
    elif isinstance(obj, dict):
        for v in obj.values():
            yield from _strings(v)
    elif isinstance(obj, list):
        for v in obj:
            yield from _strings(v)


@register
class JmrdAdapter(SpokenAdapter):
    name = "jmrd"

    @classmethod
    def info(cls) -> SourceInfo:
        return SourceInfo(
            name=cls.name, title="JMRD: Japanese Movie Recommendation Dialogue（映画推薦の対話、京都大学）",
            homepage=f"https://github.com/{JMRD_REPO}",
            license=CC_BY_SA_4, license_url=CC_BY_SA_4_URL,
            attribution=(f"出典: JMRD（京都大学 黒橋・褚・村脇研究室、https://github.com/{JMRD_REPO}）、"
                         "CC BY-SA 4.0。発話の本文だけを取り出し加工して作成"),
            used_fields=["dialog[].text", "knowledge / checked_knowledge（写しの判定だけ）"],
            notes=["推薦者の発話のうち、映画の知識文（あらすじ・レビューなど）を overlap_chars 字以上そのまま"
                   "含むものは捨てる（話し言葉ではなく、知識文の出典の文章になるため）",
                   f"版を固定: {JMRD_REVISION}"])

    def download(self, *, force: bool = False) -> dict:
        out = {}
        for f in JMRD_FILES:
            dest = download_to(f"https://raw.githubusercontent.com/{JMRD_REPO}/{JMRD_REVISION}/data/{f}",
                               self.raw_dir / f, force=force)
            out[f] = dest.stat().st_size
        return {"files": out, "revision": JMRD_REVISION}

    def dialogues(self) -> Iterator[tuple[str, list[Utterance]]]:
        n = int(self.source_cfg.get("overlap_chars", 15))
        for f in JMRD_FILES:
            path = self.raw_dir / f
            if not path.exists():
                raise FileNotFoundError(f"{path} が無い。先に `python -m iroha download --source jmrd` を実行する")
            for d in json.loads(path.read_text(encoding="utf-8")):
                grams = {s[i:i + n] for s in _strings(d.get("knowledge")) for i in range(len(s) - n + 1)}
                out = []
                for u in d.get("dialog", []):
                    text = u.get("text") or ""
                    exclude = None
                    if u.get("speaker") == "recommender":
                        checked = {s[i:i + n] for s in _strings(u.get("checked_knowledge"))
                                   if s != "[知識なし]" for i in range(len(s) - n + 1)}
                        if any(text[i:i + n] in grams or text[i:i + n] in checked
                               for i in range(len(text) - n + 1)):
                            exclude = "copied_knowledge"
                    out.append(Utterance(text, u.get("speaker", ""), exclude))
                yield str(d["dialog_id"]), out


# ---------------------------------------------------------------- 感想付きニュース雑談対話コーパス

NEWSCHAT_REPO, NEWSCHAT_REVISION = "fukanarita/newschat-with-impression", "fd09d8a7cad99449c5353447b785985fe8ac9a32"


@register
class NewsChatAdapter(SpokenAdapter):
    name = "newschat"
    license = "MIT"

    @classmethod
    def info(cls) -> SourceInfo:
        return SourceInfo(
            name=cls.name, title="感想付きニュース雑談対話コーパス（Wizard of Oz 法の雑談）",
            homepage=f"https://github.com/{NEWSCHAT_REPO}",
            license="MIT", license_url=f"https://github.com/{NEWSCHAT_REPO}/blob/{NEWSCHAT_REVISION}/LICENSE",
            attribution=(f"出典: 感想付きニュース雑談対話コーパス（Copyright (c) 2023 Fuka Narita、"
                         f"https://github.com/{NEWSCHAT_REPO}）、MIT License。発話の本文だけを取り出し加工して作成"),
            used_fields=["dialog[].utterance（speaker = U のユーザ役だけ）"],
            notes=["システム役（S）の発話は使わない: ツイート本文やニュース記事の引用が入る（used_tweet・記事の要約）",
                   "tweet_choices（ツイート ID）と news_url は使わない",
                   f"版を固定: {NEWSCHAT_REVISION}"])

    def download(self, *, force: bool = False) -> dict:
        dest = download_to(f"https://raw.githubusercontent.com/{NEWSCHAT_REPO}/{NEWSCHAT_REVISION}/all.jsonl",
                           self.raw_dir / "all.jsonl", force=force)
        return {"file": str(dest), "bytes": dest.stat().st_size, "revision": NEWSCHAT_REVISION}

    def dialogues(self) -> Iterator[tuple[str, list[Utterance]]]:
        path = self.raw_dir / "all.jsonl"
        if not path.exists():
            raise FileNotFoundError(f"{path} が無い。先に `python -m iroha download --source newschat` を実行する")
        with open(path, encoding="utf-8") as fh:
            for line in fh:
                if not line.strip():
                    continue
                d = json.loads(line)
                yield str(d["dialog_id"]), [Utterance(u.get("utterance") or "", u.get("speaker", ""))
                                            for u in d.get("dialog", [])]


# ---------------------------------------------------------------- J-CRe3

JCRE3_REPO, JCRE3_REVISION = "riken-grp/J-CRe3", "61814236abf16258c7fb9778e32e660c1602ecfa"


def knp_sentences(text: str) -> Iterator[str]:
    """KNP 形式から文の表層を取り出す（形態素行の 1 列目をつなぐ）。"""
    words: list[str] = []
    for line in text.splitlines():
        if line == "EOS":
            if words:
                yield "".join(words)
            words = []
        elif line and not line.startswith(("#", "*", "+")):
            words.append(line.split(" ", 1)[0])
    if words:
        yield "".join(words)


@register
class JCre3Adapter(SpokenAdapter):
    name = "jcre3"

    @classmethod
    def info(cls) -> SourceInfo:
        return SourceInfo(
            name=cls.name, title="J-CRe3（実世界の日本語会話の書き起こし、理化学研究所）",
            homepage=f"https://github.com/{JCRE3_REPO}",
            license=CC_BY_SA_4, license_url=CC_BY_SA_4_URL,
            attribution=(f"出典: J-CRe3（理化学研究所 ほか、https://github.com/{JCRE3_REPO}）、CC BY-SA 4.0。"
                         "書き起こし（textual_annotations の KNP）の表層だけを取り出し加工して作成"),
            used_fields=["textual_annotations/*.knp の形態素の表層"],
            notes=["KNP の 1 文を 1 発話として扱う（発話と文の対応 info.json は動画と同じ Box にあり取得しない）",
                   "話者（主人・ロボット役）はどちらも人が演じた発話なので両方使う",
                   f"版を固定: {JCRE3_REVISION}"])

    def download(self, *, force: bool = False) -> dict:
        return self._tarball(JCRE3_REPO, JCRE3_REVISION, force)

    def dialogues(self) -> Iterator[tuple[str, list[Utterance]]]:
        for name, data in self._tar_members(JCRE3_REPO, JCRE3_REVISION, r"/textual_annotations/[^/]+\.knp$"):
            scenario = name.rsplit("/", 1)[1][:-4]
            yield scenario, [Utterance(s) for s in knp_sentences(data.decode("utf-8"))]


# ---------------------------------------------------------------- open2ch

OPEN2CH_REPO, OPEN2CH_REVISION = "1never/open2ch-dialogue-corpus", "a8ccdf2cffa81ea5f9ff5ed01c35d0272f0936f3"
# corpus.zip は Git LFS。ポインタに書かれた実体の SHA-256 と大きさ
OPEN2CH_ZIP_SHA256 = "74cc746f6a2b49c9ab1ba4e5decdda99346a050fdb7309a60c7a78e1ad151c56"
OPEN2CH_ZIP_BYTES = 534070324
OPEN2CH_BOARDS = ("livejupiter", "news4vip", "newsplus")


@register
class Open2chAdapter(SpokenAdapter):
    name = "open2ch"
    license = "Apache-2.0"

    @classmethod
    def info(cls) -> SourceInfo:
        return SourceInfo(
            name=cls.name, title="おーぷん2ちゃんねる対話コーパス（掲示板の投稿の連鎖、稲葉通将）",
            homepage=f"https://github.com/{OPEN2CH_REPO}",
            license="Apache-2.0", license_url=f"https://github.com/{OPEN2CH_REPO}/blob/{OPEN2CH_REVISION}/LICENSE",
            attribution=(f"出典: おーぷん2ちゃんねる対話コーパス（稲葉通将、https://github.com/{OPEN2CH_REPO}）、"
                         "リポジトリのライセンスは Apache License 2.0。投稿の本文を選別・加工して作成"),
            used_fields=["corpus.zip の各行（タブ区切りの投稿）", "data/ng_words.txt（不適切語の除外）"],
            notes=["**掲示板をクロールしたデータ**。Apache-2.0 はリポジトリに付いたもので、投稿の本文の権利が"
                   "整理されているとは README に書かれていない（LICENSES.md の H 節）",
                   "全件は使わない: ネットスラング・笑いの w/草・AA・半角カナ・アンカー・不適切語・コピペを除き、"
                   "dialogue_ratio で 100 万〜300 万発話に間引く",
                   f"版を固定: {OPEN2CH_REVISION}（corpus.zip は LFS、SHA-256 を照合する）"])

    def download(self, *, force: bool = False) -> dict:
        base = f"https://media.githubusercontent.com/media/{OPEN2CH_REPO}/{OPEN2CH_REVISION}"
        dest = download_stream_to(f"{base}/corpus.zip", self.raw_dir / "corpus.zip", force=force)
        digest = hashlib.sha256()
        with open(dest, "rb") as fh:
            for block in iter(lambda: fh.read(1 << 20), b""):
                digest.update(block)
        if digest.hexdigest() != OPEN2CH_ZIP_SHA256 or dest.stat().st_size != OPEN2CH_ZIP_BYTES:
            dest.unlink()
            raise RuntimeError(f"corpus.zip の SHA-256 が LFS ポインタと違う（取り直す）: {digest.hexdigest()}")
        ng = download_to(f"https://raw.githubusercontent.com/{OPEN2CH_REPO}/{OPEN2CH_REVISION}/data/ng_words.txt",
                         self.raw_dir / "ng_words.txt", force=force)
        return {"file": str(dest), "bytes": dest.stat().st_size, "sha256": OPEN2CH_ZIP_SHA256,
                "ng_words": str(ng), "revision": OPEN2CH_REVISION}

    def _ng_words(self) -> list[str]:
        path = self.raw_dir / "ng_words.txt"
        return [l.strip() for l in path.read_text(encoding="utf-8").splitlines()] if path.exists() else []

    def raw_filter(self, stats: CleanStats):
        return Open2chFilter(self.source_cfg, stats, self._ng_words())

    def dialogue_ratio(self, dialogue_id: str) -> float:
        board = dialogue_id.rsplit("_", 1)[0]
        ratios = self.source_cfg.get("board_ratio", None) or {}
        return float(ratios.get(board, self.source_cfg.get("dialogue_ratio", 1.0)))

    def _lines(self) -> Iterator[tuple[str, int, list[str]]]:
        path = self.raw_dir / "corpus.zip"
        if not path.exists():
            raise FileNotFoundError(f"{path} が無い。先に `python -m iroha download --source open2ch` を実行する")
        boards = list(self.source_cfg.get("boards", list(OPEN2CH_BOARDS)) or OPEN2CH_BOARDS)
        with zipfile.ZipFile(path) as zf:
            names = {n.rsplit("/", 1)[-1][:-4]: n for n in zf.namelist() if n.endswith(".tsv")}
            for board in boards:
                if board not in names:
                    raise KeyError(f"corpus.zip に {board}.tsv が無い（あるのは {sorted(names)}）")
                with zf.open(names[board]) as raw:
                    for i, line in enumerate(io.TextIOWrapper(raw, encoding="utf-8", errors="replace")):
                        posts = [p.replace("__BR__", "\n") for p in line.rstrip("\n").split("\t") if p.strip()]
                        if posts:
                            yield board, i, posts

    def _copypaste(self) -> set[int]:
        """同じ長い投稿が何度も出るもの（コピペ・定型文）のキーの集合。1 周目で数える。"""
        import numpy as np
        from iroha.spoken.clean import _h64, near_key
        min_chars = int(self.source_cfg.get("copypaste_min_chars", 20))
        min_count = int(self.source_cfg.get("copypaste_min_count", 3))
        from array import array
        hashes = array("Q")   # 1,800 万投稿でも 8 バイトずつ
        for _, _, posts in self._lines():
            hashes.extend(_h64(near_key(p)) for p in posts if len(p) >= min_chars)
        if not hashes:
            return set()
        values, counts = np.unique(np.frombuffer(hashes, dtype=np.uint64), return_counts=True)
        return {int(v) for v in values[counts >= min_count]}

    def dialogues(self) -> Iterator[tuple[str, list[Utterance]]]:
        from iroha.spoken.clean import _h64, near_key
        copies = self._copypaste()
        min_chars = int(self.source_cfg.get("copypaste_min_chars", 20))
        for board, i, posts in self._lines():
            yield f"{board}_{i:08d}", [
                Utterance(p, exclude="copypaste" if len(p) >= min_chars and _h64(near_key(p)) in copies else None)
                for p in posts]
