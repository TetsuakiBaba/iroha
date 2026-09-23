"""HTTP 取得。標準ライブラリだけで済ませる（依存を増やさない）。

公開サーバを叩くので、待ち時間（RateLimiter）と User-Agent を必ず入れる。
"""
from __future__ import annotations

import os
import time
import urllib.error
import urllib.request
from pathlib import Path

from iroha_dataset import __version__

USER_AGENT = (f"iroha-dataset/{__version__} "
              "(+https://github.com/tetsuakibaba/iroha; dataset builder for a Japanese IME)")


class DownloadError(RuntimeError):
    pass


def _body_hint(error: urllib.error.HTTPError, limit: int = 300) -> str:
    """エラー応答の本文を短く付ける。

    KAKEN は appid が通らないと ``<detail>Invalid APPID</detail>`` を本文で返すので、
    これが無いと「HTTP 403」だけになって原因が分からない。
    """
    try:
        body = error.read().decode("utf-8", "replace").strip()
    except Exception:
        return ""
    if not body:
        return ""
    body = " ".join(body.split())
    return f" — {body[:limit]}"


class RateLimiter:
    """前回のリクエストから interval 秒経つまで待つ。"""

    def __init__(self, interval: float):
        self.interval = max(0.0, float(interval))
        self._last = 0.0

    def wait(self) -> None:
        if self.interval <= 0:
            return
        elapsed = time.monotonic() - self._last
        if elapsed < self.interval:
            time.sleep(self.interval - elapsed)
        self._last = time.monotonic()


def http_get(url: str, *, timeout: float = 60.0, retries: int = 3,
             backoff: float = 2.0, accept_status: tuple[int, ...] = (200,)) -> bytes:
    last: Exception | None = None
    for attempt in range(retries):
        req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                if resp.status not in accept_status:
                    raise DownloadError(f"{url}: HTTP {resp.status}")
                return resp.read()
        except urllib.error.HTTPError as e:
            last = e
            # 4xx は再試行しても同じ（429 だけは待って再試行する）
            if e.code != 429 and 400 <= e.code < 500:
                raise DownloadError(f"{url}: HTTP {e.code}{_body_hint(e)}") from e
        except (urllib.error.URLError, TimeoutError, OSError) as e:
            last = e
        if attempt < retries - 1:
            time.sleep(backoff * (attempt + 1))
    raise DownloadError(f"{url}: {last}") from last


def download_to(url: str, dest: str | os.PathLike, *, force: bool = False,
                timeout: float = 120.0) -> Path:
    """dest が既にあれば取り直さない（force で上書き）。"""
    dest = Path(dest)
    if dest.exists() and dest.stat().st_size > 0 and not force:
        return dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    data = http_get(url, timeout=timeout)
    tmp = dest.with_suffix(dest.suffix + ".part")
    tmp.write_bytes(data)
    os.replace(tmp, dest)
    return dest


def download_stream_to(url: str, dest: str | os.PathLike, *, force: bool = False,
                       timeout: float = 120.0, retries: int = 5,
                       chunk_size: int = 1 << 20) -> Path:
    """大きなファイルをメモリに載せずに書く。途中で切れたら ``.part`` の続きから取り直す。

    dest が既にあれば取り直さない（force で上書き）。LLM-jp Corpus や zenz-v2.5 の
    ファイルは 1 本で数百 MB〜数 GB あるので ``download_to`` は使えない。
    """
    dest = Path(dest)
    if dest.exists() and dest.stat().st_size > 0 and not force:
        return dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix(dest.suffix + ".part")
    if force and tmp.exists():
        tmp.unlink()
    last: Exception | None = None
    for attempt in range(retries):
        offset = tmp.stat().st_size if tmp.exists() else 0
        headers = {"User-Agent": USER_AGENT}
        if offset:
            headers["Range"] = f"bytes={offset}-"
        req = urllib.request.Request(url, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                # Range を無視して全体を返すサーバでは最初から書き直す
                mode = "ab" if offset and resp.status == 206 else "wb"
                with open(tmp, mode) as fh:
                    while True:
                        block = resp.read(chunk_size)
                        if not block:
                            break
                        fh.write(block)
            os.replace(tmp, dest)
            return dest
        except urllib.error.HTTPError as e:
            last = e
            if e.code == 416 and tmp.exists():      # 取り終わっている
                os.replace(tmp, dest)
                return dest
            if e.code != 429 and 400 <= e.code < 500:
                raise DownloadError(f"{url}: HTTP {e.code}{_body_hint(e)}") from e
        except (urllib.error.URLError, TimeoutError, OSError) as e:
            last = e
        if attempt < retries - 1:
            time.sleep(2.0 * (attempt + 1))
    raise DownloadError(f"{url}: {last}") from last
