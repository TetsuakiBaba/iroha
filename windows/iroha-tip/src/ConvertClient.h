#pragma once
#include <cstdint>
#include <string>

#include "iroha/input_session.h"

// 変換サーバ（iroha-server.exe）への問い合わせクライアント。
// サーバが起動していなければ、このDLLと同じディレクトリのexeを起動する。
//
// 入力の状態（読み・ライブ変換・文節・候補）は変換サーバのセッションが持つ。
// TIP はキーを送り、返ってきた表示（iroha::SessionOutput）を画面に反映する
namespace ConvertClient {

// サーバの事前起動（TIP有効化時に呼ぶとモデルのプリロードが先に走る）。
// 起動を待たずにすぐ返る。
void EnsureServer();

// キー 1 つをセッションに渡し、画面に反映する内容を受け取る。
// leftContext は、合成していないときにカーソルより前から読んだ文書のテキスト（読めなければ nullptr）。
// サーバに届かなければ起動を試みて待つ（コールドスタート時は数秒ブロックしうる）
bool SendKey(uint64_t sessionId, iroha::SessionKey key, char32_t character, bool shift,
             const std::u32string* leftContext, iroha::SessionOutput* output);

// セッションの合成を捨てる（アプリの側で合成が終わった）。届かなくても待たない
void ResetSession(uint64_t sessionId);

// ライブ変換が届いたか（revision のときから表示が変わったか）を確かめる。
// 変わっていれば changed が真になり、output に今の表示が入る。
// アプリのメッセージループから呼ぶので、サーバに届かなくても待たずに偽を返す
bool Poll(uint64_t sessionId, uint64_t revision, bool* changed, iroha::SessionOutput* output);

// セッションを捨てる（TIP が無効になった）。失敗は無視する
void EndSession(uint64_t sessionId);

} // namespace ConvertClient
