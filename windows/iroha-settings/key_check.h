#pragma once
// キーの確認の窓（設定 > 入力 > かな・英数の切り替え の「キーの確認...」）。
// リモートデスクトップや Mac のキーボードで、どのキーが Windows まで届いているかを見るためのもの。
// 窓が前面にある間だけ、押したキーの仮想キーコード・スキャンコード・修飾キーと、iroha がそのキーをどう扱うかを
// 一覧に出す（低水準のキーボードのフックで見るので、IME やアプリより先に、届いたままの形で見える）。
// 窓が前面にないときのキーは見ない・残さない

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "iroha/config.h"

namespace key_check {

// 窓を開く（開いていれば前面に出す）。config は iroha の扱いの説明に使う（開いた時点の設定）
void Show(HWND owner, const iroha::Config& config);

} // namespace key_check
