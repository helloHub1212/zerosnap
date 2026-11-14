"""Camera daemon for the native C++ UI.

The daemon owns everything that needs Python: the camera, the encoder and the
SD card.  It publishes preview frames, state and side files through one
POSIX shared memory segment; the C++ ``zerosnap-ui`` process owns the panel and
the touchscreen.

Run with::

    python3 -m app.daemon

It is intentionally linear: one loop that polls commands, copies the newest
preview frame and republishes the state.  All blocking work (capture, encode,
file IO) still happens inside :class:`Picamera2Service`'s worker thread.
"""

from __future__ import annotations

import ctypes
import logging
import os
import signal
import sys
import threading
import time
from pathlib import Path

import numpy as np

from . import config
from .backend import create_camera_service
from .media.storage import Storage

try:  # installed next to this module by install-camera.sh
    from .camera_shm import CameraShm
except ImportError:  # source checkout or /opt/zerosnap/native
    for candidate in (
        Path(__file__).resolve().parents[2] / "native",
        Path("/opt/zerosnap/native/python"),
    ):
        if (candidate / "camera_shm.py").is_file():
            sys.path.insert(0, str(candidate))
            break
    from camera_shm import CameraShm  # type: ignore[no-redef]

logger = logging.getLogger("camera.daemon")

#: CameraState.video_mode <-> CamVideoMode in cam.h
VIDEO_MODE_TO_ID = {"1080p30": 0, "720p30": 1, "480p60": 2}
ID_TO_VIDEO_MODE = {v: k for k, v in VIDEO_MODE_TO_ID.items()}

#: Video was removed from the product: only stills remain.
_CMD = {
    1: "shutter",
    7: "refresh",
    8: "quit",
}

_RGB565: object | None = None
_RGB565_TRIED = False


def _rgb565_function() -> object | None:
    """Load the C RGB888->RGB565 helper compiled on first boot."""
    global _RGB565, _RGB565_TRIED
    if _RGB565_TRIED:
        return _RGB565
    _RGB565_TRIED = True
    for path in (
        os.path.join(os.path.expanduser("~"), ".cache", "camera", "libyuv420.so"),
        "/opt/zerosnap/lib/libyuv420.so",
    ):
        try:
            library = ctypes.CDLL(path)
            function = library.rgb888_to_rgb565
        except (OSError, AttributeError):
            continue
        function.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
        function.restype = None
        _RGB565 = function
        logger.info("native RGB565 conversion enabled: %s", path)
        return function
    logger.info("native RGB565 conversion not available; using numpy")
    return None


def to_rgb565(rgb: np.ndarray) -> bytes:
    """Convert HxWx3 uint8 RGB to packed little-endian RGB565 bytes."""
    height, width = int(rgb.shape[0]), int(rgb.shape[1])
    native = _rgb565_function()
    if native is not None:
        source = np.ascontiguousarray(rgb)
        out = np.empty((height * width,), dtype=np.uint16)
        native(source.ctypes.data, width, height, out.ctypes.data)
        return out.tobytes()
    a = rgb.astype(np.uint16)
    packed = ((a[..., 0] >> 3) << 11) | ((a[..., 1] >> 2) << 5) | (a[..., 2] >> 3)
    return packed.astype("<u2").tobytes()


class PreviewWorker(threading.Thread):
    """Generate the 320x240 RGB565 previews the C++ gallery reads.

    Photos are decoded with Pillow's draft mode (1/8 scale DCT) and video
    posters come from one ffmpeg frame.  It runs at the lowest priority the
    scheduler allows and pauses while the encoder is running, because a Pi Zero
    has exactly one core to share.
    """

    def __init__(self, service: object, media_dir: str, preview_dir: str,
                 stop: threading.Event) -> None:
        super().__init__(daemon=True, name="preview-worker")
        self.service = service
        self.media_dir = media_dir
        self.preview_dir = preview_dir
        self.stop = stop

    def _pending(self) -> list[str]:
        try:
            entries = [
                e for e in os.scandir(self.media_dir)
                if e.is_file() and e.name.lower().endswith((".jpg", ".jpeg"))
            ]
        except OSError:
            return []
        pending = []
        for entry in sorted(entries, key=lambda e: e.stat().st_mtime, reverse=True):
            base = os.path.splitext(entry.name)[0]
            target = os.path.join(self.preview_dir, base + ".565")
            try:
                if os.path.getsize(target) == 320 * 240 * 2:
                    continue
            except OSError:
                pass
            pending.append(entry.path)
        return pending

    @staticmethod
    def _load_photo(path: str) -> np.ndarray:
        from PIL import Image

        with Image.open(path) as image:
            image.draft("RGB", (320, 240))
            image = image.convert("RGB")
            scale = max(320.0 / image.width, 240.0 / image.height)
            size = (max(320, int(image.width * scale + 0.5)),
                    max(240, int(image.height * scale + 0.5)))
            image = image.resize(size, Image.BILINEAR)
            left = (image.width - 320) // 2
            top = (image.height - 240) // 2
            image = image.crop((left, top, left + 320, top + 240))
            return np.asarray(image, dtype=np.uint8)

    def run(self) -> None:
        logger.info("preview worker started")
        while not self.stop.is_set():
            try:
                self.service.get_state()
            except Exception:
                time.sleep(1.0)
                continue

            pending = self._pending()
            if not pending:
                for _ in range(20):
                    if self.stop.is_set():
                        return
                    time.sleep(0.25)
                continue

            for path in pending:
                if self.stop.is_set():
                    return
                base = os.path.splitext(os.path.basename(path))[0]
                target = os.path.join(self.preview_dir, base + ".565")
                try:
                    rgb = self._load_photo(path)
                    data = to_rgb565(rgb)
                    tmp = target + ".tmp"
                    with open(tmp, "wb") as handle:
                        handle.write(data)
                    os.replace(tmp, target)
                    logger.info("preview generated: %s", os.path.basename(target))
                except Exception as exc:
                    logger.warning("preview failed for %s: %s", path, exc)
                time.sleep(0.05)


class Daemon:
    def __init__(self) -> None:
        self.storage = Storage()
        self.service = create_camera_service(self.storage)
        self.shm: CameraShm | None = None
        self.running = True
        self.last_photo: str | None = None
        self.last_video: str | None = None
        self.last_rgb: np.ndarray | None = None
        self.preview_dir = os.path.join(self.storage.media_dir, ".preview")
        self._counts_at = 0.0
        self._counts = (0, 0)
        self._last_frame_at = 0.0
        self._published_rgb: object | None = None
        self._last_retry = 0.0
        self._first_frame_logged = False
        self._preview_stop = threading.Event()
        self._preview_worker: PreviewWorker | None = None

    # -- setup ---------------------------------------------------------

    @staticmethod
    def _wait_for_camera(timeout: float | None = None) -> bool:
        """Wait until libcamera can actually enumerate the sensor.

        The native UI starts at ~8 s into boot, while the old X11 build only
        appeared around 25 s.  At 8 s the OV5647 is often not probed yet, so
        ``Picamera2()`` raises ``IndexError: list index out of range``.  The
        old build then sat on that error forever; here we wait for the camera
        to exist and retry later if it still fails.
        """
        if timeout is None:
            try:
                timeout = float(os.environ.get("CAMERA_WAIT_TIMEOUT", "25"))
            except ValueError:
                timeout = 25.0
        deadline = time.monotonic() + timeout
        announced = False
        while time.monotonic() < deadline:
            try:
                from picamera2 import Picamera2

                info = Picamera2.global_camera_info()
                if info:
                    logger.info(
                        "libcamera sees %d camera(s), first is %s",
                        len(info), info[0].get("Model", "unknown"),
                    )
                    time.sleep(0.3)
                    return True
            except Exception:
                pass
            if not announced and time.monotonic() > deadline - timeout + 3.0:
                logger.info("waiting for libcamera to enumerate a camera")
                announced = True
            time.sleep(0.5)
        logger.warning("libcamera found no camera within %.0f s; will retry", timeout)
        return False

    def open(self) -> None:
        logger.info("daemon: creating shared memory")
        os.makedirs(self.preview_dir, exist_ok=True)
        self.shm = CameraShm(create=True)
        logger.info("daemon: shared memory ready")
        # Publish an empty state immediately so the UI can link and tell
        # "camera still opening" apart from "daemon is not running".
        self.shm.publish_state(
            mode=0, recording=0, video_mode=0, flags=0, rec_seconds=0,
            photo_count=0, video_count=0, free_mb=0, battery_pct=0,
            last_photo="", last_video="", status_text="",
        )
        self._wait_for_camera()
        logger.info("daemon: starting camera service")
        self.service.start()
        logger.info("daemon: camera service started")
        self._preview_worker = PreviewWorker(
            self.service, self.storage.media_dir, self.preview_dir,
            self._preview_stop,
        )
        self._preview_worker.start()
        logger.info("daemon started, media=%s", self.storage.media_dir)

    def close(self) -> None:
        logger.info("daemon stopping")
        self._preview_stop.set()
        if self._preview_worker is not None:
            self._preview_worker.join(timeout=3.0)
        try:
            self.service.stop()
        except Exception:
            logger.exception("error stopping the camera service")
        if self.shm is not None:
            try:
                self.shm.publish_state(flags=0, status_text="")
            except Exception:
                pass
            self.shm.close()
            self.shm = None

    # -- helpers -------------------------------------------------------

    def _media_counts(self) -> tuple[int, int]:
        now = time.monotonic()
        if now - self._counts_at < 2.0:
            return self._counts
        photos = 0
        try:
            with os.scandir(self.storage.media_dir) as entries:
                for entry in entries:
                    if entry.name.lower().endswith((".jpg", ".jpeg")):
                        photos += 1
        except OSError:
            pass
        self._counts = (photos, 0)
        self._counts_at = now
        return self._counts

    def _write_preview(self, media_path: str, rgb: np.ndarray) -> None:
        """Save a 320x240 RGB565 copy next to the media file."""
        try:
            name = os.path.splitext(os.path.basename(media_path))[0]
            target = os.path.join(self.preview_dir, name + ".565")
            data = to_rgb565(rgb)
            tmp = target + ".tmp"
            with open(tmp, "wb") as handle:
                handle.write(data)
            os.replace(tmp, target)
        except OSError:
            logger.exception("cannot write preview for %s", media_path)

    def _handle(self, cmd: int, arg: int) -> None:
        name = _CMD.get(cmd)
        if name is None:
            return
        logger.info("command %s(%s)", name, arg)
        if name == "shutter":
            self.service.request_photo()
        elif name == "refresh":
            self._counts_at = 0.0
        elif name == "quit":
            self.running = False

    def _publish_state(self, state: object) -> None:
        photos, videos = self._media_counts()
        flags = 0
        if getattr(state, "available", False):
            flags |= 1 << 2  # CAM_FLAG_CAM_READY
        if getattr(state, "photo_busy", False) or getattr(state, "recording", False):
            flags |= 1 << 0  # CAM_FLAG_BUSY
        if getattr(state, "error", None):
            flags |= 1 << 1  # CAM_FLAG_ERROR

        self.shm.publish_state(
            mode=0,
            recording=0,
            video_mode=0,
            flags=flags,
            rec_seconds=int(getattr(state, "recording_seconds", 0.0) or 0),
            photo_count=photos,
            video_count=videos,
            free_mb=int((getattr(state, "free_bytes", 0) or 0) // (1024 * 1024)),
            battery_pct=0,
            last_photo=os.path.basename(getattr(state, "last_photo", "") or ""),
            last_video=os.path.basename(getattr(state, "last_video", "") or ""),
            status_text=(getattr(state, "error", None) or "")[:200],
        )

    # -- main loop -----------------------------------------------------

    def run(self) -> None:
        while self.running:
            try:
                pending = self.shm.poll_command()
                if pending is not None:
                    self._handle(int(pending[0]), int(pending[1]))

                state = self.service.get_state()

                # The camera can fail to open early in boot (regulator, udev
                # or sensor probe ordering).  Retry until it works instead of
                # sitting on "camera not started" forever.
                if (not getattr(state, "available", False)
                        and getattr(state, "error", None)
                        and time.monotonic() - self._last_retry > 5.0):
                    self._last_retry = time.monotonic()
                    logger.warning("camera unavailable (%s); retrying",
                                   getattr(state, "error", "unknown"))
                    try:
                        self.service.retry()
                    except Exception:
                        logger.exception("camera retry failed")

                self._publish_state(state)

                rgb = self.service.get_preview()
                if rgb is not None and rgb.size:
                    # The service publishes a new array per captured frame, so
                    # identity is enough to skip redundant conversions.
                    if rgb is not self._published_rgb:
                        self.shm.publish_frame(to_rgb565(rgb))
                        self._published_rgb = rgb
                        self._last_frame_at = time.monotonic()
                        if not self._first_frame_logged:
                            self._first_frame_logged = True
                            logger.info("daemon: first preview published (%dx%d)",
                                        rgb.shape[1], rgb.shape[0])
                    self.last_rgb = rgb

                photo = getattr(state, "last_photo", None)
                if photo and photo != self.last_photo:
                    self.last_photo = photo
                    if self.last_rgb is not None:
                        self._write_preview(photo, self.last_rgb)

                video = getattr(state, "last_video", None)
                if video and video != self.last_video:
                    self.last_video = video
                    if self.last_rgb is not None:
                        self._write_preview(video, self.last_rgb)

                if (self.last_rgb is not None and
                        time.monotonic() - self._last_frame_at > 5.0):
                    # Do not hold a stale frame forever when the camera stalls.
                    self.last_rgb = None
            except Exception:
                logger.exception("daemon loop error")
                time.sleep(0.5)

            time.sleep(0.008)


def main() -> int:
    cache_dir = Path(getattr(config, "CACHE_DIR", os.path.expanduser("~/.cache/zerosnap")))
    cache_dir.mkdir(parents=True, exist_ok=True)
    log_path = os.environ.get("CAMERA_LOG_PATH") or str(cache_dir / "app.log")
    try:
        Path(log_path).parent.mkdir(parents=True, exist_ok=True)
        handler = logging.FileHandler(log_path)
    except OSError:
        handler = logging.FileHandler(cache_dir / "app.log")
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)-7s %(name)s: %(message)s",
        handlers=[handler, logging.StreamHandler()],
    )

    daemon = Daemon()
    daemon.open()

    def _stop(signum: int, frame: object) -> None:
        daemon.running = False

    signal.signal(signal.SIGTERM, _stop)
    signal.signal(signal.SIGINT, _stop)

    try:
        daemon.run()
    finally:
        daemon.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
