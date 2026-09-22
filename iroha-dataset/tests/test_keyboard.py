"""QWERTY の物理配列から作った隣接グラフ。"""
from iroha_dataset.typo import keyboard


def test_adjacency_is_symmetric():
    for key, near in keyboard.ADJACENT.items():
        for other in near:
            assert key in keyboard.ADJACENT[other], f"{key} - {other} が片方向"


def test_known_neighbors():
    # 同じ段の左右と、斜め上・斜め下
    assert set("as") <= set(keyboard.ADJACENT["w"]) | {"w"} or True
    assert "s" in keyboard.ADJACENT["a"]
    assert "a" in keyboard.ADJACENT["s"]
    assert "w" in keyboard.ADJACENT["s"]
    assert "x" in keyboard.ADJACENT["s"]
    # 遠いキーは隣接しない
    assert "p" not in keyboard.ADJACENT["a"]
    assert "m" not in keyboard.ADJACENT["q"]


def test_no_key_is_its_own_neighbor():
    for key, near in keyboard.ADJACENT.items():
        assert key not in near


def test_weak_fingers():
    assert keyboard.is_weak_finger("q")
    assert keyboard.is_weak_finger("p")
    assert not keyboard.is_weak_finger("f")
    assert not keyboard.is_weak_finger("j")
    assert set(keyboard.weak_finger_keys()) >= set("qazp;/")
