iroha（Linux版）

ニューラルかな漢字変換の入力メソッドです。IBus（GNOME など）と fcitx5 で使えます。

入れ方
  ./install.sh
  （sudo のパスワードを求められます。最後に ibus-daemon を起動し直します）
  そのあと「設定 > キーボード > 入力ソース」で「日本語 (iroha)」を足してください。
  初めて入れるときは、変換モデル（約82MB）を ~/.local/share/iroha/models に取ります。

外し方
  ./uninstall.sh
  （学習・ユーザ辞書・モデルの入ったデータフォルダ ~/.local/share/iroha は残します）

要るもの
  Ubuntu 22.04 以降（x86_64）。ライブラリ: libibus-1.0-5 libgtk-4-1 libcurl4 libsecret-1-0
  libfontconfig1 libharfbuzz0b libxtst6

設定
  上のバーの入力ソースのメニューの「設定…」、または /usr/local/lib/iroha/iroha-settings

記録
  ~/.local/share/iroha/logs/（ibus-iroha-<ホスト名>.log・launch-<ホスト名>.log）

ライセンス
  本体: MIT。変換モデル iroha-kkc-v1: CC BY-SA 4.0。辞書データ（azooKey_dictionary_storage）: Apache-2.0
  （/usr/local/lib/iroha/Dictionary/LICENSE-azooKey_dictionary_storage）。記号のデータ（Mozc）: BSD 3-Clause
  （/usr/local/lib/iroha/Symbols/LICENSE）。
  https://github.com/TetsuakiBaba/iroha
