"""Synthetic camera backend for development on a PC.

The mock needs no picamera2, no pygame and no display:

* preview frames are numpy arrays with a moving box and a frame counter,
* photos are real JPEG files written with Pillow (a tiny embedded JPEG is used
  when Pillow is missing),
* recordings are produced with an ffmpeg ``testsrc`` clip and fall back to a
  small placeholder file when ffmpeg is unavailable.

The threading model mirrors the real backend: a worker thread publishes frames
and the public methods only flip state under a lock.
"""

from __future__ import annotations

import base64
import logging
import math
import os
import subprocess
import threading
import time
from dataclasses import replace
from typing import Any

import numpy as np

from .. import config
from ..interfaces import CameraState, StorageProtocol

logger = logging.getLogger("camera.backend")

_PREVIEW_WIDTH, _PREVIEW_HEIGHT = config.PREVIEW_SIZE
_STARTUP_DELAY = 0.25
_FRAME_INTERVAL = 1.0 / 30.0
_PHOTO_DELAY = 0.35
_FFMPEG_TIMEOUT = 15.0

#: Minimal 1x1 grey JPEG, used only when Pillow cannot write the photo.
_FALLBACK_JPEG = base64.b64decode(
    "/9j/4AAQSkZJRgABAQEAYABgAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0a"
    "HBwgJC4nICIsIxwcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/wAALCAABAAEBAREA/8QAFAABAAAAAAAA"
    "AAAAAAAAAAAACf/EABQQAQAAAAAAAAAAAAAAAAAAAAD/2gAIAQEAAD8AVN//2Q=="
)

#: Small non-empty placeholder written when ffmpeg cannot produce the MP4.
_FALLBACK_MP4 = (
    b"\x00\x00\x00\x18ftypmp42\x00\x00\x00\x00mp42isom"
    + b"\x00" * 2048
)


class MockCameraService:
    """CameraProtocol implementation that never touches real hardware.

    ``storage`` provides media paths and fsync; a lightweight fake is enough
    for the unit tests.
    """

    def __init__(self, storage: StorageProtocol) -> None:
        self._storage = storage

        self._lock = threading.Lock()
        self._state = CameraState(
            available=False,
            mode="photo",
            video_mode=config.DEFAULT_VIDEO_MODE,
            sensor_max=(2592, 1944),
        )
        self._frame_index = 0
        self._started_at = time.monotonic()
        self._preview: np.ndarray | None = None
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._photo_thread: threading.Thread | None = None
        self._video_thread: threading.Thread | None = None
        self._record_path: str | None = None
        self._record_started = 0.0
        self._record_bitrate = config.VIDEO_MODES[config.DEFAULT_VIDEO_MODE][3]

        self._pillow_checked = False
        self._image_mod: Any = None
        self._image_draw_mod: Any = None
        self._pillow()

        # A frame is available immediately, even before start().
        self._preview = self._render_frame(0, 0.0)
        self._frame_index = 1

    # ------------------------------------------------------------------
    # Public API (non-blocking)
    # ------------------------------------------------------------------

    def start(self) -> None:
        """Start the preview generator after a short simulated delay."""
        try:
            if self._thread is not None and self._thread.is_alive():
                return
            self._stop.clear()
            self._thread = threading.Thread(
                target=self._run, name="mock-camera", daemon=True
            )
            self._thread.start()
        except Exception:
            logger.exception("cannot start the mock camera thread")
            self._set_error("cannot start the mock camera")

    def stop(self) -> None:
        """Stop the preview generator and any helper thread."""
        try:
            self._stop.set()
            thread = self._thread
            if thread is not None and thread.is_alive():
                thread.join(timeout=2.0)
            self._thread = None
            with self._lock:
                if self._state.recording:
                    self._state.recording = False
            for helper in (self._photo_thread, self._video_thread):
                if helper is not None and helper.is_alive():
                    helper.join(timeout=2.0)
            with self._lock:
                self._state.available = False
                self._state.photo_busy = False
                self._state.fps = 0.0
        except Exception:
            logger.exception("cannot stop the mock camera thread")

    def get_state(self) -> CameraState:
        """Return a copy of the current state."""
        try:
            with self._lock:
                return replace(self._state)
        except Exception:
            logger.exception("cannot read the mock camera state")
            return CameraState()

    def get_preview(self) -> np.ndarray | None:
        """Return the newest synthetic frame (uint8 HxWx3 RGB)."""
        try:
            with self._lock:
                return self._preview
        except Exception:
            logger.exception("cannot read the mock preview")
            return None

    def set_mode(self, mode: str) -> None:
        """Switch between ``"photo"`` and ``"video"``."""
        try:
            if mode not in ("photo", "video"):
                logger.warning("ignoring invalid camera mode %r", mode)
                return
            with self._lock:
                if self._state.mode == mode:
                    return
                was_recording = self._state.recording
            if was_recording:
                self.stop_recording()
            with self._lock:
                self._state.mode = mode
            logger.info("mock camera mode set to %s", mode)
        except Exception:
            logger.exception("set_mode(%r) failed", mode)

    def set_video_mode(self, mode: str) -> None:
        """Select a video mode; rejected while a recording is running."""
        try:
            if mode not in config.VIDEO_MODES:
                logger.warning("ignoring unknown video mode %r", mode)
                return
            with self._lock:
                if self._state.recording:
                    logger.info("cannot change video mode while recording")
                    return
                self._state.video_mode = mode
        except Exception:
            logger.exception("set_video_mode(%r) failed", mode)

    def request_photo(self) -> None:
        """Pretend to capture a JPEG; returns immediately."""
        try:
            with self._lock:
                if self._state.recording:
                    logger.info("cannot take a photo while recording")
                    return
                if self._state.photo_busy:
                    logger.info("a photo is already being captured")
                    return
                self._state.photo_busy = True
            thread = threading.Thread(
                target=self._photo_worker, name="mock-photo", daemon=True
            )
            self._photo_thread = thread
            thread.start()
        except Exception:
            logger.exception("request_photo failed")
            self._set_photo_busy(False)

    def start_recording(self) -> None:
        """Pretend to start recording; requires video mode."""
        try:
            with self._lock:
                if self._state.recording:
                    return
                if self._state.mode != "video":
                    logger.info("recording requires video mode; ignored")
                    return
                video_mode = self._state.video_mode
                try:
                    path = self._storage.next_video_path()
                except Exception as exc:
                    logger.exception("cannot allocate a video path")
                    self._state.error = f"cannot allocate a video path: {exc}"
                    return
                self._record_path = path
                self._record_started = time.monotonic()
                self._record_bitrate = config.VIDEO_MODES[video_mode][3]
                self._state.recording = True
                self._state.recording_seconds = 0.0
                self._state.recording_size = 0
                self._state.error = None
            logger.info("mock recording started: %s", path)
        except Exception:
            logger.exception("start_recording failed")
            self._set_error("cannot start the mock recording")

    def stop_recording(self) -> None:
        """Stop recording and render the file in a background thread."""
        try:
            with self._lock:
                if not self._state.recording:
                    return
                path = self._record_path
                started = self._record_started
                self._record_path = None
                self._state.recording = False
                self._state.recording_seconds = round(
                    max(0.0, time.monotonic() - started), 2
                )
            if path is None:
                return
            thread = threading.Thread(
                target=self._finalize_video,
                args=(path, max(0.0, time.monotonic() - started)),
                name="mock-video",
                daemon=True,
            )
            self._video_thread = thread
            thread.start()
        except Exception:
            logger.exception("stop_recording failed")

    def retry(self) -> None:
        """Clear the error and report the camera as available again."""
        try:
            with self._lock:
                self._state.error = None
                self._state.available = False
                self._state.photo_busy = False
                self._state.recording = False
            if self._thread is None or not self._thread.is_alive():
                self.start()
                return
            threading.Thread(
                target=self._delayed_available, name="mock-retry", daemon=True
            ).start()
        except Exception:
            logger.exception("retry failed")

    # ------------------------------------------------------------------
    # Preview generation
    # ------------------------------------------------------------------

    def _run(self) -> None:
        logger.info("mock camera thread starting")
        try:
            self._stop.wait(_STARTUP_DELAY)
            if self._stop.is_set():
                return
            with self._lock:
                self._state.available = True
                self._state.sensor_max = (2592, 1944)
            while not self._stop.is_set():
                tick = time.monotonic()
                with self._lock:
                    index = self._frame_index
                    self._frame_index += 1
                    started = self._record_started
                    recording = self._state.recording
                frame = self._render_frame(index, tick - self._started_at)
                with self._lock:
                    self._preview = frame
                    self._state.fps = 30.0
                    if recording and started:
                        elapsed = tick - started
                        self._state.recording_seconds = round(max(0.0, elapsed), 2)
                        self._state.recording_size = int(
                            max(0.0, elapsed) * self._record_bitrate / 8.0
                        )
                sleep_for = _FRAME_INTERVAL - (time.monotonic() - tick)
                if sleep_for > 0:
                    self._stop.wait(sleep_for)
        except Exception:
            logger.exception("mock camera thread failed")
            self._set_error("mock camera thread failed")

    def _delayed_available(self) -> None:
        if self._stop.wait(_STARTUP_DELAY):
            return
        with self._lock:
            self._state.available = True
            self._state.error = None

    def _render_frame(self, index: int, stamp: float) -> np.ndarray:
        """Build one 320x240 RGB test pattern with a moving box and counter."""
        frame = np.empty((_PREVIEW_HEIGHT, _PREVIEW_WIDTH, 3), dtype=np.uint8)
        frame[:, :, :] = (24, 28, 36)

        # Static grid makes scaling artefacts obvious.
        frame[::40, :, :] = (44, 50, 62)
        frame[:, ::40, :] = (44, 50, 62)

        # Moving box bouncing horizontally.
        box_w, box_h = 48, 48
        x = int((index * 3) % max(1, _PREVIEW_WIDTH - box_w))
        y = int((_PREVIEW_HEIGHT - box_h) / 2.0 + 40.0 * math.sin(index / 18.0))
        y = max(0, min(_PREVIEW_HEIGHT - box_h, y))
        frame[y : y + box_h, x : x + box_w] = (230, 80, 60)
        frame[y + 4 : y + box_h - 4, x + 4 : x + box_w - 4] = (250, 160, 70)

        # Frame counter as six bits, visible without Pillow.
        for bit in range(6):
            if index & (1 << bit):
                left = 8 + bit * 14
                frame[8:18, left : left + 10] = (250, 250, 250)

        self._draw_overlay(frame, index, stamp)
        return np.ascontiguousarray(frame)

    def _draw_overlay(self, frame: np.ndarray, index: int, stamp: float) -> None:
        image_mod = self._image_mod
        draw_mod = self._image_draw_mod
        if image_mod is None or draw_mod is None:
            return
        try:
            image = image_mod.fromarray(frame)
            draw = draw_mod.Draw(image)
            draw.text((6, 26), f"MOCK {index:06d}", fill=(255, 255, 255))
            draw.text((6, 38), f"{stamp:8.2f}s", fill=(170, 214, 255))
            draw.text(
                (6, _PREVIEW_HEIGHT - 16),
                f"{_PREVIEW_WIDTH}x{_PREVIEW_HEIGHT} RGB",
                fill=(190, 250, 190),
            )
            frame[:, :, :] = np.asarray(image)
        except Exception:
            logger.debug("cannot draw the mock overlay", exc_info=True)
            self._image_mod = None
            self._image_draw_mod = None

    def _pillow(self) -> Any:
        """Import Pillow once and remember whether it is usable."""
        if self._pillow_checked:
            return self._image_mod
        self._pillow_checked = True
        try:
            from PIL import Image, ImageDraw

            self._image_mod = Image
            self._image_draw_mod = ImageDraw
        except Exception:
            logger.info("Pillow is unavailable; mock photos use an embedded JPEG")
            self._image_mod = None
            self._image_draw_mod = None
        return self._image_mod

    # ------------------------------------------------------------------
    # Photo / video helpers
    # ------------------------------------------------------------------

    def _photo_worker(self) -> None:
        try:
            time.sleep(_PHOTO_DELAY)
            path = self._storage.next_photo_path()
            self._write_photo(path)
            try:
                self._storage.fsync_file(path)
            except Exception:
                logger.exception("cannot fsync the mock photo %s", path)
            with self._lock:
                self._state.last_photo = path
                self._state.error = None
            logger.info("mock photo saved: %s", path)
        except Exception as exc:
            logger.exception("mock photo capture failed")
            self._set_error(f"photo capture failed: {exc}")
        finally:
            self._set_photo_busy(False)

    def _write_photo(self, path: str) -> None:
        directory = os.path.dirname(path) or "."
        os.makedirs(directory, exist_ok=True)
        image_mod = self._image_mod
        if image_mod is None:
            with open(path, "wb") as handle:
                handle.write(_FALLBACK_JPEG)
            return

        with self._lock:
            index = self._frame_index
        frame = self._render_frame(index, time.monotonic() - self._started_at)
        image = image_mod.fromarray(frame)
        try:
            image.save(path, format="JPEG", quality=config.PHOTO_QUALITY)
        finally:
            image.close()

    def _finalize_video(self, path: str, elapsed: float) -> None:
        try:
            directory = os.path.dirname(path) or "."
            os.makedirs(directory, exist_ok=True)
            if not self._render_test_video(path):
                logger.warning("writing a placeholder video to %s", path)
                with open(path, "wb") as handle:
                    handle.write(_FALLBACK_MP4)
            size = self._file_size(path)
            try:
                self._storage.fsync_file(path)
            except Exception:
                logger.exception("cannot fsync the mock video %s", path)
            with self._lock:
                self._state.last_video = path
                self._state.recording_size = size
                self._state.recording_seconds = round(max(elapsed, 1.0), 2)
            logger.info("mock video saved: %s (%d bytes)", path, size)
        except Exception:
            logger.exception("mock video finalisation failed")

    @staticmethod
    def _render_test_video(path: str) -> bool:
        """Render a short valid MP4 with ffmpeg; False when that fails."""
        command = [
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "error",
            "-f",
            "lavfi",
            "-i",
            "testsrc=size=320x240:rate=30",
            "-t",
            "1",
            "-pix_fmt",
            "yuv420p",
            "-y",
            path,
        ]
        try:
            result = subprocess.run(
                command,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                timeout=_FFMPEG_TIMEOUT,
                check=False,
            )
        except Exception:
            logger.info("ffmpeg is unavailable for the mock recording", exc_info=True)
            return False
        if result.returncode != 0:
            logger.info(
                "ffmpeg mock recording failed (%s): %s",
                result.returncode,
                result.stderr.decode("utf-8", "replace").strip(),
            )
            return False
        return os.path.getsize(path) > 0

    # ------------------------------------------------------------------
    # Small helpers
    # ------------------------------------------------------------------

    def _set_photo_busy(self, busy: bool) -> None:
        with self._lock:
            self._state.photo_busy = busy

    def _set_error(self, message: str) -> None:
        logger.error("mock camera error: %s", message)
        with self._lock:
            self._state.error = message
            self._state.available = False
            self._state.photo_busy = False
            self._state.recording = False
            self._state.fps = 0.0

    @staticmethod
    def _file_size(path: str) -> int:
        try:
            return os.path.getsize(path)
        except OSError:
            return 0
