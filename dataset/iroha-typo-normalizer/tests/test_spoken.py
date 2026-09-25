"""話し言葉のソース（config/typo-spoken.yaml）: 発話の清掃・open2ch のフィルタ・各ソースの読み取り・統計。"""
import io
import json
import tarfile
import zipfile

from iroha.config import load_config
from iroha.paths import Paths
from iroha.preprocess.pipeline import CanonicalBuilder
from iroha.sources import get_adapter
from iroha.sources.spoken import (JCRE3_REPO, JCRE3_REVISION, MRMP_REPO, MRMP_REVISION, RPC_REPO,
                                  RPC_REVISION, knp_sentences)
from iroha.spoken.clean import CleanStats, Open2chFilter, UtteranceCleaner, UtteranceDeduplicator, near_key
from iroha.spoken.report import collect, render

from tests.conftest import needs_sudachi


def _cleaner(**cfg):
    stats = CleanStats()
    return UtteranceCleaner(cfg, stats), stats


def _adapter(tmp_path, name, overrides=()):
    cfg = load_config("config/typo-spoken.yaml",
                      overrides=[f"data_dir={tmp_path / 'out'}", f"raw_dir={tmp_path / 'raw'}", *overrides])
    paths = Paths(tmp_path / "out", tmp_path / "raw").ensure()
    paths.raw_for(name).mkdir(parents=True, exist_ok=True)
    return get_adapter(name, cfg, paths)


def _tarball(path, repo, revision, files: dict[str, bytes]):
    """codeload の tar.gz と同じく <repo 名>-<revision>/ の下に置く。"""
    top = f"{repo.split('/')[1]}-{revision}"
    with tarfile.open(path, "w:gz") as tar:
        for name, data in files.items():
            info = tarfile.TarInfo(f"{top}/{name}")
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))


def _texts(adapter):
    return [p for d in adapter.documents() for p in d.paragraphs]


# ---------------------------------------------------------------- 清掃

def test_cleaner_strips_emoji_kaomoji_decoration_and_laugh():
    c, st = _cleaner()
    assert c.clean("今日は楽しかった😊♪(^^)") == "今日は楽しかった"
    assert c.clean("それはすごいね(笑)") == "それはすごいね"
    assert c.clean("マジでウケるwww") == "マジでウケる"
    assert st.normalized["emoji"] == 1 and st.normalized["kaomoji"] == 1
    assert st.normalized["laugh_tail"] == 2


def test_cleaner_collapses_repeats_spaces_and_newlines():
    c, _ = _cleaner()
    assert c.clean("ほんとに！！！すごい？？") == "ほんとに！すごい？"
    assert c.clean("えーーーーっと、あのーーー") == "えーーっと、あのーー"
    assert c.clean("そうそう それそれ") == "そうそう、それそれ"
    assert c.clean("ですよね。 私もです") == "ですよね。私もです"
    assert c.clean("若かったですよね。、今は無理かも") == "若かったですよね。今は無理かも"
    assert c.clean("おはよう\nげんき？") == "おはよう。げんき？"
    assert c.clean("すごおおおおおい") == "すごおおおい"


def test_cleaner_rejects_url_mention_and_noise():
    c, st = _cleaner(min_chars=4, max_chars=20)
    assert c.clean("これ見て https://example.com/a") is None
    assert c.clean("@taro それな") is None
    assert c.clean("＠はなこ そうだね") is None
    assert c.clean("OK OK OK") is None
    assert c.clean("うん") is None
    assert c.clean("これはとても長い発話です" * 3) is None
    assert c.clean("abc defg hijk です") is None
    assert st.rejected == {"url": 1, "mention": 2, "no_japanese": 1, "too_short": 1, "too_long": 1,
                           "low_japanese_ratio": 1}


def test_dedup_drops_exact_and_near_but_keeps_different_endings():
    st = CleanStats()
    d = UtteranceDeduplicator(st)
    assert not d.is_duplicate("そうだね")
    assert d.is_duplicate("そうだね")
    assert d.is_duplicate("そうだね！")          # 記号だけの違い
    assert d.is_duplicate("そうだねー")          # 長音だけの違い（「そーだね」は打ち方が違うので別）
    assert not d.is_duplicate("そうだよ")        # 終助詞の違いは別の入力
    assert st.rejected == {"duplicate_exact": 1, "duplicate_near": 2}
    assert near_key("すごーい！！") == near_key("すごい")


# ---------------------------------------------------------------- open2ch

def _o2_filter(**cfg):
    base = load_config("config/typo-spoken.yaml").sub("sources.open2ch")
    merged = {**{k: base.get(k) for k in ("slang", "max_aa_ratio")}, **cfg}
    st = CleanStats()
    return Open2chFilter(merged, st, ["ngword"]), st


def test_open2ch_filter_rejects_board_style_posts():
    f, st = _o2_filter()
    rejected = {
        "それはないわwww": "net_laugh",
        "これは草": "net_laugh",
        "大草原不可避": "net_laugh",
        "ワイは今日も仕事や": "net_slang",
        "負けたンゴ": "net_slang",
        "イッチはどこいったんや": "net_slang",
        "ｷﾀｰ": "halfwidth_kana",
        "(´・ω・｀)ｼｮﾎﾞｰﾝ": "halfwidth_kana",
        "┌(┌^o^)┐ホモォ…": "ascii_art",
        ">>12 それはちゃうで": "anchor",
        "スレ立て乙": "net_slang",
        "それってNGWORDじゃん": "ng_word",
    }
    for text, reason in rejected.items():
        assert f.reject_reason(text) == reason, text


def test_open2ch_filter_keeps_ordinary_words_that_contain_slang():
    f, _ = _o2_filter()
    for text in ("リンゴとマンゴーを買った", "スイッチを入れてください", "ワインが好きです",
                 "草原を走るのが好き", "雑草を抜いた", "ノートを忘れた", "明日は雨だと思う"):
        assert f.reject_reason(text) is None, text


def test_open2ch_reads_zip_and_drops_copypaste(tmp_path):
    a = _adapter(tmp_path, "open2ch", ["sources.open2ch.copypaste_min_count=2",
                                       "sources.open2ch.copypaste_min_chars=10",
                                       "sources.open2ch.board_ratio={}"])
    copy = "これはよくあるコピペの文章でございますので皆さん気をつけてください"
    rows = {"livejupiter": [f"今日はいい天気だね\tそうだね__BR__散歩でもするか\t{copy}",
                            f"{copy}\tなんでやねん"],
            "news4vip": ["明日は雨らしいよ\tほんまかいな", "ワイは寝る\tおやすみ"],
            "newsplus": ["増税は困るよね\t本当にそう思う"]}
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        for board, lines in rows.items():
            zf.writestr(f"corpus/{board}.tsv", "\n".join(lines) + "\n")
    (a.raw_dir / "corpus.zip").write_bytes(buf.getvalue())
    (a.raw_dir / "ng_words.txt").write_text("", encoding="utf-8")
    texts = _texts(a)
    st = a.extraction_stats()
    assert copy not in texts and st["rejected"]["copypaste"] == 2
    assert "そうだね。散歩でもするか" in texts         # __BR__ は文の区切り
    assert "ワイは寝る" not in texts and st["rejected"]["net_slang"] == 1
    assert st["dialogues"] == 5 and st["utterances_raw"] == 11


def test_open2ch_board_ratio_overrides_dialogue_ratio(tmp_path):
    a = _adapter(tmp_path, "open2ch", ["sources.open2ch.board_ratio={news4vip: 0.0, newsplus: 1.0}",
                                       "sources.open2ch.dialogue_ratio=0.5"])
    assert a.dialogue_ratio("news4vip_00000001") == 0.0
    assert a.dialogue_ratio("newsplus_00000001") == 1.0
    assert a.dialogue_ratio("livejupiter_00000001") == 0.5     # 書いていない板は dialogue_ratio


def test_open2ch_filter_rejects_quotes():
    f, _ = _o2_filter()
    assert f.reject_reason("＞騒音と振動を強く感じた。それはないだろ") == "quote"
    assert f.reject_reason("それはないだろ") is None


# ---------------------------------------------------------------- 各ソースの読み取り

def test_realpersonachat_reads_tarball(tmp_path):
    a = _adapter(tmp_path, "realpersonachat")
    dialogue = {"dialogue_id": 7, "interlocutors": ["AA", "AB"], "utterances": [
        {"utterance_id": 0, "interlocutor_id": "AA", "text": "よろしくお願いします！"},
        {"utterance_id": 1, "interlocutor_id": "AB", "text": "よろしくお願いします！"},
        {"utterance_id": 2, "interlocutor_id": "AA", "text": "今日は暑いですね"}]}
    _tarball(a.raw_dir / f"real-persona-chat-{RPC_REVISION[:12]}.tar.gz", RPC_REPO, RPC_REVISION,
             {"real_persona_chat/dialogues/00007.json": json.dumps(dialogue, ensure_ascii=False).encode()})
    docs = list(a.documents())
    assert [d.document_id for d in docs] == ["realpersonachat_7"]
    assert docs[0].paragraphs == ["よろしくお願いします！", "今日は暑いですね"]
    assert a.extraction_stats()["rejected"] == {"duplicate_exact": 1}


def test_mrmp_strips_mentions_by_interlocutor_name(tmp_path):
    a = _adapter(tmp_path, "mrmp")
    dialogue = {"dialogue_id": "A00101", "interlocutors": ["てばさき", "いくら"], "utterances": [
        {"interlocutor_id": "いくら", "text": "@てばさき、そうです！僕もやってます"},
        {"interlocutor_id": "てばさき", "text": "@いくらそれって本当ですか？"}]}
    _tarball(a.raw_dir / f"multi-relational-multi-party-chat-corpus-{MRMP_REVISION[:12]}.tar.gz",
             MRMP_REPO, MRMP_REVISION,
             {"multi_relational_multi_party_chat_corpus/dialogues/A_first_time/A00101.json":
              json.dumps(dialogue, ensure_ascii=False).encode()})
    assert _texts(a) == ["そうです！僕もやってます", "それって本当ですか？"]


def test_jmrd_drops_recommender_utterances_that_copy_knowledge(tmp_path):
    a = _adapter(tmp_path, "jmrd")
    plot = "少女が不思議な森で大きな生き物と出会い家族の絆を取り戻していく物語"
    dialog = {"dialog_id": "00001", "knowledge": {"あらすじ": [plot]}, "dialog": [
        {"speaker": "seeker", "text": "こんにちは、おすすめの映画はありますか？"},
        {"speaker": "recommender", "text": f"{plot}です。", "checked_knowledge": []},
        {"speaker": "recommender", "text": "家族で観るのにぴったりですよ", "checked_knowledge": []}]}
    for f in ("train.json", "valid.json", "test.json"):
        (a.raw_dir / f).write_text(json.dumps([dialog] if f == "train.json" else [], ensure_ascii=False),
                                   encoding="utf-8")
    assert _texts(a) == ["こんにちは、おすすめの映画はありますか？", "家族で観るのにぴったりですよ"]
    assert a.extraction_stats()["rejected"] == {"copied_knowledge": 1}


def test_newschat_uses_only_the_user_role(tmp_path):
    a = _adapter(tmp_path, "newschat")
    row = {"dialog_id": "1", "dialog": [
        {"speaker": "S", "used_tweet": ["123"], "utterance": "ツイートの本文をそのまま入れた発話です"},
        {"speaker": "U", "utterance": "それは知りませんでした。"}]}
    (a.raw_dir / "all.jsonl").write_text(json.dumps(row, ensure_ascii=False) + "\n", encoding="utf-8")
    assert _texts(a) == ["それは知りませんでした。"]
    assert a.extraction_stats()["utterances_skipped_speaker"] == 1


def test_jcre3_reads_knp_sentences(tmp_path):
    knp = ("# S-ID:x-00\n* 1D\n+ 1D\nそろそろ そろそろ そろそろ 副詞 8 * 0 * 0 * 0 NIL\n"
           "書類 しょるい 書類 名詞 6 普通名詞 1 * 0 * 0 NIL\nを を を 助詞 9 格助詞 1 * 0 * 0 NIL\n"
           "作ろう つくろう 作る 動詞 2 * 0 * 0 * 0 NIL\n。 。 。 特殊 1 句点 1 * 0 * 0 NIL\nEOS\n"
           "# S-ID:x-01\n* -1D\n+ -1D\nはい はい はい 感動詞 12 * 0 * 0 * 0 NIL\nEOS\n")
    assert list(knp_sentences(knp)) == ["そろそろ書類を作ろう。", "はい"]
    a = _adapter(tmp_path, "jcre3")
    _tarball(a.raw_dir / f"J-CRe3-{JCRE3_REVISION[:12]}.tar.gz", JCRE3_REPO, JCRE3_REVISION,
             {"textual_annotations/20220302-1.knp": knp.encode()})
    docs = list(a.documents())
    assert docs[0].document_id == "jcre3_20220302-1"
    assert docs[0].paragraphs == ["そろそろ書類を作ろう。"]   # 「はい」は短すぎる


def test_dialogue_ratio_samples_whole_dialogues(tmp_path):
    a = _adapter(tmp_path, "newschat", ["sources.newschat.dialogue_ratio=0.5"])
    rows = [{"dialog_id": str(i), "dialog": [{"speaker": "U", "utterance": f"発話その{i}です"}]}
            for i in range(200)]
    (a.raw_dir / "all.jsonl").write_text("".join(json.dumps(r, ensure_ascii=False) + "\n" for r in rows),
                                         encoding="utf-8")
    docs = list(a.documents())
    st = a.extraction_stats()
    assert st["dialogues"] == 200 and st["dialogues_sampled_out"] == 200 - len(docs)
    assert 60 < len(docs) < 140


# ---------------------------------------------------------------- 既存の前処理との統合

@needs_sudachi
def test_preprocess_records_extraction_stats_and_report(tmp_path):
    a = _adapter(tmp_path, "newschat")
    rows = [{"dialog_id": "1", "dialog": [{"speaker": "U", "utterance": "そうなんですね。知りませんでした！"},
                                          {"speaker": "U", "utterance": "https://example.com を見て"}]}]
    (a.raw_dir / "all.jsonl").write_text("".join(json.dumps(r, ensure_ascii=False) + "\n" for r in rows),
                                         encoding="utf-8")
    builder = CanonicalBuilder(a.cfg, a.paths)
    report = builder.build_source(a).as_dict()
    assert report["extraction"]["utterances_kept"] == 1 and report["extraction"]["rejected"] == {"url": 1}
    assert report["records"] == 2        # 1 発話が 2 文に分かれる
    from iroha.jsonlio import write_json
    write_json(a.paths.stage_stats("preprocess.newschat"), report)
    stats = collect(a.paths, ["newschat"])
    assert stats["newschat"]["extraction"]["utterances_raw"] == 2
    assert "newschat" in render(stats)


def test_readings_follow_source_order(tmp_path):
    """同じ読みは source_order で先のソースに残る（書き言葉の一覧は source_order を使わない）。"""
    from iroha.jsonlio import read_jsonl
    from iroha.typo.readings import ReadingsBuilder
    cfg = load_config("config/typo-spoken.yaml", overrides=[
        f"data_dir={tmp_path / 'out'}", f"raw_dir={tmp_path / 'raw'}", "typo.exclude_readings_from=[]",
        "typo.source_order=[realpersonachat, open2ch]"])
    paths = Paths(tmp_path / "out", tmp_path / "raw").ensure()
    paths.canonical.mkdir(parents=True, exist_ok=True)
    for name in ("open2ch", "realpersonachat"):
        rec = {"source": name, "document_id": f"{name}_1", "split": "train", "reading": "そうなんですね", "chunks": []}
        paths.canonical_for(name).write_text(json.dumps(rec, ensure_ascii=False) + "\n", encoding="utf-8")
    adapters = [get_adapter(n, cfg, paths) for n in ("open2ch", "realpersonachat")]   # 名前の順
    ReadingsBuilder(cfg, paths).run(adapters)
    rows = list(read_jsonl(paths.root / "readings" / "train.jsonl"))
    assert [r["source"] for r in rows] == ["realpersonachat"]
