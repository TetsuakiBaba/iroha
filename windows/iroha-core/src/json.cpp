#include "iroha/json.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace iroha::json {

const Value* Value::Find(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& [k, v] : object) {
        if (k == key) return &v;
    }
    return nullptr;
}

namespace {

struct Parser {
    std::string_view text;
    size_t pos = 0;
    int depth = 0;

    void SkipWhitespace() {
        while (pos < text.size() &&
               (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' ||
                text[pos] == '\r')) {
            ++pos;
        }
    }

    bool Consume(char c) {
        SkipWhitespace();
        if (pos < text.size() && text[pos] == c) {
            ++pos;
            return true;
        }
        return false;
    }

    bool ParseValue(Value* out) {
        if (++depth > 64) return false; // 深すぎる入れ子は不正扱い
        SkipWhitespace();
        bool ok = false;
        if (pos >= text.size()) {
            ok = false;
        } else if (text[pos] == '{') {
            ok = ParseObject(out);
        } else if (text[pos] == '[') {
            ok = ParseArray(out);
        } else if (text[pos] == '"') {
            out->type = Value::Type::String;
            ok = ParseString(&out->string);
        } else if (text.compare(pos, 4, "true") == 0) {
            out->type = Value::Type::Bool;
            out->boolean = true;
            pos += 4;
            ok = true;
        } else if (text.compare(pos, 5, "false") == 0) {
            out->type = Value::Type::Bool;
            out->boolean = false;
            pos += 5;
            ok = true;
        } else if (text.compare(pos, 4, "null") == 0) {
            out->type = Value::Type::Null;
            pos += 4;
            ok = true;
        } else {
            ok = ParseNumber(out);
        }
        --depth;
        return ok;
    }

    bool ParseObject(Value* out) {
        out->type = Value::Type::Object;
        ++pos; // '{'
        SkipWhitespace();
        if (Consume('}')) return true;
        while (true) {
            SkipWhitespace();
            std::string key;
            if (pos >= text.size() || text[pos] != '"' || !ParseString(&key)) {
                return false;
            }
            if (!Consume(':')) return false;
            Value value;
            if (!ParseValue(&value)) return false;
            out->object.emplace_back(std::move(key), std::move(value));
            if (Consume(',')) continue;
            return Consume('}');
        }
    }

    bool ParseArray(Value* out) {
        out->type = Value::Type::Array;
        ++pos; // '['
        SkipWhitespace();
        if (Consume(']')) return true;
        while (true) {
            Value value;
            if (!ParseValue(&value)) return false;
            out->array.push_back(std::move(value));
            if (Consume(',')) continue;
            return Consume(']');
        }
    }

    bool ParseHex4(unsigned* out) {
        if (pos + 4 > text.size()) return false;
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text[pos + i];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= c - '0';
            else if (c >= 'a' && c <= 'f') value |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') value |= c - 'A' + 10;
            else return false;
        }
        pos += 4;
        *out = value;
        return true;
    }

    void AppendUtf8(std::string* s, unsigned cp) {
        if (cp < 0x80) {
            s->push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            s->push_back(static_cast<char>(0xC0 | (cp >> 6)));
            s->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            s->push_back(static_cast<char>(0xE0 | (cp >> 12)));
            s->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            s->push_back(static_cast<char>(0xF0 | (cp >> 18)));
            s->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            s->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool ParseString(std::string* out) {
        ++pos; // '"'
        out->clear();
        while (pos < text.size()) {
            const char c = text[pos];
            if (c == '"') {
                ++pos;
                return true;
            }
            if (c == '\\') {
                ++pos;
                if (pos >= text.size()) return false;
                const char escape = text[pos++];
                switch (escape) {
                    case '"': out->push_back('"'); break;
                    case '\\': out->push_back('\\'); break;
                    case '/': out->push_back('/'); break;
                    case 'b': out->push_back('\b'); break;
                    case 'f': out->push_back('\f'); break;
                    case 'n': out->push_back('\n'); break;
                    case 'r': out->push_back('\r'); break;
                    case 't': out->push_back('\t'); break;
                    case 'u': {
                        unsigned cp = 0;
                        if (!ParseHex4(&cp)) return false;
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            // サロゲートペア
                            if (pos + 1 < text.size() && text[pos] == '\\' &&
                                text[pos + 1] == 'u') {
                                pos += 2;
                                unsigned low = 0;
                                if (!ParseHex4(&low) || low < 0xDC00 || low > 0xDFFF) {
                                    return false;
                                }
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            } else {
                                return false;
                            }
                        }
                        AppendUtf8(out, cp);
                        break;
                    }
                    default:
                        return false;
                }
            } else {
                out->push_back(c);
                ++pos;
            }
        }
        return false; // 閉じ引用符がない
    }

    bool ParseNumber(Value* out) {
        const size_t start = pos;
        if (pos < text.size() && text[pos] == '-') ++pos;
        while (pos < text.size() &&
               (std::isdigit(static_cast<unsigned char>(text[pos])) ||
                text[pos] == '.' || text[pos] == 'e' || text[pos] == 'E' ||
                text[pos] == '+' || text[pos] == '-')) {
            ++pos;
        }
        if (pos == start) return false;
        out->type = Value::Type::Number;
        out->number = std::strtod(std::string(text.substr(start, pos - start)).c_str(),
                                  nullptr);
        return true;
    }
};

} // namespace

bool Parse(std::string_view text, Value* out) {
    // メモ帳等が付けるUTF-8 BOMは読み飛ばす
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }
    Parser parser{text};
    if (!parser.ParseValue(out)) return false;
    parser.SkipWhitespace();
    return parser.pos == parser.text.size();
}

std::string QuoteString(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04X", c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out += "\"";
    return out;
}

std::string FormatNumber(double value) {
    if (!std::isfinite(value)) return "0";
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    return result.ec == std::errc() ? std::string(buffer, result.ptr) : "0";
}

namespace {

void SerializeInto(const Value& value, bool pretty, int depth, std::string* out) {
    const auto newline = [&](int level) {
        if (!pretty) return;
        out->push_back('\n');
        out->append(static_cast<size_t>(level) * 2, ' ');
    };
    switch (value.type) {
    case Value::Type::Null: *out += "null"; break;
    case Value::Type::Bool: *out += value.boolean ? "true" : "false"; break;
    case Value::Type::Number: *out += FormatNumber(value.number); break;
    case Value::Type::String: *out += QuoteString(value.string); break;
    case Value::Type::Array:
        out->push_back('[');
        for (size_t i = 0; i < value.array.size(); ++i) {
            if (i > 0) out->push_back(',');
            newline(depth + 1);
            SerializeInto(value.array[i], pretty, depth + 1, out);
        }
        if (!value.array.empty()) newline(depth);
        out->push_back(']');
        break;
    case Value::Type::Object: {
        std::vector<const std::pair<std::string, Value>*> members;
        for (const auto& member : value.object) members.push_back(&member);
        std::stable_sort(members.begin(), members.end(),
                         [](const auto* a, const auto* b) { return a->first < b->first; });
        out->push_back('{');
        for (size_t i = 0; i < members.size(); ++i) {
            if (i > 0) out->push_back(',');
            newline(depth + 1);
            *out += QuoteString(members[i]->first);
            *out += pretty ? " : " : ":";
            SerializeInto(members[i]->second, pretty, depth + 1, out);
        }
        if (!members.empty()) newline(depth);
        out->push_back('}');
        break;
    }
    }
}

} // namespace

std::string Serialize(const Value& value, bool pretty) {
    std::string out;
    SerializeInto(value, pretty, 0, &out);
    return out;
}

Value MakeString(std::string_view text) {
    Value value;
    value.type = Value::Type::String;
    value.string = std::string(text);
    return value;
}

Value MakeNumber(double number) {
    Value value;
    value.type = Value::Type::Number;
    value.number = number;
    return value;
}

Value MakeBool(bool boolean) {
    Value value;
    value.type = Value::Type::Bool;
    value.boolean = boolean;
    return value;
}

Value MakeArray() {
    Value value;
    value.type = Value::Type::Array;
    return value;
}

Value MakeObject() {
    Value value;
    value.type = Value::Type::Object;
    return value;
}

void Value::Set(const std::string& key, Value member) {
    type = Type::Object;
    for (auto& entry : object) {
        if (entry.first == key) {
            entry.second = std::move(member);
            return;
        }
    }
    object.emplace_back(key, std::move(member));
}

} // namespace iroha::json
