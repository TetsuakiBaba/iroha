#pragma once
#include <string>
#include <vector>

// 変換サーバ（iroha-server.exe）への問い合わせクライアント。
// サーバが起動していなければ、このDLLと同じディレクトリのexeを起動する。
namespace ConvertClient {

// サーバの事前起動（TIP有効化時に呼ぶとモデルのプリロードが先に走る）。
// 起動を待たずにすぐ返る。
void EnsureServer();

// 変換要求。成功時true（candidatesは尤度順・UTF-32）。
// サーバ未起動なら起動を試みて待つ（コールドスタート時は数秒ブロックしうる）。
bool Convert(const std::u32string& reading, const std::u32string& context,
             int candidateCount, std::vector<std::u32string>* candidates);

// 変換確定の通知（学習用）。baselineはエンジンの第一候補で、
// これと違う確定だけがサーバ側で学習される。失敗は無視する
void NotifyCommit(const std::u32string& reading, const std::u32string& committed,
                  const std::u32string& baseline);

} // namespace ConvertClient
