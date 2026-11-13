#pragma once
// 読みから機械的に作る特別な候補（読み全体に一致する候補として、辞書の候補の間に入る）:
// 桁区切りの数字（「12345」→ 12,345）、時刻（「1230」→ 12:30）、和暦・西暦（「2026ねん」→ 令和8年）。
// macOS 版の LatticeConverter.specialCandidateProviders と同じ 3 つを同じ順で作る
// （和暦・西暦は macOS 版の EraYearCandidates.swift と同じ規則。変えるときは両方を合わせる）。
// 移植元: AzooKeyKanaKanjiConverter v0.11.2（MIT License, Copyright (c) 2023 Miwa / Ensan）
//   Sources/KanaKanjiConverterModule/ConverterAPI/SpecialConversion/
//   CommaSeparatedNumber.swift・TimeExpression.swift・CalendarCandidate.swift
// ライセンス文は同じフォルダの LICENSE-AzooKeyKanaKanjiConverter.txt

#include <string>
#include <vector>

#include "lattice/dicdata.h"

namespace iroha::lattice {

// `convertTarget` は入力した読み（ひらがなのまま）。
// 返すのは候補 1 つにつき 1 エントリ（語 = 候補の文字列、評価値 = 候補の評価値）
std::vector<DicdataElement> SpecialCandidates(const std::u32string& convertTarget);

} // namespace iroha::lattice
