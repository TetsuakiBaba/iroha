import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from iroha_dataset.config import load_config  # noqa: E402


@pytest.fixture
def cfg():
    return load_config()


def _sudachi_available() -> bool:
    try:
        from sudachipy import Dictionary
        Dictionary(dict="full").create()
        return True
    except Exception:
        return False


SUDACHI = _sudachi_available()
needs_sudachi = pytest.mark.skipif(
    not SUDACHI, reason="SudachiPy + SudachiDict-full が入っていない")
