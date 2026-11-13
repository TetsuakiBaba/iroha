#!/usr/bin/python3
"""IBus を通して iroha のエンジンにキーを送り、アプリが受け取るもの（確定・未確定文字列・候補）を見る。

インストールせずに試すには、先に開発用のエンジンを起動しておく（コンポーネントを登録する）:
    ./linux/build/ibus-iroha/ibus-engine-iroha &
    /usr/bin/python3 linux/ibus-iroha/tests/ibus_e2e.py "kyouhaiitenkidesune<space><enter>"

テスト用の入力コンテキストを作って iroha を割り当てる。IBus 1.5 はエンジンをすべての入力コンテキストで共有するので、
終わったら元のエンジンに戻す。キー列の書き方は iroha-cli session と同じ（<space> <enter> <esc> <bs>
<left> <right> <up> <down> <tab> <f6>〜<f10> <wait>。Shift は <S-left> のように。<C-x> は Ctrl+x）。
--context で周辺テキスト（カーソルより前の文書）を渡す。
"""
import argparse
import sys
import time

import gi

gi.require_version("IBus", "1.0")
from gi.repository import GLib, IBus  # noqa: E402

KEYS = {
    "space": IBus.KEY_space, "enter": IBus.KEY_Return, "esc": IBus.KEY_Escape, "bs": IBus.KEY_BackSpace,
    "left": IBus.KEY_Left, "right": IBus.KEY_Right, "up": IBus.KEY_Up, "down": IBus.KEY_Down,
    "tab": IBus.KEY_Tab, "f6": IBus.KEY_F6, "f7": IBus.KEY_F7, "f8": IBus.KEY_F8, "f9": IBus.KEY_F9,
    "f10": IBus.KEY_F10, "zenkaku": IBus.KEY_Zenkaku_Hankaku,
}


def parse(text):
    strokes = []
    i = 0
    while i < len(text):
        if text[i] != "<":
            c = text[i]
            strokes.append((c, ord(c), IBus.ModifierType.SHIFT_MASK if c.isupper() else 0))
            i += 1
            continue
        close = text.index(">", i)
        name = text[i + 1:close]
        state = 0
        while len(name) > 2 and name[1] == "-" and name[0] in "SC":
            state |= IBus.ModifierType.SHIFT_MASK if name[0] == "S" else IBus.ModifierType.CONTROL_MASK
            name = name[2:]
        if name == "wait":
            strokes.append(("<wait>", None, 0))
        else:
            keyval = KEYS.get(name, ord(name) if len(name) == 1 else None)
            if keyval is None:
                sys.exit(f"不明なキー: <{name}>")
            strokes.append((text[i:close + 1], keyval, state))
        i = close + 1
    return strokes


class Watcher:
    def __init__(self, context):
        self.document = ""      # 確定した文字列（アプリの文書にあたる）
        self.preedit = ""
        self.lookup = []
        self.cursor = 0
        self.lookup_visible = False
        self.aux = ""
        context.connect("commit-text", self.on_commit)
        context.connect("update-preedit-text", self.on_preedit)
        context.connect("update-preedit-text-with-mode", self.on_preedit_with_mode)
        context.connect("hide-preedit-text", lambda *_: setattr(self, "preedit", ""))
        context.connect("update-lookup-table", self.on_lookup)
        context.connect("hide-lookup-table", lambda *_: setattr(self, "lookup_visible", False))
        context.connect("update-auxiliary-text", lambda _c, t, v: setattr(self, "aux", t.get_text() if v else ""))
        context.connect("hide-auxiliary-text", lambda *_: setattr(self, "aux", ""))
        context.connect("delete-surrounding-text", self.on_delete)

    def on_commit(self, _context, text):
        self.document += text.get_text()

    def on_preedit(self, _context, text, _cursor, visible):
        self.preedit = text.get_text() if visible else ""

    def on_preedit_with_mode(self, _context, text, _cursor, visible, _mode):
        self.preedit = text.get_text() if visible else ""

    def on_lookup(self, _context, table, visible):
        self.lookup = [table.get_candidate(i).get_text() for i in range(table.get_number_of_candidates())]
        self.cursor = table.get_cursor_in_page()
        self.lookup_visible = visible

    def on_delete(self, _context, offset, nchars):
        if offset < 0 and -offset == nchars:
            self.document = self.document[:offset]

    def line(self, label, ms):
        out = f"{label:<9}| 文書「{self.document}」"
        if self.preedit:
            out += f" 未確定「{self.preedit}」"
        if self.lookup_visible:
            out += " 候補: " + " ".join(("*" if i == self.cursor else "") + c for i, c in enumerate(self.lookup))
        if self.aux:
            out += f"  ({self.aux})"
        return out + f"  [{ms:.0f}ms]"


def pump(seconds):
    end = time.monotonic() + seconds
    loop = GLib.MainContext.default()
    while time.monotonic() < end:
        while loop.pending():
            loop.iteration(False)
        time.sleep(0.005)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("keys")
    parser.add_argument("--context", default="")
    parser.add_argument("--settle", type=float, default=0.6, help="キーごとに待つ秒数（ライブ変換の到着）")
    args = parser.parse_args()

    IBus.init()
    bus = IBus.Bus()
    if not bus.is_connected():
        sys.exit("ibus-daemon に接続できません")
    previous = bus.get_global_engine()
    previous_name = previous.get_name() if previous else None
    context = bus.create_input_context("iroha-e2e")
    caps = (IBus.Capabilite.PREEDIT_TEXT | IBus.Capabilite.AUXILIARY_TEXT | IBus.Capabilite.LOOKUP_TABLE
            | IBus.Capabilite.FOCUS | IBus.Capabilite.SURROUNDING_TEXT)
    context.set_capabilities(caps)
    watcher = Watcher(context)
    try:
        context.focus_in()
        context.set_engine("iroha")
        pump(0.3)
        engine = context.get_engine()
        if not engine or engine.get_name() != "iroha":
            sys.exit("iroha のエンジンがありません（開発用のエンジンを起動したか確かめる）")
        watcher.document = args.context
        for label, keyval, state in parse(args.keys):
            if keyval is None:
                pump(args.settle)
                continue
            text = IBus.Text.new_from_string(watcher.document)
            context.set_surrounding_text(text, len(watcher.document), len(watcher.document))
            start = time.monotonic()
            handled = context.process_key_event(keyval, 0, state)
            if not handled and keyval < 0x7f and not state & IBus.ModifierType.CONTROL_MASK:
                watcher.document += chr(keyval)  # アプリがそのまま受けた
            context.process_key_event(keyval, 0, state | IBus.ModifierType.RELEASE_MASK)
            pump(args.settle)
            print(watcher.line(label + ("" if handled else " (渡す)"), (time.monotonic() - start) * 1000))
        print(f"結果: 「{watcher.document}」" + (f" 未確定「{watcher.preedit}」" if watcher.preedit else ""))
    finally:
        context.focus_out()
        pump(0.2)
        context.destroy()
        if previous_name and previous_name != "iroha":
            bus.set_global_engine(previous_name)
            print(f"（全体のエンジンを {previous_name} に戻した）", file=sys.stderr)


if __name__ == "__main__":
    main()
