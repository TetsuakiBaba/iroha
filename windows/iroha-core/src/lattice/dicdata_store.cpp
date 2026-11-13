// 移植元は dicdata_store.h のコメントを参照
#include "lattice/dicdata_store.h"

#include <cstdio>
#include <cstring>
#include <fstream>

#include "iroha/unicode.h"
#include "lattice/number.h"

namespace iroha::lattice {

namespace {

// loudstxt3 は LOUDS のノード番号の下位 11 ビットごとに 1 ファイルに分かれている
constexpr int kShardShift = 11;
constexpr int kLocalMask = (1 << kShardShift) - 1;

bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>* bytes) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const std::streamsize size = file.tellg();
    if (size < 0) return false;
    bytes->resize(static_cast<size_t>(size));
    file.seekg(0);
    if (size > 0) file.read(reinterpret_cast<char*>(bytes->data()), size);
    return static_cast<bool>(file);
}

uint16_t ReadUInt16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t ReadUInt32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

float ReadFloat32(const uint8_t* p) {
    const uint32_t bits = ReadUInt32(p);
    float value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// 辞書ファイルの名前に使う識別子。文字を UTF-16 の 4 桁の 16 進で並べる（「あ」→ "[3042]"）
std::string EscapedIdentifier(const std::u32string& text) {
    std::string result = "[";
    bool first = true;
    for (const wchar_t unit : Utf32ToUtf16(text)) {
        char chunk[8];
        std::snprintf(chunk, sizeof(chunk), "%04X", static_cast<unsigned>(unit) & 0xFFFF);
        if (!first) result += "_";
        result += chunk;
        first = false;
    }
    return result + "]";
}

// 1 ノードぶんのエントリを読む。
// 形式: UInt16 の件数、件数ぶんの（左品詞 ID・右品詞 ID・意味 ID の UInt16 と評価値の Float32）、
// タブ区切りの文字列（最初がルビ、続きが語。空の語はルビと同じ）
void ParseEntries(const uint8_t* data, size_t size, std::vector<DicdataElement>* elements) {
    if (size < 2) return;
    const size_t count = ReadUInt16(data);
    const size_t stringStart = 2 + count * 10;
    if (stringStart > size) return;
    const size_t firstIndex = elements->size();
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* entry = data + 2 + i * 10;
        elements->emplace_back(U"", U"", ReadUInt16(entry), ReadUInt16(entry + 2),
                               ReadUInt16(entry + 4), ReadFloat32(entry + 6));
    }
    std::u32string ruby;
    size_t rangeStart = stringStart;
    size_t index = firstIndex;
    for (size_t offset = stringStart + 1; offset <= size; ++offset) {
        if (offset != size && data[offset] != '\t') continue;
        const std::string_view field(reinterpret_cast<const char*>(data) + rangeStart,
                                     offset - rangeStart);
        if (rangeStart == stringStart) {
            ruby = Utf8ToUtf32(field);
        } else if (index < elements->size()) {
            DicdataElement& element = (*elements)[index++];
            element.ruby = ruby;
            element.word = field.empty() ? ruby : Utf8ToUtf32(field);
        }
        rangeStart = offset + 1;
    }
}

struct WidthTables {
    std::unordered_map<char32_t, char32_t> fullwidthToHalfwidth;
    std::unordered_map<char32_t, char32_t> halfwidthToFullwidth;
};

// 記号の全角と半角の対応（元の実装は全角の列に ICU の全角→半角の変換をかけて作っている）
const WidthTables& Widths() {
    static const WidthTables tables = [] {
        static const char32_t kFullwidth[] = U"＋ー＊＝・！＃％＆＇＂〜｜￡＄￥＠｀；：＜＞，．＼／＿￣－";
        static const char32_t kHalfwidth[] = U"+ｰ*=･!#%&'\"〜|£$¥@`;:<>,.\\/_¯-";
        WidthTables result;
        for (size_t i = 0; kFullwidth[i] != 0 && kHalfwidth[i] != 0; ++i) {
            result.fullwidthToHalfwidth[kFullwidth[i]] = kHalfwidth[i];
            result.halfwidthToFullwidth[kHalfwidth[i]] = kFullwidth[i];
        }
        return result;
    }();
    return tables;
}

// 記号 → 同じ組の記号（その記号を含む組をつなげ、重複を除いたもの）
const std::unordered_map<std::u32string, std::vector<std::u32string>>& WeakRelatingSymbols() {
    static const std::unordered_map<std::u32string, std::vector<std::u32string>> lookup = [] {
        const std::vector<std::vector<std::u32string>> groups = {
#include "lattice/weak_relating_symbols.inc"
        };
        std::unordered_map<std::u32string, std::vector<std::u32string>> map;
        for (const auto& group : groups) {
            for (const std::u32string& symbol : group) {
                std::vector<std::u32string>& related = map[symbol];
                for (const std::u32string& other : group) {
                    if (std::find(related.begin(), related.end(), other) == related.end()) {
                        related.push_back(other);
                    }
                }
            }
        }
        return map;
    }();
    return lookup;
}

} // namespace

std::u32string ToKatakana(const std::u32string& text) {
    std::u32string result = text;
    for (char32_t& c : result) {
        if (c >= 0x3041 && c <= 0x3096) c += 96;
    }
    return result;
}

std::u32string ToHiragana(const std::u32string& text) {
    std::u32string result = text;
    for (char32_t& c : result) {
        if (c >= 0x30A1 && c <= 0x30F6) c -= 96;
    }
    return result;
}

DicdataStore::DicdataStore(std::filesystem::path dictionaryPath)
    : dictionaryPath_(std::move(dictionaryPath)), ccParsed_(cid::kCount, false) {
    std::vector<uint8_t> bytes;
    if (ReadFile(dictionaryPath_ / "louds" / "charID.chid", &bytes)) {
        const std::u32string text = Utf8ToUtf32(
            std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        const std::vector<std::u32string> characters = SplitGraphemes(text);
        for (size_t i = 0; i < characters.size() && i < 256; ++i) {
            // 文字 ID の表は 1 文字 = 1 コードポイントのものだけ（この辞書ではすべてそう）
            if (characters[i].size() == 1) charIds_[characters[i][0]] = static_cast<uint8_t>(i);
        }
    }
    if (ReadFile(dictionaryPath_ / "mm.binary", &bytes)) {
        mmValues_.resize(bytes.size() / 4);
        for (size_t i = 0; i < mmValues_.size(); ++i) mmValues_[i] = ReadFloat32(&bytes[i * 4]);
    }
    if (mmValues_.size() < static_cast<size_t>(mid::kCount) * mid::kCount) {
        mmValues_.assign(static_cast<size_t>(mid::kCount) * mid::kCount, 0);
    }
}

uint8_t DicdataStore::CharId(const std::u32string& character) const {
    if (character.size() != 1) return UINT8_MAX;
    const auto it = charIds_.find(character[0]);
    return it == charIds_.end() ? UINT8_MAX : it->second;
}

const Louds* DicdataStore::LoadLouds(const std::u32string& firstCharacter) {
    auto it = loudses_.find(firstCharacter);
    if (it == loudses_.end()) {
        const std::string identifier = EscapedIdentifier(firstCharacter);
        const std::filesystem::path directory = dictionaryPath_ / "louds";
        it = loudses_
                 .emplace(firstCharacter, Louds::Load(directory / (identifier + ".louds"),
                                                      directory / (identifier + ".loudschars2")))
                 .first;
    }
    return it->second ? &*it->second : nullptr;
}

const std::vector<uint8_t>* DicdataStore::LoadLoudstxt(const std::string& fileId) {
    auto it = loudstxts_.find(fileId);
    if (it == loudstxts_.end()) {
        std::vector<uint8_t> bytes;
        std::optional<std::vector<uint8_t>> loaded;
        if (ReadFile(dictionaryPath_ / "louds" / (fileId + ".loudstxt3"), &bytes)) {
            loaded = std::move(bytes);
        }
        it = loudstxts_.emplace(fileId, std::move(loaded)).first;
    }
    return it->second ? &*it->second : nullptr;
}

void DicdataStore::AppendEntries(const std::string& identifier, int nodeIndex,
                                 std::vector<DicdataElement>* elements) {
    const std::vector<uint8_t>* file =
        LoadLoudstxt(identifier + std::to_string(nodeIndex >> kShardShift));
    if (!file || file->size() < 2) return;
    const size_t local = static_cast<size_t>(nodeIndex & kLocalMask);
    const size_t count = ReadUInt16(file->data());
    if (local >= count || 2 + count * 4 > file->size()) return;
    const size_t start = ReadUInt32(file->data() + 2 + local * 4);
    const size_t end =
        local == count - 1 ? file->size() : ReadUInt32(file->data() + 2 + (local + 1) * 4);
    if (start > end || end > file->size()) return;
    ParseEntries(file->data() + start, end - start, elements);
}

void DicdataStore::SetDynamicUserDictionary(std::vector<DicdataElement> words) {
    dynamicUserDictionary_ = std::move(words);
}

std::vector<DicdataStore::Found> DicdataStore::Lookup(
    const std::vector<std::u32string>& characters, size_t start) {
    std::vector<Found> found;
    const size_t right = std::min(characters.size(), start + kMaxLength);
    if (start >= right) return found;

    // 読みを 1 文字ずつ伸ばしながらトライ木をたどる。辞書に続きが無くなった長さで止める
    // （止める判定に、この変換の間だけ加えた語は入らない。元の実装と同じ）
    struct Reached {
        size_t length;
        int nodeIndex;
    };
    std::vector<Reached> reached;
    const Louds* louds = LoadLouds(characters[start]);
    int nodeIndex = Louds::kRootNodeIndex;
    bool alive = louds != nullptr;
    std::u32string prefix;
    std::vector<std::u32string> prefixes; // prefixes[n - 1] = 長さ n の読み
    for (size_t end = start; end < right; ++end) {
        prefix += characters[end];
        prefixes.push_back(prefix);
        const size_t length = end - start + 1;
        bool reachedThisLength = false;
        if (alive) {
            const std::optional<int> next =
                louds->SearchCharNodeIndex(nodeIndex, CharId(characters[end]));
            if (next) {
                nodeIndex = *next;
                reached.push_back({length, nodeIndex});
                reachedThisLength = true;
            } else {
                alive = false;
            }
        }
        for (const DicdataElement& word : dynamicUserDictionary_) {
            if (word.ruby == prefix) found.push_back({word, static_cast<int>(end + 1)});
        }
        if (length >= 2 && !reachedThisLength) break;
    }

    const std::string identifier = EscapedIdentifier(characters[start]);
    std::vector<DicdataElement> elements;
    for (const Reached& node : reached) {
        elements.clear();
        AppendEntries(identifier, node.nodeIndex, &elements);
        for (DicdataElement& element : elements) {
            if (element.ruby != prefixes[node.length - 1]) continue;
            found.push_back({std::move(element), static_cast<int>(start + node.length)});
        }
    }

    // 機械的に作る語は、辞書をたどれた長さに関係なく最大の長さまで試す
    std::u32string segment;
    for (size_t end = start; end < right; ++end) {
        segment += characters[end];
        for (DicdataElement& element :
             WiseDicdata(segment, end - start + 1, start, end + 1, characters)) {
            found.push_back({std::move(element), static_cast<int>(end + 1)});
        }
    }
    return found;
}

std::vector<DicdataElement> DicdataStore::WiseDicdata(
    const std::u32string& convertTarget, size_t characterCount, size_t lowerBound,
    size_t upperBound, const std::vector<std::u32string>& fullText) const {
    std::vector<DicdataElement> result = JapaneseNumberDicdata(convertTarget);

    // 数字の並び。前後が数字のときは作らない（並びの途中を切り出さない）
    const bool previousIsNumber = lowerBound >= 1 && IsNumberCharacter(fullText[lowerBound - 1]);
    const bool nextIsNumber = upperBound < fullText.size() && IsNumberCharacter(fullText[upperBound]);
    if (!previousIsNumber && !nextIsNumber) {
        if (const std::optional<int64_t> number = ParseInteger(convertTarget)) {
            result.emplace_back(convertTarget, convertTarget, cid::kNumber, mid::kSmallNumber,
                                PValue(-14));
            if (static_cast<double>(*number) <= 1e12 && -1e12 <= static_cast<double>(*number)) {
                result.emplace_back(SpellOutJapanese(*number), convertTarget, cid::kNumber,
                                    mid::kSmallNumber, PValue(-16));
            }
        }
    }

    if (characterCount == 1) {
        // 1 文字は、ひらがなとカタカナをそのまま語にする（どの読みでも経路が途切れないようにする）
        const std::u32string hiragana = ToHiragana(convertTarget);
        if (hiragana == convertTarget) {
            result.emplace_back(convertTarget, convertTarget, cid::kProperNoun, mid::kGeneral,
                                PValue(-14));
        } else {
            result.emplace_back(hiragana, convertTarget, cid::kProperNoun, mid::kGeneral,
                                PValue(-13));
            result.emplace_back(convertTarget, convertTarget, cid::kProperNoun, mid::kGeneral,
                                PValue(-14));
        }

        // 記号の全角と半角
        if (convertTarget.size() == 1) {
            const char32_t first = convertTarget[0];
            const WidthTables& widths = Widths();
            PValue value = -14;
            const auto half = widths.fullwidthToHalfwidth.find(first);
            if (half != widths.fullwidthToHalfwidth.end() && half->second != first) {
                result.emplace_back(convertTarget, convertTarget, cid::kSymbol, mid::kGeneral, value);
                value -= PValue(5);
                result.emplace_back(std::u32string(1, half->second), convertTarget, cid::kSymbol,
                                    mid::kGeneral, value);
                value -= PValue(5);
            }
            const auto full = widths.halfwidthToFullwidth.find(first);
            if (full != widths.halfwidthToFullwidth.end() && full->second != first) {
                result.emplace_back(convertTarget, convertTarget, cid::kSymbol, mid::kGeneral, value);
                value -= PValue(5);
                result.emplace_back(std::u32string(1, full->second), convertTarget, cid::kSymbol,
                                    mid::kGeneral, value);
                value -= PValue(5);
            }
        }
    }

    const auto& symbols = WeakRelatingSymbols();
    const auto group = symbols.find(convertTarget);
    if (group != symbols.end()) {
        const WidthTables& widths = Widths();
        PValue value = -34;
        for (const std::u32string& symbol : group->second) {
            if (symbol == convertTarget) continue;
            result.emplace_back(symbol, convertTarget, cid::kSymbol, mid::kGeneral, value);
            value -= PValue(5);
            if (symbol.size() == 1) {
                const auto full = widths.halfwidthToFullwidth.find(symbol[0]);
                if (full != widths.halfwidthToFullwidth.end() && full->second != symbol[0]) {
                    result.emplace_back(std::u32string(1, full->second), convertTarget,
                                        cid::kSymbol, mid::kGeneral, value);
                    value -= PValue(5);
                }
            }
        }
    }
    return result;
}

bool DicdataStore::ShouldBeRemoved(const DicdataElement& data) {
    const PValue d = data.Value() - kThreshold;
    if (d < 0) return true;
    // 語が短いほど厳しくする（文字数で決める罰則）
    return PValue(-2.0) / static_cast<PValue>(GraphemeCount(data.word)) < -d;
}

const std::vector<PValue>* DicdataStore::CCLine(int former) {
    if (former < 0 || former >= cid::kCount) return nullptr;
    if (!ccParsed_[former]) {
        ccParsed_[former] = true;
        // (Int32 の左品詞 ID, Float32 の値) の並び。最初の組（ID = -1）は行の既定値
        std::vector<uint8_t> bytes;
        if (ReadFile(dictionaryPath_ / "cb" / (std::to_string(former) + ".binary"), &bytes) &&
            bytes.size() >= 8) {
            std::vector<PValue> line(cid::kCount, ReadFloat32(&bytes[4]));
            for (size_t offset = 8; offset + 8 <= bytes.size(); offset += 8) {
                const int32_t key = static_cast<int32_t>(ReadUInt32(&bytes[offset]));
                if (key >= 0 && key < cid::kCount) line[key] = ReadFloat32(&bytes[offset + 4]);
            }
            ccLines_.emplace(former, std::move(line));
        }
    }
    const auto it = ccLines_.find(former);
    return it == ccLines_.end() ? nullptr : &it->second;
}

PValue DicdataStore::CCValue(int former, int latter) {
    const std::vector<PValue>* line = CCLine(former);
    if (!line || latter < 0 || latter >= cid::kCount) return -25;
    return (*line)[latter];
}

PValue DicdataStore::MMValue(int former, int latter) const {
    if (former == 500 || latter == 500) return 0;
    const size_t index = static_cast<size_t>(former) * mid::kCount + static_cast<size_t>(latter);
    return index < mmValues_.size() ? mmValues_[index] : 0;
}

} // namespace iroha::lattice
