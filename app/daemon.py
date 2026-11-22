"""Camera daemon for the native C UI.

The daemon owns everything that needs Python: the camera, the encoder and the
SD card.  It publishes preview frames, state and side files through one
POSIX shared memory segment; the C ``zerosnap-ui`` process owns the panel and
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
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    import numpy as np

from . import config
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
    9: "desktop",
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
    # Imported here (not at module import time) so the daemon can create its
    # shared-memory segment before paying numpy's multi-second cold-start cost.
    import numpy as np

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
    """Generate the 320x240 RGB565 previews the C gallery reads.

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
        import numpy as np
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
        # The daemon starts this thread as soon as the camera worker is up.
        # Give Picamera2 a few seconds to finish opening before touching the
        # FAT partition: on a full card the directory scan and JPEG decoding
        # would otherwise compete with sensor setup on the single core.
        if self.stop.wait(3.0):
            return
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
        # The Storage object is created in open(): the unit now starts before
        # /boot/firmware (the FAT media partition) is mounted, and creating the
        # media directory too early would put it on the root filesystem where
        # the later mount hides it.
        self.storage: Storage | None = None
        # Building the service imports numpy and picamera2 (and its libcamera
        # bindings), which is the single slowest part of a cold start on a Pi
        # Zero.  open() creates the shared-memory segment *before* doing that,
        # so the UI can link and show "camera starting" instead of "daemon
        # down" while Python is still loading.
        self.service: object | None = None
        self.shm: CameraShm | None = None
        self.running = True
        self._started_at = time.monotonic()
        self.last_photo: str | None = None
        self.last_video: str | None = None
        self.last_rgb: np.ndarray | None = None
        self.preview_dir = ""
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
    def _preload_imports() -> None:
        """Warm the expensive imports while the media partition mounts.

        This runs in a helper thread started by :meth:`open`; the main thread
        waits for /boot/firmware in parallel, so the two costs overlap instead
        of adding up.
        """
        # The main daemon runs at nice -10 (set by start-camera.sh) so the UI
        # stays responsive.  This thread does the multi-second Python import;
        # at the same priority it starves udev on the single-core Pi Zero and
        # delays the camera kernel drivers by ~11 s.  Give the CPU back so
        # /dev/media* appears on time.
        try:
            os.nice(10)
        except OSError:
            pass
        started = time.monotonic()
        try:
            import numpy  # noqa: F401  (imported for its side effect)
            if not config.MOCK:
                import picamera2  # noqa: F401  (same)
            from .backend import create_camera_service  # noqa: F401  (same)
            logger.info(
                "daemon: camera imports preloaded (%.1f s)",
                time.monotonic() - started,
            )
        except Exception:
            logger.info("daemon: camera import preload failed", exc_info=True)

    @staticmethod
    def _media_mount_expected(parent: str) -> bool:
        """True when fstab lists ``parent`` as its own filesystem."""
        try:
            with open("/etc/fstab", "r", encoding="utf-8", errors="replace") as fh:
                for line in fh:
                    line = line.strip()
                    if not line or line.startswith("#"):
                        continue
                    fields = line.split()
                    if len(fields) >= 2 and fields[1] == parent:
                        return True
        except OSError:
            pass
        return False

    @classmethod
    def _wait_for_media_mount(cls, timeout: float = 60.0) -> None:
        """Wait until the FAT media partition is mounted.

        ``zerosnap-ui.service`` starts before ``local-fs.target`` so the Python
        import overlaps the rest of the boot.  On a Pi Zero udev needs a few
        seconds to publish the SD partition, so the media directory must not be
        created before its mount point exists.
        """
        parent = os.path.dirname(os.path.abspath(config.MEDIA_DIR))
        if not cls._media_mount_expected(parent):
            return  # media dir lives on the root filesystem
        started = time.monotonic()
        while time.monotonic() - started < timeout:
            try:
                if os.path.ismount(parent):
                    logger.info(
                        "daemon: media partition ready (%.1f s)",
                        time.monotonic() - started,
                    )
                    return
            except OSError:
                pass
            time.sleep(0.05)
        logger.error(
            "daemon: %s is still not mounted after %.0f s; "
            "photos may be written to the root filesystem",
            parent,
            timeout,
        )

    @staticmethod
    def _camera_devices_ready() -> bool:
        """Cheap check that libcamera's media drivers have probed.

        ``Picamera2._cm`` wraps ``libcamera.CameraManager.singleton()``, which
        enumerates cameras **once** for the whole process.  Calling
        ``global_camera_info()`` before the Unicam/ISP/sensor drivers have
        created their nodes freezes an empty camera list forever - the UI then
        sits on "camera not started" no matter how long we retry.  Waiting on
        device nodes costs nothing and leaves the CPU to udev.
        """
        try:
            media = [name for name in os.listdir("/dev")
                     if name.startswith("media") and name[5:].isdigit()]
        except OSError:
            media = []
        # rpi/vc4 needs both the Unicam and the ISP media devices.
        return len(media) >= 2

    @classmethod
    def _wait_for_camera_devices(cls, timeout: float = 60.0) -> bool:
        started = time.monotonic()
        announced = False
        while time.monotonic() - started < timeout:
            if cls._camera_devices_ready():
                # Let udev finish creating the subdev nodes before the first
                # (process-wide cached) libcamera enumeration.
                time.sleep(0.3)
                logger.info("daemon: camera media devices ready (%.1f s)",
                            time.monotonic() - started)
                return True
            if not announced and time.monotonic() - started > 3.0:
                logger.info("waiting for the camera kernel drivers "
                            "(/dev/media0, /dev/media1)")
                announced = True
            time.sleep(0.1)
        logger.error("camera kernel drivers did not appear within %.0f s", timeout)
        return False

    @staticmethod
    def _wait_for_camera(timeout: float | None = None) -> bool:
        """Wait until libcamera can actually enumerate the sensor.

        Must only be called after :meth:`_wait_for_camera_devices` succeeded:
        the first call caches the camera list for the lifetime of the process.
        """
        if timeout is None:
            try:
                timeout = float(os.environ.get("CAMERA_WAIT_TIMEOUT", "20"))
            except ValueError:
                timeout = 20.0
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
                    # No settle sleep here: Picamera2() is opened on the worker
                    # thread immediately and retries internally if the sensor
                    # needs another moment.
                    return True
            except Exception:
                pass
            if not announced and time.monotonic() > deadline - timeout + 3.0:
                logger.info("waiting for libcamera to enumerate a camera")
                announced = True
            time.sleep(0.15)
        logger.error("libcamera found no camera within %.0f s", timeout)
        return False

    def open(self) -> None:
        # Shared memory first: the UI can link even while Python is still
        # importing numpy/picamera2.  This also makes the daemon's own startup
        # time visible in the log.
        logger.info("daemon: creating shared memory")
        self.shm = CameraShm(create=True)
        elapsed = time.monotonic() - self._started_at
        logger.info("daemon: shared memory ready (%.1f s, uptime %.1f s)",
                    elapsed, time.monotonic())
        # Publish an empty state immediately so the UI can link and tell
        # "camera still opening" apart from "daemon is not running".
        self.shm.publish_state(
            mode=0, recording=0, video_mode=0, flags=0, rec_seconds=0,
            photo_count=0, video_count=0, free_mb=0, battery_pct=0,
            last_photo="", last_video="", status_text="",
        )

        # Start the multi-second import now and wait for the media partition in
        # parallel.  The unit starts before local-fs.target, so at this point
        # /boot/firmware may not be mounted yet.
        preload = threading.Thread(
            target=self._preload_imports, name="import-preload", daemon=True
        )
        preload.start()
        self._wait_for_media_mount()
        # CRITICAL: do not touch libcamera until the kernel camera drivers have
        # created their media devices.  CameraManager.singleton() enumerates
        # once per process; an early call leaves the daemon with an empty
        # camera list forever.  This check is just a directory listing, so the
        # import in the helper thread keeps overlapping the wait for free.
        if not config.MOCK and not self._wait_for_camera_devices():
            raise RuntimeError("camera kernel drivers never appeared")
        preload.join()

        self.storage = Storage()
        self.preview_dir = os.path.join(self.storage.media_dir, ".preview")
        try:
            os.makedirs(self.preview_dir, exist_ok=True)
        except OSError:
            logger.warning("cannot create preview directory %s", self.preview_dir,
                           exc_info=True)

        logger.info("daemon: creating camera service")
        # Importing the backend pulls in numpy and picamera2.  Do it only
        # after the shared memory exists, so the UI can link immediately.
        from .backend import create_camera_service

        self.service = create_camera_service(self.storage)
        logger.info("daemon: camera service created (%.1f s, uptime %.1f s)",
                    time.monotonic() - self._started_at, time.monotonic())
        # CAMERA_MOCK is used by the host tests; there is no libcamera sensor to
        # wait for, and the 20 s poll would only slow the test down.
        if not config.MOCK and not self._wait_for_camera():
            # The first global_camera_info() call may already have cached an
            # empty camera list.  A fresh process is the only reliable reset;
            # start-camera.sh's supervisor restarts us after this failure.
            raise RuntimeError("libcamera cannot see the camera")
        logger.info("daemon: starting camera service")
        self.service.start()
        logger.info("daemon: camera service started (%.1f s, uptime %.1f s)",
                    time.monotonic() - self._started_at, time.monotonic())
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
        if self.service is not None:
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
        if self.storage is None:
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
            if self.service is not None:
                self.service.request_photo()
        elif name == "refresh":
            self._counts_at = 0.0
        elif name in ("quit", "desktop"):
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

                if self.service is None:
                    # open() failed before the worker existed.  Keep the shm
                    # alive so the UI can report the error instead of spinning
                    # on "daemon down".
                    time.sleep(0.5)
                    continue

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
                            logger.info(
                                "daemon: first preview published "
                                "(%dx%d, %.1f s, uptime %.1f s)",
                                rgb.shape[1], rgb.shape[0],
                                time.monotonic() - self._started_at,
                                time.monotonic(),
                            )
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
    # On the Pi stdout already points at the same file, so adding a
    # StreamHandler would write every line twice (and it did, historically).
    handlers: list[logging.Handler] = [handler]
    if sys.stderr.isatty():
        handlers.append(logging.StreamHandler())
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)-7s %(name)s: %(message)s",
        handlers=handlers,
    )

    # This is the first line after the cheap standard-library imports.  The
    # gap from start-camera.sh's header is Python's own startup cost; numpy and
    # picamera2 are imported later, after the shared-memory segment is visible.
    logger.info("daemon: modules loaded, initializing (uptime %.1f s)",
                time.monotonic())
    daemon = Daemon()
    try:
        daemon.open()
    except Exception:
        logger.exception("daemon could not initialise; supervisor will restart it")
        daemon.close()
        return 1

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
