"""Media directory storage: file naming, fsync and listing.

The media directory is the FAT32 boot partition on the Pi
(``/boot/firmware/Camera``), so it can be slow, case-insensitive and it may be
mounted read-only.  Every filesystem operation therefore has a best-effort
error path: the UI must never crash because a file could not be touched.

This module never imports pygame or picamera2.
"""

from __future__ import annotations

import logging
import os
import re
import threading
from dataclasses import dataclass
from typing import Iterator

from .. import config

logger = logging.getLogger("camera.media")

#: ``IMG_000123.jpg`` -> photo, ``VID_000123.mp4`` -> video.
_PHOTO_RE = re.compile(r"^IMG_(\d{6})\.jpg$")
_VIDEO_RE = re.compile(r"^VID_(\d{6})\.mp4$")

#: Highest sequence number that still fits the six-digit file name.
MAX_SEQ = 999_999

try:  # The shared dataclass lives in app.interfaces, which imports numpy.
    from ..interfaces import MediaItem
except ImportError:  # pragma: no cover - bare PC without numpy
    logger.warning(
        "app.interfaces unavailable (numpy missing); using a local MediaItem"
    )

    @dataclass
    class MediaItem:  # type: ignore[no-redef]
        """Fallback copy of :class:`app.interfaces.MediaItem`."""

        path: str
        kind: str  # "photo" | "video"
        name: str
        size: int
        mtime: float
        seq: int


def _parse_name(name: str) -> tuple[str, int] | None:
    """Return ``(kind, seq)`` for a recognised media name, else ``None``."""
    match = _PHOTO_RE.match(name)
    if match is not None:
        return "photo", int(match.group(1))
    match = _VIDEO_RE.match(name)
    if match is not None:
        return "video", int(match.group(1))
    return None


class Storage:
    """Thread-safe access to the media directory.

    Implements :class:`app.interfaces.StorageProtocol`.  Photos and videos
    share one sequence counter so names sort together and never collide.
    """

    def __init__(self, media_dir: str | None = None) -> None:
        raw_dir = media_dir if media_dir is not None else config.MEDIA_DIR
        self.media_dir: str = os.path.abspath(os.path.expanduser(raw_dir))
        self._lock = threading.Lock()

        # Creating the directory is only best effort: in the tests (and when
        # the FAT partition is read-only) this may legitimately fail.  The
        # error is reported again when an actual write is attempted.
        try:
            os.makedirs(self.media_dir, exist_ok=True)
        except OSError as exc:
            logger.warning(
                "cannot create media directory %s: %s", self.media_dir, exc
            )

        # The scan walks the whole FAT directory.  On a card with thousands of
        # photos that is slow enough to delay the daemon's shared-memory
        # handshake, so do it lazily on the first photo instead.
        self._next_seq: int | None = None

    # ------------------------------------------------------------------
    # Sequence handling
    # ------------------------------------------------------------------
    def _scan_next_seq(self) -> int:
        """Rescan the directory and return ``max(existing seq) + 1``."""
        highest = 0
        try:
            with os.scandir(self.media_dir) as entries:
                for entry in entries:
                    parsed = _parse_name(entry.name)
                    if parsed is None:
                        continue
                    try:
                        if not entry.is_file():
                            continue
                    except OSError:
                        # A dangling entry is not worth a log line per scan.
                        continue
                    highest = max(highest, parsed[1])
        except OSError as exc:
            logger.warning("cannot scan media directory %s: %s", self.media_dir, exc)
            return 1
        return highest + 1

    def _next_path(self, prefix: str, extension: str) -> str:
        """Return a fresh, unused path for ``prefix`` and ``extension``."""
        with self._lock:
            if self._next_seq is None:
                self._next_seq = self._scan_next_seq()
            seq = self._next_seq
            # Skip names that appeared after the initial scan (e.g. the user
            # copied files onto the card, or another process wrote one).
            while seq <= MAX_SEQ:
                path = os.path.join(self.media_dir, f"{prefix}_{seq:06d}{extension}")
                if not os.path.exists(path):
                    break
                seq += 1
            else:  # pragma: no cover - 999 999 files on a FAT partition
                logger.error(
                    "media sequence exhausted in %s; reusing %d", self.media_dir, seq
                )
            if seq > MAX_SEQ:  # pragma: no cover - defensive
                seq = MAX_SEQ
            self._next_seq = seq + 1
            return os.path.join(self.media_dir, f"{prefix}_{seq:06d}{extension}")

    # ------------------------------------------------------------------
    # StorageProtocol
    # ------------------------------------------------------------------
    def next_photo_path(self) -> str:
        """Return the next free ``IMG_NNNNNN.jpg`` path (file not created)."""
        return self._next_path("IMG", config.PHOTO_EXT)

    def next_video_path(self) -> str:
        """Return the next free ``VID_NNNNNN.mp4`` path (file not created)."""
        return self._next_path("VID", config.VIDEO_EXT)

    def free_bytes(self) -> int:
        """Free bytes available to the user on the media filesystem."""
        try:
            stat = os.statvfs(self.media_dir)
        except OSError as exc:
            logger.warning("statvfs failed for %s: %s", self.media_dir, exc)
            return 0
        return int(stat.f_bavail * stat.f_frsize)

    def fsync_file(self, path: str) -> None:
        """Flush ``path`` to the card, then flush its parent directory.

        On vfat the directory fsync normally fails with ``EINVAL``; that is
        expected and only logged at debug level.
        """
        try:
            fd = os.open(path, os.O_RDONLY)
        except OSError as exc:
            logger.warning("fsync: cannot open %s: %s", path, exc)
            return
        try:
            os.fsync(fd)
        except OSError as exc:
            logger.warning("fsync failed for %s: %s", path, exc)
        finally:
            try:
                os.close(fd)
            except OSError:  # pragma: no cover - close rarely fails
                logger.debug("fsync: close failed for %s", path, exc_info=True)

        parent = os.path.dirname(os.path.abspath(path)) or "."
        flags = os.O_RDONLY
        o_directory = getattr(os, "O_DIRECTORY", 0)
        try:
            dir_fd = os.open(parent, flags | o_directory)
        except OSError as exc:
            logger.debug("fsync: cannot open directory %s: %s", parent, exc)
            return
        try:
            os.fsync(dir_fd)
        except OSError as exc:
            # vfat does not support fsync on directories.
            logger.debug("fsync of directory %s failed: %s", parent, exc)
        finally:
            try:
                os.close(dir_fd)
            except OSError:  # pragma: no cover
                logger.debug("fsync: close failed for directory %s", parent)

    def list_media(self) -> list[MediaItem]:
        """List photos and videos, newest (highest sequence) first."""
        items: list[MediaItem] = []
        try:
            entries: Iterator[os.DirEntry[str]] = os.scandir(self.media_dir)
        except OSError as exc:
            logger.warning("cannot list media in %s: %s", self.media_dir, exc)
            return items

        with entries:
            for entry in entries:
                parsed = _parse_name(entry.name)
                if parsed is None:
                    continue
                try:
                    if not entry.is_file():
                        continue
                    stat = entry.stat()
                except OSError as exc:
                    logger.debug("skipping %s: %s", entry.name, exc)
                    continue
                kind, seq = parsed
                items.append(
                    MediaItem(
                        path=entry.path,
                        kind=kind,
                        name=entry.name,
                        size=int(stat.st_size),
                        mtime=float(stat.st_mtime),
                        seq=seq,
                    )
                )

        # Newest first: by sequence, then by mtime as a tie breaker (users can
        # end up with IMG_000007.jpg and VID_000007.mp4 side by side).
        items.sort(key=lambda item: (item.seq, item.mtime), reverse=True)
        return items

    # ------------------------------------------------------------------
    # Gallery helpers (not part of StorageProtocol)
    # ------------------------------------------------------------------
    def delete(self, path: str) -> bool:
        """Delete a media file, its cached thumbnail(s) and fsync the directory.

        Returns ``True`` when the file is gone (including "was already gone").
        """
        target = os.path.abspath(os.path.expanduser(os.fspath(path)))
        ok = True
        try:
            os.remove(target)
        except FileNotFoundError:
            logger.debug("delete: %s already gone", target)
        except OSError as exc:
            logger.warning("cannot delete %s: %s", target, exc)
            ok = False

        # Thumbnails are regenerable cache entries; never fail the delete.
        try:
            from .gallery import remove_thumbnails

            remove_thumbnails(os.path.basename(target))
        except Exception:  # noqa: BLE001 - cache cleanup must never hurt
            logger.debug("thumbnail cleanup failed for %s", target, exc_info=True)

        parent = os.path.dirname(target) or "."
        flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
        try:
            dir_fd = os.open(parent, flags)
        except OSError as exc:
            logger.debug("delete: cannot open directory %s: %s", parent, exc)
            return ok
        try:
            os.fsync(dir_fd)
        except OSError as exc:
            logger.debug("delete: fsync of %s failed: %s", parent, exc)
        finally:
            try:
                os.close(dir_fd)
            except OSError:  # pragma: no cover
                logger.debug("delete: close failed for %s", parent)
        return ok

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return f"Storage(media_dir={self.media_dir!r})"
