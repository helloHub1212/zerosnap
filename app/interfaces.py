"""Interfaces shared between the camera backend, media layer and UI.

This module must stay free of pygame / picamera2 imports so the unit tests can
run on a PC.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Protocol, runtime_checkable

import numpy as np


@dataclass
class CameraState:
    """Thread-safe snapshot of the camera/service state."""

    available: bool = False
    error: str | None = None
    mode: str = "photo"  # "photo" | "video"
    video_mode: str = "1080p30"
    photo_busy: bool = False
    recording: bool = False
    recording_seconds: float = 0.0
    recording_size: int = 0
    last_photo: str | None = None
    last_video: str | None = None
    sensor_max: tuple[int, int] | None = None
    free_bytes: int = 0
    dropped_frames: int = 0
    fps: float = 0.0


@dataclass
class MediaItem:
    """One photo or video in the media directory."""

    path: str
    kind: str  # "photo" | "video"
    name: str
    size: int
    mtime: float
    seq: int


@runtime_checkable
class StorageProtocol(Protocol):
    media_dir: str

    def next_photo_path(self) -> str: ...

    def next_video_path(self) -> str: ...

    def free_bytes(self) -> int: ...

    def fsync_file(self, path: str) -> None: ...

    def list_media(self) -> list[MediaItem]: ...


@runtime_checkable
class CameraProtocol(Protocol):
    def start(self) -> None: ...

    def stop(self) -> None: ...

    def get_state(self) -> CameraState: ...

    def get_preview(self) -> np.ndarray | None: ...

    def set_mode(self, mode: str) -> None: ...

    def set_video_mode(self, mode: str) -> None: ...

    def request_photo(self) -> None: ...

    def start_recording(self) -> None: ...

    def stop_recording(self) -> None: ...

    def retry(self) -> None: ...
