// 移植元は kana_kanji_converter.h のコメントを参照
#include "lattice/kana_kanji_converter.h"

#include <deque>
#include <unordered_map>
#include <unordered_set>

#include "iroha/unicode.h"
#include "lattice/special_candidates.h"

namespace iroha::lattice {

namespace {

// 経路の 1 歩（ある語を、ある 1 つの手前の経路につないだもの）。手前へたどると文頭に着く
struct RegisteredNode {
    const DicdataElement* data;
    const RegisteredNode* prev;
    // 文頭からこの語までの評価値の合計
    PValue totalValue;
    // この語が覆う読みの文字数
    int rubyCount;
};

// ラティスのノード（読みの from 文字目から to 文字目の手前までを覆う語）
struct LatticeNode {
    DicdataElement data;
    int from = 0;
    int to = 0;
    // 評価値が低すぎて計算に使わない語か
    bool removed = false;
    // このノードの手前の経路。評価値の高い順に nBest 個まで
    std::vector<const RegisteredNode*> prevs;
    // prevs のそれぞれに、このノードの語を足した評価値
    std::vector<PValue> values;
};

const DicdataElement& BosData() {
    static const DicdataElement data(U"", U"", cid::kBOS, mid::kBOS, PValue(0));
    return data;
}

// 候補（まとめている間の形。語はラティスか、この変換の間だけの置き場を指す）
struct Candidate {
    std::u32string text;
    PValue value = 0;
    int rubyCount = 0;
    std::vector<const DicdataElement*> data;
};

struct Clause {
    int mid = mid::kEOS;
    std::u32string text;
    // この文節の最後の語の、CandidateData::data での位置（-1 = まだ語が無い）
    int dataEndIndex = -1;
    // この文節の最後の語までの評価値の合計
    PValue value = 0;
};

// 1 つの経路を文節に分けたもの
struct CandidateData {
    std::vector<Clause> clauses;
    std::vector<const DicdataElement*> data;
    std::vector<int> rubyCounts;
};

CandidateData BuildCandidateData(const RegisteredNode* last) {
    std::vector<const RegisteredNode*> chain;
    for (const RegisteredNode* node = last; node != nullptr; node = node->prev) chain.push_back(node);
    std::reverse(chain.begin(), chain.end());

    CandidateData result;
    Clause head;
    head.mid = chain[0]->data->mid;
    result.clauses.push_back(head);
    for (size_t i = 1; i < chain.size(); ++i) {
        const RegisteredNode* node = chain[i];
        const DicdataElement& data = *node->data;
        if (data.word.empty()) continue;
        Clause& lastClause = result.clauses.back();
        if (lastClause.text.empty() || !IsClause(chain[i - 1]->data->rcid, data.lcid)) {
            // 文節の続き
            lastClause.text += data.word;
            if ((lastClause.mid == 500 && data.mid != 500) || IncludeMMValueCalculation(data)) {
                lastClause.mid = data.mid;
            }
            result.data.push_back(&data);
            result.rubyCounts.push_back(node->rubyCount);
            lastClause.dataEndIndex = static_cast<int>(result.data.size()) - 1;
            lastClause.value = node->totalValue;
        } else {
            // 文節の切れ目
            Clause clause;
            clause.text = data.word;
            if (IncludeMMValueCalculation(data)) clause.mid = data.mid;
            result.data.push_back(&data);
            result.rubyCounts.push_back(node->rubyCount);
            clause.dataEndIndex = static_cast<int>(result.data.size()) - 1;
            clause.value = node->totalValue;
            result.clauses.push_back(std::move(clause));
        }
    }
    return result;
}

// 文全体の候補。文節どうしの意味の連接の値を足す
Candidate WholeSentenceCandidate(const CandidateData& data, const DicdataStore& store) {
    Candidate candidate;
    PValue mmValue = 0;
    int mid = mid::kEOS;
    for (const Clause& clause : data.clauses) {
        mmValue = mmValue + store.MMValue(mid, clause.mid);
        mid = clause.mid;
        candidate.text += clause.text;
    }
    candidate.value = data.clauses.back().value + mmValue;
    candidate.data = data.data;
    for (const int count : data.rubyCounts) candidate.rubyCount += count;
    return candidate;
}

// 先頭の文節だけの候補
Candidate FirstClauseCandidate(const CandidateData& data) {
    Candidate candidate;
    const Clause& first = data.clauses.front();
    candidate.text = first.text;
    candidate.value = first.value;
    const size_t count = static_cast<size_t>(std::max(0, first.dataEndIndex)) + 1;
    for (size_t i = 0; i < count && i < data.data.size(); ++i) {
        candidate.data.push_back(data.data[i]);
        candidate.rubyCount += data.rubyCounts[i];
    }
    return candidate;
}

using TextSet = std::unordered_set<std::u32string>;

// 文字列が同じ候補を 1 つにする（最初に出た位置に残し、評価値かルビの文字数が大きい方を取る）。
// 空の候補と `seen` にある候補は除く
std::vector<Candidate> Unique(std::vector<Candidate> candidates, const TextSet& seen) {
    std::vector<Candidate> result;
    std::unordered_map<std::u32string, size_t> textIndex;
    for (Candidate& candidate : candidates) {
        if (candidate.text.empty() || seen.count(candidate.text) != 0) continue;
        const auto it = textIndex.find(candidate.text);
        if (it != textIndex.end()) {
            Candidate& existing = result[it->second];
            if (existing.value < candidate.value || existing.rubyCount < candidate.rubyCount) {
                existing = std::move(candidate);
            }
        } else {
            textIndex.emplace(candidate.text, result.size());
            result.push_back(std::move(candidate));
        }
    }
    return result;
}

bool ByValue(const Candidate& a, const Candidate& b) { return a.value > b.value; }

// 覆う読みが長い順、同じなら評価値の高い順
bool ByRubyCountThenValue(const Candidate& a, const Candidate& b) {
    if (a.rubyCount == b.rubyCount) return a.value > b.value;
    return a.rubyCount > b.rubyCount;
}

// 並べ替えて先頭の `count` 個を取る（同じ順位のものは元の順のまま）
template <typename Less>
std::vector<Candidate> Best(std::vector<Candidate> candidates, size_t count, Less less) {
    std::stable_sort(candidates.begin(), candidates.end(), less);
    if (candidates.size() > count) candidates.resize(count);
    return candidates;
}

void Insert(TextSet* seen, const std::vector<Candidate>& candidates) {
    for (const Candidate& candidate : candidates) seen->insert(candidate.text);
}

// カタカナ語らしさ（小さいほどカタカナ語らしい）
PValue KatakanaScore(const std::vector<std::u32string>& characters) {
    static const std::u32string kStrong = U"プヴペィフ";
    static const std::u32string kMiddle = U"ュピポ";
    static const std::u32string kWeak = U"パォグーム";
    PValue score = 1;
    for (const std::u32string& character : characters) {
        if (character.size() != 1) continue;
        if (kStrong.find(character[0]) != std::u32string::npos) {
            score *= PValue(0.5);
        } else if (kMiddle.find(character[0]) != std::u32string::npos) {
            score *= PValue(0.6);
        } else if (kWeak.find(character[0]) != std::u32string::npos) {
            score *= PValue(0.7);
        }
    }
    return score;
}

// 大文字にする（Swift の `uppercased()` のうち、英字・全角英字・ギリシャ文字・キリル文字）
std::u32string ToUpper(const std::u32string& text) {
    std::u32string result = text;
    for (char32_t& c : result) {
        if ((c >= U'a' && c <= U'z') || (c >= 0xFF41 && c <= 0xFF5A) ||
            (c >= 0x00E0 && c <= 0x00FE && c != 0x00F7) ||
            (c >= 0x03B1 && c <= 0x03C9 && c != 0x03C2) || (c >= 0x0430 && c <= 0x044F)) {
            c -= 0x20;
        } else if (c >= 0x0450 && c <= 0x045F) {
            c -= 0x50;
        }
    }
    return result;
}

} // namespace

std::vector<ConvertedCandidate> KanaKanjiConverter::RequestCandidates(
    const std::u32string& reading, int nBest, bool specialCandidates) {
    const std::u32string katakana = ToKatakana(reading);
    const std::vector<std::u32string> characters = SplitGraphemes(katakana);
    const int count = static_cast<int>(characters.size());
    if (count == 0 || nBest <= 0) return {};

    // この変換の間だけ生かす置き場（追加してもアドレスが変わらない）
    std::deque<LatticeNode> nodeStorage;
    std::deque<RegisteredNode> registered;
    std::deque<DicdataElement> extraElements;
    registered.push_back({&BosData(), nullptr, PValue(0), 0});
    const RegisteredNode* bos = &registered.front();

    // 読みの各位置から始まる語を引く
    std::vector<std::vector<LatticeNode*>> lattice(count);
    for (int start = 0; start < count; ++start) {
        for (DicdataStore::Found& found : store_->Lookup(characters, static_cast<size_t>(start))) {
            LatticeNode& node = nodeStorage.emplace_back();
            node.data = std::move(found.data);
            node.from = start;
            node.to = found.endIndex;
            node.removed = DicdataStore::ShouldBeRemoved(node.data);
            if (start == 0) node.prevs.push_back(bos);
            lattice[start].push_back(&node);
        }
    }

    // 前から順に、各ノードの手前の経路（nBest 個まで）を決めていく
    std::vector<const RegisteredNode*> resultPrevs;
    for (int index = 0; index < count; ++index) {
        for (LatticeNode* node : lattice[index]) {
            if (node->prevs.empty() || node->removed) continue;
            const PValue wValue = node->data.Value();
            node->values.clear();
            for (const RegisteredNode* prev : node->prevs) {
                if (index == 0) {
                    // 文頭との連接はここで足す（それ以外は、手前のノードから登録するときに足してある）
                    node->values.push_back(prev->totalValue + wValue +
                                           store_->CCValue(prev->data->rcid, node->data.lcid));
                } else {
                    node->values.push_back(prev->totalValue + wValue);
                }
            }
            const int rubyCount = node->to - node->from;
            if (node->to == count) {
                // 読みの終わりに届いた経路
                for (size_t k = 0; k < node->prevs.size(); ++k) {
                    registered.push_back({&node->data, node->prevs[k], node->values[k], rubyCount});
                    resultPrevs.push_back(&registered.back());
                }
                continue;
            }
            const std::vector<PValue>* ccLine = store_->CCLine(node->data.rcid);
            for (LatticeNode* next : lattice[node->to]) {
                if (next->removed) continue;
                const int lcid = next->data.lcid;
                const PValue ccValue =
                    ccLine && lcid >= 0 && lcid < cid::kCount ? (*ccLine)[lcid] : PValue(-25);
                for (size_t k = 0; k < node->values.size(); ++k) {
                    const PValue newValue = ccValue + node->values[k];
                    // 評価値が同じかより高い経路の後ろに入れる
                    size_t position = next->prevs.size();
                    while (position > 0 && !(next->prevs[position - 1]->totalValue >= newValue)) {
                        --position;
                    }
                    if (position == static_cast<size_t>(nBest)) continue;
                    registered.push_back({&node->data, node->prevs[k], newValue, rubyCount});
                    if (next->prevs.size() >= static_cast<size_t>(nBest)) next->prevs.pop_back();
                    next->prevs.insert(next->prevs.begin() + position, &registered.back());
                }
            }
        }
    }

    // 読み全体をそのまま出す候補（カタカナ・ひらがな・大文字）
    const auto additionalCandidates = [&]() {
        std::vector<Candidate> candidates;
        const auto add = [&](const std::u32string& word, PValue dataValue, PValue value) {
            extraElements.emplace_back(word, katakana, cid::kProperNoun, mid::kGeneral, dataValue);
            Candidate candidate;
            candidate.text = word;
            candidate.value = value;
            candidate.rubyCount = count;
            candidate.data.push_back(&extraElements.back());
            candidates.push_back(std::move(candidate));
        };
        const PValue katakanaValue = PValue(-14) * KatakanaScore(characters);
        add(katakana, katakanaValue, katakanaValue);
        add(ToHiragana(katakana), PValue(-14.5), PValue(-14.5));
        add(ToUpper(katakana), PValue(-15), PValue(-14.6));
        return candidates;
    };

    std::vector<Candidate> results;
    if (resultPrevs.empty()) {
        results = Unique(additionalCandidates(), {});
    } else {
        std::vector<CandidateData> clauseResult;
        clauseResult.reserve(resultPrevs.size());
        for (const RegisteredNode* node : resultPrevs) {
            clauseResult.push_back(BuildCandidateData(node));
        }

        // 文全体の候補の上位 5 件。
        // 元の実装はここに予測変換・英単語・ユーザショートカットの候補を混ぜて上位 5 件を取り直し、
        // さらに「誤り訂正でない候補を 3 番目までに入れる」調整をするが、どれも iroha の条件では
        // 何も足さず何も動かさないので省いている
        std::vector<Candidate> wholeSentence;
        wholeSentence.reserve(clauseResult.size());
        for (const CandidateData& data : clauseResult) {
            wholeSentence.push_back(WholeSentenceCandidate(data, *store_));
        }
        results = Best(Unique(std::move(wholeSentence), {}), 5, ByValue);
        TextSet seen;
        Insert(&seen, results);

        // 先頭の文節だけの候補（覆う読みが長い順に 5 件）
        std::vector<Candidate> firstClause;
        firstClause.reserve(clauseResult.size());
        for (const CandidateData& data : clauseResult) {
            firstClause.push_back(FirstClauseCandidate(data));
        }
        std::vector<Candidate> firstClauseCandidates =
            Best(Unique(Unique(std::move(firstClause), {}), seen), 5, ByRubyCountThenValue);
        Insert(&seen, firstClauseCandidates);

        // 読みの先頭から始まる辞書の語と、読み全体をそのまま出す候補
        std::vector<Candidate> wordCandidates;
        for (const LatticeNode* node : lattice[0]) {
            Candidate candidate;
            candidate.text = node->data.word;
            candidate.value = node->data.Value();
            candidate.rubyCount = node->to - node->from;
            candidate.data.push_back(&node->data);
            wordCandidates.push_back(std::move(candidate));
        }
        for (Candidate& candidate : additionalCandidates()) {
            wordCandidates.push_back(std::move(candidate));
        }
        wordCandidates = Unique(std::move(wordCandidates), seen);
        std::stable_sort(wordCandidates.begin(), wordCandidates.end(), ByRubyCountThenValue);
        Insert(&seen, wordCandidates);

        // 特別な候補は、語の候補の 5 番目の後ろに挟む
        if (specialCandidates) {
            std::vector<Candidate> wise;
            for (DicdataElement& element : SpecialCandidates(reading)) {
                extraElements.push_back(std::move(element));
                const DicdataElement& stored = extraElements.back();
                Candidate candidate;
                candidate.text = stored.word;
                candidate.value = stored.Value();
                candidate.rubyCount = static_cast<int>(GraphemeCount(stored.ruby));
                candidate.data.push_back(&stored);
                wise.push_back(std::move(candidate));
            }
            wise = Unique(std::move(wise), seen);
            const size_t position = std::min<size_t>(5, wordCandidates.size());
            wordCandidates.insert(wordCandidates.begin() + position,
                                  std::make_move_iterator(wise.begin()),
                                  std::make_move_iterator(wise.end()));
        }

        for (Candidate& candidate : firstClauseCandidates) results.push_back(std::move(candidate));
        for (Candidate& candidate : wordCandidates) results.push_back(std::move(candidate));
    }

    std::vector<ConvertedCandidate> converted;
    converted.reserve(results.size());
    for (const Candidate& candidate : results) {
        ConvertedCandidate out;
        out.text = candidate.text;
        out.value = candidate.value;
        out.rubyCount = candidate.rubyCount;
        out.data.reserve(candidate.data.size());
        for (const DicdataElement* element : candidate.data) out.data.push_back(*element);
        converted.push_back(std::move(out));
    }
    return converted;
}

} // namespace iroha::lattice
