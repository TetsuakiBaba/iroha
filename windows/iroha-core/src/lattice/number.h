#pragma once
// 数の読み（「ニセンニジュウロク」）から作る辞書エントリと、数の文字列の扱い。
// 移植元: AzooKeyKanaKanjiConverter v0.11.2（MIT License, Copyright (c) 2023 Miwa / Ensan）
//   Sources/KanaKanjiConverterModule/DictionaryManagement/JapaneseNumber.swift
// ライセンス文は同じフォルダの LICENSE-AzooKeyKanaKanjiConverter.txt

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "lattice/dicdata.h"

namespace iroha::lattice {

// カタカナの数の読みから、漢数字とアラビア数字のエントリを作る（数の読みでなければ空）
std::vector<DicdataElement> JapaneseNumberDicdata(const std::u32string& head);

// Swift の `Int(文字列)` と同じ読み方（符号 1 つと ASCII の数字だけ。それ以外・桁あふれは nullopt）
std::optional<int64_t> ParseInteger(const std::u32string& text);

// 整数を漢数字で書く（macOS の NumberFormatter の spellOut・ja-JP と同じ形。1234 → 千二百三十四）
std::u32string SpellOutJapanese(int64_t number);

// Swift の `Character.isNumber`（先頭の文字が数を表すか）の主な範囲
bool IsNumberCharacter(const std::u32string& character);

} // namespace iroha::lattice
