#pragma once
// 起動・終了の記録（macOS版の LaunchLog と同じ考え方）。予期しない終了・再起動を後から確かめるためのもの。
//
// - <データフォルダ>/logs/launch-<ホスト名>.log: 1 行 1 件の追記
//     launch        起動（PID・版・OS・ホスト）
//     exit          終了とその理由（disconnected = ibus-daemon が終わった / SIGTERM など）
//     unclean-exit  前回の起動の exit が記録されないまま次の起動に至った（クラッシュ・強制終了など）
//     crash-report  Ubuntu の apport が /var/crash に残したこのエンジンのレポート（前回の確認より新しいもの）
//     note          別のエンジンのプロセスが動作中だった等
// - <データフォルダ>/logs/state-<ホスト名>.json: 直前の起動の記録（終了時に exitedAt を書く）
// データフォルダは複数の端末で共有されることがあるので、ファイル名にホスト名を入れる

#include <filesystem>
#include <string>
#include <vector>

namespace iroha {

class LaunchLog {
public:
    // name: 記録するプロセスの名前（クラッシュレポートのファイル名と照らす。例 "ibus-engine-iroha"）
    LaunchLog(std::filesystem::path directory, std::string hostName, std::string name);

    // 起動を記録する（前回の終了が無ければ unclean-exit、新しいクラッシュレポートがあれば crash-report を先に書く）。
    // 書いた行を返す
    std::vector<std::string> RecordLaunch(const std::string& version,
                                          const std::filesystem::path& crashDirectory = "/var/crash");
    // 終了を記録する（直前の記録が自分のものであるときだけ終了時刻を書く）
    void RecordExit(const std::string& reason);

    std::filesystem::path LogPath() const;
    std::filesystem::path StatePath() const;

    // この行数を超えたら古い半分を捨てる
    static constexpr size_t kMaxLines = 2000;

private:
    void Append(const std::vector<std::string>& lines);
    std::filesystem::path directory_;
    std::string hostName_;
    std::string name_;
};

} // namespace iroha
